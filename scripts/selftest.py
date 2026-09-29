#!/usr/bin/env python3
"""Проверки прошивки на ПК, без железа.

Что делает:
  * вырезает из исходников настоящие функции (CRC, jsonEscape, buildPingReply, rawBuildFrame),
    собирает их хостовым компилятором с санитайзерами и гоняет на граничных данных —
    так ловятся переполнения буферов, которые на плате проявились бы падением;
  * проверяет сканер маркера платы: маркер должен находиться при любой нарезке образа
    на куски, чужой код платы — отвергаться, отсутствие маркера — не считаться отказом;
  * проверяет формат .otaz (заголовок, CRC32, распаковка кусками по 240 Б, как на сенсоре);
  * проверяет синтаксис JavaScript страницы OTA.

Запуск: python3 scripts/selftest.py
Нужен g++; node — по желанию (без него проверка JS пропускается).
"""
import os
import pathlib
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
failures = []

# Ядро протокола (radio, mesh_rx/tx, ota, crypto, appconfig) живёт отдельным репозиторием и
# подключается symlink://../mesh-network-core — см. lib_deps в platformio.ini. Половина
# проверяемых функций лежит поэтому ВНЕ проекта: путь к ядру задаётся переменной
# MESHCORE_CORE, по умолчанию это соседний каталог, тот же, что в lib_deps.
CORE = pathlib.Path(os.environ.get("MESHCORE_CORE") or (ROOT.parent / "mesh-network-core"))



def check(name, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + name + ((" — " + detail) if detail and not ok else ""))
    if not ok:
        failures.append(name)


def grab(hint, signature):
    """Вырезает функцию из исходника по началу сигнатуры, считая фигурные скобки.

    hint — строка (путь от корня проекта) либо готовый Path: функции ядра берутся как
    CORE / "src/crypto.cpp", функции прошивки — как "lib/meshcore/src/mqtt.cpp".
    """
    # Путь — подсказка, а не требование: файлы переезжают при разборке на модули, а то и
    # целиком уезжают в отдельный репозиторий ядра, и жёсткая привязка ломает проверки на
    # ровном месте. Не нашлось по подсказке (в том числе если файла вовсе нет) — ищем
    # сигнатуру по всем исходникам прошивки и ядра.
    path = hint if isinstance(hint, pathlib.Path) else ROOT / hint
    if not (path.is_file() and signature in path.read_text(encoding="utf-8")):
        for cand in sorted(p for d in (ROOT / "lib/meshcore/src", ROOT / "src", CORE / "src")
                           if d.is_dir() for p in d.glob("*.cpp")):
            if signature in cand.read_text(encoding="utf-8"):
                path = cand
                break
        else:
            raise RuntimeError("не найдена функция %s (ядро: %s)" % (signature, CORE))
    src = path.read_text(encoding="utf-8")
    start = src.index(signature)
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise RuntimeError("не найден конец функции " + signature)


HOST_MAIN = r"""
int main() {
    // buildPingReply: путь приходит из эфира, до 63 хопов по 4 байта
    uint8_t path[63 * 4];
    for (size_t i = 0; i < sizeof(path); i++) path[i] = (uint8_t)i;
    for (int hs = 1; hs <= 4; hs++) {
        for (int hops = 0; hops <= 63; hops++) {
            char out[100];
            memset(out, 'X', sizeof(out));
            buildPingReply(out, sizeof(out), path, hops, hs);
            if (strnlen(out, sizeof(out)) >= sizeof(out)) {
                printf("buildPingReply: строка не завершена hs=%d hops=%d\n", hs, hops);
                return 1;
            }
        }
    }
    // jsonEscape: произвольный текст не должен вылезать за выходной буфер
    for (int len = 0; len < 200; len++) {
        char in[256], out[64];
        for (int i = 0; i < len; i++) in[i] = (char)(1 + rand() % 254);
        in[len] = 0;
        memset(out, 'X', sizeof(out));
        jsonEscape(in, out, sizeof(out));
        if (strnlen(out, sizeof(out)) >= sizeof(out)) {
            printf("jsonEscape: строка не завершена len=%d\n", len);
            return 1;
        }
    }
    // rawBuildFrame: кадр обязан влезать в буфер OTA_RAW_FRAME_MAX и в лимит SX1262
    uint8_t data[OTA_RAW_CHUNK_BYTES + 2], frame[OTA_RAW_FRAME_MAX];
    memset(data, 0xA5, sizeof(data));
    for (int n = 0; n <= (int)sizeof(data); n++) {
        int f = rawBuildFrame(frame, 0x02, 12345, data, n);
        if (f != 9 + n) { printf("rawBuildFrame: длина %d при n=%d\n", f, n); return 1; }
        if (f > 255) { printf("rawBuildFrame: кадр %d > 255 байт\n", f); return 1; }
    }
    const char* s = "meshcore";
    printf("crc32=%08X\n", (unsigned)~crc32_upd(0xFFFFFFFF, (const uint8_t*)s, strlen(s)));
    printf("crc16=%04X\n", crc16buf((const uint8_t*)s, strlen(s)));
    return 0;
}
"""


