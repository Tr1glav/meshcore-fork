#include "config.h"
#include "globals.h"
#include "companion.h"
#include "companion_internal.h"
#include "mesh.h"
#include "display.h"   // batteryVoltage для кадра о заряде

#ifdef COMPANION_NODE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <Preferences.h>
#include <LittleFS.h>


static BLEServer* bleServer = nullptr;
static BLECharacteristic* txChar = nullptr;
volatile bool bleConnected = false;
static uint32_t blePin = 0;
// Экран с кодом убираем не по факту соединения, а только когда сопряжение состоялось:
// код нужен телефону именно в промежутке между подключением и вводом кода.
volatile bool blePaired = false;
QueuedMsg msgQueue[MSG_QUEUE_MAX];
uint8_t msgHead = 0, msgCount = 0;
uint8_t inQueue[IN_QUEUE_MAX][MAX_FRAME_SIZE];
uint8_t inQueueLen[IN_QUEUE_MAX];
volatile uint8_t inHead = 0, inCount = 0;
// Колбэк BLE выполняется в своей задаче, разбор — в главном цикле: индексы очереди
// трогаем только под блокировкой, иначе счётчик разъедется между ядрами.
portMUX_TYPE inMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t inDropped = 0;

void sendFrameToApp(const uint8_t* data, size_t len) {
    if (!bleConnected || txChar == nullptr || len == 0) return;
    if (data[0] != PUSH_CODE_LOG_RX_DATA) {
        // Сырой журнал приёма уходит на КАЖДЫЙ принятый кадр, и строка в UART на каждый из
        // них стоит миллисекунды главного цикла — а сам кадр и так уже напечатан строкой
        // [RX]. Но и совсем молчать нельзя: сколько кадров 0x88 реально ушло приложению —
        // не видно ниоткуда. Поэтому для 0x88 печатается КОРОТКИЙ счётчик, а не дамп.
        Serial.printf("[BLE] -> код %u, %u байт\n", data[0], (unsigned)len);
    } else {
        diagLogRxPushed++;
        Serial.printf("[BLE] 0x88 ушёл приложению (#%lu)\n", (unsigned long)diagLogRxPushed);
    }
    txChar->setValue((uint8_t*)data, len);
    txChar->notify();
}

// ===== Контакты =====
// Список узлов, чьи адверты мы слышали: приложение забирает его командой 4 и пополняет
// командой 9. Порядок полей в кадре повторяет оригинал байт в байт — приложение читает
// его по смещениям, и любая перестановка превращается в мусор на экране телефона.
Contact contacts[COMPANION_MAX_CONTACTS];
uint8_t contactCount = 0;
bool contactsDirty = false;
unsigned long contactsDirtyMs = 0;
int contactIterIdx = -1;               // -1 — перебор списка не идёт
int appChanBase = 0;                   // с этого номера идут каналы из приложения
uint32_t contactIterSince = 0, contactIterNewest = 0;

// ===== Диагностика сырого журнала (0x88) =====
// Приложение ставит отметку «принято ретранслятором» по потоку 0x88, который уходит на
// КАЖДЫЙ принятый радио-кадр до дедупа. Чтобы по UART было видно, где этот поток рвётся,
// считаем по концам и печатаем эхо собственного пакета (такое эхо — и есть переиздание
// ретранслятором). Счётчики общие с companion_proto.cpp, где раз в 30 с печатается сводка.
uint32_t diagLogRxRecv = 0, diagLogRxPushed = 0, diagLogRxLost = 0, diagEchoOwn = 0;

static const char* COMPANION_NS = "companion";

int contactFind(const uint8_t* pub) {
    for (int i = 0; i < contactCount; i++)
        if (memcmp(contacts[i].pub, pub, 32) == 0) return i;
    return -1;
}

// Поиск по НАЧАЛУ ключа. Приложение спрашивает узел не всегда полным ключом: у хопа в
// маршруте оно знает только префикс, а полного ключа у него и быть не может — в пути
// передаются хэши ретрансляторов, а не ключи.
//
// Оригинал ключует свою таблицу путей семью байтами (AdvertPath::pubkey_prefix[7]) и
// сравнивает ровно столько, сколько там лежит. Мы сравниваем столько, сколько прислали, —
// это строже и точнее, но главное, что НЕ требуем все 32.
int contactFindPrefix(const uint8_t* pub, size_t n) {
    if (n > 32) n = 32;
    if (n < CONTACT_KEY_PREFIX_MIN) return -1;   // слишком короткий ключ совпадёт с кем угодно
    for (int i = 0; i < contactCount; i++)
        if (memcmp(contacts[i].pub, pub, n) == 0) return i;
    return -1;
}


#define CONTACTS_FILE "/contacts.bin"
// Метка формата файла контактов: 4 байта подписи, размер записи и число записей.
// Без неё смена раскладки Contact читалась бы как мусор, стоило длине файла совпасть.
// Размер записи входит в заголовок именно потому, что он меняется вместе с раскладкой.
#define CONTACTS_MAGIC "MCC1"
#define CONTACTS_HDR   7

// Контакты живут файлом, а не записью в NVS: раздел nvs — это 20 КБ на всё вместе с
// настройками, а список на 200 узлов занимает уже сорок с лишним килобайт.
void contactsSave() {
    File f = LittleFS.open(CONTACTS_FILE, "w");
    if (!f) { Serial.println("[BLE] файл контактов не открылся на запись"); return; }
    uint8_t hdr[CONTACTS_HDR];
    memcpy(hdr, CONTACTS_MAGIC, 4);
    uint16_t rec = (uint16_t)sizeof(Contact);
    memcpy(hdr + 4, &rec, 2);
    hdr[6] = contactCount;
    size_t need = sizeof(Contact) * contactCount;
    bool ok = f.write(hdr, sizeof(hdr)) == sizeof(hdr);
    if (ok && contactCount > 0) ok = f.write((const uint8_t*)contacts, need) == need;
    f.close();
    if (!ok) { Serial.println("[BLE] список контактов записан не полностью"); return; }
    contactsDirty = false;
    Serial.printf("[BLE] контакты сохранены: %u\n", contactCount);
}

