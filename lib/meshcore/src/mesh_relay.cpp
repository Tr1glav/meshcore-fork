#include "config.h"
#include "globals.h"
#include "radio.h"
#include "mesh.h"

// ===== Ретрансляция чужих флуд-кадров =====
// Сеть устроена флудом: услышавший кадр добавляет в путь свой хэш и переиздаёт его, чтобы
// сообщение уходило дальше одного хопа. Раньше в кадре всегда был пустой путь — узел умел
// читать маршруты, но никто их не строил, и сообщения умирали на первом же хопе.
//
// Переиздание отделено от разбора и не блокирует главный цикл: кадр кладётся в маленькую
// очередь со случайной паузой (отправитель ещё повторяет свой флуд, а ретрансляторы,
// услышавшие тот же кадр, должны разойтись по времени, иначе нагрузят друг друга), а
// переиздаёт его главный цикл, когда пауза истечёт. Дедуп делает остальное: копия кадра,
// пришедшая с уже достроенным путём, хэшируется так же и отбрасывается (путь в хэш не
// входит), поэтому петли не возникает. Своё эхо узел не переиздаёт потому, что любой
// собственный кадр помечается «уже виденным» на передаче (markOwnFrameSeen).

#if FEATURE_RELAY

// Отложенные переиздания: пауза — чтобы не попасть в повторы отправителя и не столкнуться
// с другими ретрансляторами, услышавшими тот же кадр. Величины размером с запасом: отправитель
// держит эфир два кадра по ~0.5 с с паузой 60 мс.
#ifndef RELAY_QUEUE_MAX
#define RELAY_QUEUE_MAX 8
#endif
#ifndef RELAY_DELAY_MIN_MS
#define RELAY_DELAY_MIN_MS 1400
#endif
#ifndef RELAY_DELAY_MAX_MS
#define RELAY_DELAY_MAX_MS 2500
#endif
#ifndef MAX_RELAY_HOPS
#define MAX_RELAY_HOPS 32
#endif

static struct {
    unsigned long dueMs;         // 0 — слот свободен
    uint8_t frame[256];
    int len;
} relayQueue[RELAY_QUEUE_MAX];

// Решение и постановка в очередь. data/len — принятый кадр ДО переиздания (с исходным путём).
void maybeQueueRelay(const uint8_t* data, int len) {
    // Быстрый канал mesh OTA — односкачный (сырые кадры ретрансляторы не переносят).
    if (otaFastMode) return;
    if (len < 2) return;
    uint8_t header = data[0];
    uint8_t payload_type = (header >> 2) & 0x0F;
    uint8_t route_type = header & 0x03;

    // Транспортные коды (0x00/0x03) — служебный обмен между соседями, и direct (0x02)
    // адресован одному хопу; переиздаются только flood-кадры (0x01).
    if (payload_type == 0x00 || payload_type == 0x03) return;
    if (route_type != 0x01) return;

    int offset = 1;              // у flood-кадров байт пути стоит сразу после заголовка
    if (offset >= len) return;
    uint8_t path_len = data[offset++];
    uint8_t path_hash_size = (path_len >> 6) + 1;
    uint8_t hop_count = path_len & 0x3F;
    if (hop_count >= MAX_RELAY_HOPS) return;   // путь упёрся в потолок — дальше не форвардим
    int pathBytes = hop_count * path_hash_size;
    if (offset + pathBytes >= len) return;     // битый кадр: тело пустое

    // Личку НЕ для нас не разносим (у каждого свои ключи, читать её кроме адресата никто
    // не сможет), а личку для нас дальше передавать незачем — мы и есть получатель.
    if (payload_type == 0x02) {
        if (offset + pathBytes + 2 > len) return;
        if (data[offset + pathBytes] == ownShortHash) return;
    }

    if (len + PATH_HASH_SIZE > 255) return;    // кадр больше не влезает в SX1262 (лимит 255 Б)

    // Строим переиздаваемый кадр: тот же заголовок и тело, путь — исходный плюс наш хэш
    // в конце. Узел добавляет в путь первые два байта своего публичного ключа: тот же
    // источник, что и ownShortHash, только длиной PATH_HASH_SIZE.
    uint8_t fwd[256];
    fwd[0] = data[0];
    fwd[1] = ((PATH_HASH_SIZE - 1) << 6) | (hop_count + 1);
    memcpy(fwd + 2, data + 2, pathBytes);                                    // исходный путь
    memcpy(fwd + 2 + pathBytes, bot_pub, PATH_HASH_SIZE);                    // наш хэш
    memcpy(fwd + 2 + pathBytes + PATH_HASH_SIZE, data + 2 + pathBytes,       // тело
           len - (2 + pathBytes));
    int fwdLen = len + PATH_HASH_SIZE;

    // Свободный слот очереди; полная — молча пропускаем (эфир и так занят).
    int slot = -1;
    for (int i = 0; i < RELAY_QUEUE_MAX; i++) {
        if (relayQueue[i].dueMs == 0) { slot = i; break; }
    }
    if (slot < 0) {
        Serial.printf("[RLY] очередь переполнена (%u ждут) — кадр не переиздан\n",
                      (unsigned)RELAY_QUEUE_MAX);
        return;
    }

    relayQueue[slot].dueMs = millis() + random(RELAY_DELAY_MIN_MS, RELAY_DELAY_MAX_MS);
    if (relayQueue[slot].dueMs == 0) relayQueue[slot].dueMs = 1;   // 0 занято признаком «свободен»
    memcpy(relayQueue[slot].frame, fwd, fwdLen);
    relayQueue[slot].len = fwdLen;

    Serial.printf("[RLY] type=%u hops=%u->%u, переиздание через %lu мс\n",
                  payload_type, hop_count, hop_count + 1,
                  (unsigned long)(relayQueue[slot].dueMs - millis()));
}

void meshRelayTick() {
    for (int i = 0; i < RELAY_QUEUE_MAX; i++) {
        if (relayQueue[i].dueMs == 0) continue;
        if ((long)(millis() - relayQueue[i].dueMs) < 0) continue;
        relayQueue[i].dueMs = 0;

        // hex-лог кадра стоит ~40 мс на UART для 245-байтного кадра — в fast-режиме молчим
        // (в нём ретрансляции и нет)
        if (!otaFastMode) {
            Serial.printf("\n[TX RLY] (%dB): ", relayQueue[i].len);
            for (int k = 0; k < relayQueue[i].len; k++) Serial.printf("%02X", relayQueue[i].frame[k]);
            Serial.println();
        }
        relayForwardedCount++;
        txFrame(relayQueue[i].frame, relayQueue[i].len);
    }
}

#else   // !FEATURE_RELAY — ретрансляцию отключили признаком: функции пустые, линковке есть что звать
void maybeQueueRelay(const uint8_t*, int) {}
void meshRelayTick() {}
#endif