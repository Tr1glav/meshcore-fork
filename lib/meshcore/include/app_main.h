#pragma once

// Общий запуск и главный цикл (lib/meshcore/src/app_main.cpp). Проект платы вызывает их
// из своих setup()/loop(): Arduino требует эти символы в самом проекте, а не в библиотеке.
void appSetup();
void appLoop();