static void contactsLoad() {
    contactCount = 0;
    File f = LittleFS.open(CONTACTS_FILE, "r");
    if (!f) { Serial.println("[BLE] списка контактов ещё нет"); return; }
    uint8_t hdr[CONTACTS_HDR];
    uint16_t rec = 0;
    if (f.read(hdr, sizeof(hdr)) != (int)sizeof(hdr)) { f.close(); return; }
    memcpy(&rec, hdr + 4, 2);
    // Файл от другой версии записи (или совсем старый, без метки) не разбираем: пустой
    // список приложение наполнит заново из адвертов, а мусор оно показать не сможет.
    if (memcmp(hdr, CONTACTS_MAGIC, 4) != 0 || rec != sizeof(Contact)) {
        f.close();
        LittleFS.remove(CONTACTS_FILE);
        Serial.println("[BLE] файл контактов другого формата — начинаем с пустого списка");
        return;
    }
    uint8_t n = hdr[6];
    if (n > COMPANION_MAX_CONTACTS) n = COMPANION_MAX_CONTACTS;
    size_t need = sizeof(Contact) * n;
    // Прочиталось меньше обещанного — файл оборван или от другой версии записи:
    // лучше пустой список, чем контакты из мусора.
    if (n > 0 && f.read((uint8_t*)contacts, need) != (int)need) {
        Serial.println("[BLE] файл контактов повреждён, начинаем с пустого списка");
        n = 0;
    }
    f.close();
    contactCount = n;
    Serial.printf("[BLE] контактов загружено: %u\n", contactCount);
}

void contactTouch() { contactsDirty = true; contactsDirtyMs = millis(); }

// ===== Ожидаемые подтверждения доставки =====
// Хэш считает ядро при сборке кадра (buildPrivateTextFrame), сверяет — приёмная сторона
// ядра через хук mcOnAckRecv. Здесь только память о том, чего мы ждём, и отметка времени:
// приложению интересно не только «доставлено», но и за сколько.
static struct {
    uint8_t ack[4];
    unsigned long sentMs;
    bool used;
} expectedAck[EXPECTED_ACK_MAX];
static int expectedAckNext = 0;

void ackExpect(const uint8_t ack4[4]) {
    int slot = -1;
    for (int i = 0; i < EXPECTED_ACK_MAX && slot < 0; i++)
        if (!expectedAck[i].used) slot = i;
    if (slot < 0) {                       // все заняты — вытесняем самое давнее по кругу
        slot = expectedAckNext;
        expectedAckNext = (expectedAckNext + 1) % EXPECTED_ACK_MAX;
    }
    memcpy(expectedAck[slot].ack, ack4, 4);
    expectedAck[slot].sentMs = millis();
    expectedAck[slot].used = true;
}

// Переопределение хука ядра: пришло подтверждение. Сверяем с тем, чего ждём, и говорим
// приложению — иначе отправленное сообщение навсегда останется у него без галочки.
//
// Одно и то же подтверждение приходит несколько раз (копии флуда и переиздания), поэтому
// слот освобождается на первом совпадении: второй пуш приложению про то же сообщение ему
// не нужен.
void mcOnAckRecv(const uint8_t ack4[4]) {
    for (int i = 0; i < EXPECTED_ACK_MAX; i++) {
        if (!expectedAck[i].used) continue;
        if (memcmp(expectedAck[i].ack, ack4, 4) != 0) continue;
        const uint32_t trip = (uint32_t)(millis() - expectedAck[i].sentMs);
        expectedAck[i].used = false;
        Serial.printf("[ACK] наше сообщение доставлено за %lu мс\n", (unsigned long)trip);
        if (!blePaired) return;
        uint8_t buf[9];
        buf[0] = PUSH_CODE_SEND_CONFIRMED;
        memcpy(&buf[1], ack4, 4);
        memcpy(&buf[5], &trip, 4);
        sendFrameToApp(buf, sizeof(buf));
        return;
    }
}

// ===== Вход на ретранслятор: сессии и ожидаемые ответы =====
// Управление чужим ретранслятором из приложения устроено так: вход по паролю кадром
// ANON_REQ, дальше команды командной строкой в личке (тип текста 1) и запросы состояния и
// телеметрии кадром REQ. Ответ на всё это приходит кадром RESPONSE с меткой запроса в
// первых четырёх байтах. Ядро такой кадр расшифровывает и отдаёт сюда хуком
// mcOnResponseRecv — помнить, на что этот ответ, может только тот, кто запрос отправлял.

static struct {
    uint8_t pub[32];
    uint16_t keepAliveSecs;      // 0 — продлевать не требуется
    unsigned long lastMs;        // когда последний раз продлевали (или вошли)
    bool used;
} sessions[LOGIN_SESSION_MAX];

