// Передача сессии прошивки узлу-прошивальщику (support).
//
// Прошивать по радио можно только те узлы, до которых достаёт само радио (не дальше
// OTA_RADIO_MAX_HOPS): сессия идёт сырыми кадрами быстрого канала, а ретрансляторы их не
// переносят. Координатор стоит там, где стоит, и до части узлов не дотягивается.
// Прошивальщик — обычный узел с WiFi, который поставили туда, где их слышно; он умеет
// ровно то же, что координатор (FEATURE_MESH_OTA_SENDER), и берёт такую сессию на себя.
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
#include "net.h"
#include "mqtt.h"     // wifiConnected: сеть нужна и для передачи образа

extern "C" {
#include "esp32s3/rom/miniz.h"
}

// SUPPORT_STALE_MS живёт в config.h ядра: там же реестр supports[] и функция supportLive,
// которая этим сроком и пользуется. Два места для одного срока уже один раз разъехались.
#define SUPPORT_PORT 3232
#define SUPPORT_HTTP_TIMEOUT_MS 15000
// Сколько ждём само соединение. Прошивальщик — узел в той же локальной сети: он либо
// отвечает сразу, либо его нет (перезагружается после прошивки, например). Ждать его
// по умолчанию несколько десятков секунд нельзя: этот вызов стоит в обработчике
// страницы, и всё это время координатор не отвечает вообще ни на что.
#define SUPPORT_CONNECT_MS 3000
// Короткие частые запросы ждут соединения гораздо меньше. Отметка уходит раз в десять секунд,
// и если координатора нет, трёхсекундный коннект съедал бы треть времени — а мы стоим в нём
// в главном цикле и всё это время глухи к эфиру. В локальной сети рукопожатие занимает
// единицы миллисекунд: секунды с запасом хватает, чтобы отличить «занят» от «нет его».
#define SUPPORT_PING_CONNECT_MS 1000
// Сколько раз пробуем скомандовать начать сессию и пауза между попытками. Пауза заметно
// больше секунды: прошивальщик должен успеть выйти из своего исходящего запроса и вернуться
// в handleClient, иначе повтор упрётся в то же самое.
#define SUPPORT_START_TRIES 4
#define SUPPORT_START_RETRY_MS 1500
// Короткие опросы — состояние сессии и список узлов — спрашивают у него на каждом
// обновлении страницы. Им общий пятнадцатисекундный срок не годится по той же причине.
#define SUPPORT_SHORT_MS 2000

// Ход текущей передачи; определены ниже вместе с задачей.
extern volatile uint32_t supJobSent;
extern volatile uint32_t supJobTotal;

// Есть ли в сети хоть один живой прошивальщик.
bool supportPresent() {
    for (int i = 0; i < supportCount; i++)
        if (supportLive(i)) return true;
    return false;
}

// Успешный обмен — тоже доказательство, что узел жив, и куда более прямое, чем его отметка:
// мы только что с ним разговаривали. Без этого запись устаревала посреди сессии прошивки —
// во время неё прошивальщик намеренно не трогает сеть и отметок не шлёт, а сессия идёт
// дольше, чем срок годности записи. Координатор решал, что ведущий пропал, ровно пока тот
// спокойно шил узел.
static void supportSeenByIp(const String& ip) {
    for (int i = 0; i < supportCount; i++)
        if (supports[i].ip == ip) { supports[i].seenMs = millis(); return; }
}

// Адрес по имени: ведущего сессии страница знает по имени (otaDelegate), а стучаться надо
// по адресу. Срок годности записи здесь не спрашиваем намеренно: пока сессия передана, нам
// важно дозвониться до ведущего, а «жив ли он» ответит сам запрос — и ответит точнее.
static String supportIpOf(const String& name) {
    const int i = supportFind(name);
    return (i >= 0) ? supports[i].ip : String("");
}

