#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "display.h"
#include "ota.h"
#include "companion.h"

// ===== Каналы сети =====
// Выделено из mesh.cpp: вывод ключа канала, автоключ по имени, хранилище имён и
// загрузка каналов из настроек устройства. Ключ канала — единственное, что связывает
// эту часть с остальными, и она отдаётся через общий массив channels[].

// Секрет канала — 16-байтный ключ, дополненный нулями до 32; hash — первый байт SHA256(ключа)
static void setChannelKey(MeshChannel& ch, const uint8_t* key16) {
    memset(ch.secret, 0, sizeof(ch.secret));
    memcpy(ch.secret, key16, 16);
    uint8_t sha[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ch.secret, 16, sha);
    ch.hash = sha[0];
}

// Автоключ канала MeshCore: SHA256(имя)[0:16]
static void autoKey16(const char* name, uint8_t key16[16]) {
    uint8_t sha[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)name, strlen(name), sha);
    memcpy(key16, sha, 16);
}

// Имена ВСЕХ каналов лежат здесь: channels[].name — указатель, поэтому строка обязана
// пережить вызов. Раньше addChannelKey16 сохраняла указатель прямо на переданную строку
// (в том числе на c_str() глобального String) — пока каналы заводились только из setup(),
// это работало, но любое повторное присваивание того String оставляло провисший указатель.
static char chanNamePool[MAX_CHANNELS][33];
// Ключ канала не секрет: автоключ по имени либо общеизвестный PSK. См. channelKeyIsOpen.
static bool chanKeyOpen[MAX_CHANNELS];

bool channelKeyIsOpen(int idx) {
    if (idx < 0 || idx >= MAX_CHANNELS) return true;   // канала нет — считаем открытым
    return chanKeyOpen[idx];
}

int channelSetSlot(int idx, const char* name, const uint8_t* key16, bool openKey) {
    if (idx < 0 || idx >= MAX_CHANNELS || name == nullptr || name[0] == 0) return -1;
    if (idx > numChannels) return -1;          // дыр в списке быть не должно
    if (idx == numChannels) numChannels = idx + 1;
    strncpy(chanNamePool[idx], name, 32);
    chanNamePool[idx][32] = 0;
    channels[idx].name = chanNamePool[idx];
    chanKeyOpen[idx] = openKey;
    setChannelKey(channels[idx], key16);
    // Хэш ключа (первый байт SHA256) печатать безопасно: восстановить по нему ключ нельзя.
    // Сам ключ в лог не идёт — он одинаков у всех плат сети.
    Serial.printf("[CH] канал %d: %s hash 0x%02X%s\n", idx, chanNamePool[idx],
                  channels[idx].hash, openKey ? " (ключ открытый)" : "");
    return idx;
}

void addChannelKey16(const char* name, const uint8_t* key16, bool openKey) {
    if (name == nullptr || name[0] == 0) return;
    if (numChannels >= MAX_CHANNELS) {
        Serial.printf("[CH] не могу добавить %s: список каналов полон\n", name);
        return;
    }
    channelSetSlot(numChannels, name, key16, openKey);
}

void deriveChannels() {
    uint8_t key16[16];

    // #public: фиксированный PSK из MeshCore
    const char* psk_b64 = "izOH6cXN6mrJ5e26oRXNcg==";
    size_t olen = 0;
    memset(key16, 0, sizeof(key16));
    mbedtls_base64_decode(key16, 16, &olen, (const uint8_t*)psk_b64, strlen(psk_b64));
    addChannelKey16("#public", key16, true);   // PSK опубликован в MeshCore — не секрет

    // #connections: автоключ по имени, то есть тоже открытый канал
    autoKey16("#connections", key16);
    addChannelKey16("#connections", key16, true);

    // Приватный канал: нужен координатору и компаньону — в приложении это обычный
    // чат наравне с остальными, и без него в списке каналов видно только сенсорный.
    #if defined(MQTT_ENABLED) || defined(COMPANION_NODE)
    loadPrivateChannel();
    #endif
    // Сенсорный канал: нужен и MQTT-боту, и сенсорным платам (без WiFi),
    // поэтому добавляем всегда. Источник — build-флаги (secrets.ini).
    loadSensorChannel();
}

int findChannelByName(const char* name) {
    for (int i = 0; i < numChannels; i++) {
        if (strcmp(channels[i].name, name) == 0) return i;
    }
    return -1;
}

// Канал из настроек устройства: PSK в base64 либо автоключ по имени. nameStore — просто
// копия имени для экрана и MQTT; channels[].name живёт в своём пуле (см. channelSetSlot).
static int setNamedChannel(const char* tag, const char* keyField, const String& name,
                           const String& keyb64, String& nameStore, int& idxStore) {
    if (name.length() == 0 || name.length() > 32) return -1;
    uint8_t key16[16];
    // Пустой (или негодный) PSK означает ключ, выведенный из имени канала. Имя каналов
    // обычно очевидное, так что такой канал открыт всякому, кто его угадает: сообщения
    // читаются и подделываются, а на канале сенсоров этим же ключом защищена прошивка по
    // радио. Молчать об этом нельзя — предупреждаем при каждом старте.
    bool openKey = privateKeyTo16(keyb64, key16) < 0;
    if (openKey) {
        autoKey16(name.c_str(), key16);
        Serial.printf("[%s] ВНИМАНИЕ: ключ канала %s выведен из его имени — канал открыт "
                      "любому, кто знает имя. Задайте свой: set %s <16 байт в base64>\n",
                      tag, name.c_str(), keyField);
    }

    int idx = findChannelByName(name.c_str());
    if (idx < 0) {
        if (numChannels >= MAX_CHANNELS) {
            Serial.printf("[%s] MAX_CHANNELS reached, channel not added\n", tag);
            return -1;
        }
        addChannelKey16(name.c_str(), key16, openKey);
        idx = numChannels - 1;
    } else {
        if (memcmp(channels[idx].secret, key16, 16) != 0) {
            setChannelKey(channels[idx], key16);
            Serial.printf("[%s] channel %s key UPDATED, hash 0x%02X\n", tag, name.c_str(), channels[idx].hash);
        }
        chanKeyOpen[idx] = openKey;
    }
    nameStore = name;
    idxStore = idx;
    return idx;
}

void loadPrivateChannel() {
    // Каналы задаются ТОЛЬКО настройками устройства (NVS), не из HA.
    if (cfg.prvName.length() == 0) return;
    Serial.printf("[PRV] channel from config: %s\n", cfg.prvName.c_str());
    setNamedChannel("PRV", "prv_key", cfg.prvName, cfg.prvKey, privateChannelName, privateChannelIdx);
}

void loadSensorChannel() {
    if (cfg.snsName.length() == 0) return;
    Serial.printf("[SNS] sensor channel from config: %s\n", cfg.snsName.c_str());
    setNamedChannel("SNS", "sns_key", cfg.snsName, cfg.snsKey, sensorChannelName, sensorChannelIdx);
}

#ifdef MQTT_ENABLED
void loadTxChannel() {
    int idx = findChannelByName(cfg.txChannel.c_str());
    if (idx < 0) idx = 1;   // #connections
    if (idx < numChannels) {
        mqttTxChannel = idx;
        Serial.printf("[MQTT] TX channel from config: %s\n", channels[idx].name);
    }
}
#endif // MQTT_ENABLED