void sessionStart(const uint8_t* pub, uint16_t keepAliveSecs) {
    int slot = -1;
    for (int i = 0; i < LOGIN_SESSION_MAX; i++) {
        if (sessions[i].used && memcmp(sessions[i].pub, pub, 32) == 0) { slot = i; break; }
        if (!sessions[i].used && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;                       // все заняты — вытесняем первую
    memcpy(sessions[slot].pub, pub, 32);
    sessions[slot].keepAliveSecs = keepAliveSecs;
    sessions[slot].lastMs = millis();
    sessions[slot].used = true;
    Serial.printf("[REQ] вход на <%02X> удержан, продлевать каждые %u с\n",
                  pub[0], (unsigned)keepAliveSecs);
}

bool sessionHas(const uint8_t* pub, size_t n) {
    for (int i = 0; i < LOGIN_SESSION_MAX; i++)
        if (sessions[i].used && memcmp(sessions[i].pub, pub, n) == 0) return true;
    return false;
}

void sessionStop(const uint8_t* pub, size_t n) {
    for (int i = 0; i < LOGIN_SESSION_MAX; i++)
        if (sessions[i].used && memcmp(sessions[i].pub, pub, n) == 0) {
            sessions[i].used = false;
            Serial.printf("[REQ] выход с <%02X>\n", sessions[i].pub[0]);
        }
}

// Продление сессий. Запрос уходит в очередь, как и всё остальное: выходить в эфир прямо
// из такта значит остановить главный цикл на время кадра.
void sessionsTick() {
    for (int i = 0; i < LOGIN_SESSION_MAX; i++) {
        if (!sessions[i].used || sessions[i].keepAliveSecs == 0) continue;
        const unsigned long every =
            (unsigned long)sessions[i].keepAliveSecs * 1000UL * KEEP_ALIVE_EARLY_PCT / 100UL;
        if ((unsigned long)(millis() - sessions[i].lastMs) < every) continue;
        sessions[i].lastMs = millis();
        uint8_t frame[96];
        uint32_t tag = 0;
        const int fl = buildSimpleReqFrame(sessions[i].pub[0], sessions[i].pub,
                                           REQ_TYPE_KEEP_ALIVE, &tag, frame, sizeof(frame));
        if (fl > 0) {
            floodSendQueued(-1, frame, fl);
            Serial.printf("[REQ] продлили вход на <%02X>\n", sessions[i].pub[0]);
        }
    }
}

// Чего мы ждём. Оригинал держит по одному ожиданию каждого вида и сбрасывает все при новом
// запросе (clearPendingReqs): ответы приходят секундами позже, и путать их между собой
// опаснее, чем потерять ответ на отменённый запрос.
static uint8_t pendingLoginPub[4], pendingStatusPub[4];
static bool pendingLoginUsed = false, pendingStatusUsed = false;
static uint32_t pendingTelemetryTag = 0, pendingBinaryTag = 0;

void reqPendingClear() {
    pendingLoginUsed = pendingStatusUsed = false;
    pendingTelemetryTag = pendingBinaryTag = 0;
}
void reqPendingLogin(const uint8_t* pub32) {
    memcpy(pendingLoginPub, pub32, 4); pendingLoginUsed = true;
}
void reqPendingStatus(const uint8_t* pub32) {
    memcpy(pendingStatusPub, pub32, 4); pendingStatusUsed = true;
}
void reqPendingTelemetry(uint32_t tag) { pendingTelemetryTag = tag; }
void reqPendingBinary(uint32_t tag)    { pendingBinaryTag = tag; }

// Переопределение хука ядра: пришёл ответ ретранслятора. Раскладка кадров для приложения —
// из оригинала (MyMesh::onContactResponse), байт в байт: приложение читает их по
// смещениям.
void mcOnResponseRecv(uint8_t srcHash, const uint8_t* srcPub, const uint8_t* data, int len) {
    (void)srcHash;
    if (len < 4 || srcPub == NULL) return;
    uint32_t tag = 0;
    memcpy(&tag, data, 4);
    const uint8_t* body = &data[4];
    const int blen = len - 4;
    uint8_t buf[64];
    int i = 0;

    if (pendingLoginUsed && memcmp(pendingLoginPub, srcPub, 4) == 0) {
        pendingLoginUsed = false;
        // Два вида ответа на вход. Старые ретрансляторы отвечают текстом «OK», новые —
        // кодом с правами и интервалом продления. Оригинал понимает оба, и мы тоже:
        // в сети стоят узлы обеих прошивок.
        if (blen >= 2 && memcmp(body, "OK", 2) == 0) {
            buf[i++] = PUSH_CODE_LOGIN_SUCCESS;
            buf[i++] = 0;                         // старый ответ прав не несёт
            memcpy(&buf[i], srcPub, 6); i += 6;
            Serial.printf("[REQ] вход на <%02X> разрешён (старый ответ)\n", srcPub[0]);
        } else if (blen >= 9 && body[0] == RESP_SERVER_LOGIN_OK) {
            const uint16_t keepAlive = (uint16_t)body[1] * 16;
            if (keepAlive > 0) sessionStart(srcPub, keepAlive);
            else sessionStart(srcPub, 0);
            buf[i++] = PUSH_CODE_LOGIN_SUCCESS;
            buf[i++] = body[2];                   // права: старший ли это клиент
            memcpy(&buf[i], srcPub, 6); i += 6;
            memcpy(&buf[i], &tag, 4); i += 4;     // часы ретранслятора
            buf[i++] = body[3];                   // права доступа
            buf[i++] = body[8];                   // уровень прошивки ретранслятора
            Serial.printf("[REQ] вход на <%02X> разрешён, права %02X\n", srcPub[0], body[2]);
        } else {
            buf[i++] = PUSH_CODE_LOGIN_FAIL;
            buf[i++] = 0;
            memcpy(&buf[i], srcPub, 6); i += 6;
            Serial.printf("[REQ] вход на <%02X> отклонён\n", srcPub[0]);
        }
        sendFrameToApp(buf, i);
        return;
    }
    if (blen > 0 && pendingStatusUsed && memcmp(pendingStatusPub, srcPub, 4) == 0) {
        pendingStatusUsed = false;
        buf[i++] = PUSH_CODE_STATUS_RESPONSE;
        buf[i++] = 0;                             // зарезервировано
        memcpy(&buf[i], srcPub, 6); i += 6;
        const int n = blen > (int)sizeof(buf) - i ? (int)sizeof(buf) - i : blen;
        memcpy(&buf[i], body, n); i += n;
        Serial.printf("[REQ] метрики <%02X>: %d Б\n", srcPub[0], n);
        sendFrameToApp(buf, i);
        return;
    }
    if (blen > 0 && tag != 0 && tag == pendingTelemetryTag) {
        pendingTelemetryTag = 0;
        buf[i++] = PUSH_CODE_TELEMETRY_RESPONSE;
        buf[i++] = 0;
        memcpy(&buf[i], srcPub, 6); i += 6;
        const int n = blen > (int)sizeof(buf) - i ? (int)sizeof(buf) - i : blen;
        memcpy(&buf[i], body, n); i += n;
        sendFrameToApp(buf, i);
        return;
    }
    if (blen > 0 && tag != 0 && tag == pendingBinaryTag) {
        pendingBinaryTag = 0;
        buf[i++] = PUSH_CODE_BINARY_RESPONSE;
        buf[i++] = 0;
        memcpy(&buf[i], &tag, 4); i += 4;         // приложение сверяет метку с ответом на запрос
        const int n = blen > (int)sizeof(buf) - i ? (int)sizeof(buf) - i : blen;
        memcpy(&buf[i], body, n); i += n;
        sendFrameToApp(buf, i);
        return;
    }
    Serial.printf("[REQ] ответ от <%02X> с меткой %08lX никто не ждал\n",
                  srcPub[0], (unsigned long)tag);
}

// Узел прислал дорогу до себя. Кладём её контакту в outPath: приложение читает маршрут
// именно оттуда (кадр контакта), и до сих пор там всегда стояло 0xFF — «пути не знаем».
// Отсюда и «обратный маршрут посмотреть нельзя»: показывать было нечего, потому что узнать
// путь было неоткуда — возврат маршрута мы не разбирали.
void mcOnPathRecv(uint8_t srcHash, uint8_t pathLen, const uint8_t* path,
                  uint8_t extraType, const uint8_t* extra, int extraLen) {
    (void)extraType; (void)extra; (void)extraLen;   // довесок разбирает ядро
    uint8_t* pub = findPeerPub(srcHash);
    if (pub == NULL) return;              // чей это маршрут — неизвестно, записать некуда
    const int idx = contactFind(pub);
    if (idx < 0) return;
    Contact& c = contacts[idx];

    const uint8_t hops = pathLen & 0x3F;
    const int bytes = (int)hops * (((pathLen >> 6) & 3) + 1);
    if (bytes > (int)sizeof(c.outPath)) {
        Serial.printf("[PATH] маршрут до %s длиннее, чем влезает (%d Б) — не сохраняю\n",
                      c.name, bytes);
        return;
    }
    const bool changed = (c.outPathLen != pathLen) || memcmp(c.outPath, path, bytes) != 0;
    c.outPathLen = pathLen;
    memcpy(c.outPath, path, bytes);
    contactTouch();
    // Новый путь обязан дойти до приложения инкрементальной синхронизацией контактов
    // (команда 4): фильтр c.lastmod <= contactIterSince выбрасывает записи, которые
    // приложение «уже знает». Пока lastmod не поднимался, маршрут лёг в контакт, но пуш
    // 0x81 ушёл один раз, а синк молча пропускал контакт — приложение работало со старым
    // outPath, и обратный маршрут продолжал не показываться.
    if (changed) c.lastmod = (uint32_t)time(NULL);
    Serial.printf("[PATH] маршрут до %s: %u хопов%s\n", c.name, hops,
                  changed ? "" : " (прежний)");

    // Пуш только на изменение: маршрут приходит с каждым подтверждением, и сообщать
    // приложению одно и то же незачем — оно на каждый пуш перезапрашивает контакт.
    if (!blePaired || !changed) return;
    uint8_t buf[1 + 32];
    buf[0] = PUSH_CODE_PATH_UPDATED;
    memcpy(&buf[1], c.pub, 32);
    sendFrameToApp(buf, 1 + 32);
}

int contactFrame(uint8_t code, const Contact& c, uint8_t* buf) {
    int i = 0;
    buf[i++] = code;
    memcpy(&buf[i], c.pub, 32);      i += 32;
    buf[i++] = c.type;
    buf[i++] = c.flags;
    buf[i++] = c.outPathLen;
    memcpy(&buf[i], c.outPath, 64);  i += 64;
    memset(&buf[i], 0, 32);
    strncpy((char*)&buf[i], c.name, 31); i += 32;
    memcpy(&buf[i], &c.lastAdvert, 4); i += 4;
    memcpy(&buf[i], &c.lat, 4);        i += 4;
    memcpy(&buf[i], &c.lon, 4);        i += 4;
    memcpy(&buf[i], &c.lastmod, 4);    i += 4;
    return i;                          // 148 байт, в кадр (176) помещается
}

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        size_t n = c->getLength();
        if (n == 0 || n > MAX_FRAME_SIZE) return;
        // Писать в эту характеристику разрешено только по зашифрованной связи,
        // так что пришедший кадр сам по себе доказывает состоявшееся сопряжение
        blePaired = true;
        portENTER_CRITICAL(&inMux);
        if (inCount >= IN_QUEUE_MAX) {
            inDropped++;                 // очередь переполнена — кадр потерян, но об этом узнаем
        } else {
            uint8_t slot = (inHead + inCount) % IN_QUEUE_MAX;
            memcpy(inQueue[slot], c->getData(), n);
            inQueueLen[slot] = (uint8_t)n;
            inCount++;
        }
        portEXIT_CRITICAL(&inMux);
    }
};