// Чтение ответа: строка состояния, заголовки и, если просят, тело. У всех трёх циклов
// один общий срок, и это здесь главное.
//
// Без срока чтение могло не кончиться никогда. После /update прошивальщик перезагружается
// через ESP.restart(): сокет закрывается не по-человечески, FIN не приходит, и у нас
// connected() ещё долго отвечает true, а read() — -1. Цикл «не connected — выходим, иначе
// ждём миллисекунду» в такой паре крутится вечно. Задача передачи переставала завершаться
// совсем, и страница до перезагрузки координатора показывала «шью…», хотя узел давно
// прошит и уже вышел на связь с новой версией.
// Заголовок с общим ключом: точки входа другого узла закрыты, и устройство приходит не с
// паролем страницы, а с ключом (см. webAuthOk в web.cpp). Пустой ключ — заголовка нет, и
// закрытая сторона откажет: так и надо, пока устройство не настроено.
static String apiHdr() {
    return cfg.apiKey.length() ? String("X-API-Key: ") + cfg.apiKey + "\r\n" : String("");
}

static bool supReadResponse(WiFiClient& c, uint32_t waitMs, String* body, size_t bodyMax,
                            bool* answered = nullptr) {
    const unsigned long deadline = millis() + waitMs;
    while (!c.available() && (long)(millis() - deadline) < 0) delay(10);
    String status = c.readStringUntil('\n');
    if (answered) *answered = status.length() > 0;
    // Заголовки пропускаем: нас интересует только код ответа и тело
    while ((c.connected() || c.available()) && (long)(millis() - deadline) < 0) {
        String line = c.readStringUntil('\n');
        if (line.length() <= 1) break;          // пустая строка — конец заголовков
    }
    if (body) {
        body->reserve(256);
        while ((c.connected() || c.available()) && body->length() < bodyMax &&
               (long)(millis() - deadline) < 0) {
            int ch = c.read();
            if (ch < 0) { if (!c.connected()) break; delay(1); continue; }
            *body += (char)ch;
        }
    }
    return status.indexOf("200") > 0;
}

// Один запрос и ответ строкой. Тело ответа нужно целиком только для /sensors — он
// короткий (имена узлов), поэтому ограничиваем и его, и время ожидания.
static bool supportRequest(const String& ip, const String& req, String* body,
                           size_t bodyMax = 2048,
                           uint32_t waitMs = SUPPORT_HTTP_TIMEOUT_MS,
                           uint32_t connectMs = SUPPORT_CONNECT_MS) {
    // О недоступности — один раз, а не на каждую попытку: отметка уходит раз в десять секунд,
    // и при выключенном соседе журнал состоял бы из одной этой строки.
    static bool wasDown = false;
    WiFiClient c;
    if (ip.length() < 7) return false;
    if (!c.connect(ip.c_str(), SUPPORT_PORT, connectMs)) {
        if (!wasDown) {
            wasDown = true;
            slog("[SUP] %s недоступен\n", ip.c_str());
        }
        return false;
    }
    if (wasDown) {
        wasDown = false;
        slog("[SUP] %s снова отвечает\n", ip.c_str());
    }
    c.setTimeout((waitMs + 999) / 1000);
    c.print(req);
    bool ok = supReadResponse(c, waitMs, body, bodyMax);
    c.stop();
    if (ok) supportSeenByIp(ip);
    return ok;
}

// Сколько хопов у ЭТОГО прошивальщика до цели. Спрашиваем у него самого: его /sensors
// отдаёт тот же список, что и наш, вместе с хопами. -1 — не знает узла, не слышал ни разу
// или не отвечает.
//
// Разбор подстрокой, а не разбором JSON: ответ свой, формат известен, а тащить парсер
// ради двух полей незачем — так же разобраны и остальные ответы в этом проекте.
static int supportHopsTo(int idx, const String& target) {
    if (!supportLive(idx)) return -1;
    String body;
    String req = String("GET /sensors HTTP/1.1\r\nHost: ") + supports[idx].ip +
                 "\r\n" + apiHdr() + "Connection: close\r\n\r\n";
    if (!supportRequest(supports[idx].ip, req, &body, 4096, SUPPORT_SHORT_MS)) return -1;

    String key = String("\"name\":\"") + target + "\"";
    int at = body.indexOf(key);
    if (at < 0) return -1;                      // прошивальщик такого узла не знает
    int end = body.indexOf('}', at);
    if (end < 0) end = body.length();
    int hopsAt = body.indexOf("\"hops\":", at);
    if (hopsAt < 0 || hopsAt > end) return -1;
    return body.substring(hopsAt + 7, end).toInt();
}

