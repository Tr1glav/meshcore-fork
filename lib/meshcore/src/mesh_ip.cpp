// ===== IP over MeshCore — транспорт =====
// Надёжная доставка фрагментированных IP-датаграмм поверх сенсорного канала.
// Формат:  данные  "ip:d" + 3 hex (seq) + 1 hex (fi<<4 | nf) + base64(raw)
//          подтв.  "ip:a" + 3 hex (base) + 2 hex (bitmap8)

#include "config.h"
#if FEATURE_MESH_IP

#include "mesh_ip.h"
#include "globals.h"
#include "mesh.h"
#include "radio.h"
#include "ota.h"        // slog: журнал (Serial + web-хвост координатора)
#include "crypto.h"     // encryptGroupText

#include <Arduino.h>
#include <cstring>

// ===================== Хелперы =====================

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t hexval(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}
static uint8_t  hex1(const char* p) { return hexval(p[0]); }
static uint16_t hex3(const char* p) {
    return (uint16_t)((hexval(p[0]) << 8) | (hexval(p[1]) << 4) | hexval(p[2]));
}
static uint16_t hex4(const char* p) {
    return (uint16_t)((hexval(p[0]) << 12) | (hexval(p[1]) << 8) |
                      (hexval(p[2]) << 4) | hexval(p[3]));
}

static int b64enc(const uint8_t* in, int inLen, char* out, int outMax) {
    int i = 0, o = 0;
    while (i + 2 < inLen && o + 4 <= outMax) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = kB64[(v >> 18) & 0x3F];
        out[o++] = kB64[(v >> 12) & 0x3F];
        out[o++] = kB64[(v >> 6) & 0x3F];
        out[o++] = kB64[v & 0x3F];
        i += 3;
    }
    if (i < inLen && o + 4 <= outMax) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < inLen) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = kB64[(v >> 18) & 0x3F];
        out[o++] = kB64[(v >> 12) & 0x3F];
        if (i + 1 < inLen) out[o++] = kB64[(v >> 6) & 0x3F];
        else               out[o++] = '=';
        out[o++] = '=';
    }
    if (o < outMax) out[o] = 0;
    return o;
}

// Таблица обратного декодирования (ленивая инициализация)
static uint8_t kB64D[256];
static void b64Init() {
    static bool inited = false;
    if (inited) return;
    memset(kB64D, 0xFF, sizeof(kB64D));
    for (int i = 0; i < 64; i++) kB64D[(uint8_t)kB64[i]] = (uint8_t)i;
    inited = true;
}

static int b64dec(const char* in, uint8_t* out, int outMax) {
    b64Init();
    int i = 0, o = 0;
    uint32_t v = 0;
    int bits = 0;
    while (in[i] && o < outMax) {
        uint8_t c = kB64D[(uint8_t)in[i++]];
        if (c > 63) continue;    // пропуска '=' и мусор
        v = (v << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((v >> bits) & 0xFF);
        }
    }
    return o;
}

// ===================== Чексумы =====================