// Приложение MeshCore подключается только к сопряжённому устройству: обе характеристики
// оригинала доступны исключительно по зашифрованной связи с подтверждением личности
// (ESP_GATT_PERM_*_ENC_MITM). Без этого телефон начинает сопряжение, не получает ответа
// и остаётся в состоянии «подключаюсь» — связь при этом даже не устанавливается.
class SecCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return blePin; }
    void onPassKeyNotify(uint32_t key) override { Serial.printf("[BLE] код сопряжения %u\n", key); }
    bool onConfirmPIN(uint32_t) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        if (cmpl.success) {
            blePaired = true;
            Serial.println("[BLE] сопряжение выполнено");
        } else {
            Serial.printf("[BLE] сопряжение не удалось (причина %u)\n", cmpl.fail_reason);
            if (bleServer) bleServer->disconnect(bleServer->getConnId());
        }
    }
};

unsigned long bleAdvFastUntil = 0;

// Переключение режима рекламы. Единицы — по 0.625 мс, поэтому числа выглядят странно:
// 48 = 30 мс, 96 = 60 мс, 1280 = 800 мс, 1920 = 1200 мс.
void bleAdvFast(bool fast) {
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (adv == NULL) return;
    adv->setMinInterval(fast ? BLE_ADV_FAST_MIN : BLE_ADV_SLOW_MIN);
    adv->setMaxInterval(fast ? BLE_ADV_FAST_MAX : BLE_ADV_SLOW_MAX);
    bleAdvFastUntil = fast ? (millis() + BLE_ADV_FAST_MS) : 0;
    Serial.printf("[BLE] реклама %s\n", fast ? "частая (подключения ждём)" : "редкая");
}

class SrvCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* p) override {
        bleConnected = true;
        bleAdvFastUntil = 0;              // подключились — частить объявлениями незачем
        Serial.println("[BLE] приложение подключилось");
        // Просим короткий интервал соединения. Телефон вправе отказать, но без просьбы он
        // обычно выбирает щадящий для себя: каждый обмен с приложением ждёт следующего
        // окна связи, и на десятках команд это складывается в заметную вялость.
        // Единицы — по 1.25 мс: 12 = 15 мс, 24 = 30 мс. Задержка (latency) нулевая: узел
        // отвечает на каждое окно, иначе экономия телефона превращается в наше ожидание.
        if (s && p) {
            s->updateConnParams(p->connect.remote_bda, BLE_CONN_MIN, BLE_CONN_MAX, 0,
                                BLE_CONN_TIMEOUT);
        }
    }
    // Размер ATT-пакета договаривается телефоном, и от него напрямую зависит, уедет кадр
    // одним уведомлением или будет нарезан стеком на куски по 20 байт. Своего влияния у нас
    // тут нет, но знать это надо: «медленно» с кадром в 176 байт и MTU 23 — это девять
    // пакетов вместо одного, и выяснять такое по догадкам дорого.
    void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
        if (p == NULL) return;
        const unsigned mtu = p->mtu.mtu;
        Serial.printf("[BLE] MTU %u: кадр %u Б уедет %s\n", mtu, (unsigned)MAX_FRAME_SIZE,
                      (mtu >= MAX_FRAME_SIZE + 3) ? "одним уведомлением"
                                                  : "кусками — приложение будет ждать");
    }
    void onDisconnect(BLEServer* s) override {
        bleConnected = false;
        blePaired = false;
        // Иначе при следующем подключении список контактов поедет с середины,
        // без начального кадра, и приложение примет обрывок за весь список.
        contactIterIdx = -1;
        Serial.println("[BLE] приложение отключилось");
        // Отключились — значит подключатся снова, и ждать этого надо быстро.
        bleAdvFast(true);
        s->startAdvertising();
    }
};

// Каналы, заведённые из приложения, живут в NVS рядом с контактами: иначе после
// перезагрузки приложение видело бы пустой список и вступать в канал пришлось бы заново.
// Номер ячейки не храним: состав каналов из настроек меняется (сегодня добавился
// приватный), и сохранённый номер попал бы на встроенный канал и затёр его. Храним имя
// с ключом, а при загрузке дописываем в конец списка.
struct AppChanRec { uint8_t idx; char name[33]; uint8_t key[16]; };

void appChannelsSave() {
    AppChanRec recs[MAX_CHANNELS];
    uint8_t n = 0;
    for (int k = 0; k < numChannels && n < MAX_CHANNELS; k++) {
        if (k < appChanBase) continue;            // каналы из настроек хранит сам конфиг
        recs[n].idx = 0;                          // поле осталось от прежнего формата
        memset(recs[n].name, 0, sizeof(recs[n].name));
        strncpy(recs[n].name, channels[k].name, 32);
        memcpy(recs[n].key, channels[k].secret, 16);
        n++;
    }
    Preferences p;
    if (!p.begin(COMPANION_NS, false)) return;
    p.putUChar("chcount", n);
    if (n > 0) p.putBytes("chans", recs, sizeof(AppChanRec) * n);
    p.end();
    Serial.printf("[CH] каналов из приложения сохранено: %u\n", n);
}

static void appChannelsLoad() {
    Preferences p;
    if (!p.begin(COMPANION_NS, false)) return;
    uint8_t n = p.getUChar("chcount", 0);
    if (n > MAX_CHANNELS) n = MAX_CHANNELS;
    AppChanRec recs[MAX_CHANNELS];
    size_t want = sizeof(AppChanRec) * n;
    // getBytes при несовпадении длины возвращает 0 и буфер НЕ заполняет. Без этой проверки
    // в список каналов уезжали бы имена и ключи из мусора на стеке.
    if (n > 0 && p.getBytes("chans", recs, want) != want) {
        Serial.println("[CH] запись каналов в NVS не той длины — список пропущен");
        n = 0;
    }
    p.end();
    for (uint8_t k = 0; k < n; k++) {
        recs[k].name[32] = 0;
        int at = findChannelByName(recs[k].name);          // уже есть — обновим ключ
        channelSetSlot(at >= 0 ? at : numChannels, recs[k].name, recs[k].key);
    }
}