def host_functions_test():
    if not shutil.which("g++"):
        print("SKIP g++ не найден — проверки границ буферов пропущены")
        return
    code = (
        "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstdlib>\n"
        "#define OTA_RAW_CHUNK_BYTES 240\n"
        "#define OTA_RAW_FRAME_MAX (11 + OTA_RAW_CHUNK_BYTES)\n"
        "#define RAW_MAGIC0 0xBE\n#define RAW_MAGIC1 0xEF\n"
        + grab(CORE / "src/crypto.cpp", "uint16_t crc16buf(") + "\n"
        + grab(CORE / "src/crypto.cpp", "uint32_t crc32_upd(") + "\n"
        + grab(CORE / "src/crypto.cpp", "void jsonEscape(") + "\n"
        + grab(CORE / "src/mesh_rx.cpp", "void buildPingReply(") + "\n"
        + grab(CORE / "src/ota.cpp", "int rawBuildFrame(") + "\n"
        + HOST_MAIN
    )
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "t.cpp"
        exe = pathlib.Path(tmp) / "t"
        src.write_text(code, encoding="utf-8")
        build = subprocess.run(
            ["g++", "-std=c++17", "-fsanitize=address,undefined", "-g", str(src), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode != 0:
            check("сборка хостового теста", False, build.stderr.strip()[:400])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check("границы буферов (buildPingReply, jsonEscape, rawBuildFrame)",
              run.returncode == 0, (run.stdout + run.stderr).strip()[:400])
        got = dict(re.findall(r"(crc\d+)=([0-9A-F]+)", run.stdout))
        expect = "%08X" % (zlib.crc32(b"meshcore") & 0xFFFFFFFF)
        check("crc32 совпадает с zlib", got.get("crc32") == expect,
              "получено %s, ожидалось %s" % (got.get("crc32"), expect))


# ===== Чистые функции: числа, версии, диапазоны настроек, slug для MQTT =====
# Раньше проверялись только буферные функции, а ошибки жили как раз здесь: диапазон
# настройки с lo == hi работал наоборот документации, а slug из кириллических имён
# схлопывался в одинаковые подчёркивания. Всё это чистые функции — их можно вырезать из
# прошивки и прогнать на хосте.
PURE_PRELUDE = (
    "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstdlib>\n"
    "#include <cmath>\n#include <string>\n"
    # Минимальный String: fwVersionCmp пользуется только c_str() и indexOf()
    "struct String {\n"
    "  std::string s;\n"
    "  String(const char* p = \"\") : s(p) {}\n"
    "  const char* c_str() const { return s.c_str(); }\n"
    "  int indexOf(char c, int from) const {\n"
    "    size_t p = s.find(c, (size_t)from);\n"
    "    return p == std::string::npos ? -1 : (int)p;\n"
    "  }\n"
    "};\n"
)

PURE_MAIN = r"""
static int fails = 0;
static void expect(bool ok, const char* what) {
    if (!ok) { printf("не сошлось: %s\n", what); fails++; }
}

int main() {
    // --- cfgRangeOk: lo == hi означает «диапазон не задан», а не «ничего нельзя» ---
    expect(cfgRangeOk(12345, 0, 0), "lo==hi пропускает любое значение");
    expect(cfgRangeOk(8, 5, 12), "8 в диапазоне 5..12");
    expect(!cfgRangeOk(4, 5, 12), "4 вне 5..12");
    expect(!cfgRangeOk(13, 5, 12), "13 вне 5..12");
    expect(cfgRangeOk(5, 5, 12) && cfgRangeOk(12, 5, 12), "границы включительно");
    expect(cfgRangeOk(-22, -22, 22), "отрицательная граница");
    // 70000 не должно «пролезть» усечением до uint16 (4464 попало бы в диапазон)
    expect(!cfgRangeOk(70000, 1, 65535), "70000 вне 1..65535");

    // --- fmtFix/parseFixed: печать и разбор чисел без float-printf ---
    const float vals[] = { 0.0f, 1.0f, -1.0f, 62.5f, 868.731018f, -12.25f, 255.0f, 0.05f };
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        for (int dec = 0; dec <= 6; dec++) {
            char buf[24];
            memset(buf, 'X', sizeof(buf));
            fmtFix(vals[i], (uint8_t)dec, buf, sizeof(buf));
            if (strnlen(buf, sizeof(buf)) >= sizeof(buf)) {
                printf("fmtFix: строка не завершена v=%f dec=%d\n", (double)vals[i], dec);
                return 1;
            }
            float back = parseFixed(buf);
            float tol = 1.0f;
            for (int k = 0; k < dec; k++) tol /= 10.0f;
            if (fabsf(back - vals[i]) > tol) {
                printf("fmtFix/parseFixed: %f -> %s -> %f (dec=%d)\n",
                       (double)vals[i], buf, (double)back, dec);
                fails++;
            }
        }
    }
    // Тесный буфер: обрезаем, но завершающий ноль обязан остаться
    for (size_t n = 1; n < 12; n++) {
        char small[12];
        memset(small, 'X', sizeof(small));
        fmtFix(-868.731018f, 6, small, n);
        if (strnlen(small, n) >= n) { printf("fmtFix: нет нуля при n=%u\n", (unsigned)n); return 1; }
    }
    expect(parseFixed(" -3,5") < -3.4f && parseFixed(" -3,5") > -3.6f, "parseFixed: запятая и пробел");

    // --- fwVersionCmp: сравнение почастям, а не строкой ---
    expect(fwVersionCmp("0.3.10", "0.3.9") > 0, "0.3.10 новее 0.3.9");
    expect(fwVersionCmp("0.1.9", "0.1.10") < 0, "0.1.9 старее 0.1.10");
    expect(fwVersionCmp("1.0.0", "0.9.9") > 0, "1.0.0 новее 0.9.9");
    expect(fwVersionCmp("0.3.6", "0.3.6") == 0, "равные версии");
    expect(fwVersionCmp("0.3.6dev", "0.3.6") == 0, "суффикс dev не считается новее");

    // --- mqttSlug: разные имена не должны давать один slug ---
    char a[48], b[48];
    mqttSlug("Tr1glav_home", a, sizeof(a));
    expect(strcmp(a, "Tr1glav_home") == 0, "латинское имя не меняется");
    mqttSlug("дверь", a, sizeof(a));
    mqttSlug("крыша", b, sizeof(b));
    expect(strcmp(a, b) != 0, "разные кириллические имена дают разный slug");
    mqttSlug("дверь", b, sizeof(b));
    expect(strcmp(a, b) == 0, "одно имя даёт один и тот же slug");
    for (int n = 1; n < 40; n++) {
        char tight[40];
        memset(tight, 'X', sizeof(tight));
        mqttSlug("датчик-в-подвале-очень-длинное-имя", tight, n);
        if (strnlen(tight, (size_t)n) >= (size_t)n) {
            printf("mqttSlug: нет нуля при maxLen=%d\n", n);
            return 1;
        }
    }
    if (fails) { printf("проверок не сошлось: %d\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
"""


def pure_functions_test():
    if not shutil.which("g++"):
        print("SKIP g++ не найден — чистые функции не проверены")
        return
    code = (PURE_PRELUDE
            + grab(CORE / "src/appconfig.cpp", "bool cfgRangeOk(") + "\n"
            + grab(CORE / "src/crypto.cpp", "char* fmtFix(") + "\n"
            + grab(CORE / "src/crypto.cpp", "float parseFixed(") + "\n"
            + grab(CORE / "src/crypto.cpp", "uint16_t crc16buf(") + "\n"
            + grab("lib/meshcore/src/mqtt.cpp", "void mqttSlug(") + "\n"
            + grab("lib/meshcore/src/fwupdate.cpp", "int fwVersionCmp(") + "\n"
            + PURE_MAIN)
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "p.cpp"
        exe = pathlib.Path(tmp) / "p"
        src.write_text(code, encoding="utf-8")
        build = subprocess.run(
            ["g++", "-std=c++17", "-fsanitize=address,undefined", "-g", str(src), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode != 0:
            check("сборка теста чистых функций", False, build.stderr.strip()[:400])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check("числа, версии, диапазоны настроек и slug для MQTT",
              run.returncode == 0, (run.stdout + run.stderr).strip()[:600])


MARKER_PRELUDE = (
    "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
    "#define BOARD_CODE \"h3\"\n"
    "#define FW_MARK_PREFIX \"MBFW:\"\n"
    "#define min(a,b) ((a)<(b)?(a):(b))\n"
    "struct FwScan { char carry[40]; uint8_t carryLen; bool mine; char other[12]; };\n"
)

MARKER_MAIN = r"""
static void feedAll(FwScan* s, const char* img, size_t n, size_t chunk) {
    fwScanReset(s);
    for (size_t i = 0; i < n; i += chunk) {
        size_t k = (n - i < chunk) ? (n - i) : chunk;
        fwScanFeed(s, (const uint8_t*)img + i, k);
    }
}

int main() {
    char img[4096];
    FwScan s;
    // маркер своей платы обязан находиться при любом размере куска, в том числе
    // когда он лёг на границу двух кусков — это и есть главный риск сканера
    const char* mine = "MBFW:" BOARD_CODE ":1.0.27";
    for (size_t at = 0; at + 64 < sizeof(img); at += 37) {
        memset(img, 0xA5, sizeof(img));
        memcpy(img + at, mine, strlen(mine));
        for (size_t chunk = 1; chunk <= 64; chunk++) {
            feedAll(&s, img, sizeof(img), chunk);
            if (fwScanVerdict(&s) != 1) {
                printf("свой маркер не найден: смещение %zu, кусок %zu\n", at, chunk);
                return 1;
            }
        }
    }
    // образ чужой платы должен быть отвергнут с указанием её кода
    memset(img, 0xA5, sizeof(img));
    memcpy(img + 700, "MBFW:zz9:1.0.27", 15);
    feedAll(&s, img, sizeof(img), 512);
    if (fwScanVerdict(&s) != -1 || strcmp(s.other, "zz9") != 0) {
        printf("чужая плата не распознана: verdict=%d other=%s\n", fwScanVerdict(&s), s.other);
        return 1;
    }
    // без маркера и голый префикс без кода — «неизвестная» прошивка, а не отказ
    memset(img, 0xA5, sizeof(img));
    feedAll(&s, img, sizeof(img), 512);
    if (fwScanVerdict(&s) != 0) { printf("образ без маркера принят за чужой\n"); return 1; }
    memcpy(img + 100, "MBFW:", 6);
    feedAll(&s, img, sizeof(img), 512);
    if (fwScanVerdict(&s) != 0) { printf("голый префикс принят за маркер\n"); return 1; }
    printf("ok\n");
    return 0;
}
"""


def marker_scan_test():
    if not shutil.which("g++"):
        print("SKIP g++ не найден — сканер маркера платы не проверен")
        return
    code = (MARKER_PRELUDE
            + grab(CORE / "src/ota.cpp", "void fwScanReset(") + "\n"
            + grab(CORE / "src/ota.cpp", "static void fwScanBuf(") + "\n"
            + grab(CORE / "src/ota.cpp", "void fwScanFeed(") + "\n"
            + grab(CORE / "src/ota.cpp", "int fwScanVerdict(") + "\n"
            + MARKER_MAIN)
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "m.cpp"
        exe = pathlib.Path(tmp) / "m"
        src.write_text(code, encoding="utf-8")
        build = subprocess.run(
            ["g++", "-std=c++17", "-fsanitize=address,undefined", "-g", str(src), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode != 0:
            check("сборка теста маркера платы", False, build.stderr.strip()[:400])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check("маркер платы: поиск в потоке, в том числе на границах кусков",
              run.returncode == 0, (run.stdout + run.stderr).strip()[:400])


def otaz_test():
    raw = bytes(random.getrandbits(8) for _ in range(50000))
    packed = (b"OTAZ" + struct.pack("<II", len(raw), zlib.crc32(raw) & 0xFFFFFFFF)
              + zlib.compress(raw, 9))
    size, crc = struct.unpack("<II", packed[4:12])
    # сенсор скармливает распаковщику куски по 240 байт, как они приходят в кадрах
    d = zlib.decompressobj()
    body = packed[12:]
    out = b"".join(d.decompress(body[i:i + 240]) for i in range(0, len(body), 240)) + d.flush()
    check("формат .otaz: заголовок", packed[:4] == b"OTAZ" and size == len(raw))
    check("формат .otaz: CRC32 образа", crc == zlib.crc32(raw) & 0xFFFFFFFF)
    check("формат .otaz: распаковка кусками по 240 Б", out == raw)
    # Бот дописывает в эфир OTA_Z_TAIL_PAD нулей после потока: без них распаковщик узла
    # придерживает хвост образа, и приём падает с «size mismatch». Длину берём из config.h,
    # чтобы проверка и прошивка не разъехались, и убеждаемся, что лишний вход безвреден.
    cfg = (CORE / "include/config.h").read_text(encoding="utf-8")
    m = re.search(r"#define\s+OTA_Z_TAIL_PAD\s+(\d+)", cfg)
    check("формат .otaz: объявлен хвост нулей", m is not None)
    padded = body + bytes(int(m.group(1)) if m else 0)
    dp = zlib.decompressobj()
    outp = b"".join(dp.decompress(padded[i:i + 240]) for i in range(0, len(padded), 240)) + dp.flush()
    check("формат .otaz: хвостовые нули не портят образ", outp == raw)


def page_js_test():
    js = (ROOT / "web/app.js").read_text(encoding="utf-8")
    check("JavaScript страницы непустой", len(js) > 1000)
    if not shutil.which("node"):
        print("SKIP node не найден — синтаксис страницы не проверен")
        return
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "page.js"
        path.write_text(js, encoding="utf-8")
        run = subprocess.run(["node", "--check", str(path)], capture_output=True, text=True)
        check("синтаксис JavaScript страницы", run.returncode == 0, run.stderr.strip()[:400])


COMPANION_PRELUDE = (
    "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstddef>\n"
    "#define MAX_FRAME_SIZE 240\n"
)

COMPANION_MAIN = r"""
int main() {
    // advertPathBytes: байт длины приезжает из NVS, то есть мог достаться от другой версии
    // прошивки. Проверяем все 256 значений против вместимости буфера пути.
    for (int v = 0; v < 256; v++) {
        uint16_t b = advertPathBytes((uint8_t)v, 64);
        if (b > 64) { printf("advertPathBytes: %u байт при advPathLen=%d\n", b, v); return 1; }
        // разумные значения не должны теряться: 0 хопов, 1 хоп с 1-байтовым хэшем
        if (v == 0x00 && b != 0) { printf("advertPathBytes: 0 хопов дало %u\n", b); return 1; }
        if (v == 0x01 && b != 1) { printf("advertPathBytes: 1 хоп 1 байт дало %u\n", b); return 1; }
        if (v == 0x41 && b != 2) { printf("advertPathBytes: 1 хоп 2 байта дало %u\n", b); return 1; }
    }
    // 63 хопа по 2 байта = 126 байт: в буфер 64 не влезает, отдаём пустой путь.
    // Разряд хэша лежит в бите 6, поэтому двухбайтовый хэш — это 0x40 и выше.
    if (advertPathBytes(0x7F, 64) != 0) { printf("advertPathBytes: не влезает, но не отброшено\n"); return 1; }
    if (advertPathBytes(0x61, 64) != 0) { printf("advertPathBytes: 33 хопа по 2 байта не отброшены\n"); return 1; }
    // ровно во вместимость: 32 хопа по 2 байта = 64 — пропускаем
    if (advertPathBytes(0x60, 64) != 64) { printf("advertPathBytes: ровно 64 байта отброшены\n"); return 1; }
    if (advertPathBytes(0x3F, 64) != 63) { printf("advertPathBytes: 63 хопа по 1 байту\n"); return 1; }

    // putTextBounded: текст из очереди не должен вылезти за предел кадра при любой
    // уже набранной длине и любой длине текста.
    uint8_t frame[512];
    for (int used = 0; used <= 300; used++) {
        for (int tl = 0; tl <= 400; tl++) {
            char text[401];
            memset(text, 'T', sizeof(text));
            memset(frame, 0, sizeof(frame));
            size_t got = putTextBounded(frame, (size_t)used, MAX_FRAME_SIZE, text, (size_t)tl);
            if (got > MAX_FRAME_SIZE) {
                printf("putTextBounded: длина %u при used=%d tl=%d\n", (unsigned)got, used, tl);
                return 1;
            }
            // сколько байт обязано было вписаться: столько, сколько влезает после used
            size_t copied = 0;
            if ((size_t)used < MAX_FRAME_SIZE) {
                size_t room = MAX_FRAME_SIZE - (size_t)used;
                copied = ((size_t)tl < room) ? (size_t)tl : room;
            }
            for (size_t k = 0; k < copied; k++) {
                if (frame[used + k] != 'T') { printf("putTextBounded: байт %u не записан\n", (unsigned)k); return 1; }
            }
            for (size_t k = used + copied; k < MAX_FRAME_SIZE; k++) {
                if (frame[k] != 0) { printf("putTextBounded: записано за пределом текста\n"); return 1; }
            }
        }
    }
    // предел превышен уже начатком кадра — вписать нельзя ничего, но и выйти нельзя
    if (putTextBounded(frame, MAX_FRAME_SIZE + 10, MAX_FRAME_SIZE, "x", 1) != MAX_FRAME_SIZE) {
        printf("putTextBounded: used больше предела\n"); return 1;
    }
    return 0;
}
"""


def companion_bounds_test():
    if not shutil.which("g++"):
        print("SKIP g++ не найден — границы команд приложения не проверены")
        return
    code = (
        COMPANION_PRELUDE
        + grab("lib/meshcore/src/companion_proto.cpp", "static uint16_t advertPathBytes(") + "\n"
        + grab("lib/meshcore/src/companion_proto.cpp", "static size_t putTextBounded(") + "\n"
        + COMPANION_MAIN
    )
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "c.cpp"
        exe = pathlib.Path(tmp) / "c"
        src.write_text(code, encoding="utf-8")
        build = subprocess.run(
            ["g++", "-std=c++17", "-fsanitize=address,undefined", "-g", str(src), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode != 0:
            check("сборка теста границ команд приложения", False, build.stderr.strip()[:400])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check("границы команд приложения (путь рекламы, текст в кадре)",
              run.returncode == 0, (run.stdout + run.stderr).strip()[:600])


def core_check():
    """Ядро на месте? Половина проверок вырезает функции из соседнего репозитория, и без
    него они не «пропускаются», а валят запуск. Тихо урезанный selftest — это ровно то,
    из-за чего сборка в CI оставалась красной, ни на что не жалуясь."""
    ok = (CORE / "src" / "crypto.cpp").is_file()
    check("ядро протокола найдено (%s)" % CORE, ok,
          "нет исходников ядра — задайте путь переменной MESHCORE_CORE")
    if not ok:
        print()
        print("ПРОВАЛЕНО: ядро протокола не найдено")
        sys.exit(1)


if __name__ == "__main__":
    core_check()
    host_functions_test()
    pure_functions_test()
    marker_scan_test()
    companion_bounds_test()
    otaz_test()
    page_js_test()
    print()
    if failures:
        print("ПРОВАЛЕНО: " + ", ".join(failures))
        sys.exit(1)
    print("все проверки прошли")
