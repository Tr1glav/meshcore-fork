#pragma once

// Общий запуск и главный цикл (lib/meshcore/src/app_main.cpp). Проект платы вызывает их
// из своих setup()/loop(): Arduino требует эти символы в самом проекте, а не в библиотеке.
void appSetup();
void appLoop();

// «Вторые уши»: отдать координатору накопленные радио-кадры (src/mc_platform.cpp).
// Зовёт главный цикл у прошивальщика; заглушкой служит пустой макрос в appLoop.
void earsTick();