// Кто может вести МЕДЛЕННУЮ сессию к этой цели. Хопы здесь не спрашиваются: медленный режим
// едет обычными сообщениями и доходит туда же, куда доходит сеть. Важно лишь, что ведущий жив
// и что цель — не он сам.
//
// Предпочитаем того, кто цель слышит: спросить дёшево (один короткий запрос), а вести сессию
// лучше тому, до кого ближе. Никто не слышит — берём первого живого: доставку всё равно
// обеспечивают ретрансляторы, а смысл передачи в том, чтобы координатор остался свободен.
int supportIndexForAny(const String& target) {
    int any = -1;
    for (int i = 0; i < supportCount; i++) {
        if (!supportLive(i)) continue;
        if (supports[i].name == target) continue;   // себя он не прошьёт
        if (any < 0) any = i;
        const int hops = supportHopsTo(i, target);
        if (hops >= 0) {
            slog("[SUP] медленную сессию к '%s' поведёт %s (%d хоп(ов))\n",
                 target.c_str(), supports[i].name.c_str(), hops);
            return i;
        }
    }
    if (any >= 0)
        slog("[SUP] медленную сессию к '%s' поведёт %s (цель он не слышит — доставят "
             "ретрансляторы)\n", target.c_str(), supports[any].name.c_str());
    return any;
}

// Кто из прошивальщиков ведёт сессию к этой цели. «Лучше всех» — это не «ближе к
// координатору», а наименьшее число хопов до САМОЙ ЦЕЛИ: прошивальщики стоят в разных
// местах, и смысл нескольких как раз в том, что у каждого своя часть сети.
//
// Порог общий с ядром (otaHopsReachable): дальше него сессия обречена, и такого ведущего
// мы не берём. Цена выбора — по одному короткому запросу на живого прошивальщика.
int supportIndexFor(const String& target) {
    int best = -1, bestHops = 0;
    for (int i = 0; i < supportCount; i++) {
        if (!supportLive(i)) continue;
        if (supports[i].name == target) continue;   // себя он не прошьёт
        const int hops = supportHopsTo(i, target);
        if (hops < 0 || !otaHopsReachable((uint8_t)hops)) continue;
        if (best < 0 || hops < bestHops) { best = i; bestHops = hops; }
        if (bestHops == 0) break;                   // ближе прямой слышимости не бывает
    }
    if (best >= 0)
        slog("[SUP] сессию к '%s' поведёт %s (%d хоп(ов))\n",
             target.c_str(), supports[best].name.c_str(), bestHops);
    return best;
}

