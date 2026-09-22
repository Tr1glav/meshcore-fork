// Передача сессии прошивки узлу-прошивальщику (support).
//
// Прошивать по радио можно только те узлы, которых слышно НАПРЯМУЮ: сессия идёт сырыми
// кадрами быстрого канала, а ретрансляторы их не переносят. Координатор стоит там, где
// стоит, и до части узлов не дотягивается. Прошивальщик — обычный узел с WiFi, который
// поставили туда, где их слышно; он умеет ровно то же, что координатор (FEATURE_MESH_OTA_SENDER),
// и берёт такую сессию на себя.
//
// Своего протокола у этого нет и не нужно: у прошивальщика та же страница, что у
// координатора, и те же точки входа. Образ уходит к нему тем же multipart-запросом, каким
// его кладут со страницы (/savefw), а команда начать — тем же /ota/start. Новое здесь
// только одно: решение, кому сессию отдать.
//
// Образ идёт по СЕТИ, а не по радио: мегабайт по LoRa не передашь, а WiFi у обоих есть.

#include "config.h"
#include "globals.h"
#include "ota.h"
#include "ota_internal.h"
#include "support.h"

#if FEATURE_MESH_OTA_SENDER

#include <LittleFS.h>
#include <WiFi.h>

// Объявление прошивальщика приходит вместе с его heartbeat. Раз в десять минут — значит
// пропуск одного объявления ещё ничего не значит, а три подряд означают, что его нет.
#define SUPPORT_STALE_MS (3UL * SENSOR_HEARTBEAT_MS)
#define SUPPORT_PORT 3232
#define SUPPORT_HTTP_TIMEOUT_MS 15000

bool supportPresent() {
    return supportIp.length() > 0 && (millis() - supportSeenMs) < SUPPORT_STALE_MS;
}

// Один запрос и ответ строкой. Тело ответа нужно целиком только для /sensors — он
// короткий (имена узлов), поэтому ограничиваем и его, и время ожидания.
static bool supportRequest(const String& req, String* body, size_t bodyMax = 2048) {
    WiFiClient c;
    if (!c.connect(supportIp.c_str(), SUPPORT_PORT)) {
        slog("[SUP] %s недоступен\n", supportIp.c_str());
        return false;
    }
    c.setTimeout(SUPPORT_HTTP_TIMEOUT_MS / 1000);
    c.print(req);
    unsigned long t0 = millis();
    while (!c.available() && millis() - t0 < SUPPORT_HTTP_TIMEOUT_MS) delay(10);
    String status = c.readStringUntil('\n');
    // Заголовки пропускаем: нас интересует только код ответа и тело
    while (c.connected() || c.available()) {
        String line = c.readStringUntil('\n');
        if (line.length() <= 1) break;          // пустая строка — конец заголовков
    }
    if (body) {
        body->reserve(256);
        while ((c.connected() || c.available()) && body->length() < bodyMax) {
            int ch = c.read();
            if (ch < 0) { if (!c.connected()) break; delay(1); continue; }
            *body += (char)ch;
        }
    }
    c.stop();
    return status.indexOf("200") > 0;
}

// Слышит ли прошивальщик этот узел напрямую. Спрашиваем у него самого: его /sensors
// отдаёт тот же список, что и наш, вместе с хопами.
//
// Разбор подстрокой, а не разбором JSON: ответ свой, формат известен, а тащить парсер
// ради двух полей незачем — так же разобраны и остальные ответы в этом проекте.
bool supportHearsDirect(const String& target) {
    String body;
    String req = String("GET /sensors HTTP/1.1\r\nHost: ") + supportIp +
                 "\r\nConnection: close\r\n\r\n";
    if (!supportRequest(req, &body, 4096)) return false;

    String key = String("\"name\":\"") + target + "\"";
    int at = body.indexOf(key);
    if (at < 0) return false;                   // прошивальщик такого узла не знает
    int end = body.indexOf('}', at);
    if (end < 0) end = body.length();
    int hopsAt = body.indexOf("\"hops\":", at);
    if (hopsAt < 0 || hopsAt > end) return false;
    return body.substring(hopsAt + 7, end).toInt() == 0;
}

// Отдать образ и команду. Образ уходит multipart-ом — ровно тем, что ждёт /savefw:
// приём файла на той стороне сделан обработчиком загрузки, он разбирает конверт сам и
// пишет файл потоком, не держа его в памяти.
bool supportHandOff(const String& target) {
    if (!supportPresent()) return false;

    File f = LittleFS.open("/ota.bin", "r");
    if (!f) { slog("[SUP] /ota.bin не открылся\n"); return false; }
    const size_t fsize = f.size();

    const char* BND = "----meshcoreota";
    String head = String("--") + BND + "\r\n"
                  "Content-Disposition: form-data; name=\"fw\"; filename=\"ota.otaz\"\r\n"
                  "Content-Type: application/octet-stream\r\n\r\n";
    String tail = String("\r\n--") + BND + "--\r\n";

    WiFiClient c;
    if (!c.connect(supportIp.c_str(), SUPPORT_PORT)) {
        f.close();
        slog("[SUP] %s недоступен для передачи образа\n", supportIp.c_str());
        return false;
    }
    c.print(String("POST /savefw HTTP/1.1\r\nHost: ") + supportIp +
            "\r\nContent-Type: multipart/form-data; boundary=" + BND +
            "\r\nContent-Length: " + String(head.length() + fsize + tail.length()) +
            "\r\nConnection: close\r\n\r\n");
    c.print(head);

    // Потоком по килобайту: образ в памяти не держим, его там просто нет столько
    uint8_t buf[1024];
    size_t sent = 0;
    while (sent < fsize) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        if (c.write(buf, n) != (size_t)n) { sent = 0; break; }
        sent += n;
    }
    f.close();
    c.print(tail);

    bool ok = false;
    if (sent == fsize) {
        unsigned long t0 = millis();
        while (!c.available() && millis() - t0 < SUPPORT_HTTP_TIMEOUT_MS) delay(10);
        String status = c.readStringUntil('\n');
        ok = status.indexOf("200") > 0;
    }
    c.stop();
    if (!ok) {
        slog("[SUP] образ не передан (%u из %u байт)\n", (unsigned)sent, (unsigned)fsize);
        return false;
    }
    slog("[SUP] образ передан на %s (%u байт)\n", supportIp.c_str(), (unsigned)fsize);

    String req = String("POST /ota/start?target=") + target + " HTTP/1.1\r\nHost: " +
                 supportIp + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (!supportRequest(req, nullptr)) {
        slog("[SUP] %s образ принял, но сессию не начал\n", supportName.c_str());
        return false;
    }
    slog("[SUP] сессию к '%s' ведёт %s\n", target.c_str(), supportName.c_str());
    return true;
}

#endif // FEATURE_MESH_OTA_SENDER
