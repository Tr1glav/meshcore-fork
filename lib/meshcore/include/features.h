#pragma once

// ===== ПРИЗНАКИ СБОРКИ =====
// Роль платы («координатор», «сенсор», «компаньон») — это не одно свойство, а набор
// независимых возможностей. Раньше код ветвился прямо по ролям (MQTT_ENABLED,
// SENSOR_NODE, COMPANION_NODE), и любое сочетание вне трёх заготовленных собрать было
// нельзя: например, координатор без веб-страницы или сенсор без экрана.
//
// Здесь роли превращаются в отдельные признаки. Каждый можно задать в platformio.ini
// явно (-DFEATURE_WEB=0 и подобное), а если он не задан — он выводится из роли, как было
// раньше. Поэтому переход ничего не ломает: старые окружения собираются ровно так же.
//
// Проверять признаки нужно через #if, а не #ifdef: они всегда определены, но могут быть
// равны нулю.

// --- сеть и её потребители ---
#ifndef FEATURE_WIFI
  #ifdef MQTT_ENABLED
    #define FEATURE_WIFI 1
  #else
    #define FEATURE_WIFI 0
  #endif
#endif

#ifndef FEATURE_MQTT          // публикация в MQTT и автообнаружение в Home Assistant
  #define FEATURE_MQTT FEATURE_WIFI
#endif

#ifndef FEATURE_WEB           // страница OTA на порту 3232
  #define FEATURE_WEB FEATURE_WIFI
#endif

#ifndef FEATURE_AUTOUPDATE    // проверка релизов GitHub и раздача прошивок узлам
  #define FEATURE_AUTOUPDATE FEATURE_WIFI
#endif

// Обновить СЕБЯ из релиза — не то же самое, что раздавать прошивки узлам. Узел-прошивальщик
// раздаёт по радио, а себя качает по сети: у него есть WiFi, и занимать эфир на сорок
// секунд ради собственного образа незачем. Поэтому признак отдельный.
#ifndef FEATURE_SELFUPDATE
  #define FEATURE_SELFUPDATE FEATURE_AUTOUPDATE
#endif

#ifndef FEATURE_NTP           // синхронизация часов и рассылка времени узлам
  #define FEATURE_NTP FEATURE_WIFI
#endif

// --- прошивка по радио ---
#ifndef FEATURE_MESH_OTA_SENDER    // сторона, раздающая образ (координатор)
  #define FEATURE_MESH_OTA_SENDER FEATURE_WIFI
#endif

#ifndef FEATURE_MESH_OTA_RECEIVER  // сторона, принимающая образ (узел)
  #ifdef SENSOR_NODE
    #define FEATURE_MESH_OTA_RECEIVER 1
  #else
    #define FEATURE_MESH_OTA_RECEIVER 0
  #endif
#endif

// Узел-прошивальщик: берёт на себя сессии OTA вместо координатора. Он стоит там, где
// слышно узлы, до которых координатору не дотянуться напрямую, — а прошивать можно только
// напрямую (сырые кадры быстрого канала ретрансляторы не переносят). Образ он получает от
// координатора по сети, поэтому ему нужны WiFi и своя страница, но не нужны ни MQTT, ни
// проверка релизов: решает по-прежнему координатор.
#ifndef FEATURE_SUPPORT
  #ifdef SUPPORT_NODE
    #define FEATURE_SUPPORT 1
  #else
    #define FEATURE_SUPPORT 0
  #endif
#endif

// --- поведение узла ---
#ifndef FEATURE_SENSOR        // heartbeat, кнопка, проверка связи, настройка по радио
  #ifdef SENSOR_NODE
    #define FEATURE_SENSOR 1
  #else
    #define FEATURE_SENSOR 0
  #endif
#endif

#ifndef FEATURE_COMPANION     // BLE и протокол телефонного приложения
  #ifdef COMPANION_NODE
    #define FEATURE_COMPANION 1
  #else
    #define FEATURE_COMPANION 0
  #endif
