#pragma once

#include "config.h"

// ===== СЕТЕВОЙ СЛОЙ =====
// Подключение к WiFi и запуск синхронизации времени. Отдельно от брокера намеренно: сеть
// нужна и тому, у кого MQTT нет вовсе — узлу-прошивальщику (страница, приём образа по сети,
// «вторые уши»), — а раньше всё это жило в mqtt.cpp под флагом MQTT_ENABLED, то есть «есть
// брокер» и «есть сеть» были одним и тем же признаком.

#if FEATURE_WIFI
#include <WiFi.h>

extern WiFiClient wifiClient;
extern bool wifiConnected;
extern bool wifiConnInProgress;        // неблокирующая машина состояния соединения из loop
extern unsigned long wifiConnStartMs;
extern unsigned long lastNetRetryMs;   // когда в последний раз заходили в netTick

// Шаг машины состояния: поднять WiFi, если он лежит, и отдать ход брокеру, если он собран.
// Зовётся из главного цикла не чаще раза в MQTT_RECONNECT_INTERVAL_MS.
void netTick();
#endif

#if FEATURE_NTP
extern bool ntpStarted;                // SNTP запущен
extern bool ntpSyncedLogged;           // факт синхронизации пишем в журнал один раз
extern unsigned long lastNtpSyncMs;
#endif
