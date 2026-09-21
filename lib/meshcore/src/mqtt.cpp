#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "mesh.h"
#include "mqtt.h"
#include "ota.h"

#ifdef MQTT_ENABLED
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[256];
    int mlen = min((unsigned int)255, length);
    memcpy(msg, payload, mlen);
    msg[mlen] = 0;

    // --- meshcore/bot/.../cmd/send ---
    if (strstr(topic, "/cmd/send")) {
        if (!isListening) {
            Serial.println("[MQTT] radio off, ignoring send cmd");
            return;
        }
        if (otaSessionActive()) {
            Serial.println("[MQTT] mesh OTA in progress, ignoring send cmd");
            return;
        }
        uint8_t frame[256];
        int fl = buildGroupFrameFlood(mqttTxChannel, msg, frame, sizeof(frame));
        if (fl > 0) {
            Serial.printf("[MQTT TX] %s: %s: %s (%dB)\n", channels[mqttTxChannel].name, cfg.name.c_str(), msg, fl);
            floodSend(mqttTxChannel, frame, fl);
        }
        return;
    }

    // --- meshcore/bot/.../cmd/listening ---
    if (strstr(topic, "/cmd/listening")) {
        if (strncmp(msg, "ON", 2) == 0 && !isListening) {
            isListening = true;
            radio.startReceive();
            Serial.println("[MQTT] listening ON");
        } else if (strncmp(msg, "OFF", 3) == 0 && isListening) {
            isListening = false;
            radio.standby();
            Serial.println("[MQTT] listening OFF");
        }
        char stateTopic[96];
        snprintf(stateTopic, sizeof(stateTopic), "%s/state", mqttPrefix);
        mqtt.publish(stateTopic, isListening ? "ON" : "OFF", true);
        return;
    }
}

void publishDiscovery() {
    // Имя узла попадает и в JSON, и в имена топиков, и там от него нужно разное:
    // в JSON — экранирование (кавычка или обратный слэш в имени ломают разбор конфига в
    // Home Assistant, и датчик просто не появляется), в топике — slug (символы '/', '+'
    // и '#' в именах топиков MQTT значат совсем другое). Для сенсоров это делалось с
    // самого начала, а имя самого координатора подставлялось дословно.
    char nameEsc[64], slug[48];
    jsonEscape(cfg.name.c_str(), nameEsc, sizeof(nameEsc));
    mqttSlug(cfg.name.c_str(), slug, sizeof(slug));

    // Device block общий для всех сущностей бота
    char devBlock[256];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_bot_%s\"],"
        "\"name\":\"%s\","
        "\"manufacturer\":\"MeshCore\","
        "\"model\":\"ESP32-S3 Listener\","
        "\"sw_version\":\"" FW_VERSION "\"",
        slug, nameEsc);

    char topic[128], payload[512];

    // --- Sensor: последний отправитель (state = имя отправителя) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/last_sender/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s LastSender\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"{{ value_json.sender }}\","
        "\"json_attributes_topic\":\"%s/state\","
        "\"unique_id\":\"meshcore_%s_lastsender\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: статус ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/status/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Status\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.uptime }}\","
        "\"json_attributes_topic\":\"%s/status\","
        "\"unique_id\":\"meshcore_%s_stat\","
        "\"icon\":\"mdi:server\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: IP адрес _mqtt (атрибут ip из /status) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/ip/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s IP\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.ip }}\","
        "\"unique_id\":\"meshcore_%s_ip\","
        "\"icon\":\"mdi:ip-network\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: температура CPU (встроенный датчик ESP32-S3) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/temp/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Temp\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.temp | float }}\","
        "\"unit_of_measurement\":\"°C\","
        "\"unique_id\":\"meshcore_%s_temp\","
        "\"icon\":\"mdi:thermometer\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: версия прошивки (поле version из /status) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/version/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Firmware\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.version }}\","
        "\"unique_id\":\"meshcore_%s_version\","
        "\"icon\":\"mdi:chip\","
        "\"entity_category\":\"diagnostic\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: last_msg выбранного канала (для триггеров) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/lastmsg/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s LastMsg\","
        "\"state_topic\":\"%s/lastmsg\","
        "\"force_update\":true,"
        "\"unique_id\":\"meshcore_%s_lastmsg\","
        "\"icon\":\"mdi:message-arrow-right\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Text: отправка сообщения ---
    snprintf(topic, sizeof(topic), "homeassistant/text/meshcore_%s/send/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Send\","
        "\"command_topic\":\"%s/cmd/send\","
        "\"unique_id\":\"meshcore_%s_send\","
        "\"icon\":\"mdi:message-text\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Switch: listening ---
    snprintf(topic, sizeof(topic), "homeassistant/switch/meshcore_%s/listening/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Listening\","
        "\"command_topic\":\"%s/cmd/listening\","
        "\"state_topic\":\"%s/lstate\","
        "\"unique_id\":\"meshcore_%s_sw\","
        "\"icon\":\"mdi:radio\","
        "\"device\":{%s}}",
        nameEsc, mqttPrefix, mqttPrefix, slug, devBlock);
    mqtt.publish(topic, payload, true);

    Serial.printf("[MQTT] discovery published for <%s>\n", slug);
    discoveryPublished = true;
}

