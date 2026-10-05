#!/usr/bin/env python3
"""Отправка кода в git после сборки и пометка релизных версий.

Схема:
  * КАЖДАЯ сборка коммитит изменения и отправляет текущую ветку (develop);
  * с флагом --release дополнительно создаётся ветка release/v<версия> и уходит на origin.

Сами файлы прошивки в репозиторий не кладутся. Их собирает и выкладывает GitHub Actions
(.github/workflows/build.yml): появление ветки release/* запускает проверки и сборку, и
только если всё прошло — создаётся релиз с .bin и .otaz. Так в релиз физически не может
попасть прошивка, которая не собралась или не прошла тесты.

Вручную:
    python3 scripts/release.py                 коммит + отправка текущей ветки
    python3 scripts/release.py --release       то же + ветка release/v<версия>
    python3 scripts/release.py --dry-run       показать, что попадёт в коммит
    python3 scripts/release.py --no-push       только локально
    python3 scripts/release.py -m "текст"      своё описание коммита

Из сборки вызывается всегда (scripts/copy_firmware.py):
    pio run                     коммит и отправка текущей ветки
    RELEASE=1 pio run           плюс релизная ветка — это и есть релиз, только по просьбе
    GIT=0 pio run               не трогать git в этой сборке
    NOGIT=1 pio run             то же; обязательно в CI, иначе push перезапустит workflow
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Пути, которых в коммите быть не должно ни при каких обстоятельствах. .gitignore их и так
# исключает, но цена ошибки — пароли и ключи каналов в истории публичного репозитория,
# поэтому проверяем ещё раз перед фиксацией.
FORBIDDEN = ("secrets.ini", "secrets.json", ".env", "firmware_output/", "build_info.h")
# Откуда разрешено добавлять НОВЫЕ файлы. Правки уже отслеживаемых файлов берутся всегда, так
# что обычная работа этого списка не замечает. Смысл списка в другом: раньше здесь стоял
# git add -A, и в публичный репозиторий уехал бы ЛЮБОЙ файл, случайно оказавшийся в дереве —
# дамп, выгрузка логов, заметка с паролем. Чёрный список ниже поймал бы только пять имён,
# которые в него вписаны; этот — пропускает только то, что действительно является исходником.
ALLOW_NEW = ("src/", "lib/", "web/", "scripts/", "boards/", "variants/",
             ".github/", "platformio.ini", "version.txt", "core.ref", "README.md",
             "secrets.example.json", ".gitignore")


def core_pin_covers_local():
    """Покрывает ли core.ref то ядро, с которым прошивка собиралась только что.

    Локально ядро подключено symlink'ом на рабочее дерево, то есть сборка идёт с ЖИВЫМ ядром,
    а CI берёт ядро строго по core.ref. Если локальное ядро ушло вперёд пина, прошивка
    собирается здесь и ломается в Actions — ровно это и случилось 1 октября 2026: T-Deck
    отправился с убранной копией sensor_tasks.cpp, а core.ref указывал на v0.9.0, где этого
    файла в ядре ещё нет. `fatal error: sensor_tasks.h: No such file or directory`.

    Возвращает (ок, пояснение). Коммит при расхождении делается — он локальный и никому не
    мешает; запрещается именно отправка.
    """
    core = pathlib.Path(ROOT).parent / "mesh-network-core"
    try:
        pinned = (ROOT / "core.ref").read_text().split()[0]
    except (OSError, IndexError):
        return True, ""                      # нет пина — нечему расходиться
    if not (core / ".git").exists():
        return True, ""                      # ядра рядом нет: в CI так и бывает
    r = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                       cwd=str(core), capture_output=True, text=True)
    actual = (r.stdout or "").strip()
    if not actual or actual == pinned:
        return True, ""
    # Пин может быть предком локального ядра (это и есть «ушло вперёд»), а может и
    # разойтись по-настоящему. Для отправки разницы нет: CI соберёт не то, что собрали мы.
    return False, ("ядро в сборке %s, а core.ref пинует %s" % (actual, pinned))


def git(*args, check=True):
    r = subprocess.run(["git", *args], cwd=str(ROOT), check=False, text=True,
                       capture_output=True)
    if check and r.returncode != 0:
        sys.exit(f"git {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
    return (r.stdout or "").strip()


def git_try(*args):
    """Сетевые операции не должны ронять сборку: нет сети — просто сообщаем."""
    r = subprocess.run(["git", *args], cwd=str(ROOT), check=False, text=True,
                       capture_output=True, env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
    return r.returncode == 0, ((r.stderr or r.stdout).strip().splitlines() or [""])[-1]


# Описание коммита по умолчанию. Голый номер версии в истории бесполезен: из таких
# сообщений не собрать список отличий для описания релиза. Поэтому перечисляем области,
# которых коснулась правка, — это видно из путей изменённых файлов.
# Путей ядра здесь нет намеренно: mesh_rx, ota, crypto и appconfig живут в отдельном
# репозитории, и в diff ЭТОГО репозитория они не попадают никогда — оставленные записи
# просто молчали. Версия ядра видна по core.ref.
AREAS = (
    ("lib/meshcore/src/companion", "компаньон"),
    ("lib/meshcore/src/mqtt",      "MQTT"),
    ("lib/meshcore/src/support",   "прошивальщик"),
    ("lib/meshcore/src/fwupdate",  "автообновление"),
    ("lib/meshcore/src/display",   "экран"),
    ("lib/meshcore/src/web",       "страница"),
    ("lib/meshcore/",              "прошивка"),
    ("core.ref",                   "версия ядра"),
    ("src/main.cpp",               "главный цикл"),
    ("web/",                       "страница"),
    ("scripts/",                   "скрипты"),
    (".github/",                   "сборка"),
    ("platformio.ini",             "сборка"),
    ("README",                     "документация"),
)


def describe(files):
    """Короткое описание правки по списку изменённых путей."""
    names = []
    for path in files:
        for prefix, name in AREAS:
            if path.startswith(prefix) and name not in names:
                names.append(name)
                break
    return ", ".join(names)


def version():
    with open(ROOT / "version.txt") as fh:
        return fh.read().split()[0]


def main():
    ap = argparse.ArgumentParser(description="коммит, отправка и релизная ветка")
    ap.add_argument("--release", action="store_true",
                    help="создать ветку release/v<версия> — она запускает выкладку в Actions")
    ap.add_argument("--no-push", action="store_true", help="не отправлять на origin")
    ap.add_argument("--dry-run", action="store_true", help="только показать план")
    ap.add_argument("-m", "--message", help="описание коммита")
    args = ap.parse_args()

    ver = version()
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    rel_branch = f"release/v{ver}"

    # Правки отслеживаемых файлов — всегда; новое — только из ALLOW_NEW (см. выше).
    git("add", "-u")
    skipped = []
    # core.quotepath=false: иначе git печатает кириллицу в именах восьмеричными кодами,
    # и строка «этот файл не добавлен» становится нечитаемой ровно тогда, когда нужна.
    for f in git("-c", "core.quotepath=false", "ls-files", "--others",
                 "--exclude-standard").splitlines():
        if f.startswith(ALLOW_NEW):
            git("add", "--", f)
        else:
            skipped.append(f)
    if skipped:
        print("[release] новые файлы вне разрешённых мест НЕ добавлены: " + ", ".join(skipped))
    # Статусы, а не только имена: удаление закрытого файла из репозитория — это ровно то,
    # что нужно разрешить, а вот добавление такого файла останавливает коммит. Раньше
    # проверка смотрела только имена и не давала удалить лишний файл вовсе.
    entries = [l.split("\t") for l in git("diff", "--cached", "--name-status").splitlines() if l]
    files = [e[-1] for e in entries]
    added = [e[-1] for e in entries if not e[0].startswith("D")]

    bad = [f for f in added if any(f.startswith(p) or f.endswith(p) for p in FORBIDDEN)]
    if bad:
        git("reset", check=False)
        sys.exit("[release] в коммит попали закрытые файлы, остановлено: " + ", ".join(bad))

    if args.dry_run:
        print(f"[release] версия {ver}, ветка {branch}, файлов к коммиту: {len(files)}")
        for f in files[:20]:
            print("   ", f)
        if len(files) > 20:
            print(f"    ... и ещё {len(files) - 20}")
        if args.release:
            print(f"[release] была бы создана ветка {rel_branch}")
        git("reset", check=False)
        return

    if files:
        summary = describe(files)
        msg = args.message or (f"v{ver}: {summary}" if summary else f"v{ver}")
        git("commit", "-m", msg)
        print(f"[release] коммит {git('rev-parse', '--short', 'HEAD')} в {branch}: файлов {len(files)}")
    else:
        git("reset", check=False)
        print(f"[release] изменений нет, коммит не нужен (версия {ver})")

    if args.release:
        git("branch", "-f", rel_branch, "HEAD")
        print(f"[release] ветка {rel_branch} указывает на {git('rev-parse', '--short', 'HEAD')}")

    if args.no_push:
        print("[release] отправка отключена (--no-push)")
        return

    # Отправлять прошивку, собранную с ядром новее пина, нельзя: в Actions она соберётся по
    # core.ref и упадёт. Коммит при этом уже сделан — он локальный и ничего не ломает.
    covered, why = core_pin_covers_local()
    if not covered:
        print("[release] отправка ОТМЕНЕНА: " + why)
        print("[release] CI собирает прошивку по core.ref, а не по рабочему дереву. Выпустите "
              "ядро тегом и переведите core.ref на него — тогда отправляйте.")
        return

    ok, msg = git_try("push", "-u", "origin", branch)
    print(f"[release] отправка {branch}: " + ("готово" if ok else f"не удалась — {msg}"))

    if args.release:
        # --force-with-lease: релизная ветка всегда указывает на текущую версию, но чужие
        # изменения на ней перетирать нельзя
        ok, msg = git_try("push", "--force-with-lease", "origin", rel_branch)
        if ok:
            print(f"[release] ветка {rel_branch} отправлена — сборка и выкладка идут в Actions")
        else:
            print(f"[release] {rel_branch} отправить не удалось — {msg}")


if __name__ == "__main__":
    main()