#endif

// --- железо ---
// Кнопка есть не у каждой платы: где PIN_USER_BTN не задан, признак должен быть выключен,
// иначе buttonTick каждый проход цикла опрашивал бы пин -1 (digitalRead отдаёт на нём LOW,
// то есть «кнопка нажата навсегда»).
#ifndef FEATURE_BUTTON
  #if BUTTON_PIN >= 0
    #define FEATURE_BUTTON FEATURE_SENSOR
  #else
    #define FEATURE_BUTTON 0
  #endif
#endif

// Проверки сочетаний: молча собрать бессмысленную прошивку хуже, чем не собрать вовсе.
#if FEATURE_MQTT && !FEATURE_WIFI
  #error "FEATURE_MQTT требует FEATURE_WIFI"
#endif
#if FEATURE_WEB && !FEATURE_WIFI
  #error "FEATURE_WEB требует FEATURE_WIFI"
#endif
#if FEATURE_AUTOUPDATE && !FEATURE_WIFI
  #error "FEATURE_AUTOUPDATE требует FEATURE_WIFI"
#endif
#if FEATURE_SELFUPDATE && !FEATURE_WIFI
  #error "FEATURE_SELFUPDATE требует FEATURE_WIFI: образ качается из релиза по сети"
#endif
#if FEATURE_COMPANION && !FEATURE_SENSOR
  #error "Компаньон собирается поверх сенсорного узла: нужен FEATURE_SENSOR"
#endif
// Код признаков узла лежит под старыми ролевыми флагами (кнопка трогает otaActive и
// sensorSendMsg, экран — screenTick, компаньон — весь companion.cpp). Без этих проверок
// признак без своего ролевого флага давал невнятную ошибку линковки вместо внятной здесь.
#if FEATURE_SENSOR && !defined(SENSOR_NODE)
  #error "FEATURE_SENSOR требует SENSOR_NODE (задаётся в platformio.ini)"
#endif
#if FEATURE_BUTTON && !FEATURE_SENSOR
  #error "FEATURE_BUTTON требует FEATURE_SENSOR: кнопка шлёт сообщения узла"
#endif
#if FEATURE_MESH_OTA_RECEIVER && !defined(SENSOR_NODE)
  #error "FEATURE_MESH_OTA_RECEIVER требует SENSOR_NODE"
#endif
#if FEATURE_SUPPORT && !FEATURE_MESH_OTA_SENDER
  #error "FEATURE_SUPPORT требует FEATURE_MESH_OTA_SENDER: прошивальщик тем и занят"
#endif
#if FEATURE_SUPPORT && !FEATURE_WEB
  #error "FEATURE_SUPPORT требует FEATURE_WEB: образ приходит к нему по сети"
#endif
#if FEATURE_SUPPORT && !FEATURE_SENSOR
  #error "FEATURE_SUPPORT требует FEATURE_SENSOR: он отвечает на hello как обычный узел"
#endif
#if FEATURE_COMPANION && !defined(COMPANION_NODE)
  #error "FEATURE_COMPANION требует COMPANION_NODE"
#endif
#if FEATURE_BUTTON && BUTTON_PIN < 0
  #error "FEATURE_BUTTON требует -DPIN_USER_BTN: на этой плате кнопки нет"
#endif
// Код пока ветвится и по старому флагу MQTT_ENABLED, который задаёт platformio.ini: под ним
// лежат wifi/mqtt/fwupdate/ota.cpp целиком. Если новый признак включён, а старого флага нет,
// сборка молча упадёт на нехватке функций. Расхождение лучше видеть сразу.
#if !defined(MQTT_ENABLED) && (FEATURE_WIFI || FEATURE_MQTT || FEATURE_WEB || FEATURE_AUTOUPDATE || FEATURE_NTP)
  #error "FEATURE_WIFI/MQTT/WEB/AUTOUPDATE/NTP требуют MQTT_ENABLED (задаётся в platformio.ini)"
#endif
