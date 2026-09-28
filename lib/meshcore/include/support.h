#pragma once

// Узел-прошивальщик (support): кому отдать сессию, когда координатор до узла не дотянется.
//
// Прошивать по радио можно только те узлы, которых слышно напрямую, — сырые кадры
// быстрого канала ретрансляторы не переносят. Прошивальщик стоит там, где их слышно, и
// умеет ровно то же самое (FEATURE_MESH_OTA_SENDER). Своего протокола у передачи нет: у
// него та же страница и те же точки входа, что у координатора.

#include "config.h"
#include <Arduino.h>

#if FEATURE_MESH_OTA_SENDER

// Есть ли прошивальщик в сети: он объявляется в эфире вместе со своим heartbeat, и
// объявление устаревает через три его периода.
bool supportPresent();

// Кто из прошивальщиков дотягивается до цели лучше всех: индекс в supports[] или -1.
// Спрашиваем у каждого — его /sensors отдаёт хопы, а порог один на всех (otaHopsReachable).
int supportIndexFor(const String& target);

// Координатор сообщает прошивальщикам свой адрес по сети (POST /coord). Зовётся из
// главного цикла, по одному адресату за проход.
void coordPushTick();

// Настройки прошивальщика по сети: post=false — забрать список полей (GET /config),
// post=true — применить query (POST /config?<поля>[&reboot=1]). Ответ отдаётся как есть.
bool supportConfigRequest(int idx, bool post, const String& query, String& answer);

// Прошить сам прошивальщик: образ уходит к нему по сети (HTTP OTA), а не по радио —
// у него есть WiFi. Сохранённый .otaz распаковывается на лету: /update ждёт сырой образ.
bool supportFlashSelf();

// Ход сессии, которую ведёт прошивальщик: его же /ota/status, слово в слово.
// false — не ответил; тогда координатор показывает своё состояние.
bool supportStatus(String& out);

// Отдать сессию: образ уходит к нему по сети, следом команда начать.
bool supportHandOff(const String& target);

// Обе передачи идут фоновой задачей: в них мегабайт по сети, а обработчик страницы
// на это время заблокировал бы веб-сервер целиком. enum SUP_JOB_* приходит из
// mesh-network-core/include/mc_platform.h (подключается из config.h).

// Идёт ли передача прямо сейчас. Пока идёт, новую сессию начинать нельзя: обе писали бы
// один и тот же /ota.bin.
bool supportBusy();

// Завести передачу. false — задача не создалась.
bool supportJobStart(uint8_t kind, int supIdx, const String& target);

// Что именно передаётся и сколько уже ушло — для полосы на странице.
uint8_t supportJobKind();
uint32_t supportJobSent();
uint32_t supportJobTotal();

// Забрать итог завершённой передачи (и освободить место под следующую). Зовётся из
// главного цикла: результат ложится в поля, которые читает страница.
bool supportJobFinished(uint8_t& kind, bool& ok, String& target, String& who);

#endif // FEATURE_MESH_OTA_SENDER