void companionBegin() {
    appChanBase = numChannels;      // всё, что дальше, добавлено из приложения
    contactsLoad();
    appChannelsLoad();
    String name = cfgReady() ? cfg.name : String("MeshCore");
    // Код свой на каждый запуск: подсмотреть его можно только у экрана самого устройства
    blePin = 100000 + (esp_random() % 900000);
    BLEDevice::init(name.c_str());
    BLEDevice::setSecurityCallbacks(new SecCallbacks());
    // Без этого ATT MTU остаётся 23 байта: в уведомление влезает 20, а наши кадры
    // длиннее (только SELF_INFO около 70). Приложение получало обрезанный ответ и
    // ждало продолжения — со стороны это выглядит как «подключается и висит».
    BLEDevice::setMTU(MAX_FRAME_SIZE);
    // setStaticPIN сам выставляет режим «только отображаем код» и SC_ONLY, поэтому
    // требуемый режим проверки подлинности задаём после него, как это делает оригинал
    BLESecurity sec;
    sec.setStaticPIN(blePin);
    sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new SrvCallbacks());
    BLEService* svc = bleServer->createService(NUS_SERVICE);
    BLECharacteristic* rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE);
    rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
    rx->setCallbacks(new RxCallbacks());
    txChar = svc->createCharacteristic(
        NUS_TX, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    txChar->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
    txChar->addDescriptor(new BLE2902());
    svc->start();
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    // Реклама объявляется ЧАСТО, пока подключения ждут, и редко — потом. Раньше здесь стоял
    // один редкий интервал (800–1200 мс) с рассуждением «телефон всё равно найдёт за пару
    // секунд, зато радио молчит». Пара секунд и оказалась тем, что видно глазами: телефон
    // ловит объявление не сразу, его окно сканирования короче интервала, и ожидание выходит
    // кратным этому интервалу.
    //
    // Поэтому два режима. Сразу после запуска и после каждого отключения — быстрый, на
    // BLE_ADV_FAST_MS; дальше, если никто не подключился, переходим на редкий. Экономия
    // сохраняется там, где она и была нужна (узел работает сутками без телефона), а
    // подключение перестаёт ждать.
    bleAdvFast(true);
    BLEDevice::startAdvertising();
    Serial.printf("[BLE] компаньон «%s» ждёт подключения, код сопряжения %u\n",
                  name.c_str(), blePin);
}

uint32_t companionBlePin() { return blePin; }
bool companionBleLinked() { return blePaired; }

// Метка времени сообщения, как её увидит приложение. Печатается при постановке в очередь:
// приложение показывает ИМЕННО эту метку (она же — ключ поиска кадра в сыром журнале
// 0x88, по которому рисуется маршрут), и когда в переписке стоит не то время, по строке
// видно сразу, чьи часы врут — отправителя или наши.
static void logMsgTs(const char* what, uint32_t ts) {
    time_t local = (time_t)ts + (time_t)cfg.tzOffset * 3600;
    struct tm tm_now;
    gmtime_r(&local, &tm_now);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
    const uint32_t now = (uint32_t)time(NULL);
    const long diff = (long)ts - (long)now;
    Serial.printf("[BLE] %s: метка %s (наши часы расходятся на %ld с)\n", what, buf, diff);
}

void companionOnChannelText(int channelIdx, const String& text, float snr, uint8_t pathLen,
                            uint32_t senderTs, bool notify) {
    if (msgCount >= MSG_QUEUE_MAX) {   // очередь полна — вытесняем самое старое
        msgHead = (msgHead + 1) % MSG_QUEUE_MAX;
        msgCount--;
    }
    QueuedMsg& m = msgQueue[(msgHead + msgCount) % MSG_QUEUE_MAX];
    memset(&m, 0, sizeof(m));
    m.kind = MSG_KIND_CHANNEL;
    m.channelIdx = (uint8_t)channelIdx;
    m.pathLen = pathLen;
    long s4 = lround(snr * 4.0f);
    m.snr4 = (int8_t)(s4 < -128 ? -128 : (s4 > 127 ? 127 : s4));
    // Время — по часам ОТПРАВИТЕЛЯ из открытого текста кадра, как в оригинале: приложение
    // сопоставляет сообщение с сырым журналом 0x88 именно по этой метке (там оно
    // расшифровывает кадр САМО). Время приёма для отправителя с другими часами не
    // совпадёт, и «маршрут сообщения» приложению показать будет не по чему.
    m.ts = senderTs;
    if (text.length() >= sizeof(m.text))
        Serial.printf("[BLE] сообщение обрезано: %u -> %u байт\n",
                      text.length(), (unsigned)sizeof(m.text) - 1);
    strlcpy(m.text, text.c_str(), sizeof(m.text));
    msgCount++;
    logMsgTs("канал", m.ts);
    // Сигнал «есть новое» рождает в телефоне уведомление. Для того, что устройство
    // отправило само, это уведомление о собственном действии — кладём в очередь молча,
    // и приложение покажет сообщение при ближайшей синхронизации.
    if (!notify) return;
    uint8_t push = PUSH_CODE_MSG_WAITING;
    sendFrameToApp(&push, 1);   // приложение заберёт сообщение командой 10
}

