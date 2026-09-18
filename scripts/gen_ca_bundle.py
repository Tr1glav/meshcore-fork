#!/usr/bin/env python3
"""Набор корневых сертификатов для проверки TLS при загрузке прошивок.

Зачем: автообновление качает образы с GitHub, и раньше прошивка соглашалась на любой
сертификат (setInsecure). Подменить образ по пути мог кто угодно на маршруте — ни маркер
платы внутри образа, ни CRC32 от этого не защищают: маркер подделывается, а CRC32 считает
тот же, кто файл отдал.

Скрипт спрашивает у самих серверов, какой цепочкой они подписаны, берёт КОРЕНЬ каждой
цепочки из доверенного хранилища ЭТОЙ машины и складывает корни в
lib/meshcore/include/ca_bundle.h. Файл попадает в git: в нём только открытые сертификаты,
и он нужен и локальной сборке, и сборке в Actions.

ВАЖНО: запускать на машине, чьей сети вы доверяете. Если трафик проходит через
перехватывающий прокси (корпоративный фильтр, антивирус с проверкой HTTPS), в набор уедет
корень этого прокси — и прошивка будет доверять ему, а не GitHub. Имена корней скрипт
печатает: там должны быть известные удостоверяющие центры (DigiCert, Sectigo, ISRG и
подобные), а не имя вашего роутера или антивируса.

Запуск:
    python3 scripts/gen_ca_bundle.py                 хосты по умолчанию
    python3 scripts/gen_ca_bundle.py --show          только показать, файл не писать
    python3 scripts/gen_ca_bundle.py host1 host2     свои хосты (если поменялся релиз)

После правки набора прошивку нужно пересобрать и залить заново. Отключить проверку на
работающем устройстве можно настройкой: set tls_check 0 (это аварийный выход на случай
смены корневого сертификата, а не рабочий режим).
"""
import argparse
import pathlib
import socket
import ssl
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "lib" / "meshcore" / "include" / "ca_bundle.h"

# Откуда прошивка качает образы: FW_RELEASE_API и FW_RELEASE_DL из config.h, плюс хосты,
# на которые GitHub перенаправляет за самим файлом.
DEFAULT_HOSTS = [
    "api.github.com",
    "github.com",
    "objects.githubusercontent.com",
    "release-assets.githubusercontent.com",
]


def chain_root(host, port=443, timeout=10):
    """Корень цепочки сертификатов хоста, взятый из хранилища этой машины."""
    ctx = ssl.create_default_context()          # системное хранилище, проверка включена
    with socket.create_connection((host, port), timeout=timeout) as raw:
        with ctx.wrap_socket(raw, server_hostname=host) as tls:
            if not hasattr(tls, "get_verified_chain"):
                sys.exit("нужен Python 3.13+ (нет SSLSocket.get_verified_chain)")
            chain = tls.get_verified_chain()
            leaf = tls.getpeercert()
    if not chain:
        raise RuntimeError("сервер не отдал цепочку сертификатов")
    # Последний элемент проверенной цепочки — доверенный корень
    return ssl.DER_cert_to_PEM_cert(chain[-1]), leaf


def subject_of(pem):
    """Человекочитаемое имя сертификата — только для вывода в консоль."""
    try:
        import subprocess
        r = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                           input=pem, capture_output=True, text=True, check=False)
        line = (r.stdout or "").strip()
        if line:
            return line.replace("subject=", "").strip()
    except OSError:
        pass
    return "(имя не определено: нет openssl)"


def main():
    ap = argparse.ArgumentParser(description="собрать набор корневых сертификатов")
    ap.add_argument("hosts", nargs="*", default=None, help="хосты (по умолчанию GitHub)")
    ap.add_argument("--show", action="store_true", help="показать корни, файл не писать")
    args = ap.parse_args()
    hosts = args.hosts or DEFAULT_HOSTS

    roots, bad = {}, []
    for host in hosts:
        try:
            pem, leaf = chain_root(host)
        except Exception as e:                  # noqa: BLE001 — причина важна пользователю
            bad.append(f"{host}: {e}")
            print(f"  {host}: НЕ УДАЛОСЬ — {e}")
            continue
        name = subject_of(pem)
        roots.setdefault(pem, []).append(host)
        print(f"  {host}: корень {name}")

    if not roots:
        sys.exit("ни одного корня получить не удалось — набор не создан")
    if bad:
        print("\nчасть хостов недоступна; в набор попали только полученные корни:")
        for b in bad:
            print("   ", b)

    print(f"\nразных корней: {len(roots)}")
    if args.show:
        return

    lines = [
        "// Сгенерировано scripts/gen_ca_bundle.py — не править вручную.",
        "// Корневые сертификаты для проверки TLS при загрузке прошивок (открытые данные).",
        "// Хосты: " + ", ".join(hosts),
        "#pragma once",
        "",
        "static const char CA_BUNDLE_PEM[] =",
    ]
    for pem, used_by in roots.items():
        lines.append("    // " + subject_of(pem) + " — " + ", ".join(used_by))
        for row in pem.strip().splitlines():
            lines.append(f'    "{row}\\n"')
    lines.append("    ;")
    lines.append("")
    HEADER.write_text("\n".join(lines), encoding="utf-8")
    size = len("\n".join(lines))
    print(f"записано: {HEADER.relative_to(ROOT)} ({size} Б в образе)")
    print("пересоберите и залейте прошивку, иначе проверка не включится")


if __name__ == "__main__":
    main()
