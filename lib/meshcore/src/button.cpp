#include "config.h"
#include "globals.h"
#include "mesh.h"
#include "display.h"
#include "button.h"

// ===== Кнопка узла =====
// Короткое нажатие (меньше секунды) идёт в счёт серии: на сенсоре одно «button», два
// «button2», три включают проверку доступности (а пока она идёт, её выключает любое короткое
// нажатие). На компаньоне короткие нажатия в сеть не уходят — это телефон в кармане.
// Долгое нажатие (от двух секунд) переключает экран, а промежуток от секунды до двух
// намеренно не делает ничего, чтобы случайное удержание не ушло сообщением в сеть.
//
// ===== ПОЧЕМУ ПРЕРЫВАНИЕ, А НЕ ОПРОС =====
// Раньше уровень на пине читался в главном цикле. Беда в том, что radio.transmit()
// блокирующий: на время передачи (сотни миллисекунд при SF8) цикл стоит целиком, и нажатие,
// которое успело начаться и кончиться внутри этого окна, не существовало для прошивки вовсе.
// Пока передачи были редкими — heartbeat раз в десять минут — это было почти незаметно. С
// режимом проверки доступности, где передача уходит каждые десять секунд, ровно тогда, когда
// на кнопку и жмут, из трёх нажатий подряд одно регулярно терялось.
//
// Теперь фронты защёлкиваются прерыванием в кольцо вместе со временем, а главный цикл их
// разбирает, когда доберётся. Обработчик держит ровно это и ничего больше: время и уровень.
#if FEATURE_BUTTON

#include <driver/gpio.h>

#define BTN_DEBOUNCE_MS 40      // дребезг контактов; фронты ближе этого — один и тот же
#define BTN_EVT_MAX     16      // кольцо фронтов; переполнение лечится пересинхронизацией
#define BTN_RESYNC_MS   200     // столько уровень должен расходиться с нашим представлением

static volatile uint32_t btnEvtMs[BTN_EVT_MAX];
static volatile bool     btnEvtDown[BTN_EVT_MAX];
static volatile uint8_t  btnEvtHead = 0;
static volatile uint8_t  btnEvtTail = 0;

// В обработчике только то, что безопасно вызывать из прерывания: esp_timer_get_time и
// gpio_get_level лежат в IRAM, digitalRead и millis() — не гарантированно, а обработчик может
// сработать в момент операции с флешем.
static void IRAM_ATTR buttonIsr() {
    const uint8_t next = (uint8_t)((btnEvtHead + 1) % BTN_EVT_MAX);
    if (next == btnEvtTail) return;      // кольцо полно: фронт теряем, дальше пересинхронизация
    btnEvtMs[btnEvtHead] = (uint32_t)(esp_timer_get_time() / 1000);
    btnEvtDown[btnEvtHead] = (gpio_get_level((gpio_num_t)BUTTON_PIN) == 0);
    btnEvtHead = next;
}

// Состояние разбора: то же, что было при опросе, только фронты приходят из кольца.
static bool btnDown = false;
static uint32_t btnEdgeMs = 0;    // когда приняли последний фронт
static uint32_t btnDownMs = 0;    // когда нажали
static int btnPresses = 0;

void buttonBegin() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonIsr, CHANGE);
}

// Один принятый фронт. Вынесено отдельно, потому что зовётся из двух мест: разбора кольца и
// пересинхронизации по уровню.
static void buttonEdge(bool down, uint32_t t) {
    if (down == btnDown) return;
    if (t - btnEdgeMs < BTN_DEBOUNCE_MS) return;   // дребезг
    btnDown = down;
    btnEdgeMs = t;
    if (down) {
        btnDownMs = t;
        return;
    }
    const uint32_t held = t - btnDownMs;
    if (held >= BTN_WAKE_MS) {
        screenToggle();             // от двух секунд — переключаем экран
    } else if (held < BTN_TRIGGER_MAX_MS) {
        btnPresses++;               // короткое нажатие идёт в счёт серии
    }
    // Между секундой и двумя — намеренно ничего: так отсекается случайное удержание кнопки,
    // которое иначе ушло бы сообщением в сеть.
}

void buttonTick() {
    const uint32_t now = millis();

    // Во время прошивки по радио кнопка не работает, но фронты всё равно разгребаем: иначе
    // кольцо переполнится, а накопленные нажатия сработали бы залпом после сессии.
    if (otaActive) {
        btnEvtTail = btnEvtHead;
        btnPresses = 0;
        return;
    }

    while (btnEvtTail != btnEvtHead) {
        const uint8_t i = btnEvtTail;
        const uint32_t t = btnEvtMs[i];
        const bool down = btnEvtDown[i];
        btnEvtTail = (uint8_t)((i + 1) % BTN_EVT_MAX);
        buttonEdge(down, t);
    }

    // Страховка на потерянные фронты: кольцо могло переполниться дребезгом, и тогда наше
    // представление о кнопке разошлось бы с действительностью навсегда — например, кнопка
    // «залипла» бы нажатой, и серия никогда не завершилась.
    if (btnEvtTail == btnEvtHead && now - btnEdgeMs > BTN_RESYNC_MS) {
        const bool level = (digitalRead(BUTTON_PIN) == LOW);
        if (level != btnDown) buttonEdge(level, now);
    }

    // Серия закончена: кнопка отпущена и окно ожидания следующего нажатия истекло
    if (btnPresses > 0 && !btnDown && now - btnEdgeMs > SNS_BTN_DBL_WINDOW_MS) {
        int presses = btnPresses;
        btnPresses = 0;
        screenWake();                       // результат должно быть видно
        // Пока проверка идёт, её выключает ЛЮБОЕ короткое нажатие, а не только тройное:
        // потерять одно нажатие втрое труднее, чем три подряд, а на T-Deck выключение
        // устроено так же. С защёлкой на прерывании терять их стало нечему, но правило
        // остаётся: во время проверки на экране и так только она.
        if (pingModeOn) pingModeToggle();
        else if (presses >= 3) pingModeToggle();   // тройное нажатие включает проверку
        // На компаньоне короткие нажатия в сеть не уходят: это телефон в кармане,
        // случайные 1-2 нажатия не должны слать "button"/"button2" в MQTT. Остаются
        // тройное нажатие (проверка) и долгое (экран). На сенсоре как было.
        else if (FEATURE_COMPANION == 0) sensorSendMsg(presses == 2 ? SENSOR_MSG_BUTTON2 : SENSOR_MSG_BUTTON);
    }
}

#endif // FEATURE_BUTTON
