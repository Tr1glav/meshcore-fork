#pragma once

#include "config.h"

// ===== СОСТОЯНИЕ СЕТИ, БРОКЕРА, ВРЕМЕНИ И HOME ASSISTANT =====
// Раньше всё это лежало в globals.h ядра — то есть библиотека протокола носила в себе
// WiFi-клиент, PubSubClient и флаги Home Assistant, не используя их ни в одной строке. Место
// им здесь: этим живёт прошивка координатора, и только она.
//
// Guard пока прежний, MQTT_ENABLED: под ним же собираются mqtt.cpp и coordinator_tasks.cpp, и
// они обращаются к этим переменным даже при FEATURE_MQTT=0 (у прошивальщика брокера нет, но
// код тот же и просто ничего не публикует в рантайме). Разложить сам код по признакам —
// следующий шаг; переносить старый флаг в ядро ради этого не нужно.
#ifdef MQTT_ENABLED
#include <WiFi.h>
#include <PubSubClient.h>

extern WiFiClient wifiClient;
extern bool wifiConnected;
extern bool wifiConnInProgress;     // неблокирующая машина состояния соединения из loop
extern unsigned long wifiConnStartMs;
extern unsigned long lastMqttReconnectMs;

extern PubSubClient mqtt;
extern char mqttPrefix[64];         // префикс топиков: meshcore/bot/<имя узла>/
extern bool mqttConnected;
extern unsigned long lastStatusPublishMs;
extern bool discoveryPublished;     // discovery бота публикуется один раз на подключение
// Обнуление lastmsg после публикации: повторный одинаковый текст должен снова стать
// триггером в Home Assistant.
extern unsigned long lastmsgClearAt;
extern bool lastmsgPendingClear;
// Тот же приём для триггера "button": через SNS_BTN_CLEAR_MS обнуляем text, иначе повторное
// нажатие не даёт "state_changed".
extern unsigned long snsBtnClearAt;
extern bool snsBtnPendingClear;
extern char snsBtnSlug[48];
extern unsigned long lastSensorAvailCheckMs;

extern bool ntpStarted;             // SNTP запущен
extern bool ntpSyncedLogged;        // факт синхронизации пишем в журнал один раз
extern unsigned long lastNtpSyncMs;
extern unsigned long lastSensorTimeSyncMs;   // рассылка времени узлам
#endif


#ifdef MQTT_ENABLED
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishDiscovery();
void publishMessage();
void clearLastMsg();
void clearSensorBtnText();
void mqttSlug(const char* name, char* out, int maxLen);
// env — имя окружения сборки узла: по нему карточка в HA получает роль в названии
void publishSensorDisc(const String& sender, const char* slug, const String& env);
void publishSensorAvailability(int idx);
bool publishSensorMessage();
void publishStatus();
void setupMQTT();
void tickRetryConnections();
#endif
