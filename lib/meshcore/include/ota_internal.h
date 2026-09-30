#pragma once

#include "config.h"

// Внутренний заголовок подсистемы прошивки: только то, что делят между собой
// раздача образа (ota.cpp), веб-страница (web.cpp) и загрузчик образов с релиза
// (fwupdate.cpp — он проставляет имя скачанного файла). Объявления сгенерированы по
// самим определениям, чтобы типы не разошлись при правках.

// Сервер страницы. Он тоже переехал сюда из globals.h ядра: WebServer в библиотеке
// протокола существовал только затем, чтобы им пользовалась прошивка.
#if FEATURE_WEB
#include <WebServer.h>
extern WebServer otaServer;
#endif

#if FEATURE_WEB || FEATURE_MESH_OTA_SENDER
extern unsigned long otaSessionMs; // старт сессии — для скорости и длительности на странице
extern unsigned long otaDoneMs;    // когда узел подтвердил прошивку
// otaImgSize переехал в ota.h ядра: он нужен и самому ядру (ota_slow.cpp объявляет его
// узлу в старте медленной сессии), а не только странице.
extern char otaLastErr[48];       // причина последнего abort — показывается на странице
extern String otaFwName;          // имя последнего загруженного файла — для страницы
extern uint16_t otaPolls;         // сколько раз пришлось переспрашивать маску за сессию
extern uint32_t otaUsBuild;
extern uint32_t otaUsTx;
extern uint32_t otaChunksSent;
extern uint16_t otaRetrTotal;     // сколько всего было повторов за сессию
extern uint32_t logTotal;
#endif