// Прошить САМ прошивальщик — по сети, а не по радио.
//
// Радио ему не нужно: у него есть WiFi, и сессия заняла бы эфир почти на минуту ради
// того, что по сети делается за несколько секунд. Но на /update он ждёт сырой образ, а у
// координатора лежит .otaz — заголовок плюс zlib-поток. Поэтому образ распаковывается на
// лету тем же tinfl из ПЗУ, которым узлы разворачивают прошивку, приходящую по радио:
// целиком в память он не влезет, да и незачем.
//
// Длина тела известна заранее — распакованный размер записан в заголовке .otaz, — поэтому
// Content-Length считается без обмана.
bool supportFlashSelf(int idx) {
    if (!supportLive(idx)) return false;
    const String ip = supports[idx].ip;

    File f = LittleFS.open("/ota.bin", "r");
    if (!f) { slog("[SUP] /ota.bin не открылся\n"); return false; }
    uint8_t hdr[OTA_Z_HDR];
    uint32_t imgSize = 0;
    if ((uint32_t)f.size() <= OTA_Z_HDR || f.read(hdr, OTA_Z_HDR) != (size_t)OTA_Z_HDR ||
        memcmp(hdr, OTA_Z_MAGIC, 4) != 0) {
        f.close();
        slog("[SUP] сохранённый образ не .otaz\n");
        return false;
    }
    memcpy(&imgSize, hdr + 4, 4);
    if (imgSize == 0) { f.close(); return false; }
    supJobTotal = imgSize;
    supJobSent = 0;

    tinfl_decompressor* inf = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    uint8_t* dict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
    uint8_t* chunk = (uint8_t*)malloc(1024);
    bool ok = false;
    uint32_t sent = 0;
    size_t dictOfs = 0;
    WiFiClient c;

    if (!inf || !dict || !chunk) { slog("[SUP] не хватило памяти на распаковку\n"); goto done; }
    tinfl_init(inf);

    if (!c.connect(ip.c_str(), SUPPORT_PORT, SUPPORT_CONNECT_MS)) {
        slog("[SUP] %s недоступен для прошивки\n", ip.c_str());
        goto done;
    }
    c.setTimeout(SUPPORT_HTTP_TIMEOUT_MS / 1000);
    {
        const char* BND = "----meshcoreself";
        String head = String("--") + BND + "\r\n"
                      "Content-Disposition: form-data; name=\"fw\"; filename=\"fw.bin\"\r\n"
                      "Content-Type: application/octet-stream\r\n\r\n";
        String tail = String("\r\n--") + BND + "--\r\n";
        c.print(String("POST /update HTTP/1.1\r\nHost: ") + ip +
                "\r\n" + apiHdr() +
                "Content-Type: multipart/form-data; boundary=" + BND +
                "\r\nContent-Length: " + String(head.length() + imgSize + tail.length()) +
                "\r\nConnection: close\r\n\r\n");
        c.print(head);

        bool last = false;
        while (!last && sent < imgSize) {
            int got = f.read(chunk, 1024);
            if (got <= 0) break;
            last = (f.position() >= f.size());
            const uint8_t* p = chunk;
            size_t avail = (size_t)got;
            for (;;) {
                size_t inBytes = avail;
                size_t outBytes = TINFL_LZ_DICT_SIZE - dictOfs;
                tinfl_status st = tinfl_decompress(inf, p, &inBytes, dict, dict + dictOfs,
                                                   &outBytes,
                                                   TINFL_FLAG_PARSE_ZLIB_HEADER |
                                                   (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT));
                p += inBytes;
                avail -= inBytes;
                if (outBytes) {
                    // Распаковщик может выдать больше объявленного: последний блок он
                    // дополняет. Лишнее в тело не пишем, иначе разъедется Content-Length.
                    size_t take = outBytes;
                    if (sent + take > imgSize) take = imgSize - sent;
                    if (take && c.write(dict + dictOfs, take) != take) { sent = 0; break; }
                    sent += take;
                    supJobSent = sent;
                    dictOfs = (dictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
                }
                if (st < 0) { slog("[SUP] образ не распаковался (%d)\n", (int)st); sent = 0; break; }
                if (st == TINFL_STATUS_DONE) { last = true; break; }
                if (avail == 0 && st != TINFL_STATUS_HAS_MORE_OUTPUT) break;
            }
            if (sent == 0) break;
        }
        c.print(tail);

        if (sent == imgSize) {
            // Ответ прошивальщик отправляет ДО перезагрузки, поэтому код состояния мы
            // обычно успеваем прочитать; тело может оборваться на середине — это не
            // отказ, а ребут, и решает код ответа.
            String body;
            bool answered = false;
            bool got200 = supReadResponse(c, SUPPORT_HTTP_TIMEOUT_MS, &body, 128, &answered);
            if (!answered) {
                // Ответа не было вовсе. Образ ушёл целиком, а применяет и проверяет его
                // сам Updater — он перезагружает узел сразу, и ответ может не успеть
                // дойти. Считать это отказом нельзя: прошивка на самом деле прошла, и
                // новую версию покажет его же объявление в эфире.
                slog("[SUP] образ передан целиком, ответа нет — узел перезагружается\n");
                ok = true;
            } else {
                ok = got200 && body.indexOf("FAIL") < 0;
                if (!ok) slog("[SUP] прошивальщик отказал: %s\n", body.c_str());
            }
        }
    }
    c.stop();

done:
    f.close();
    free(inf); free(dict); free(chunk);
    if (ok) slog("[SUP] %s прошит по сети (%u байт)\n", supports[idx].name.c_str(), (unsigned)sent);
    else if (sent != imgSize) slog("[SUP] передано %u из %u байт\n",
                                   (unsigned)sent, (unsigned)imgSize);
    return ok;
}

// Ход переданной сессии. Ответ отдаём как есть: формат у прошивальщика тот же, и
// страница координатора разбирает его теми же полями, что и свой.
// ===== ХОД ПЕРЕДАННОЙ СЕССИИ =====
// Опрос вынесен из обработчика страницы в главный цикл, и вот почему. Страница дёргает
// /ota/status раз в секунду, а обработчик ходил за ответом к прошивальщику по сети — и всё
// это время веб-сервер координатора занят одним запросом и не отвечает никому, в том числе
// на POST /ears от того же прошивальщика. Тот, в свою очередь, стоял в своём исходящем
// запросе и не отвечал нам. Два однопоточных сервера, зовущие друг друга синхронно, глушили
// друг друга по очереди — отсюда и «периодически не проходит».
//
// Теперь ответ забирается раз в секунду из главного цикла и кладётся в кэш, а обработчик
// только читает кэш. И промах больше не означает «ведущий пропал»: он занят сессией, чанки
// уходят каждые сорок миллисекунд, и не ответить один раз для него нормально.
#define SUPPORT_STATUS_POLL_MS  1000
#define SUPPORT_STATUS_FAILS_MAX 5

static String supStatusJson;
static unsigned long supStatusMs = 0;
static uint8_t supStatusFails = 0;

void supportStatusTick() {
    if (otaDelegate.length() == 0) {
        supStatusJson = "";
        supStatusFails = 0;
        supStatusMs = 0;
        return;
    }
    if (supStatusMs != 0 && millis() - supStatusMs < SUPPORT_STATUS_POLL_MS) return;
    supStatusMs = millis();
    String out;
    if (supportStatus(out)) {
        supStatusJson = out;
        supStatusFails = 0;
    } else if (supStatusFails < 0xFF) {
        supStatusFails++;
        if (supStatusFails == SUPPORT_STATUS_FAILS_MAX)
            slog("[SUP] %s не отвечает %d раз подряд — сессию больше не показываем\n",
                 otaDelegate.c_str(), (int)SUPPORT_STATUS_FAILS_MAX);
    }
}

int supportStatusCached(String& out) {
    if (supStatusFails >= SUPPORT_STATUS_FAILS_MAX) return -1;   // молчит слишком долго
    if (supStatusJson.length() == 0) return 0;                   // ещё не спрашивали
    out = supStatusJson;
    return 1;
}

// Отменить сессию у того, кто её ведёт. Кнопка «Прервать» на странице координатора должна
// останавливать прошивку и тогда, когда ведёт не он.
bool supportAbortDelegated() {
    const String ip = supportIpOf(otaDelegate);
    if (ip.length() == 0) return false;
    String req = String("POST /ota/abort HTTP/1.1\r\nHost: ") + ip + "\r\n" + apiHdr() +
                 "Content-Length: 0\r\nConnection: close\r\n\r\n";
    return supportRequest(ip, req, nullptr, 0, SUPPORT_SHORT_MS);
}

// Ход сессии спрашиваем у ТОГО, кто её ведёт: otaDelegate держит его имя.
bool supportStatus(String& out) {
    const String ip = supportIpOf(otaDelegate);
    if (ip.length() == 0) return false;
    String req = String("GET /ota/status HTTP/1.1\r\nHost: ") + ip + "\r\n" + apiHdr() +
                 "\r\nConnection: close\r\n\r\n";
    out = "";
    return supportRequest(ip, req, &out, 512, SUPPORT_SHORT_MS) && out.indexOf('{') >= 0;
}

// Отдать образ и команду. Образ уходит multipart-ом — ровно тем, что ждёт /savefw:
// приём файла на той стороне сделан обработчиком загрузки, он разбирает конверт сам и
// пишет файл потоком, не держа его в памяти.
bool supportHandOff(int idx, const String& target, bool slow) {
    if (!supportLive(idx)) return false;
    const String ip = supports[idx].ip;

    File f = LittleFS.open("/ota.bin", "r");
    if (!f) { slog("[SUP] /ota.bin не открылся\n"); return false; }
    const size_t fsize = f.size();
    supJobTotal = fsize;
    supJobSent = 0;

    const char* BND = "----meshcoreota";
    String head = String("--") + BND + "\r\n"
                  "Content-Disposition: form-data; name=\"fw\"; filename=\"ota.otaz\"\r\n"
                  "Content-Type: application/octet-stream\r\n\r\n";
    String tail = String("\r\n--") + BND + "--\r\n";

    WiFiClient c;
    if (!c.connect(ip.c_str(), SUPPORT_PORT, SUPPORT_CONNECT_MS)) {
        f.close();
        slog("[SUP] %s недоступен для передачи образа\n", ip.c_str());
        return false;
    }
    c.setTimeout(SUPPORT_HTTP_TIMEOUT_MS / 1000);
    c.print(String("POST /savefw HTTP/1.1\r\nHost: ") + ip +
            "\r\n" + apiHdr() +
            "Content-Type: multipart/form-data; boundary=" + BND +
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
        supJobSent = sent;
    }
    f.close();
    c.print(tail);

    bool ok = (sent == fsize) && supReadResponse(c, SUPPORT_HTTP_TIMEOUT_MS, nullptr, 0);
    c.stop();
    if (!ok) {
        slog("[SUP] образ не передан (%u из %u байт)\n", (unsigned)sent, (unsigned)fsize);
        return false;
    }
    supportSeenByIp(ip);   // образ он принял — значит жив, и отметки для этого не нужно
    slog("[SUP] образ передан на %s (%u байт)\n", ip.c_str(), (unsigned)fsize);

    // slow=1 — команда начать медленную сессию. Образ ушёл тем же путём, отличается только
    // способ раздачи, поэтому и точка входа та же.
    String req = String("POST /ota/start?target=") + target + (slow ? "&slow=1" : "") +
                 " HTTP/1.1\r\nHost: " + ip + "\r\n" + apiHdr() +
                 "Content-Length: 0\r\nConnection: close\r\n\r\n";
    // Команда повторяется, и это не перестраховка. Оба узла зовут друг друга синхронно из
    // главного цикла, а HTTP-сервер у каждого однопоточный и обслуживается там же: пока
    // прошивальщик стоит в своём исходящем POST /ears, он не отвечает на наш /ota/start.
    // Мегабайт образа к этому моменту уже уехал, и терять его из-за одной неудачной секунды
    // нельзя — так прошивка «периодически не проходила», и приходилось начинать заново.
    bool started = false;
    for (int attempt = 1; attempt <= SUPPORT_START_TRIES && !started; attempt++) {
        started = supportRequest(ip, req, nullptr);
        if (!started) {
            slog("[SUP] %s не принял команду (попытка %d из %d)\n",
                 supports[idx].name.c_str(), attempt, SUPPORT_START_TRIES);
            if (attempt < SUPPORT_START_TRIES) delay(SUPPORT_START_RETRY_MS);
        }
    }
    if (!started) {
        slog("[SUP] %s образ принял, но сессию не начал\n", supports[idx].name.c_str());
        return false;
    }
    slog("[SUP] %sсессию к '%s' ведёт %s\n", slow ? "медленную " : "",
         target.c_str(), supports[idx].name.c_str());
    return true;
}

// Строковое поле из JSON-ответа: "<key>":"<значение>". Разбор подстрокой, как и везде
// здесь — ответ свой, формат известен.
static String jsonField(const String& body, const char* key) {
    const String pat = String("\"") + key + "\":\"";
    const int at = body.indexOf(pat);
    if (at < 0) return "";
    const int from = at + pat.length();
    const int end = body.indexOf('"', from);
    return end < 0 ? String("") : body.substring(from, end);
}

// ===== Настройки прошивальщика по сети =====
//
// Прошивальщик настраивается со страницы координатора, как любой узел, — но не по радио, а
// по своему API: он в той же сети, и мегабайтные паузы очереди сообщений ему не нужны. Со
// стороны страницы это тот же /sensors/config с тем же target; координатор сам видит, что
// цель — прошивальщик, и идёт к нему по HTTP (см. otaHandleSensorsConfig).
//
// Ответ отдаём как есть: у прошивальщика те же обработчики /config, что у координатора, и
// страница разбирает их теми же полями.
bool supportConfigRequest(int idx, bool post, const String& query, String& answer) {
    if (!supportLive(idx)) return false;
    const String ip = supports[idx].ip;
    String req = String(post ? "POST" : "GET") + " /config";
    if (post && query.length()) req += "?" + query;
    req += " HTTP/1.1\r\nHost: " + ip + "\r\n" + apiHdr();
    if (post) req += "Content-Length: 0\r\n";
    req += "Connection: close\r\n\r\n";
    answer = "";
    return supportRequest(ip, req, &answer, 2048, SUPPORT_HTTP_TIMEOUT_MS);
}

// ===== ПРОШИВАЛЬЩИК ОТМЕЧАЕТСЯ У КООРДИНАТОРА (по сети) =====
//
// Отметка идёт от прошивальщика к координатору, а не наоборот: кто существует, тот и
// сообщает о себе. Адрес координатора прошивальщик знает из своих настроек (coord_ip), имя
// называет заголовком, а свой адрес координатор берёт из самого соединения — подменить его
// нельзя даже случайно, и параметра для него не нужно.
//
// Вместе с отметкой уходят версия и окружение: у координатора они появляются сразу, без
// отдельного запроса /info и без ожидания радио-heartbeat.
//
// Раньше это работало наоборот и через эфир: прошивальщик объявлял себя сообщением
// "support:<ip>" в сенсорном канале, координатор отвечал своим адресом по сети. Эфир платил
// за служебный обмен, запись о прошивальщике жила три периода heartbeat (полчаса) и почти
// никогда не отражала действительность. По сети отметка стоит одного пакета — поэтому она
// частая (SUPPORT_PING_MS), и уход прошивальщика виден за полминуты.
#if FEATURE_SUPPORT
#define SUPPORT_PING_RETRY_MAX_MS 120000UL
static unsigned long supPingNextMs = 0;
static uint8_t supPingFails = 0;

void supportPingTick() {
    if (!mcWifiConnected()) return;
    // Во время сессии прошивки сеть не трогаем вовсе. Чанки уходят каждые сорок миллисекунд,
    // а блокирующий POST останавливает главный цикл на сотни: отметка подождёт, сессия — нет.
    // Она же не даст нам ответить на входящий запрос, если координатор захочет что-то
    // спросить в этот момент.
    if (otaAnySessionActive()) return;   // любая сессия, в любой роли
    if (cfg.coordHost.length() < 7) return;      // адрес координатора не настроен
    if (cfg.apiKey.length() == 0) return;        // без ключа координатор отметку не примет
    if (supPingNextMs != 0 && (long)(millis() - supPingNextMs) < 0) return;

    String body = String("ver=") + FW_VERSION + "&env=" + FW_ENV;
    String req = String("POST /support HTTP/1.1\r\nHost: ") + cfg.coordHost + "\r\n" +
                 apiHdr() + "X-Support: " + cfg.name + "\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: " + String((unsigned)body.length()) + "\r\n"
                 "Connection: close\r\n\r\n" + body;
    if (supportRequest(cfg.coordHost, req, nullptr, 0, SUPPORT_SHORT_MS,
                       SUPPORT_PING_CONNECT_MS)) {
        // Отметка дошла — значит координатор на этом адресе есть и слушает. Это же и адрес
        // для «вторых ушей»: кадры уходят туда же, откуда пришло подтверждение.
        if (coordIp != cfg.coordHost) {
            coordIp = cfg.coordHost;
            slog("[SUP] координатор на %s принимает отметки\n", coordIp.c_str());
        }
        coordSeenMs = millis();
        supPingFails = 0;
        supPingNextMs = millis() + SUPPORT_PING_MS;
    } else {
        if (supPingFails < 8) supPingFails++;
        unsigned long wait = SUPPORT_PING_MS << (supPingFails - 1);
        if (wait > SUPPORT_PING_RETRY_MAX_MS) wait = SUPPORT_PING_RETRY_MAX_MS;
        supPingNextMs = millis() + wait;
    }
    if (supPingNextMs == 0) supPingNextMs = 1;
}
#endif // FEATURE_SUPPORT

// ===== Фоновая передача =====
//
// Образ — это мегабайт по сети, десятки секунд. Раньше это делалось прямо в обработчике
// /ota/start, и пока он не вернётся, веб-сервер координатора не принимал НИ ОДНОГО
// соединения: страница у пользователя просто умирала на всё время прошивки. Сетевая
// работа вынесена в отдельную задачу — ровно так же, как загрузка образа из релизов
// (fwStartNetTask в fwupdate.cpp), и по той же причине.
//
// Итог задача не применяет сама: otaDelegate и otaNote читает веб-обработчик, и менять
// их должен один поток. Задача лишь поднимает флаг, а разбирает его главный цикл.
//
// Сроком сверху накрыта и сама задача. Внутри у неё сроки на каждом ожидании, но цена
// ошибки тут слишком велика: одна не кончившаяся задача навсегда оставляла бы страницу в
// состоянии «шью», а новую передачу — запрещённой, и лечилось бы это только
// перезагрузкой координатора. Поэтому зависшую задачу мы бросаем: результат её больше не
// принимается (сверяем поколение), но пока она жива, новую не заводим — иначе две задачи
// читали бы один /ota.bin и слали бы образ одновременно.
#define SUPPORT_JOB_MAX_MS 180000UL

static volatile uint8_t supJobKind = SUP_JOB_NONE;
static volatile bool supJobDone = false;
static volatile bool supJobOk = false;
static volatile bool supJobAlive = false;
static volatile uint32_t supJobGen = 0;
static unsigned long supJobStartMs = 0;
static String supJobTarget;
// Кто ведёт: индекс в реестре и имя на момент старта. Имя копией, а не по индексу: пока
// задача работает, реестр может вытеснить запись, и итог назвал бы чужой узел.
static int supJobIdx = -1;
static String supJobWho;
// Сколько байт уже ушло — чтобы на странице двигалась полоса, а не висела надпись.
// Мегабайт по сети идёт полминуты, и без этого прошивка выглядит зависшей.
volatile uint32_t supJobSent = 0;
volatile uint32_t supJobTotal = 0;

static void supJobTask(void* arg) {
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    bool ok = (supJobKind == SUP_JOB_SELF)
                  ? supportFlashSelf(supJobIdx)
                  : supportHandOff(supJobIdx, supJobTarget, supJobKind == SUP_JOB_SLOW);
    slog("[SUP] запас стека задачи: %u Б\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (gen == supJobGen) {          // нас не успели бросить по сроку
        supJobOk = ok;
        supJobDone = true;
    }
    supJobAlive = false;
    vTaskDelete(NULL);
}

bool supportBusy() {
    return supJobKind != SUP_JOB_NONE;
}

uint8_t supportJobKind() { return supJobKind; }
uint32_t supportJobSent() { return supJobSent; }
uint32_t supportJobTotal() { return supJobTotal; }

bool supportJobStart(uint8_t kind, int supIdx, const String& target) {
    if (supJobKind != SUP_JOB_NONE) return false;
    if (!supportLive(supIdx)) return false;
    if (supJobAlive) {               // брошенная задача ещё не вышла — второй такой не надо
        slog("[SUP] прошлая передача ещё не завершилась\n");
        return false;
    }
    supJobTarget = target;
    supJobIdx = supIdx;
    supJobWho = supports[supIdx].name;
    supJobDone = false;
    supJobOk = false;
    supJobSent = 0;
    supJobTotal = 0;
    supJobStartMs = millis();
    supJobAlive = true;
    supJobKind = kind;
    // 8 КБ хватает: шифрования здесь нет (свой узел в локальной сети, обычный HTTP), а
    // словарь распаковки и буфер чтения берутся из кучи. Запас печатается на выходе из
    // задачи — по нему видно, не подошли ли мы к краю.
    if (xTaskCreate(supJobTask, "supjob", 8192, (void*)(uintptr_t)supJobGen, 1, nullptr) == pdPASS)
        return true;
    supJobKind = SUP_JOB_NONE;
    supJobAlive = false;
    slog("[SUP] не удалось создать задачу передачи\n");
    return false;
}

bool supportJobFinished(uint8_t& kind, bool& ok, String& target, String& who) {
    if (supJobKind == SUP_JOB_NONE) return false;
    if (!supJobDone) {
        if (millis() - supJobStartMs < SUPPORT_JOB_MAX_MS) return false;
        slog("[SUP] передача не завершилась за %lu с — бросаем\n", SUPPORT_JOB_MAX_MS / 1000);
        supJobGen++;                 // результат брошенной задачи больше не принимаем
        supJobOk = false;
    }
    kind = supJobKind;
    ok = supJobOk;
    target = supJobTarget;
    who = supJobWho;
    supJobDone = false;
    supJobKind = SUP_JOB_NONE;
    return true;
}

#endif // FEATURE_MESH_OTA_SENDER
