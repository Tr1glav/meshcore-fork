#pragma once

#include "config.h"

// ===== СОСТОЯНИЕ БРОКЕРА И HOME ASSISTANT =====
// Сеть и время жили здесь же (и до того — в globals.h ядра). Теперь они в net.h: сеть нужна
// и тому, у кого брокера нет вовсе, и держать их одним признаком было неправильно.
//
// Guard файла пока MQTT_ENABLED, а не FEATURE_MQTT: в mqtt.cpp вместе с публикацией лежит
// РЕЕСТР УЗЛОВ (sensorIndex, publishSensorMessage), а он нужен и прошивальщику с
// FEATURE_MQTT=0 — по его /sensors координатор выбирает ведущего сессии. Вынести реестр —
// следующий шаг, и только он позволит убрать этот флаг совсем.
#ifdef MQTT_ENABLED
#include <WiFi.h>
#include <PubSubClient.h>

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
extern unsigned long lastSensorTimeSyncMs;   // рассылка времени узлам

// Шаг подключения к брокеру. Зовётся из netTick, когда сеть уже поднята: без сети брокеру
// делать нечего, и порядок этих двух шагов не должен зависеть от того, кто их вызывает.
void mqttConnectTick();
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