// Личное сообщение. В очередь кладётся тем же путём, что и групповое, но с другим видом:
// приложение ждёт для него кадр с началом ключа собеседника, а не с номером канала. Пока
// личка приезжала каналом, она показывалась в общей переписке, без отправителя, и
// «маршрут сообщения» приложению показать было не из чего.
void companionOnDirectText(const uint8_t* srcPub, const String& text, float snr,
                           uint8_t pathLen, uint32_t senderTs, uint8_t txtType) {
    if (msgCount >= MSG_QUEUE_MAX) {   // очередь полна — вытесняем самое старое
        msgHead = (msgHead + 1) % MSG_QUEUE_MAX;
        msgCount--;
    }
    QueuedMsg& m = msgQueue[(msgHead + msgCount) % MSG_QUEUE_MAX];
    memset(&m, 0, sizeof(m));
    m.kind = MSG_KIND_CONTACT;
    memcpy(m.pub6, srcPub, sizeof(m.pub6));
    m.txtType = txtType;
    m.pathLen = pathLen;
    long s4 = lround(snr * 4.0f);
    m.snr4 = (int8_t)(s4 < -128 ? -128 : (s4 > 127 ? 127 : s4));
    // Время — по часам ОТПРАВИТЕЛЯ, как в оригинале: приложение по нему расставляет
    // сообщения в переписке и отличает повтор от нового.
    m.ts = senderTs;
    strlcpy(m.text, text.c_str(), sizeof(m.text));
    msgCount++;
    logMsgTs("личка", m.ts);
    // Контакт слышали — обновляем отметку, чтобы ротация не вытеснила живого собеседника.
    int idx = contactFindPrefix(srcPub, 6);
    if (idx >= 0) {
        contacts[idx].lastmod = (uint32_t)time(NULL);
        contactTouch();
    }
    uint8_t push = PUSH_CODE_MSG_WAITING;
    sendFrameToApp(&push, 1);   // приложение заберёт сообщение командой 10
}

// Сырой принятый кадр — приложению. Оригинал (Dispatcher::checkRecv -> logRxRaw) отдаёт
// ВСЁ, что услышало радио, до дедупа и разбора: по этому потоку приложение видит, что его
// собственный пакет переиздал ретранслятор, и показывает это в переписке. Длинные кадры
// оригинал молча пропускает — кадр приложения короче эфирного.
void mcOnRawRx(const uint8_t* raw, int len, float snr, float rssi) {
    diagLogRxRecv++;
    // Эхо собственного пакета. У лички (TXT_MSG) и возврата маршрута (PATH) в конверте
    // после заголовка, байта разметки и САМОГО ПУТИ идут адресат и короткий хэш ИСТОЧНИКА.
    // Когда такой кадр ловится снова, его переиздал ретранслятор — это и есть событие, по
    // которому приложение ставит отметку. У канального кадра хэша источника нет вовсе:
    // своё сообщение в канал этим признаком не увидеть.
    //
    // Смещение считается по байту разметки, а не берётся постоянным. Жёсткий байт 3 верен
    // только для кадра с ПУСТЫМ путём, то есть для нашей собственной передачи, которой мы
    // не слышим (на передаче приёмник глух). У переизданного кадра ретранслятор дописал в
    // путь свой хэш, адресат с источником уехали вправо — и детектор не срабатывал ровно
    // на том единственном случае, ради которого написан.
    if (len >= 4) {
        const uint8_t ptype = (raw[0] >> 2) & 0x0F;
        const uint8_t pl = raw[1];
        const uint8_t hops = pl & 0x3F;
        const uint8_t hsize = ((pl >> 6) & 3) + 1;
        const int off = 2 + (int)hops * hsize;        // [заголовок][разметка][путь] -> адресат
        if ((ptype == PAYLOAD_TYPE_TXT_MSG || ptype == PAYLOAD_TYPE_PATH)
            && len >= off + 2 && raw[off + 1] == ownShortHash) {
            diagEchoOwn++;
            Serial.printf("[DIAG] эхо СВОЕГО кадра (тип %u, хопов %u), всего %lu\n",
                          (unsigned)ptype, (unsigned)hops, (unsigned long)diagEchoOwn);
        }
    }
    if (!bleConnected || len <= 0 || len + 3 > MAX_FRAME_SIZE) {
        diagLogRxLost++;
        Serial.printf("[DIAG] кадр %u Б приложению не ушёл, потерь %lu\n",
                      (unsigned)(len > 0 ? (size_t)len : 0), (unsigned long)diagLogRxLost);
        return;
    }
    uint8_t buf[MAX_FRAME_SIZE];
    long s4 = lround(snr * 4.0f);
    buf[0] = PUSH_CODE_LOG_RX_DATA;
    buf[1] = (uint8_t)(int8_t)(s4 < -128 ? -128 : (s4 > 127 ? 127 : s4));
    long r = lround(rssi);
    buf[2] = (uint8_t)(int8_t)(r < -128 ? -128 : (r > 127 ? 127 : r));
    memcpy(&buf[3], raw, len);
    sendFrameToApp(buf, len + 3);
}

// По одному контакту за проход главного цикла: пачка кадров подряд переполняет
// очередь уведомлений BLE, и приложение получает обрывки списка.
void contactsIterStep() {
    uint8_t buf[160];
    while (contactIterIdx < contactCount) {
        Contact& c = contacts[contactIterIdx++];
        if (c.lastmod <= contactIterSince) continue;      // приложение уже знает эту запись
        if (c.lastmod > contactIterNewest) contactIterNewest = c.lastmod;
        sendFrameToApp(buf, contactFrame(RESP_CODE_CONTACT, c, buf));
        return;
    }
    buf[0] = RESP_CODE_END_OF_CONTACTS;
    memcpy(&buf[1], &contactIterNewest, 4);
    sendFrameToApp(buf, 5);
    contactIterIdx = -1;
}

