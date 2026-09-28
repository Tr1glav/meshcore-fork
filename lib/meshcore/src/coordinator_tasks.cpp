#include "config.h"
#include "globals.h"
#include "mesh.h"
#include "ota.h"
#include "mqtt.h"
#include "fwupdate.h"
#include "display.h"
#include <esp_sntp.h>

// ===== Периодические задачи координатора =====
// Выделено из главного цикла: публикация состояния в MQTT, слежение за доступностью
// узлов, пересинхронизация часов и рассылка времени в сеть, проверка новых версий.
// Всё это делается по расписанию и ничего не ждёт, поэтому живёт отдельно от приёма
// радио и от обслуживания страницы.

#if FEATURE_MQTT || FEATURE_AUTOUPDATE || FEATURE_SELFUPDATE || FEATURE_NTP

void coordinatorTasksTick() {
    // ===== MQTT STATUS (раз в 60 сек) =====
    if (mqttConnected && millis() - lastStatusPublishMs > MQTT_STATUS_INTERVAL_MS) {
        lastStatusPublishMs = millis();
        publishStatus();
    }
    // ===== ДОСТУПНОСТЬ ДАТЧИКОВ: если от датчика давно ничего нет — offline =====
    if (millis() - lastSensorAvailCheckMs > 10000) {
        lastSensorAvailCheckMs = millis();
        for (int i = 0; i < sensorDeviceDiscCount; i++) {
            if (sensorOnlineNow[i] && millis() - sensorLastActive[i] > SENSOR_OFFLINE_MS) {
                sensorOnlineNow[i] = false;
                publishSensorAvailability(i);
                Serial.printf("[SNS] %s OFFLINE (no data for %lus)\n",
                              sensorDeviceDisc[i].c_str(), (unsigned long)(SENSOR_OFFLINE_MS / 1000));
            }
        }
    }
    // ===== Ре-синк NTP раз в час (configTime снова делает stop+init) =====
    if (ntpStarted && wifiConnected && (int32_t)(millis() - lastNtpSyncMs) >= (int32_t)NTP_RESYNC_INTERVAL_MS) {
        lastNtpSyncMs = millis();
        configTime(0, 0, "pool.ntp.org");
    }
    // ===== Первая успешная синхронизация SNTP: лог + сразу рассылка времени сенсорам =====
    // Отслеживаем статус lwIP SNTP (SNTP_SYNC_STATUS_COMPLETED), а не сдвиг часов:
    // при свежем билде реальное время может лишь на минуты отличаться от build-time.
    if (ntpStarted && !ntpSyncedLogged &&
        sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ntpSyncedLogged = true;
        time_t local = time(NULL) + (time_t)cfg.tzOffset * 3600;
        struct tm tm_now;
        gmtime_r(&local, &tm_now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S %d.%m.%Y", &tm_now);
        Serial.printf("[RTC] NTP time synced: %s\n", tbuf);
        if (sensorChannelIdx >= 0 && !otaSessionActive()) {
            lastSensorTimeSyncMs = millis();
            sendSensorTimeSync();
        }
    }
    // ===== Рассылка актуального времени сенсорам в сенсорный канал =====
    // Работает только при реально синхронизированном времени (ntpSyncedLogged):
    // build-time устаревает, и датчики должны получать истинный epoch.
    // во время mesh OTA любой TX бота бьёт ответы сенсора; рассылка догонит после сессии
    if (ntpSyncedLogged && sensorChannelIdx >= 0 && !otaSessionActive() &&
        (int32_t)(millis() - lastSensorTimeSyncMs) >= (int32_t)SENSOR_TIME_SYNC_INTERVAL_MS) {
        lastSensorTimeSyncMs = millis();
        sendSensorTimeSync();
    }
    fwUpdateTick();   // новые версии из релизов GitHub
    // ===== «Вторые уши»: объявляем адрес, куда слать услышанное по радио =====
    // Слушает узел-прошивальщик (разбор SENSOR_MSG_COORD в ядре): слыша узлы, до которых
    // мы по радио не добираемся, он шлёт их кадры сюда, на POST /ears. Как с "support:",
    // объявление отдельным сообщением — платить байтами за него в каждом heartbeat некому.
    // Период (COORD_ANNOUNCE_MS в config.h) держит компромисс: перезагрузившийся
    // координатор прошивальщик должен найти за десятки секунд, но и эфир мусором забивать
    // незачем. Отсюда же и условие: объявляемся, только пока прошивальщик в сети. Слушать
    // это объявление больше некому, и в сети без прошивальщика оно было бы лишним кадром в
    // сенсорном канале каждые 45 секунд — в тринадцать раз чаще, чем heartbeat самих узлов.
    // Перезагрузился координатор — прошивальщик напомнит о себе своим heartbeat, и с этого
    // момента объявления пойдут (первое — сразу, lastCoordAnnounceMs ещё нулевой).
    #if FEATURE_WEB && !defined(SENSOR_NODE)
    static unsigned long lastCoordAnnounceMs = 0;
    if (mcWifiConnected() && sensorChannelIdx >= 0 && !otaSessionActive() && supportPresent() &&
        (lastCoordAnnounceMs == 0 || millis() - lastCoordAnnounceMs >= COORD_ANNOUNCE_MS)) {
        lastCoordAnnounceMs = millis();
        char msg[32];
        snprintf(msg, sizeof(msg), "%s%s", SENSOR_MSG_COORD, mcLocalIp().c_str());
        sensorSendMsg(msg);
    }
    #endif
    #if FEATURE_WEB
    webTick();        // очередь настроек узла: по одному сообщению раз в CFG_MSG_GAP_MS
    #endif

    // ===== Сброс lastmsg после паузы (чтобы повторный одинаковый текст триггерил HA) =====
    clearLastMsg();
    // ===== Сброс text-топика после триггера "button" (повторное нажатие = новый state_changed) =====
    clearSensorBtnText();
}

#endif