void publishMessage() {
    if (!mqttConnected) return;
    char escSender[64], escText[128], escChan[64], escRoute[128];
    jsonEscape(lastSender.c_str(), escSender, sizeof(escSender));
    jsonEscape(lastMessage.c_str(), escText, sizeof(escText));
    jsonEscape(lastChannelName.c_str(), escChan, sizeof(escChan));
    jsonEscape(lastPath[0] ? lastPath : "direct", escRoute, sizeof(escRoute));
    // Числа — через fmtFix: float-printf тянет в образ ~35 КБ newlib (см. crypto.h),
    // а ради него в проекте и появились fmtFix/parseFixed.
    char rssiS[12], snrS[12];
    fmtFix(lastRSSI, 1, rssiS, sizeof(rssiS));
    fmtFix(lastSNR, 1, snrS, sizeof(snrS));
    char topic[96], payload[512];
    snprintf(topic, sizeof(topic), "%s/state", mqttPrefix);
    snprintf(payload, sizeof(payload),
        "{\"sender\":\"%s\",\"text\":\"%s\",\"channel\":\"%s\","
        "\"rssi\":%s,\"snr\":%s,\"hops\":%d,\"route\":\"%s\"}",
        escSender, escText, escChan,
        rssiS, snrS, lastHopCount, escRoute);
    mqtt.publish(topic, payload);
    Serial.printf("[MQTT] message published\n");

    // last_msg: публикуется только для ВЫБРАННОГО в HA канала (для триггеров).
    // Значение меняется только когда пришло сообщение с канала mqttTxChannel.
    // Спустя LASTMSG_RESET_MS после публикации обнуляется: HA-триггер на
    // одинаковый текст срабатывает снова (состояние менялось /gate -> "" -> /gate).
    if (lastChannelIdx == mqttTxChannel && lastMessage.length() > 0) {
        char lmTopic[96];
        snprintf(lmTopic, sizeof(lmTopic), "%s/lastmsg", mqttPrefix);
        mqtt.publish(lmTopic, lastMessage.c_str(), true);
        lastmsgClearAt = millis() + LASTMSG_RESET_MS;
        lastmsgPendingClear = true;
        Serial.printf("[MQTT] lastmsg published (ch %s)\n", channels[mqttTxChannel].name);
    }
}

void clearLastMsg() {
    if (!lastmsgPendingClear || !mqttConnected) return;
    if ((int32_t)(millis() - lastmsgClearAt) < 0) return;   // ещё не время (учёт wrap)
    lastmsgPendingClear = false;
    char lmTopic[96];
    snprintf(lmTopic, sizeof(lmTopic), "%s/lastmsg", mqttPrefix);
    mqtt.publish(lmTopic, "", true);
    Serial.println("[MQTT] lastmsg cleared");
}

void clearSensorBtnText() {
    if (!snsBtnPendingClear || !mqttConnected) return;
    if ((int32_t)(millis() - snsBtnClearAt) < 0) return;
    snsBtnPendingClear = false;
    if (snsBtnSlug[0] == 0) return;
    char tText[128];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, snsBtnSlug);
    mqtt.publish(tText, "", false);
    Serial.printf("[SNS] button text cleared for %s\n", snsBtnSlug);
    snsBtnSlug[0] = 0;
}