uint16_t meshIpChecksum(const uint8_t* data, uint16_t len) {
    uint32_t sum = 0;
    while (len > 1) { sum += ((uint32_t)data[0] << 8) | data[1]; data += 2; len -= 2; }
    if (len) sum += (uint32_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

uint16_t meshIpTcpUdpChecksum(const uint8_t* src_ip, const uint8_t* dst_ip,
                               uint8_t proto, const uint8_t* data, uint16_t len) {
    uint32_t sum = 0;
    // pseudo-header
    sum += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    sum += proto;
    sum += len;
    // payload
    while (len > 1) { sum += ((uint32_t)data[0] << 8) | data[1]; data += 2; len -= 2; }
    if (len) sum += (uint32_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// ===================== Состояние линка =====================

#if defined(COMPANION_NODE) && FEATURE_MESH_IP
// Счётчики перехвата кадров телефона живут в mesh_ip_ap.cpp
extern uint32_t g_meshIpRxFwd;
extern uint32_t g_meshIpRxTun;
#endif

static String       s_linkPeer;       // имя пира (первый полученный фрагмент)
static uint32_t     s_lastRxMs = 0;   // момент последнего кадра от пира
static bool         s_linkUp   = false;
static meshIpRecvCb s_recvCb   = nullptr;
static uint32_t     s_dbgStatusMs = 0; // последний вывод статуса

// --- Исходящая очередь (кольцо IP-датаграмм) ---
static uint8_t  s_txQueue[MESH_IP_QUEUE_MAX][MESH_IP_PKT_MAX];
static uint16_t s_txQLen[MESH_IP_QUEUE_MAX];
static uint8_t  s_txHead  = 0;
static uint8_t  s_txCount = 0;   // сколько в очереди

// --- Исходящий фрагмент-движок (один пакет в полёте) ---
static bool     s_txInFlight     = false;
static uint16_t s_txBaseSeq      = 0;
static uint8_t  s_txFragCount    = 0;
static uint16_t s_txWaitBitmap   = 0;   // какие фрагменты уже ACK'нуты
static uint16_t s_txExpectBitmap = 0;
static uint8_t  s_txRetries      = 0;
static uint32_t s_txLastMs       = 0;

// --- Исходящая нумерация (независимая от пира) ---
static uint16_t s_nextSeq = 0;

// --- Входящее окно (восстановление из фрагментов пира) ---
static uint16_t s_rxBase    = 0;     // база текущего окна
static bool     s_rxWindow  = false;  // окно инициализировано
static uint16_t s_rxAckBase = 0;     // head-сeq сообщения, по которому шлём ACK
static uint16_t s_rxBitmap  = 0;     // бит i = 1 → фрагмент с seq=rxBase+i получен
static uint8_t  s_rxNf      = 0;     // nf из fi==0 (0 = ещё не знаем)
static uint8_t  s_rxHold[MESH_IP_WINDOW][MESH_IP_FRAG_MAX];
static uint8_t  s_rxHoldLen[MESH_IP_WINDOW];

// Собранный пакет, готовый к передаче в recvCb
static bool     s_rxMsgReady = false;
static uint16_t s_rxMsgLen   = 0;
static uint8_t  s_rxMsgBuf[MESH_IP_PKT_MAX];

// Статический общий буфер для base64
static char s_b64buf[MESH_IP_FRAG_MAX * 4 / 3 + 8];

// --- Токен-слоты (полудуплексный маятник) ---
// Строгое чередование TX/RX: кто последний передал — уступает ход пиру.
// s_slotTurn: true = наш ход (мы передаём), false = ход пира (мы слушаем)
static bool     s_slotTurn   = true;   // компаньон начинает (у него есть апстрим)
static uint32_t s_lastSlotMs = 0;      // момент отправки последнего фрагмента (guard)

// Отложенный ACK координатора: когда в очереди есть даунлинк, ACK на пришедший
// апстрим-фрагмент не уходит сразу — его заменяет даунлинк-фрагмент (он же даёт
// ход компаньону). По ACK на даунлинк накопленный ACK сбрасывается одной посылкой.
static bool     s_ackQValid  = false;
static uint16_t s_ackQBase   = 0;
static uint16_t s_ackQBitmap = 0;

// Принят poll "ip:p" от пира (у координатора: надо вернуть ход — эхо/даунлинк)
static bool     s_receivedPoll = false;

// Счетчик seq для IP-датграмм в fast-режиме
static uint8_t  s_ipSeq      = 0;

// --- Fast-режим (хендшейк переключения радио на FSK 250 кбит/с) ---
// s_fastModePending: хендшейк начат, радио ещё на LoRa; переключение произойдёт
// в meshIpTick в момент s_fastSwitchAt (пауза SETTLE: оба узла успевают услышать
// подтверждение "ip:fast:ok", пока ещё в LoRa).
static bool     s_fastModePending = false;
static uint32_t s_fastSwitchAt    = 0;

// ===================== Публичные =====================

uint16_t meshIpFragCap() {
    int nameLen = (int)cfg.name.length();
    if (nameLen < 0) nameLen = 0;
    // buildGroupEnc: plaintext = 4 (ts) + 1 (type) + nameLen + 2 (": ") + tunnelMsg
    // tunnelMsg <= 240 - 5 - (nameLen + 2)
    int tunnelMax = GROUP_TEXT_MAX_PLAIN - 5 - nameLen - 2;
    // tunnelMsg = "ip:"(3) + cmd(1) + seq3(3) + fn1(1) + base64
    int b64Max = tunnelMax - 3 - 1 - 3 - 1;
    if (b64Max < 0) b64Max = 0;
    // base64 длина = 4*ceil(raw/3) → rawMax = (b64Max/4)*3 (чтобы влезло с паддингом)
    int rawMax = (b64Max / 4) * 3;
    if (rawMax > MESH_IP_FRAG_MAX) rawMax = MESH_IP_FRAG_MAX;
    if (rawMax < 1) rawMax = 1;
    return (uint16_t)rawMax;
}

bool meshIpLinkUp() { return s_linkUp; }

void meshIpReset() {
    s_linkUp = false;
    s_linkPeer = "";
    s_txInFlight = false;
    s_txHead = s_txCount = 0;
    s_txWaitBitmap = 0;
    s_txRetries = 0;
    s_rxBitmap = 0;
    s_rxNf = 0;
    memset(s_rxHoldLen, 0, sizeof(s_rxHoldLen));
    s_rxMsgReady = false;
    s_slotTurn   = true;
    s_lastSlotMs = 0;
}

void meshIpInit() {
    meshIpReset();
    s_nextSeq = (uint16_t)(millis() & 0xFFF);  // начальный seq — псевдослучайный
    s_recvCb = nullptr;

    #if defined(COMPANION_NODE)
    meshIpApInit();
    #endif
    #if defined(MQTT_ENABLED)
    meshIpNatInit();
    #endif
}

void meshIpSetRecvCb(meshIpRecvCb cb) { s_recvCb = cb; }

static void sendSensorFrame(const char* msg);

// ===================== Fast-режим (тесты на FSK 250 кбит/с) =====================

// Переключение радио и флага в момент таймера хендшейка. Вызывается из meshIpTick.
static void meshIpFastApply() {
    ipFastMode = true;
    radioSetFastConfig();
    s_fastModePending = false;
    slog("[IP] fast: радио -> FSK %d кбит/с\n", (int)OTA_FSK_BR);
}

void meshIpSetFastMode(bool on) {
    if (on) {
        if (ipFastMode || s_fastModePending) return;
        slog("[IP] fast: запрос входа\n");
        sendSensorFrame("ip:fast");
        return;   // переключимся, когда придёт "ip:fast:ok"
    }
    // Выход
    if (!ipFastMode) {
        s_fastModePending = false;
        return;
    }
    if (s_fastModePending) return;
    slog("[IP] fast: запрос выхода\n");
    sendSensorFrame("ip:slow");
}

// Сырой кадр fast-канала: [BE EF][type][seq 4][text][crc16 2]
// Принимаем только RAW_TYPE_IP; содержимое — тот же текст туннеля, что летит
// и обычным каналом, поэтому отдаём его в meshIpOnChannelText.
void meshIpOnRawFrame(const uint8_t* buf, int len) {
    if (len < 9 || buf[0] != RAW_MAGIC0 || buf[1] != RAW_MAGIC1) return;
    if (buf[2] != RAW_TYPE_IP) return;
    int paylen = len - 2;
    uint16_t c = (uint16_t)(buf[len - 1] << 8) | buf[len - 2];
    if (crc16buf(buf, paylen) != c) return;
    int tlen = len - 9;                 // header 7 + crc 2
    if (tlen <= 0) return;
    char txt[257];
    if (tlen > 256) tlen = 256;
    memcpy(txt, buf + 7, tlen);
    txt[tlen] = 0;
    // fast-канал не несёт имени отправителя — используем имя пира из линка
    meshIpOnChannelText(s_linkPeer, String(txt));
}

// ===================== Внутренняя очередь =====================

// Потокобезопасная (portENTER_CRITICAL) обёртка
static portMUX_TYPE s_ipMux = portMUX_INITIALIZER_UNLOCKED;

static void meshIpInjectRaw(const uint8_t* pkt, uint16_t len) {
    if (s_txCount >= MESH_IP_QUEUE_MAX) return;
    if (len > MESH_IP_PKT_MAX) len = MESH_IP_PKT_MAX;
    uint8_t tail = (s_txHead + s_txCount) % MESH_IP_QUEUE_MAX;
    memcpy(s_txQueue[tail], pkt, len);
    s_txQLen[tail] = len;
    s_txCount++;
}

void meshIpInject(const uint8_t* pkt, uint16_t len) {
    portENTER_CRITICAL(&s_ipMux);
    meshIpInjectRaw(pkt, len);
    portEXIT_CRITICAL(&s_ipMux);
}

// ===================== Отправка фрагмента =====================

static void sendSensorFrame(const char* msg) {
    String sMsg(msg);
    // Быстрый режим: отправка через FSK 250 кбит/с (режим прошивки).
    // Сенсорный канал не нужен — кадр сырой, вне meshcore-маршрутизации.
    if (ipFastMode) {
        uint8_t frame[300];
        int f = rawBuildFrame(frame, RAW_TYPE_IP, ++s_ipSeq, (const uint8_t*)sMsg.c_str(), sMsg.length());
        rawTxFrame(frame, f);
        // Любая передача съедает ход: после TX уступаем пиру, RX его вернёт.
        s_slotTurn   = false;
        s_lastSlotMs = millis();
        return;
    }
    if (sensorChannelIdx < 0) return;
    uint8_t frame[256];
    int f = buildGroupFrameFlood(sensorChannelIdx, sMsg, frame, sizeof(frame));
    if (f > 0) {
        txFrame(frame, f);
        // Любая передача съедает ход: после TX уступаем пиру, RX его вернёт.
        s_slotTurn   = false;
        s_lastSlotMs = millis();
    }
}

static void sendFragment(const uint8_t* raw, uint8_t len, uint16_t seq, uint8_t fi, uint8_t nf) {
    int b64len = b64enc(raw, len, s_b64buf, (int)sizeof(s_b64buf) - 1);
    s_b64buf[b64len] = 0;
    char frame[1400];
    snprintf(frame, sizeof(frame), "%sd%03X%01X%s", MESH_IP_PFX,
             seq & 0xFFF, ((fi & 0xF) << 4) | (nf & 0xF), s_b64buf);
    sendSensorFrame(frame);
}

// ===================== Обработка входящего ACK =====================

static void handleAck(uint16_t ackBase, uint16_t bitmap) {
    if (!s_txInFlight) return;
    if (ackBase != s_txBaseSeq) return;
    if (bitmap == 0 || (s_txWaitBitmap | bitmap) == s_txWaitBitmap) return;
    s_txWaitBitmap |= bitmap;
    s_txRetries = 0;                 // был прогресс — пачку не рвём
    s_txLastMs = millis();           // следующий фрагмент выйдем сразу после сброса ниже
    slog("[IP] ACK base=%03X bitmap=%04X wait=%04X expect=%04X\n",
                  ackBase, bitmap, s_txWaitBitmap, s_txExpectBitmap);
    if (s_txWaitBitmap == s_txExpectBitmap) {
        s_txInFlight = false;
        s_txHead = (s_txHead + 1) % MESH_IP_QUEUE_MAX;
        s_txCount--;
        slog("[IP] TX complete, queued=%d\n", s_txCount);
        // Координатор: пачку даунлинка приняли целиком — пора выдать накопленный
        // апстрим-ACK (мы его задерживали, пока шёл даунлинк).
        #if defined(MQTT_ENABLED)
        if (s_ackQValid) {
            char ackFrame[16];
            snprintf(ackFrame, sizeof(ackFrame), "%sa%03X%04X", MESH_IP_PFX,
                     s_ackQBase & 0xFFF, s_ackQBitmap);
            sendSensorFrame(ackFrame);
            s_ackQValid = false;
            slog("[IP] ACK flush base=%03X bitmap=%04X\n",
                          s_ackQBase & 0xFFF, s_ackQBitmap);
        }
        #endif
    } else {
        // Пачка не собрана — повторных ретраев не нужно, следующий фрагмент в tick
        s_txLastMs = 0;
    }
}

// ===================== Обработка входящего фрагмента =====================

static void handleDataFragment(uint16_t seq, uint8_t fi, uint8_t nf,
                               const uint8_t* raw, uint8_t rawLen) {
    if (nf == 0 || nf > MESH_IP_FRAGS_PER_MSG || fi >= nf) return;

    s_lastRxMs = millis();
    s_linkUp = true;

    // Пир ушёл в переполнение окна (сброс на своей стороне) → начинаем заново
    uint16_t headSeq = seq - fi;
    s_rxAckBase = headSeq;   // все фрагменты одного сообщения делят head → base ACK текущий
    uint16_t delta = (uint16_t)((seq - s_rxBase) & 0xFFF);
    if (!s_rxWindow || delta >= MESH_IP_WINDOW) {
        slog("[IP] RX window reset: seq=%03X base=%03X delta=%u\n", seq, s_rxBase, delta);
        s_rxBase = headSeq;
        s_rxAckBase = headSeq;
        s_rxBitmap = 0;
        s_rxNf = 0;
        s_rxWindow = true;
        memset(s_rxHoldLen, 0, sizeof(s_rxHoldLen));
        delta = (uint16_t)((seq - headSeq) & 0xFFF);   // = fi (база уже = headSeq)
    }
    uint8_t slot = (uint8_t)(delta & (MESH_IP_WINDOW - 1));

    if (rawLen > MESH_IP_FRAG_MAX) rawLen = MESH_IP_FRAG_MAX;
    memcpy(s_rxHold[slot], raw, rawLen);
    s_rxHoldLen[slot] = rawLen;
    s_rxBitmap |= (uint16_t)(1 << slot);

    if (s_rxNf == 0 || fi == 0) s_rxNf = nf;
    if (s_rxNf > MESH_IP_FRAGS_PER_MSG) s_rxNf = MESH_IP_FRAGS_PER_MSG;

    slog("[IP] RX frag seq=%03X fi=%d/%d raw=%d bitmap=%04X\n",
                  seq, fi, nf, rawLen, s_rxBitmap);

#if defined(MQTT_ENABLED)
    // Координатор: if есть даунлинк в очереди и он ещё не в полёте — ACK откладываем.
    // Ход тратим на даунлинк-фрагмент; накопленный ACK сбросится по ACK даунлинка.
    if (s_txCount > 0 && !s_txInFlight) {
        s_ackQValid  = true;
        s_ackQBase   = s_rxAckBase;   // копируем ДО сброса окна ниже
        s_ackQBitmap = s_rxBitmap;
        slog("[IP] ACK deferred base=%03X bitmap=%04X (downlink queued=%d)\n",
                      s_rxAckBase & 0xFFF, s_rxBitmap, s_txCount);
    } else
#endif
    {
        // ACK этой порции до сброса состояния: bitmap ещё отражает принятые фрагменты
        char ackFrame[16];
        snprintf(ackFrame, sizeof(ackFrame), "%sa%03X%04X", MESH_IP_PFX,
                 s_rxAckBase & 0xFFF, s_rxBitmap);
        sendSensorFrame(ackFrame);
    }

    if (s_rxNf != 0) {
        bool complete = true;
        for (uint8_t j = 0; j < s_rxNf; j++) {
            uint16_t d2 = (uint16_t)(((headSeq + j) - s_rxBase) & 0xFFF);
            if (d2 >= MESH_IP_WINDOW ||
                !(s_rxBitmap & (uint16_t)(1 << (d2 & (MESH_IP_WINDOW - 1))))) {
                complete = false; break;
            }
        }
        if (complete) {
            int total = 0;
            for (uint8_t j = 0; j < s_rxNf; j++) {
                uint16_t d2 = (uint16_t)(((headSeq + j) - s_rxBase) & 0xFFF);
                uint8_t k = (uint8_t)(d2 & (MESH_IP_WINDOW - 1));
                if (total + s_rxHoldLen[k] > MESH_IP_PKT_MAX) break;
                memcpy(s_rxMsgBuf + total, s_rxHold[k], s_rxHoldLen[k]);
                total += s_rxHoldLen[k];
            }
            s_rxMsgLen = (uint16_t)total;
            s_rxMsgReady = true;
            slog("[IP] RX ASSEMBLED %dB, delivering via recvCb\n", total);
            s_rxAckBase = headSeq;   // ACK базой остаётся head собранного сообщения
            s_rxBase = (uint16_t)((headSeq + s_rxNf) & 0xFFF);
            s_rxBitmap = 0;
            s_rxNf = 0;
            memset(s_rxHoldLen, 0, sizeof(s_rxHoldLen));
        }
    }
}

// ===================== Приём текста из сенсорного канала =====================

bool meshIpOnChannelText(const String& name, const String& text) {
    if (text.length() <= 3 || !text.startsWith(MESH_IP_PFX)) return false;
    // Управление fast-режимом IP-туннеля (выполняется перед обычными cmd).
    // Хендшейк идёт по текущему каналу, СЕЙЧАС ещё штатному LoRa:
    //   инициатор -> "ip:fast"        (шлёт при ipfast on)
    //   ведомый   -> "ip:fast:ok"     (подтвердил, переключает радио)
    //   инициатор <- "ip:fast:ok"     (переключает радио)
    // Выход — зеркально "ip:slow" / "ip:slow:ok", но уже по fast-каналу,
    // поэтому sendSensorFrame в этих ветках тоже учитывает ipFastMode.
    if (text == "ip:fast" || text == "ip:fast:ok") {
        if (name == cfg.name) return true;   // собственное echo — игнорируем
        if (s_fastModePending) return true;  // уже переключаемся
        slog("[IP] fast: %s\n", text == "ip:fast" ? "request" : "confirm");
        if (text == "ip:fast") sendSensorFrame("ip:fast:ok");
        s_fastModePending = true;
        s_fastSwitchAt = millis() + MESH_IP_FAST_SETTLE_MS;
        return true;
    }
    if (text == "ip:slow" || text == "ip:slow:ok") {
        if (name == cfg.name) return true;
        if (!ipFastMode && !s_fastModePending) return true;
        slog("[IP] slow: %s\n", text == "ip:slow" ? "request" : "confirm");
        if (text == "ip:slow") sendSensorFrame("ip:slow:ok");
        ipFastMode = false;
        radioSetNormalConfig();  // вернём радио в LoRa режим
        s_fastModePending = false;
        return true;
    }
    char cmd = text.charAt(3);
    if (cmd != 'd' && cmd != 'D' && cmd != 'a' && cmd != 'A' &&
        cmd != 'p' && cmd != 'P') return false;

    // Игнорируем собственные фрагменты (echo自己的 flood)
    if (name == cfg.name) return true;

    s_linkPeer = name;
    s_lastRxMs = millis();
    s_linkUp = true;
    // Пир передал — ход наш (следующий TX съест turn в sendSensorFrame)
    s_slotTurn = true;

    if (cmd == 'p' || cmd == 'P') {
        // Poll пира: он даёт ход и ждёт, что у нас есть (ACK/даунлинк) — либо
        // молчит; обработчик в tick вернёт ход эхом. Принимаем и пометим.
        s_receivedPoll = true;
        return true;
    }

    if (cmd == 'd' || cmd == 'D') {
        // Данные: "ip:d" + 3seq + 1fn + base64
        if (text.length() < 8) return false;
        const char* p = text.c_str() + 4;
        uint16_t seq = hex3(p); p += 3;
        uint8_t fn   = hex1(p); p += 1;
        uint8_t fi   = (fn >> 4) & 0x0F;
        uint8_t nf   = fn & 0x0F;
        uint8_t rawBuf[MESH_IP_FRAG_MAX];
        int rawLen = b64dec(p, rawBuf, sizeof(rawBuf));
        if (rawLen <= 0) return false;
        handleDataFragment(seq, fi, nf, rawBuf, (uint8_t)rawLen);
        return true;
    }
    // ACK
    if (text.length() < 11) return false;
    const char* p = text.c_str() + 4;
    uint16_t ackBase = hex3(p); p += 3;
    uint16_t bitmap  = hex4(p);
    handleAck(ackBase, bitmap);
    return true;
}

// ===================== Сторожевой таймер =====================

void meshIpTick() {
    // Применение отложенного переключения в fast-режим (оба узла ждут паузу)
    if (s_fastModePending && !ipFastMode && millis() >= s_fastSwitchAt) {
        meshIpFastApply();
    }

    // Доставка собранного пакета в recvCb
    if (s_rxMsgReady && s_recvCb) {
        s_recvCb(s_rxMsgBuf, s_rxMsgLen);
        s_rxMsgReady = false;
    }

    // Периодический статус (раз в 5 c) — видно, есть ли вообще трафик телефона
    if ((millis() - s_dbgStatusMs) >= 5000) {
        s_dbgStatusMs = millis();
        #if defined(COMPANION_NODE) && FEATURE_MESH_IP
        slog("[IP] st: link=%d rxFwd=%lu rxTun=%lu txQue=%d peer=%s\n",
                      (int)s_linkUp, (unsigned long)g_meshIpRxFwd,
                      (unsigned long)g_meshIpRxTun, (int)s_txCount,
                      s_linkPeer.length() ? s_linkPeer.c_str() : "-");
        #else
        slog("[IP] st: link=%d txQue=%d peer=%s\n",
                      (int)s_linkUp, (int)s_txCount,
                      s_linkPeer.length() ? s_linkPeer.c_str() : "-");
        #endif
    }

    // Во время OTAfastMode не отправляем (радио занято)
    if (otaFastMode) return;

    // Таймаут пира: только если линк реально был поднят. Иначе не трогаем
    // очередь — она нужна для bootstrap, пока мы ещё не слышали пира.
    if (s_linkUp && millis() - s_lastRxMs > MESH_IP_IDLE_MS) {
        s_linkUp = false;
        meshIpReset();
        return;
    }

    // Bootstrap: пока пир не слышен, но есть что слать — шлём. Первый же
    // фрагмент, принятый координатором, поднимет линк и на его стороне.
    if (!s_linkUp && s_txCount == 0) return;

    // ---- Токен-слоты (полудуплексный маятник) ----
    // Передаём, когда: (а) линк не поднят — bootstrap, ждать нечего;
    // (б) ход наш (s_slotTurn, вернётся RX от пира); (в) ретрай созрел, а пир
    // молчит — иначе маятник бы завис, потеряв кадр. Ретрай НЕ пускаем, если пир
    // недавно говорил (он живой и, возможно, шлёт нам пачку — не мешаем ему).
    uint32_t nowMs = millis();
    bool peerSilent = (nowMs - s_lastRxMs) >= MESH_IP_RTT_MS;
    bool retryDue = s_txInFlight &&
                    (nowMs - s_txLastMs) >= MESH_IP_RTT_MS && peerSilent;
    bool myTurn = !s_linkUp || s_slotTurn || retryDue;
    if (!myTurn) return;

    // Координатор: пир дал ход, даунлинк пуст — выдать накопленный апстрим-ACK
    // (страховка на случай, если пачка даунлинка сдохла до полного сбора).
    #if defined(MQTT_ENABLED)
    if (s_ackQValid && !s_txInFlight) {
        char ackFrame[16];
        snprintf(ackFrame, sizeof(ackFrame), "%sa%03X%04X", MESH_IP_PFX,
                 s_ackQBase & 0xFFF, s_ackQBitmap);
        sendSensorFrame(ackFrame);
        s_ackQValid = false;
        slog("[IP] ACK flush (idle) base=%03X bitmap=%04X\n",
                      s_ackQBase & 0xFFF, s_ackQBitmap);
    }
    #endif

    // Компаньон: очередь пуста, линк жив, пир молчит — опрос координатора.
    // Отдаёт ход, бодрит линк и будит даунлинк, если тот висит в очереди.
    #if defined(COMPANION_NODE)
    if (s_linkUp && !s_txInFlight && s_txCount == 0 &&
        (nowMs - s_lastRxMs) >= MESH_IP_POLL_MS) {
        slog("[IP] POLL\n");
        sendSensorFrame("ip:p");
        return;
    }
    #endif

    if (s_receivedPoll) {
        s_receivedPoll = false;
        if (s_txInFlight || s_txCount > 0) {
            // Есть трафик — движок ниже использует наш ход по делу.
        } else {
            // Говорить нечего. Координатор обязан эхо-вернуть ход кометьсьону
            // (иначе тот встанет на своём таймере в жёсткую паузу); компаньон
            // на poll всегда молчит — эхо породило бы бесконечный маятник.
            #if defined(MQTT_ENABLED)
            slog("[IP] POLL echo\n");
            sendSensorFrame("ip:p");
            #endif
            return;
        }
    }

    // Исходящий фрагмент-движок: один фрагмент за ход. Каждый кадр ~245 Б на
    // SF8/BW62.5 занимает в эфире ~1.8 c — передав, мы уступаем ход пиру, и его
    // ACK/даунлинк успевает дойти до возврата нашего слота.
    const uint8_t* pkt = s_txQueue[s_txHead];
    uint16_t pktLen = s_txQLen[s_txHead];

    if (s_txInFlight) {
        // Пачка собрана — снимаем пакет с очереди
        if (s_txWaitBitmap == s_txExpectBitmap) {
            s_txInFlight = false;
            s_txHead = (s_txHead + 1) % MESH_IP_QUEUE_MAX;
            s_txCount--;
            slog("[IP] TX complete, queued=%d\n", s_txCount);
            return;
        }
        // ACK пришёл с прогрессом (s_txLastMs==0) → шлём следующий фрагмент сразу
        bool wantSend = (s_txLastMs == 0);
        if (!wantSend && retryDue) {
            s_txRetries++;
            if (s_txRetries > MESH_IP_RETRY_MAX) {
                s_txInFlight = false;
                s_txHead = (s_txHead + 1) % MESH_IP_QUEUE_MAX;
                s_txCount--;
                slog("[IP] TX TIMEOUT (retries=%d), packet dropped\n", s_txRetries);
                #if defined(MQTT_ENABLED)
                // Пачка даунлинка сдохла — освобождаем застрявший апстрим-ACK
                if (s_ackQValid) {
                    char ackFrame[16];
                    snprintf(ackFrame, sizeof(ackFrame), "%sa%03X%04X", MESH_IP_PFX,
                             s_ackQBase & 0xFFF, s_ackQBitmap);
                    sendSensorFrame(ackFrame);
                    s_ackQValid = false;
                    slog("[IP] ACK flush (drop) base=%03X bitmap=%04X\n",
                                  s_ackQBase & 0xFFF, s_ackQBitmap);
                }
                #endif
                return;
            }
            slog("[IP] TX RETRY %d/%d\n", s_txRetries, MESH_IP_RETRY_MAX);
            wantSend = true;
        }
        if (wantSend) {
            uint16_t rawMax = meshIpFragCap();
            // первый неподтверждённый фрагмент (обычно следующий по порядку)
            uint8_t i = 0;
            while (i < s_txFragCount && (s_txWaitBitmap & (1 << i))) i++;
            if (i < s_txFragCount) {
                uint16_t off = (uint16_t)(i * rawMax);
                uint8_t  flen = (uint8_t)((off + rawMax <= pktLen) ? rawMax : (pktLen - off));
                sendFragment(pkt + off, flen, s_txBaseSeq + i, i, s_txFragCount);
            }
            s_txLastMs = millis();
        }
    } else if (s_txCount > 0) {
        uint16_t rawMax = meshIpFragCap();
        s_txFragCount = (pktLen == 0) ? 1 : (uint8_t)((pktLen + rawMax - 1) / rawMax);
        if (s_txFragCount > MESH_IP_FRAGS_PER_MSG) s_txFragCount = MESH_IP_FRAGS_PER_MSG;
        s_txBaseSeq = s_nextSeq;
        s_nextSeq = (uint16_t)((s_nextSeq + s_txFragCount) & 0xFFF);
        s_txExpectBitmap = (uint16_t)((1U << s_txFragCount) - 1);
        s_txWaitBitmap = 0;
        s_txRetries = 0;
        s_txInFlight = true;

        slog("[IP] TX START seq=%03X frags=%d pktLen=%d rawMax=%d expect=%04X\n",
                      s_txBaseSeq, s_txFragCount, pktLen, rawMax, s_txExpectBitmap);
        sendFragment(pkt, (uint8_t)min((uint16_t)rawMax, pktLen),
                     s_txBaseSeq, 0, s_txFragCount);
        s_txLastMs = millis();
    }
}

#endif // FEATURE_MESH_IP