void companionOnAdvert(const uint8_t* pub, const uint8_t* app, int applen,
                       uint8_t pathLen, const uint8_t* path) {
    uint8_t advFlags = applen > 0 ? app[0] : 0;
    uint8_t type = advFlags & 0x0F;
    // Имя лежит не на втором байте, а после всех присутствующих полей. Раньше мы брали
    // его со смещения 1 всегда, и у узлов с координатами в начало имени попадали байты
    // широты и долготы — в приложении это выглядело как мусор перед именем.
    int o = 1;
    int32_t lat = 0, lon = 0;
    bool hasLoc = false;
    if (advFlags & ADV_LATLON_MASK) {
        if (o + 8 <= applen) {
            memcpy(&lat, &app[o], 4);
            memcpy(&lon, &app[o + 4], 4);
            hasLoc = true;
        }
        o += 8;
    }
    if (advFlags & ADV_FEAT1_MASK) o += 2;
    if (advFlags & ADV_FEAT2_MASK) o += 2;
    char nm[32];
    memset(nm, 0, sizeof(nm));
    if ((advFlags & ADV_NAME_MASK) && o < applen) {
        int n = applen - o;
        if (n > 31) n = 31;
        memcpy(nm, &app[o], n);
    }

    int idx = contactFind(pub);
    bool isNew = (idx < 0);
    if (isNew) {
        if (contactCount >= COMPANION_MAX_CONTACTS) {
            // РОТАЦИЯ. Память узла кончилась — освобождаем место под новый узел, вытесняя
            // самый давно не слышанный контакт. Избранные (младший бит флагов, его ставит
            // само приложение) не трогаем никогда: их владелец отметил руками, и потерять
            // их из-за случайного адверта прохожего нельзя. Так же устроен оригинал
            // (BaseChatMesh::allocateContactSlot).
            idx = -1;
            uint32_t oldest = 0xFFFFFFFF;
            for (int k = 0; k < contactCount; k++) {
                if (contacts[k].flags & 0x01) continue;   // избранный — не вытесняем
                if (contacts[k].lastmod < oldest) { oldest = contacts[k].lastmod; idx = k; }
            }
            if (idx < 0) {
                // Вытеснять нечего: все контакты избранные. Молчать об этом нельзя —
                // приложение должно сказать владельцу, что новые узлы больше не
                // запоминаются, иначе узел просто «перестаёт видеть сеть».
                Serial.println("[ADV] память контактов полна, все избранные — новый не добавлен");
                if (blePaired) {
                    uint8_t full = PUSH_CODE_CONTACTS_FULL;
                    sendFrameToApp(&full, 1);
                }
                return;
            }
            Serial.printf("[ADV] вытеснен контакт <%02X> %s (не слышали дольше всех)\n",
                          contacts[idx].pub[0], contacts[idx].name);
            if (blePaired) {
                // Приложение держит свою копию списка: без этого кадра оно продолжало бы
                // показывать контакт, которого на узле уже нет, и слать ему сообщения.
                uint8_t del[1 + 32];
                del[0] = PUSH_CODE_CONTACT_DELETED;
                memcpy(&del[1], contacts[idx].pub, 32);
                sendFrameToApp(del, sizeof(del));
            }
        } else {
            idx = contactCount++;
        }
        memset(&contacts[idx], 0, sizeof(Contact));
        memcpy(contacts[idx].pub, pub, 32);
        contacts[idx].outPathLen = 0xFF;                  // пути не знаем — только флудом
    }
    Contact& c = contacts[idx];
    // Путь мог поменяться: контакт перекочевал на другой ретранслятор. В этом случае
    // приложению нужен пуш 0x81 — по нему оно переспросит команду 42 и обновит маршрут.
    bool pathChanged = false;
    uint8_t oldLen = c.advPathLen;
    uint8_t oldHops = oldLen & 0x3F, oldHsize = (oldLen >> 6) + 1;
    uint16_t oldBytes = (uint16_t)oldHops * oldHsize;
    c.type  = type;
    // c.flags — это флаги контакта в приложении (например «избранный»), а не признаки
    // адверта: их выставляет само приложение командой 9, и затирать их нельзя.
    if (nm[0]) strncpy(c.name, nm, sizeof(c.name) - 1);
    if (hasLoc) { c.lat = lat; c.lon = lon; }
    c.lastAdvert = c.lastmod = (uint32_t)time(NULL);
    // Путь запоминаем как пришёл: приложение спрашивает его командой 42, чтобы показать,
    // через каких соседей слышно узел.
    uint8_t hops = pathLen & 0x3F, hsize = (pathLen >> 6) + 1;
    uint16_t bytes = (uint16_t)hops * hsize;
    if (path != nullptr && bytes <= sizeof(c.advPath)) {
        if (oldBytes <= sizeof(c.advPath)) {   // сравниваем только если и старый путь читаем
            pathChanged = (c.advPathLen != pathLen) || memcmp(c.advPath, path, min(oldBytes, bytes)) != 0;
        }
        c.advPathLen = pathLen;
        memcpy(c.advPath, path, bytes);
    }
    contactTouch();
    Serial.printf("[ADV] %s контакт <%02X> %s\n", isNew ? "новый" : "известный", pub[0], c.name);

    if (!blePaired) return;
    uint8_t buf[160];
    if (isNew) {
        sendFrameToApp(buf, contactFrame(PUSH_CODE_NEW_ADVERT, c, buf));
    } else if (pathChanged) {
        // Маршрут к знакомому контакту изменился — шлём 0x81, приложение переспросит путь
        buf[0] = PUSH_CODE_PATH_UPDATED;
        memcpy(&buf[1], c.pub, 32);
        sendFrameToApp(buf, 1 + 32);
        Serial.printf("[ADV] путь к контакту <%02X> изменился\n", pub[0]);
    } else {
        buf[0] = PUSH_CODE_ADVERT;
        memcpy(&buf[1], c.pub, 32);
        sendFrameToApp(buf, 33);
    }
}

void appChannelsSave();   // определена ниже, вызывается при вступлении в канал

#endif