void mqttSlug(const char* name, char* out, int maxLen) {
    if (maxLen <= 0) return;
    int n = 0;
    bool lossy = false;
    for (int i = 0; name[i] && n < maxLen - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[n++] = c;
        else { out[n++] = '_'; lossy = true; }
    }
    out[n] = 0;
    // Кириллица (и любой другой не-ASCII) превращалась в вереницу подчёркиваний, поэтому
    // два РАЗНЫХ имени из одинакового числа байт давали один и тот же slug — сущности
    // таких узлов в Home Assistant затирали друг друга. Добавляем к slug хвост от хэша
    // исходного имени. Для чисто латинских имён, где ничего не потерялось, slug остаётся
    // прежним: уже созданные в HA сущности не переименовываются.
    if (!lossy) return;
    uint16_t h = crc16buf((const uint8_t*)name, strlen(name));
    int room = maxLen - 1 - 5;            // место под "_xxxx" и завершающий ноль
    if (room < 0) return;
    if (n > room) n = room;
    snprintf(out + n, maxLen - n, "_%04x", h);
}

// Роль узла для карточки в Home Assistant. Берётся из имени окружения сборки, которое
// узел сообщает в hello: сенсор и компаньон живут на одной плате, и различить их иначе
// нельзя. Пусто — прошивка узла старая, окружение она не передаёт; такие узлы у нас
// всегда были сенсорами, поэтому это и значение по умолчанию.
static const char* roleFromEnv(const String& env) {
    if (env.endsWith("companion"))   return "Companion";
    if (env.endsWith("coordinator")) return "Coordinator";
    return "Sensor";
}

// Похоже ли поле на координату. Текст пришёл из эфира и уедет прямо в JSON-атрибуты
// Home Assistant, поэтому проверка тут не формальность: кавычка или скобка в «координате»
// сломала бы разбор, а буквы молча превратили бы точку на карте в мусор. Формат ровно
// тот, что шлёт узел, — необязательный минус, цифры, одна точка.
static bool isCoordText(const String& v) {
    int len = v.length();
    if (len < 3 || len > 12) return false;
    int i = 0, dots = 0, digits = 0;
    if (v[0] == '-') i = 1;
    for (; i < len; i++) {
        char c = v[i];
        if (c == '.') { if (++dots > 1) return false; }
        else if (c >= '0' && c <= '9') digits++;
        else return false;
    }
    return dots == 1 && digits >= 2;
}

// Точка узла на карте. Отдельной функцией, а не строкой в publishSensorDisc: сущность
// заводится не при появлении узла, а при первых пришедших координатах.
static void publishSensorPosDisc(const String& sender, const char* slug, const String& env) {
    const char* role = roleFromEnv(env);
    char senderEsc[64];
    jsonEscape(sender.c_str(), senderEsc, sizeof(senderEsc));
    char devBlock[192];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_sensor_%s\"],\"name\":\"MeshBot %s %s\","
        "\"manufacturer\":\"MeshCore\",\"model\":\"%s node\"",
        slug, role, slug, role);
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic), "homeassistant/device_tracker/meshcore_sensor_%s/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s position\",\"json_attributes_topic\":\"%s/sensor/%s/position\","
        "\"source_type\":\"gps\",\"icon\":\"mdi:map-marker\","
        "\"unique_id\":\"meshcore_sensor_%s_position\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);
    Serial.printf("[MQTT] карта: %s\n", slug);
}

