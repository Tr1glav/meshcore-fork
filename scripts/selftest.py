#!/usr/bin/env python3
"""Проверки прошивки на ПК — обёртка над общим репозиторием проверок.

Сами проверки живут в `mesh-network-tools` и одни на все прошивки: ядро у них общее, и
держать его проверки в потребителе значило проверять ядро из форка. Так и было — половина проверок существовала только здесь, а T-Deck возил то же ядро и проверял его на треть.

Здесь остался только запуск с нужной целью, чтобы привычная команда работала как раньше:

    python3 scripts/selftest.py

Тот же прогон напрямую, вместе с остальными целями:

    python3 ../mesh-network-tools/selftest.py --target fork
    python3 ../mesh-network-tools/selftest.py --target all

Где искать репозиторий проверок: переменная окружения MESHCORE_TOOLS, иначе соседний
каталог рабочего дерева — там же, где лежит ядро (см. symlink://../mesh-network-core в
platformio.ini). Ключи после имени скрипта уходят в общий запускатель как есть.
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = pathlib.Path(os.environ.get("MESHCORE_TOOLS") or (ROOT.parent / "mesh-network-tools"))

runner = TOOLS / "selftest.py"
if not runner.is_file():
    # Молча пропустить нельзя: прогон без проверок выглядит как успешный, и это ровно тот
    # случай, из-за которого CI однажды оставался зелёным, ничего не проверив.
    sys.exit(
        "[selftest] не найден репозиторий проверок: %s\n"
        "           положите mesh-network-tools рядом с прошивкой или задайте MESHCORE_TOOLS\n"
        "           git clone https://github.com/Tr1glav/mesh-network-tools" % runner)

sys.exit(subprocess.call([sys.executable, str(runner), "--target", "fork",
                          "--root", str(ROOT)] + sys.argv[1:]))
