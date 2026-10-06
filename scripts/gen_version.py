import hashlib
import os
import subprocess
import sys
import time
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

# Версия MAJOR.MINOR.BUILD и время сборки. BUILD растёт, только когда меняются исходники прошивки.
# version.txt: строка 1 — версия (MAJOR/MINOR правятся руками), 2 — хэш исходников, 3 — unix-время версии.
# В код значения попадают через сгенерированный build_info.h, а не через -D: изменение глобального
# флага меняет команду компиляции каждого файла и заставляет пересобирать ядро Arduino и все библиотеки.
PROJECT_DIR = env.subst("$PROJECT_DIR")
VERSION_FILE = os.path.join(PROJECT_DIR, "version.txt")
HEADER = os.path.join(PROJECT_DIR, "lib", "meshcore", "include", "build_info.h")

# Ядро протокола (radio, mesh_rx/tx, ota, crypto, appconfig) живёт отдельным репозиторием и
# подключается symlink://../mesh-network-core (см. lib_deps в platformio.ini). Его исходники
# ОБЯЗАНЫ входить в хэш: иначе правка ядра не поднимает номер сборки, .otaz перезаписывается
# под тем же именем, а автообновление и mesh OTA считают узлы актуальными — прошивка другая,
# версия прежняя. Путь можно задать переменной MESHCORE_CORE, по умолчанию — соседний
# каталог, тот же, что в lib_deps.
CORE_DIR = os.path.abspath(os.environ.get("MESHCORE_CORE")
                           or os.path.join(PROJECT_DIR, os.pardir, "mesh-network-core"))
if not os.path.isdir(os.path.join(CORE_DIR, "src")):
    # Молча взять хэш без ядра нельзя: получилась бы прошивка с чужим номером версии.
    sys.exit("[gen_version] ядро протокола не найдено: %s (путь задаётся MESHCORE_CORE)"
             % CORE_DIR)

SOURCE_PATHS = [
    (PROJECT_DIR, "src"),
    (PROJECT_DIR, "lib"),
    (PROJECT_DIR, "web"),
    (PROJECT_DIR, "include"),
    (PROJECT_DIR, "boards"),
    (PROJECT_DIR, "variants"),
    (PROJECT_DIR, "platformio.ini"),
    (CORE_DIR, "src"),
    (CORE_DIR, "include"),
    (CORE_DIR, "library.json"),
]

# Служебные запуски (IntelliSense в IDE, clean) тоже исполняют pre-скрипты — версию не трогаем
NO_BUMP_TARGETS = {"idedata", "__idedata", "clean", "cleanall", "envdump", "compiledb", "menuconfig", "size"}


def source_files():
    for base, path in SOURCE_PATHS:
        full = os.path.join(base, path)
        if os.path.isfile(full):
            yield base, path
        for root, dirs, files in os.walk(full):
            dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]
            for name in files:
                rel = os.path.relpath(os.path.join(root, name), base)
                # build_info.h генерируем сами — в хэш он не входит, иначе версия менялась бы
                # от самой записи файла
                if not name.startswith(".") and not rel.endswith("build_info.h"):
                    yield base, rel


def sources_hash():
    h = hashlib.sha1()
    for base, rel in sorted(source_files(), key=lambda x: (x[0], x[1])):
        h.update(rel.replace(os.sep, "/").encode())
        with open(os.path.join(base, rel), "rb") as fh:
            h.update(fh.read())
    return h.hexdigest()[:12]


# Номер сборки держится двузначным: правило владельца — «если версия 0.5.99 становится
# 0.5.100, то она становится 0.6.1». Трёхзначный хвост читается плохо и ничего не сообщает:
# номер сборки растёт от каждой правки исходников, и его смысл — «насколько новее», а не
# «сколько всего». Поэтому сотая сборка внутри минорной версии переводит счётчик: минорная
# увеличивается, сборка начинается с ЕДИНИЦЫ (не с нуля — нулевой сборки не бывает, версия
# всегда собрана хотя бы раз).
BUILD_MAX = 99


def next_version(major, minor, build):
    build += 1
    if build > BUILD_MAX:
        minor += 1
        build = 1
    return major, minor, build


def write_if_changed(path, content):
    # не трогаем файл без нужды: для SCons перезапись того же содержимого — лишняя работа
    try:
        with open(path) as fh:
            if fh.read() == content:
                return
    except OSError:
        pass
    with open(path, "w") as fh:
        fh.write(content)


with open(VERSION_FILE) as fh:
    fields = fh.read().split()
major, minor, build = (int(p) for p in fields[0].split("."))
saved_hash = fields[1] if len(fields) > 1 else None
build_time = int(fields[2]) if len(fields) > 2 else int(time.time())
current_hash = sources_hash()

if not NO_BUMP_TARGETS & set(COMMAND_LINE_TARGETS):
    # без сохранённого хэша текущая версия просто привязывается к текущему коду
    if saved_hash is not None and saved_hash != current_hash:
        major, minor, build = next_version(major, minor, build)
        build_time = int(time.time())
    write_if_changed(VERSION_FILE, f"{major}.{minor}.{build}\n{current_hash}\n{build_time}\n")

def core_ref():
    """Какое ядро попало в сборку: пин из core.ref и то, на чём стоит соседний репозиторий.

    Локально ядро правится живьём (symlink://), поэтому расхождение с пином — не ошибка, а
    предупреждение: по нему видно, что прошивка собрана с кодом ядра, которого в core.ref
    ещё нет, и в CI эта же ветка соберётся иначе.
    """
    pinned = ""
    try:
        with open(os.path.join(PROJECT_DIR, "core.ref")) as fh:
            pinned = fh.read().split()[0]
    except (OSError, IndexError):
        return ""
    actual = ""
    try:
        r = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                           cwd=CORE_DIR, capture_output=True, text=True)
        actual = r.stdout.strip()
    except OSError:
        pass
    if actual and actual != pinned:
        print("[gen_version] ВНИМАНИЕ: ядро в сборке %s, а core.ref пинует %s"
              % (actual, pinned))
    return actual or pinned


def current_branch():
    """Имя ветки: в Actions оно есть в окружении, локально спрашиваем git."""
    name = os.environ.get("GITHUB_REF_NAME")
    if name:
        return name
    try:
        r = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                           cwd=PROJECT_DIR, capture_output=True, text=True)
        return r.stdout.strip()
    except OSError:
        return ""


# Чистый номер версии получают релизные сборки: в CI это сборка с ветки release/*, а
# локально — запуск с RELEASE=1, то есть тот, который и станет релизом. Всё остальное
# помечается как dev, чтобы на экране устройства сразу было видно, что прошивка не из
# релиза. В version.txt номер остаётся без суффикса.
#
# Оговорка: локально ветка release/* появляется только ПОСЛЕ сборки (её создаёт
# scripts/release.py пост-действием), поэтому ориентироваться на неё локально нельзя —
# отсюда и проверка переменной окружения.
version = f"{major}.{minor}.{build}"
is_release = os.environ.get("RELEASE") == "1" or current_branch().startswith("release/")
if not is_release:
    version += "dev"

write_if_changed(HEADER,
                 "// Сгенерировано scripts/gen_version.py перед сборкой — не править\n"
                 "#pragma once\n"
                 f"#define FW_VERSION \"{version}\"\n"
                 f"#define BUILD_UNIX_TIME {build_time}UL\n")
print(f"[gen_version] FW_VERSION = {version} (src {current_hash}, ядро {core_ref() or '?'})")