void publishSensorDisc(const String& sender, const char* slug, const String& env) {
    const char* role = roleFromEnv(env);
    // Имя узла вставляется в discovery-конфиг дословно. Имя может содержать кавычки,
    // обратные слэши и управляющие символы — без экранирования Home Assistant сломает
    // разбор JSON-конфига, и датчик просто не появится в интерфейсе.
    char senderEsc[64];
    jsonEscape(sender.c_str(), senderEsc, sizeof(senderEsc));
    char devBlock[192];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_sensor_%s\"],\"name\":\"MeshBot %s %s\","
        "\"manufacturer\":\"MeshCore\",\"model\":\"%s node\"",
        slug, role, slug, role);
    // Один буфер на все сущности: функция вызывается глубоко из разбора пакета, стек не бесконечен
    char topic[128], payload[512];

    // Данные: sensor с текстовым state (HA MQTT text требует command_topic,
    // а у нас сущность read-only — это state от сенсора).
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/text/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s data\",\"state_topic\":\"%s/sensor/%s/text\","
        "\"icon\":\"mdi:sprout\",\"unique_id\":\"meshcore_sensor_%s_text\","
        "\"entity_category\":\"diagnostic\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/rssi/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s RSSI\",\"state_topic\":\"%s/sensor/%s/rssi\","
        "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
        "\"unique_id\":\"meshcore_sensor_%s_rssi\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // Availability: бинарник device_class=connectivity, «online» пока датчик шлёт.
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/meshcore_sensor_%s/available/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s available\",\"state_topic\":\"%s/sensor/%s/available\","
        "\"payload_on\":\"online\",\"payload_off\":\"offline\",\"device_class\":\"connectivity\","
        "\"unique_id\":\"meshcore_sensor_%s_available\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/version/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s firmware\",\"state_topic\":\"%s/sensor/%s/version\","
        "\"icon\":\"mdi:chip\",\"entity_category\":\"diagnostic\","
        "\"unique_id\":\"meshcore_sensor_%s_version\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // Заряд и напряжение батареи сенсора — приходят в hello
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/battery/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s battery\",\"state_topic\":\"%s/sensor/%s/battery\","
        "\"device_class\":\"battery\",\"unit_of_measurement\":\"%%\",\"state_class\":\"measurement\","
        "\"unique_id\":\"meshcore_sensor_%s_battery\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/board/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s board\",\"state_topic\":\"%s/sensor/%s/board\","
        "\"icon\":\"mdi:developer-board\",\"entity_category\":\"diagnostic\","
        "\"unique_id\":\"meshcore_sensor_%s_board\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/voltage/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s voltage\",\"state_topic\":\"%s/sensor/%s/voltage\","
        "\"device_class\":\"voltage\",\"unit_of_measurement\":\"V\",\"state_class\":\"measurement\","
        "\"entity_category\":\"diagnostic\","
        "\"unique_id\":\"meshcore_sensor_%s_voltage\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    // Event entity: MQTT event platform — появляется как device-trigger "Fired"
    // в автоматизациях HA. При нажатии кнопки сенсор шлёт "button", бот
    // публикует его в event_type_topic, HA генерирует событие.
    snprintf(topic, sizeof(topic), "homeassistant/event/meshcore_sensor_%s/button/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Button\","
        "\"state_topic\":\"%s/sensor/%s/button\","
        "\"event_types\":[\"button\",\"button2\"],"
        "\"unique_id\":\"meshcore_sensor_%s_button\","
        "\"device\":{%s}}",
        senderEsc, mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);

    Serial.printf("[MQTT] sensor discovery published: %s\n", slug);
}

// Реестр сенсоров ведётся независимо от MQTT — его показывает страница OTA
static int sensorIndex(const String& name) {
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        if (sensorDeviceDisc[i] == name) return i;
    }
    if (sensorDeviceDiscCount >= SENSOR_DEV_CACHE_MAX) return -1;
    sensorDeviceDisc[sensorDeviceDiscCount] = name;
    sensorDiscPublished[sensorDeviceDiscCount] = false;
    sensorPosPublished[sensorDeviceDiscCount] = false;
    sensorBattery[sensorDeviceDiscCount] = -1;
    sensorHops[sensorDeviceDiscCount] = 0xFF;    // пока не услышали — не «напрямую», а «неизвестно»
    return sensorDeviceDiscCount++;
}

void publishSensorAvailability(int idx) {
    if (!mqttConnected) return;
    char slug[48], topic[128];
    mqttSlug(sensorDeviceDisc[idx].c_str(), slug, sizeof(slug));
    snprintf(topic, sizeof(topic), "%s/sensor/%s/available", mqttPrefix, slug);
    mqtt.publish(topic, sensorOnlineNow[idx] ? "online" : "offline", true);
}

