#include "config.h"
#include "globals.h"
#include "net.h"
#include "mqtt.h"     // mqttConnectTick: ход брокера идёт следом за поднятой сетью
#include "ota.h"      // slog

#if FEATURE_WIFI

WiFiClient wifiClient;
bool wifiConnected = false;
bool wifiConnInProgress = false;
unsigned long wifiConnStartMs = 0;
unsigned long lastNetRetryMs = 0;

#if FEATURE_NTP
bool ntpStarted = false;
bool ntpSyncedLogged = false;
unsigned long lastNtpSyncMs = 0;
#endif

// Сколько ждём само подключение, прежде чем начать заново.
#define WIFI_CONNECT_TIMEOUT_MS 15000

void netTick() {
    // без настроек подключаться некуда: устройство ждёт настройки в консоли
    if (cfg.wifiSsid.length() == 0) return;

    bool connectedNow = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected && !connectedNow) {
        // обрыв — сбрасываем флаги, дальше переподключаемся
        wifiConnected = false;
#if FEATURE_MQTT
        mqttConnected = false;
#endif
    }
    if (!connectedNow) {
        if (!wifiConnInProgress) {
            wifiConnInProgress = true;
            wifiConnStartMs = millis();
            Serial.println("[WiFi] connecting...");
            WiFi.disconnect();
            WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
        } else if ((int32_t)(millis() - wifiConnStartMs) > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("[WiFi] timeout, retry in 5s");
            wifiConnInProgress = false;
        }
        return;   // без сети дальше идти некуда
    }
    if (!wifiConnected) {
        wifiConnected = true;
        wifiConnInProgress = false;
        Serial.printf("[WiFi] connected (%s)\n", WiFi.localIP().toString().c_str());
#if FEATURE_NTP
        // SNTP сразу при появлении WiFi (не ждём брокера): часы уточняются с сервера
        // времени, build-time устаревает уже через пару дней.
        if (!ntpStarted) {
            ntpStarted = true;
            lastNtpSyncMs = millis();
            configTime(0, 0, "pool.ntp.org");   // синхронизация в UTC
        }
#endif
    }

#if FEATURE_MQTT
    mqttConnectTick();   // сеть есть — теперь брокер
#endif
}

#endif // FEATURE_WIFI
