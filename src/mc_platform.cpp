// Реализация хуков платформы (mesh-network-core/include/mc_platform.h).
//
// Файл лежит в src/, а не в lib/meshcore/, и это не вкусовщина. Ядро объявляет хуки
// слабыми (__attribute__((weak))) заглушками в своём mc_platform.cpp, чтобы прошивка
// могла переопределить только нужные. Но lib/ собирается в АРХИВ (libmeshcore.a), а
// линкер достаёт из архива только те объекты, на символы которых остались неразрешённые
// ссылки. Слабая заглушка ссылку разрешает — значит, к моменту разбора libmeshcore.a
// неразрешённых ссылок на mcWifiConnected и остальные уже нет, и объект с сильными
// реализациями в сборку не попадает вовсе. Прошивка молча остаётся с заглушками:
// mcWifiConnected() всегда false (узел-прошивальщик не объявляет себя в эфире, и
// координатор шьёт его по радио вместо сети), экран не рисует приём и ход OTA, батарея
// не показывается. Ошибки линковки при этом нет — поймать такое можно только по карте.
//
// Объекты из src/ линкер получает напрямую, а не архивом, поэтому сильные реализации
// отсюда всегда перекрывают слабые заглушки ядра. По той же причине в src/ живёт и
// main.cpp.

#include "config.h"
#include "globals.h"
#include "crypto.h"      // fmtFix: печать RSSI/SNR без float-printf
#include "ota.h"         // slog: журнал показывается на странице координатора
#include "display.h"
#include <WiFi.h>

// Экран рисуем на локальном дисплее платы; нет экрана (HAS_OLED=0) — хуки молчат.

void mcUiIncoming(const String& channelName, const String& sender, const String& msg,
                  float rssi, float snr, int hopCount, const String& path) {
    #if HAS_OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(channelName.c_str());
    display.printf("From: %s\n", sender.c_str());
    char r2[12], s2[12];
    display.printf("RSSI:%s SNR:%s\n", fmtFix(rssi, 0, r2, sizeof(r2)),
                   fmtFix(snr, 0, s2, sizeof(s2)));
    if (hopCount > 0) {
        display.drawLine(0, 24, 128, 24, SSD1306_WHITE);
        display.setCursor(0, 26);
        display.print("via: ");
        String pathStr = path;
        if (pathStr.length() > 24) pathStr = pathStr.substring(0, 20) + "..";
        display.println(pathStr);
        display.drawLine(0, 36, 128, 36, SSD1306_WHITE);
        display.setCursor(0, 40);
    } else {
        display.drawLine(0, 32, 128, 32, SSD1306_WHITE);
        display.setCursor(0, 36);
    }
    String showMsg = msg;
    if (showMsg.length() > 26) showMsg = showMsg.substring(0, 24) + "..";
    display.println(showMsg);
    display.display();
    #else
    (void)channelName; (void)sender; (void)msg; (void)rssi; (void)snr;
    (void)hopCount; (void)path;
    #endif
}

void mcUiSensorRx(bool snsPub, float rssi) {
    #if HAS_OLED
    display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
    display.setCursor(0, 50);
    char r3[12];
    if (snsPub) display.printf("SNS -> MQTT RSSI:%s", fmtFix(rssi, 0, r3, sizeof(r3)));
    else        display.printf("SNS RX, MQTT %s", mcWifiConnected() ? "off" : "no-wifi");
    display.display();
    #else
    (void)snsPub; (void)rssi;
    #endif
}

void mcUiHexScreen(int pktLen, float rssi, float snr, const uint8_t* buffer) {
    #if HAS_OLED
    display.setTextSize(1);
    display.clearDisplay();
    display.setCursor(0, 0);
    char dr[12], ds[12];
    display.printf("RX %dB RSSI:%s\n", pktLen, fmtFix(rssi, 0, dr, sizeof(dr)));
    display.printf("SNR:%s pkts:%d\n", fmtFix(snr, 0, ds, sizeof(ds)), packetCount);
    display.printf("hex:");
    for (int i = 0; i < min(pktLen, 21); i++) display.printf("%02X", buffer[i]);
    display.display();
    #else
    (void)pktLen; (void)rssi; (void)snr; (void)buffer;
    #endif
}

void mcUiSetBrightness(uint8_t bri) {
    #if HAS_OLED
    display.setBrightness(bri);
    #else
    (void)bri;
    #endif
}