bool publishSensorMessage() {
    int idx = sensorIndex(lastSender);
    bool cameOnline = false;
    if (idx >= 0) {
        sensorLastActive[idx] = millis();
        sensorRssi[idx] = lastRSSI;
        sensorHops[idx] = lastHopCount;      // сколько ретрансляторов прошёл этот пакет
        cameOnline = !sensorOnlineNow[idx];
        sensorOnlineNow[idx] = true;
    }
    // heartbeat "hello:<версия>[:<заряд %>:<напряжение>]"; сенсоры постарше шлют просто "hello"
    bool hello = lastMessage == SENSOR_MSG_HELLO || lastMessage.startsWith(SENSOR_MSG_HELLO ":");
    // hello:<версия>:<заряд %>:<напряжение>:<окружение>[:<широта>:<долгота>]
    // "-" = поля нет. Полей может быть меньше: координаты шлют только узлы с приёмником
    // и только когда решение есть, — цикл сам остановится на конце строки.
    //
    // Кода платы в heartbeat больше нет: окружение и так называет плату, причём точнее
    // (сенсор и компаньон живут на одной h43). Узел со старой прошивкой шлёт его пятым
    // полем, и здесь оно прочтётся как окружение — такой узел нужно обновить один раз
    // вручную или по USB, дальше он снова понятен.
    String ver, batPct, batVolt, envName, lat, lon;
    if (hello) {
        String rest = lastMessage.substring(strlen(SENSOR_MSG_HELLO) + 1);
        String* fields[] = { &ver, &batPct, &batVolt, &envName, &lat, &lon };
        for (int i = 0; i < 6 && rest.length() > 0; i++) {
            int p = rest.indexOf(':');
            *fields[i] = (p < 0) ? rest : rest.substring(0, p);
            rest = (p < 0) ? String() : rest.substring(p + 1);
            if (*fields[i] == "-") *fields[i] = "";
        }
    }
    if (idx >= 0 && ver.length() > 0) sensorFwVersion[idx] = ver;
    if (idx >= 0 && envName.length() > 0) sensorEnv[idx] = envName;
    if (idx >= 0 && batPct.length() > 0) sensorBattery[idx] = batPct.toInt();

    if (!mqttConnected) {
        Serial.printf("[SNS] %s: %s (MQTT not connected, skipped)\n",
                      lastSender.c_str(), lastMessage.c_str());
        return false;
    }
    char slug[48];
    mqttSlug(lastSender.c_str(), slug, sizeof(slug));
    // discovery — один раз на сенсор за подключение к брокеру
    if (idx < 0) {
        // Реестр полон: публиковать discovery некуда — иначе каждое сообщение такого
        // сенсора заново рассылало бы весь набор retained-конфигов в брокер.
        static bool regFullWarned = false;
        if (!regFullWarned) {
            regFullWarned = true;
            slog("[SNS] реестр сенсоров полон (%d) — %s остаётся без сущностей в HA\n",
                 SENSOR_DEV_CACHE_MAX, lastSender.c_str());
        }
    } else if (!sensorDiscPublished[idx]) {
        // Окружение разобрано строкой выше, поэтому карточка сразу получает верную роль
        publishSensorDisc(lastSender, slug, sensorEnv[idx]);
        sensorDiscPublished[idx] = true;
        cameOnline = true;
    }
    if (cameOnline) {
        publishSensorAvailability(idx);
        Serial.printf("[SNS] %s AVAILABLE (online)\n", lastSender.c_str());
    }

    if (hello) {
        char t[128];
        if (ver.length() > 0) {
            snprintf(t, sizeof(t), "%s/sensor/%s/version", mqttPrefix, slug);
            mqtt.publish(t, ver.c_str(), true);
        }
        if (batPct.length() > 0) {
            snprintf(t, sizeof(t), "%s/sensor/%s/battery", mqttPrefix, slug);
            mqtt.publish(t, batPct.c_str(), true);
        }
        if (batVolt.length() > 0) {
            snprintf(t, sizeof(t), "%s/sensor/%s/voltage", mqttPrefix, slug);
            mqtt.publish(t, batVolt.c_str(), true);
        }
        if (envName.length() > 0) {
            // В поле «плата» уходит имя окружения сборки: в нём есть и плата, и тип
            // прошивки, тогда как короткий код («h43») одинаков у сенсора и компаньона.
            snprintf(t, sizeof(t), "%s/sensor/%s/board", mqttPrefix, slug);
            mqtt.publish(t, envName.c_str(), true);
        }
        // Координаты. Приходят из эфира, поэтому перед тем как вклеить их в JSON,
        // проверяем, что это действительно числа: кадр мог прийти битым или подделанным,
        // а кавычка в «координате» сломала бы разбор атрибутов в Home Assistant.
        if (idx >= 0 && isCoordText(lat) && isCoordText(lon)) {
            snprintf(t, sizeof(t), "%s/sensor/%s/latitude", mqttPrefix, slug);
            mqtt.publish(t, lat.c_str(), true);
            snprintf(t, sizeof(t), "%s/sensor/%s/longitude", mqttPrefix, slug);
            mqtt.publish(t, lon.c_str(), true);
            // Точка на карте: Home Assistant берёт широту и долготу из атрибутов
            // device_tracker и сам определяет зону.
            char pos[96];
            snprintf(pos, sizeof(pos), "{\"latitude\":%s,\"longitude\":%s}",
                     lat.c_str(), lon.c_str());
            snprintf(t, sizeof(t), "%s/sensor/%s/position", mqttPrefix, slug);
            mqtt.publish(t, pos, true);
            // Сущность карты заводится при ПЕРВЫХ координатах, а не вместе с остальными:
            // у узла без приёмника её быть не должно — она висела бы «неизвестно» вечно.
            if (!sensorPosPublished[idx]) {
                publishSensorPosDisc(lastSender, slug, sensorEnv[idx]);
                sensorPosPublished[idx] = true;
            }
        }
        Serial.printf("[SNS] heartbeat from %s: v%s, батарея %s%%%s%s\n", lastSender.c_str(),
                      ver.length() ? ver.c_str() : "?",
                      batPct.length() ? batPct.c_str() : "?",
                      lat.length() ? ", координаты " : "",
                      lat.length() ? lat.c_str() : "");
        return true;
    }
    // --- данные: text + rssi ---
    char tText[128], tRssi[128], rssiStr[24];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, slug);
    snprintf(tRssi, sizeof(tRssi), "%s/sensor/%s/rssi", mqttPrefix, slug);
    fmtFix(lastRSSI, 1, rssiStr, sizeof(rssiStr));
    mqtt.publish(tText, lastMessage.c_str());
    mqtt.publish(tRssi, rssiStr);
    // --- button: event entity trigger для автоматизаций ---
    if ((lastMessage == SENSOR_MSG_BUTTON) || (lastMessage == SENSOR_MSG_BUTTON2)) {
        char tEvt[128];
        snprintf(tEvt, sizeof(tEvt), "%s/sensor/%s/button", mqttPrefix, slug);
        // MQTT event entity ожидает JSON с "event_type" (docs event.mqtt),
        // ретранслированные retained-сообщения отбрасываются.
        char evtJson[64];
        snprintf(evtJson, sizeof(evtJson), "{\"event_type\":\"%s\"}", lastMessage.c_str());
        mqtt.publish(tEvt, evtJson, false);
        // auto-clear: через SNS_BTN_CLEAR_MS текст сбрасывается "",
        // чтобы следующее нажатие снова вызвало "state_changed" в HA.
        snsBtnClearAt = millis() + SNS_BTN_CLEAR_MS;
        snsBtnPendingClear = true;
        strlcpy(snsBtnSlug, slug, sizeof(snsBtnSlug));
        Serial.printf("[SNS] %s from %s — trigger published\n", lastMessage.c_str(), lastSender.c_str());
    } else {
        Serial.printf("[SNS] %s: %s (rssi %s)\n", lastSender.c_str(), lastMessage.c_str(), rssiStr);
    }
    return true;
}

