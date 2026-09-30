Import("env")
import os
import re
import shutil
import struct
import zlib

# Сколько последних версий хранить для каждой платы
KEEP = 3
OUTPUT_DIR = "firmware_output"


def is_sensor_env(env):
    for d in env.get("CPPDEFINES", []):
        name = d[0] if isinstance(d, (list, tuple)) else d
        if name == "SENSOR_NODE":
            return True
    return False


def read_version(env):
    # version.txt уже обновлён pre-скриптом gen_version.py в этой же сборке
    with open(os.path.join(env.subst("$PROJECT_DIR"), "version.txt")) as fh:
        return fh.read().split()[0]


def cleanup(board_name, ext):
    # Только файлы этой платы (<env>_v<версия>.<ext>), старые — по времени сборки
    pattern = re.compile(rf"^{re.escape(board_name)}_v\d+\.\d+\.\d+\.{ext}$")
    candidates = sorted(
        (f for f in os.listdir(OUTPUT_DIR) if pattern.match(f)),
        key=lambda f: os.path.getmtime(os.path.join(OUTPUT_DIR, f)),
    )
    for old in candidates[:-KEEP]:
        try:
            os.remove(os.path.join(OUTPUT_DIR, old))
            print(f"🗑️  Removed old firmware: {old}")
        except OSError as e:
            print(f"⚠️  Failed to remove {old}: {e}")
    return min(len(candidates), KEEP)


def copy_firmware(source, target, env):
    # Путь к собранному файлу ("$BUILD_DIR/firmware.bin")
    firmware_path = str(target[0])
    board_name = env.get("PIOENV")
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    base = os.path.join(OUTPUT_DIR, f"{board_name}_v{read_version(env)}")

    if is_sensor_env(env):
        # Сенсоры обновляются только по mesh OTA сжатым файлом, формат OTA_Z_* в
        # lib/meshcore/include/config.h: [OTAZ][размер образа 4B LE][CRC32 образа 4B LE][zlib]
        with open(firmware_path, "rb") as fh:
            raw = fh.read()
        packed = (b"OTAZ" + struct.pack("<II", len(raw), zlib.crc32(raw) & 0xFFFFFFFF)
                  + zlib.compress(raw, 9))
        path = base + ".otaz"
        with open(path, "wb") as fh:
            fh.write(packed)
        print(f"\n🗜️  Mesh OTA firmware: {path} ({len(raw)} -> {len(packed)} байт)")
        kept = cleanup(board_name, "otaz")
    else:
        path = base + ".bin"
        shutil.copyfile(firmware_path, path)
        print(f"\n✅ Firmware copied to: {path}")
        kept = cleanup(board_name, "bin")

    print(f"📦 Kept {kept} firmware(s) for '{board_name}'\n")

    # Каждая успешная сборка коммитит и отправляет. Так велит правило 6 в instruction.md:
    # «комит не проблема, не делать релиз только без разрешения». Раньше отправка стояла за
    # GIT=1 и по умолчанию не делалась — теперь наоборот, по умолчанию делается.
    #
    # Выключается двумя способами, и оба обязаны работать:
    #   NOGIT=1 — в CI. Там push запустил бы workflow заново, то есть петлю. Это больше не
    #             «страховка»: без него сборка в Actions начнёт коммитить сама.
    #   GIT=0   — разовая сборка руками, когда в дереве лежит незаконченное.
    #
    # Релизом это НЕ является. Ветка release/v<версия>, появление которой запускает выкладку
    # в Actions, создаётся только при RELEASE=1 — то есть по прямой просьбе пользователя.
    # Что уедет в коммит, ограничено списками FORBIDDEN и ALLOW_NEW в scripts/release.py.
    if os.environ.get("NOGIT") != "1" and os.environ.get("GIT") != "0":
        import subprocess, sys
        script = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "release.py")
        cmd = [sys.executable, script]
        if os.environ.get("RELEASE") == "1":
            cmd.append("--release")
        subprocess.run(cmd, check=False)


# Регистрируем функцию как пост-действие для файла firmware.bin
env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)
