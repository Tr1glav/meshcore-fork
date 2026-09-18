// Точка входа прошивки. Вся логика — в библиотеке lib/meshcore (app_main.cpp); здесь
// остаются только setup()/loop(), потому что Arduino ищет эти символы в проекте, а не
// в библиотеке.
#include "app_main.h"

void setup() { appSetup(); }
void loop()  { appLoop(); }