void publishStatus() {
    if (!mqttConnected) return;
    // Экранируем кавычки в именах/сообщениях для JSON
    char prvEsc[64];
    jsonEscape(privateChannelName.c_str(), prvEsc, sizeof(prvEsc));
    char chEsc[64];
    const char* chName = (mqttTxChannel >= 0 && mqttTxChannel < numChannels)
                         ? channels[mqttTxChannel].name : "?";
    jsonEscape(chName, chEsc, sizeof(chEsc));
    char topic[96], payload[384];
    unsigned long upSec = millis() / 1000;
    char tempS[12];
    fmtFix(cpuTempC(), 1, tempS, sizeof(tempS));   // без float-printf
    snprintf(topic, sizeof(topic), "%s/status", mqttPrefix);
    snprintf(payload, sizeof(payload),
        "{\"version\":\"" FW_VERSION "\",\"board\":\"" BOARD_CODE "\",\"wifi\":true,\"mqtt\":true,\"lora_rx\":%s,"
        "\"uptime\":%lu,\"packets\":%d,\"duplicates\":%lu,"
        "\"temp\":%s,\"ip\":\"%s\","
        "\"channel\":\"%s\",\"private\":\"%s\"}",
        isListening ? "true" : "false",
        upSec, packetCount, duplicateCount,
        tempS,
        wifiConnected ? WiFi.localIP().toString().c_str() : "0.0.0.0",
        chEsc, prvEsc);
    mqtt.publish(topic, payload, true);

    // Listening state
    char lTopic[96];
    snprintf(lTopic, sizeof(lTopic), "%s/lstate", mqttPrefix);
    mqtt.publish(lTopic, isListening ? "ON" : "OFF", true);
}