// ===== Mesh OTA на экране (раздающая сторона) =====
void mcUiOtaAbort(const char* why) {
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA abort");
    display.println(why);
    display.display();
    #else
    (void)why;
    #endif
}

void mcUiOtaProgress(const String& target, uint32_t pct, uint32_t pkts, float rssi, float snr) {
    #if HAS_OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.printf("OTA %s\n", target.c_str());
    if (pct > 100) pct = 100;
    display.printf("%u%%\n", (unsigned)pct);
    int bw = (int)((long)pct * 128 / 100);
    if (bw > 128) bw = 128;
    display.drawRect(0, 28, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 28, bw, 10, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.printf("Pkts: %d", (unsigned)pkts);
    // Качество связи по последнему принятому пакету (ack/nack сенсора)
    display.setCursor(0, 54);
    char pr[12], ps[12];
    display.printf("RSSI:%s SNR:%s", fmtFix(rssi, 0, pr, sizeof(pr)),
                   fmtFix(snr, 0, ps, sizeof(ps)));
    display.display();
    #else
    (void)target; (void)pct; (void)pkts; (void)rssi; (void)snr;
    #endif
}

void mcUiOtaDone(const String& target) {
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA OK");
    display.println(target);
    display.println("reboot sensor");
    display.display();
    #else
    (void)target;
    #endif
}

// ===== Mesh OTA на экране (принимающая сторона) =====
void mcUiOtaSensorProgress(uint32_t got, uint32_t total, uint32_t pkts, float rssi, float snr) {
    #if HAS_OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("OTA update");
    uint32_t pct = total ? (got * 100 / total) : 0;
    if (pct > 100) pct = 100;
    display.printf("%u%%\n", (unsigned)pct);
    int bw = total ? (int)((long)got * 128 / total) : 0;
    if (bw > 128) bw = 128;
    display.drawRect(0, 28, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 28, bw, 10, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.printf("Pkts: %d", (unsigned)pkts);
    // Качество связи: RSSI/SNR последнего принятого чанка
    display.setCursor(0, 54);
    char pr[12], ps[12];
    display.printf("RSSI:%s SNR:%s", fmtFix(rssi, 0, pr, sizeof(pr)),
                   fmtFix(snr, 0, ps, sizeof(ps)));
    display.display();
    #else
    (void)got; (void)total; (void)pkts; (void)rssi; (void)snr;
    #endif
}

void mcUiOtaSensorAbort(const char* why, uint32_t rxFrames, uint32_t rxErr) {
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA ABORT");
    display.println(why);
    display.printf("rx:%u err:%u\n", (unsigned)rxFrames, (unsigned)rxErr);
    display.display();
    #else
    (void)why; (void)rxFrames; (void)rxErr;
    #endif
}

// ===== Сеть (узел-прошивальщик) =====
bool mcWifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String mcLocalIp() {
    return WiFi.localIP().toString();
}

// ===== Батарея (узел) — срез на локальные функции display.cpp =====
#if HAS_BATTERY
bool mcBatteryPresent()   { return batteryPresent(); }
int  mcBatteryPercent()   { return batteryPercent(); }
float mcBatteryVoltage()  { return batteryVoltage(); }
#endif

// ===== «Вторые уши»: кадры, услышанные по радио, уходят координатору по сети =====
// Прошивальщик стоит ближе, чем координатор, к части узлов и слышит их, а координатор —
// нет. Услышанные кадры он копит в кольцо и батчем отдаёт координатору POST /ears; там
// они проходят тот же дедуп и разбор, что и с радио, — координатор видит узлы и их hello,
// до которых его радиокарта не достаёт.
//
// Кольцо маленькое намеренно: это дренаж до ближайшего POST, а не архив. Когда сеть лежит
// и очередь переполняется, новые кадры вытесняют старые — старые и так никому не нужны.
#if FEATURE_SUPPORT
#define EARS_QUEUE_MAX 24
#define EARS_FRAME_MAX 255
#define EARS_BATCH_CHARS 6000   // тело запроса: ~11 кадров, остальное доберёт следующий POST
#define EARS_CONNECT_MS 3000
// Координатор объявляется "coord:<ip>" каждые COORD_ANNOUNCE_MS; молчит дольше трёх
// периодов (COORD_STALE_MS) — считаем его ушедшим и не стучимся в пустоту.
//
// Попытки — не чаще EARS_RETRY_MS, и после неудачи пауза удваивается до EARS_RETRY_MAX_MS.
// Это не вежливость к координатору, а защита приёма: connect() к недоступному адресу стоит
// до EARS_CONNECT_MS, и стоим мы в главном цикле. Пока он стоит, radioRxTick() не зовётся —
// узел глух к эфиру. Без паузы каждый проход цикла упирался в этот connect, и узел терял
// приём на все две минуты, пока адрес координатора не устареет.
#define EARS_RETRY_MS 2000UL
#define EARS_RETRY_MAX_MS 60000UL

static uint8_t earsPool[EARS_QUEUE_MAX][EARS_FRAME_MAX];
static uint8_t  earsLen[EARS_QUEUE_MAX];
static float    earsRssi[EARS_QUEUE_MAX];
static float    earsSnr[EARS_QUEUE_MAX];
static uint8_t  earsHead = 0;    // самый старый кадр
static uint8_t  earsCount = 0;
static unsigned long earsNextTryMs = 0;   // раньше этого времени не пробуем
static uint8_t earsFails = 0;             // неудач подряд: по ним растёт пауза

void mcOnFreshFrame(const uint8_t* buf, size_t len, float rssi, float snr) {
    if (len == 0 || len > EARS_FRAME_MAX) return;
    uint8_t tail = (earsHead + earsCount) % EARS_QUEUE_MAX;
    memcpy(earsPool[tail], buf, len);
    earsLen[tail] = (uint8_t)len;
    earsRssi[tail] = rssi;
    earsSnr[tail] = snr;
    if (earsCount < EARS_QUEUE_MAX) earsCount++;
    else earsHead = (earsHead + 1) % EARS_QUEUE_MAX;   // кольцо полно — самое старое долой
}

static void earsPop(unsigned n) {
    earsHead = (earsHead + n) % EARS_QUEUE_MAX;
    earsCount -= n;
}

// Главный цикл зовёт раз в проход; работает только у прошивальщика (FEATURE_SUPPORT),
// там же, где и хук, — оба в одном контексте, гонок нет.
static void earsFailed() {
    if (earsFails < 8) earsFails++;
    unsigned long wait = EARS_RETRY_MS << (earsFails - 1);
    if (wait > EARS_RETRY_MAX_MS) wait = EARS_RETRY_MAX_MS;
    earsNextTryMs = millis() + wait;
    if (earsNextTryMs == 0) earsNextTryMs = 1;   // 0 занято признаком «пробуем сразу»
}

void earsTick() {
    if (earsCount == 0) return;
    if (!mcWifiConnected()) return;
    // Без ключа координатор нас и не примет: лучше не занимать эфир и сеть впустую.
    if (cfg.apiKey.length() == 0) return;
    if (coordIp.length() < 7) return;
    if ((unsigned long)(millis() - coordSeenMs) > COORD_STALE_MS) return;
    if (earsNextTryMs != 0 && (long)(millis() - earsNextTryMs) < 0) return;

    String body;
    body.reserve(EARS_BATCH_CHARS);
    unsigned drained = 0;
    static const char HEXCH[] = "0123456789ABCDEF";
    for (unsigned k = 0; k < earsCount && body.length() < EARS_BATCH_CHARS; k++) {
        const uint8_t i = (earsHead + k) % EARS_QUEUE_MAX;
        for (int b = 0; b < earsLen[i]; b++) {
            body += HEXCH[earsPool[i][b] >> 4];
            body += HEXCH[earsPool[i][b] & 0x0F];
        }
        body += ':';
        body += (int)lround(earsRssi[i]);
        body += ':';
        body += (int)lround(earsSnr[i]);
        body += '\n';
        drained++;
    }
    if (drained == 0) return;

    WiFiClient c;
    if (!c.connect(coordIp.c_str(), 3232, EARS_CONNECT_MS)) {
        earsFailed();
        slog("[EARS] %s недоступен, кадры остаются в очереди (следующая попытка через %lu мс)\n",
             coordIp.c_str(), earsNextTryMs - millis());
        return;
    }
    c.setTimeout(4000);
    c.print(String("POST /ears HTTP/1.1\r\nHost: ") + coordIp +
            "\r\nX-API-Key: " + cfg.apiKey +
            "\r\nContent-Type: text/plain\r\n"
            "Content-Length: " + String((unsigned)body.length()) +
            "\r\nConnection: close\r\n\r\n");
    c.print(body);
    // Ответ нам не нужен, но код статуса скажет честно, принял ли координатор кадры:
    // снятая с очереди пачка, уехавшая в пустоту, была бы потеряна навсегда.
    bool ok = false;
    unsigned long deadline = millis() + 4000;
    String head;
    while (c.connected() && (long)(millis() - deadline) < 0) {
        if (!c.available()) { delay(1); continue; }
        char ch = (char)c.read();
        if (ch == '\r' || ch == '\n') {
            if (head.startsWith("HTTP/1.") && head.indexOf(" 200 ") > 0) ok = true;
            if (head.length() == 0) break;   // пустая строка = конец заголовка
            head = "";
        } else {
            head += ch;
        }
    }
    c.stop();
    if (ok) {
        earsPop(drained);
        earsFails = 0;
        earsNextTryMs = 0;   // очередь ещё не пуста — остаток уйдёт следующим проходом
        slog("[EARS] ушло %u кадров, в очереди %u\n", drained, earsCount);
    } else {
        earsFailed();
        slog("[EARS] POST /ears не принят (%u кадров остаются, следующая попытка через %lu мс)\n",
             drained, earsNextTryMs - millis());
    }
}
#endif // FEATURE_SUPPORT

// ===== Обратный канал «вторых ушей»: ответ пингу уходит через прошивальщика =====
// На пинг, пришедший через поддержку (/ears), координатор отвечает не со своего радио —
// отправитель его не слышит. Готовый кадр ответа уходит прошивальщику POST /radiotx, и
// тот передаёт его со своего радио (см. web.cpp). Хук зовёт ядро только у координатора
// (meshReplyTick под #ifndef SENSOR_NODE), но и здесь не мешает оставить под тем же
// условием — лишний символ в прошивке поддержки не нужен.
#ifndef SENSOR_NODE
#define RADIOTX_CONNECT_MS 3000
bool mcRelayFrameToSupport(const uint8_t* frame, int len) {
    if (len <= 0 || len > 255) return false;
    if (supportIp.length() < 7) return false;
    if (cfg.apiKey.length() == 0) return false;   // прошивальщик без ключа кадр не примет
    // Жив ли прошивальщик — спрашиваем у supportPresent(), а не у своего срока годности.
    // Здесь стоял свой литерал в 135 с, снятый с периода объявления КООРДИНАТОРА, тогда как
    // сверялся он с временем объявления ПРОШИВАЛЬЩИКА — а тот объявляется своим heartbeat,
    // раз в десять минут. Обратный канал поэтому жил около двух минут из каждых десяти, и
    // ответ на пинг, пришедший через «вторые уши», почти всегда уходил обычным флудом — как
    // раз к тем узлам, которые координатора не слышат.
    if (!supportPresent()) return false;

    static const char RADIO_HEX[] = "0123456789ABCDEF";
    String body;
    body.reserve(2 * len);
    for (int i = 0; i < len; i++) {
        body += RADIO_HEX[frame[i] >> 4];
        body += RADIO_HEX[frame[i] & 0x0F];
    }

    WiFiClient c;
    if (!c.connect(supportIp.c_str(), 3232, RADIOTX_CONNECT_MS)) {
        Serial.printf("[RADIOTX] %s недоступен\n", supportIp.c_str());
        return false;
    }
    c.setTimeout(4000);
    c.print(String("POST /radiotx HTTP/1.1\r\nHost: ") + supportIp +
            "\r\nX-API-Key: " + cfg.apiKey +
            "\r\nContent-Type: text/plain\r\n"
            "Content-Length: " + String((unsigned)body.length()) +
            "\r\nConnection: close\r\n\r\n");
    c.print(body);

    bool ok = false;
    unsigned long deadline = millis() + 4000;
    String head;
    while (c.connected() && (long)(millis() - deadline) < 0) {
        if (!c.available()) { delay(1); continue; }
        char ch = (char)c.read();
        if (ch == '\r' || ch == '\n') {
            if (head.startsWith("HTTP/1.") && head.indexOf(" 200 ") > 0) ok = true;
            if (head.length() == 0) break;
            head = "";
        } else {
            head += ch;
        }
    }
    c.stop();
    Serial.printf("[RADIOTX] ответ %s (%d байт %s прошивальщику)\n",
         ok ? "отдан" : "не принят", len, ok ? "ушло" : "осталось");
    return ok;
}
#endif // !SENSOR_NODE