void setupMQTT() {
    // Префикс — часть имени топика, поэтому имя приводим к slug: '/', '+' и '#' в именах
    // топиков MQTT значат совсем другое. У латинских имён slug совпадает с именем, так
    // что уже настроенные в Home Assistant сущности от этого не меняются.
    char slug[48];
    mqttSlug(cfg.name.c_str(), slug, sizeof(slug));
    snprintf(mqttPrefix, sizeof(mqttPrefix), "meshcore/bot/%s", slug);
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(768);   // discovery-конфиг весит до ~600 Б
    mqtt.setSocketTimeout(3);  // ограничиваем блокировку connect() до ~3 с
}

void tickRetryConnections() {
    // без настроек подключаться некуда: устройство ждёт настройки в консоли
    if (cfg.wifiSsid.length() == 0) return;
    // ===== WiFi =====
    bool connectedNow = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected && !connectedNow) {
        // обрыв — сбрасываем флаги, дальше переподключаемся
        wifiConnected = false;
        mqttConnected = false;
    }
    if (!connectedNow) {
        if (!wifiConnInProgress) {
            wifiConnInProgress = true;
            wifiConnStartMs = millis();
            Serial.println("[WiFi] connecting...");
            WiFi.disconnect();
            WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
        } else if ((int32_t)(millis() - wifiConnStartMs) > 15000) {
            Serial.println("[WiFi] timeout, retry in 5s");
            wifiConnInProgress = false;
        }
        return;   // без WiFi MQTT не трогаем
    }
    if (!wifiConnected) {
        wifiConnected = true;
        wifiConnInProgress = false;
        Serial.printf("[WiFi] connected (%s)\n", WiFi.localIP().toString().c_str());
        // SNTP сразу при появлении WiFi (не ждём MQTT): часы уточняются с
        // сервера времени, build-time устаревает уже через пару дней.
        if (!ntpStarted) {
            ntpStarted = true;
            lastNtpSyncMs = millis();
            configTime(0, 0, "pool.ntp.org");   // синхронизация в UTC
        }
    }

    // ===== MQTT =====
    if (cfg.mqttHost.length() == 0) return;
    if (mqttConnected && mqtt.connected()) return;
    if (mqttConnected) mqttConnected = false;

    Serial.printf("[MQTT] connecting to %s:%d ...", cfg.mqttHost.c_str(), cfg.mqttPort);
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "meshcore_%s_%lu", cfg.name.c_str(), millis() % 100000);

    const char* mqUser = cfg.mqttUser.length() ? cfg.mqttUser.c_str() : NULL;
    const char* mqPass = cfg.mqttPass.length() ? cfg.mqttPass.c_str() : NULL;
    if (mqtt.connect(clientId, mqUser, mqPass)) {
        mqttConnected = true;
        Serial.println(" OK");
        char cmdTopic[96];
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/send", mqttPrefix);
        mqtt.subscribe(cmdTopic);
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/listening", mqttPrefix);
        mqtt.subscribe(cmdTopic);

        // Discovery бота публикуем только при ПЕРВОМ подключении (reconnect его
        // повторяет поток retained-конфигов). Обновления — по факту изменений.
        if (!discoveryPublished) publishDiscovery();
        publishStatus();
        // Брокер мог потерять retained-состояние, пока нас не было: discovery сенсоров
        // уйдёт с их следующим сообщением, availability — сразу
        for (int i = 0; i < sensorDeviceDiscCount; i++) {
            sensorDiscPublished[i] = false;
            sensorPosPublished[i] = false;
            publishSensorAvailability(i);
        }
    } else {
        mqttConnected = false;
        Serial.printf(" FAILED (rc=%d)\n", mqtt.state());
    }
}

#endif // MQTT_ENABLED
