#include "config.h"
#include "globals.h"
#include "companion.h"
#include "companion_internal.h"
#include "mesh.h"
#include "appconfig.h"
#include "display.h"
#include "radio.h"        // radioAirtimeMs: оценка времени кадра для метки est в CMD_SEND_TXT_MSG

// Оценка времени до подтверждения доставки — как в оригинальном companion_radio
// (MyMesh.cpp): база плюс копии флуда, каждая по времени кадра в эфире. Приложение ждёт
// ACK ровно столько, сколько мы ему здесь назвали: занизишь — оно перестанет ждать раньше,
// чем придёт подтверждение, и сообщение останется без галочки.
#define SEND_TIMEOUT_BASE_MS       500
#define FLOOD_SEND_TIMEOUT_FACTOR  16.0f

// ===== Разбор кадров телефонного приложения =====
// Выделено из companion.cpp: команды приложения, ответы на них и главный тик, который
// забирает кадры из очереди. Транспорт BLE, контакты и каналы остались в companion.cpp,
// общее между частями объявлено в companion_internal.h.

#if FEATURE_COMPANION

static uint8_t appVer = 0;      // версия протокола приложения из DEVICE_QUERY
static uint8_t out[MAX_FRAME_SIZE + 8];

// ===== Короткие ответы =====
// В каждой ветке один и тот же ответ повторялся тремя строками: код, код ошибки, отправка.
// 16 раз на 400 строк switch. Теперь ветка читается как «что проверяем», а упаковка ответа
// живёт здесь.
static void sendRes(uint8_t code) {
    out[0] = code;
    sendFrameToApp(out, 1);
}

static void sendErr(uint8_t code) {
    out[0] = RESP_CODE_ERR;
    out[1] = code;
    sendFrameToApp(out, 2);
}

// ===== Границы буферов =====
// Две проверки, где длина приходит извне, а размер буфера — константа. Обе вынесены
// отдельно от разбора кадров, потому что именно их стоит проверять на хосте: в одном
// случае длина приезжает из NVS (то есть могла достаться от другой версии прошивки), в
// другом — из очереди сообщений. В selftest.py обе гоняются на всём возможном входе под
// asan/ubsan.

// Длина пути рекламы упакована в байт: хопы в младших 6 битах, размер хэша хопа — в
// старших. Произведение при максимальных хопах и двубайтовом хэше достигает 252, а в кадр
// столько не влезает, поэтому лишнее отбрасываем и отдаём пустой путь. Раньше это была
// арифметика посреди ветки CMD_GET_ADVERT_PATH, где её никто не проверял.
static uint16_t advertPathBytes(uint8_t advPathLen, size_t cap) {
    uint8_t hops = advPathLen & 0x3F, hsize = (advPathLen >> 6) + 1;
    uint16_t bytes = (uint16_t)hops * hsize;
    if (bytes > cap) return 0;
    return bytes;
}

// Копирует текст в кадр, не давая ему выйти за предел cap, и возвращает длину кадра,
// которая гарантированно не больше cap. Вызывается из CMD_SYNC_NEXT_MESSAGE, где текст
// занимает хвост ответа целиком: без арифметики ниже длинное сообщение дописывалось
// поверх следующего кадра.
static size_t putTextBounded(uint8_t* dst, size_t used, size_t cap, const char* src, size_t srcLen) {
    if (used >= cap) return cap;            // кадр уже полон: писать некуда и нечего
    size_t room = cap - used;
    size_t n = (srcLen < room) ? srcLen : room;
    memcpy(dst + used, src, n);
    return used + n;
}

static void handleFrame(const uint8_t* f, size_t len) {
    int i = 0;
    switch (f[0]) {
    case CMD_APP_START: {
        // [01][резерв 7][имя приложения] — версии протокола здесь нет
        out[i++] = RESP_CODE_SELF_INFO;
        out[i++] = ADV_TYPE_CHAT;
        out[i++] = (uint8_t)cfg.loraTx;
        out[i++] = 22;                       // предел мощности платы
        memcpy(&out[i], bot_pub, 32); i += 32;
        int32_t zero = 0;
        memcpy(&out[i], &zero, 4); i += 4;   // широта: не знаем
        memcpy(&out[i], &zero, 4); i += 4;   // долгота
        out[i++] = 0;                        // multi_acks
        out[i++] = 0;                        // advert_loc_policy
        out[i++] = 0;                        // телеметрия запрещена
        out[i++] = 1;                        // контакты добавляются вручную
        uint32_t freq = (uint32_t)(cfg.loraFreq * 1000.0f);
        memcpy(&out[i], &freq, 4); i += 4;
        uint32_t bw = (uint32_t)(cfg.loraBw * 1000.0f);
        memcpy(&out[i], &bw, 4); i += 4;
        out[i++] = (uint8_t)cfg.loraSf;
        out[i++] = (uint8_t)cfg.loraCr;
        size_t nlen = cfg.name.length();
        if (nlen > 32) nlen = 32;
        memcpy(&out[i], cfg.name.c_str(), nlen); i += nlen;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_DEVICE_QUERY: {
        // Версию протокола приложение сообщает именно здесь, вторым байтом. Читать её из
        // APP_START нельзя: там это поле зарезервировано, и мы получали ноль — то есть
        // считали приложение древним и слали ему кадр сообщения без качества связи и пути.
        if (len >= 2) appVer = f[1];
        out[i++] = RESP_CODE_DEVICE_INFO;
        out[i++] = COMPANION_VER_CODE;
        // В этом кадре — вместимость, а не текущее число: у оригинала здесь
        // MAX_CONTACTS / 2, то есть предел в единицах по два.
        out[i++] = COMPANION_MAX_CONTACTS / 2;
        out[i++] = MAX_CHANNELS;
        uint32_t pin = 0;
        memcpy(&out[i], &pin, 4); i += 4;
        memset(&out[i], 0, 12);
        strncpy((char*)&out[i], __DATE__, 11); i += 12;
        memset(&out[i], 0, 40);
        strncpy((char*)&out[i], "meshcore-fork", 39); i += 40;
        memset(&out[i], 0, 20);
        strncpy((char*)&out[i], FW_VERSION, 19); i += 20;
        out[i++] = 0;                        // ретрансляция выключена
        out[i++] = PATH_HASH_SIZE - 1;       // режим хэша пути: 0 — 1 байт, 1 — 2 байта
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_DEVICE_TIME: {
        out[i++] = RESP_CODE_CURR_TIME;
        uint32_t now = (uint32_t)time(NULL);
        memcpy(&out[i], &now, 4); i += 4;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SET_DEVICE_TIME: {
        if (len >= 5) {
            uint32_t epoch;
            memcpy(&epoch, &f[1], 4);
            if (epoch > (uint32_t)BUILD_UNIX_TIME) {
                struct timeval tv = { (time_t)epoch, 0 };
                settimeofday(&tv, NULL);
                timeSyncMs = millis();
            }
        }
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_GET_CONTACTS: {
        // Необязательный аргумент «since»: приложение просит только изменившееся
        contactIterSince = 0;
        if (len >= 5) memcpy(&contactIterSince, &f[1], 4);
        out[i++] = RESP_CODE_CONTACTS_START;
        uint32_t count = contactCount;       // общее число, не отфильтрованное
        memcpy(&out[i], &count, 4); i += 4;
        sendFrameToApp(out, i);
        contactIterIdx = 0;                  // сами записи уходят из главного цикла
        contactIterNewest = 0;
        break;
    }
    case CMD_ADD_UPDATE_CONTACT: {
        // [09][ключ 32][тип][флаги][длина пути][путь 64][имя 32][время 4] + необязательные
        if (len < 1 + 32 + 3 + 64 + 32 + 4) {
            sendErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        const uint8_t* pub = &f[1];
        int idx = contactFind(pub);
        if (idx < 0) {
            if (contactCount >= COMPANION_MAX_CONTACTS) {
                sendErr(ERR_CODE_TABLE_FULL);
                break;
            }
            idx = contactCount++;
            memset(&contacts[idx], 0, sizeof(Contact));
        }
        Contact& c = contacts[idx];
        int o = 1;
        memcpy(c.pub, &f[o], 32); o += 32;
        c.type = f[o++];
        c.flags = f[o++];
        c.outPathLen = f[o++];
        memcpy(c.outPath, &f[o], 64); o += 64;
        memset(c.name, 0, sizeof(c.name));
        memcpy(c.name, &f[o], 31); o += 32;
        memcpy(&c.lastAdvert, &f[o], 4); o += 4;
        if (len >= (size_t)o + 8) {
            memcpy(&c.lat, &f[o], 4); o += 4;
            memcpy(&c.lon, &f[o], 4); o += 4;
        }
        c.lastmod = (uint32_t)time(NULL);
        contactTouch();
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_RESET_PATH: {
        // Забыть маршрут до узла: следующая отправка пойдёт флудом и путь построится заново
        int idx = (len >= 1 + 32) ? contactFind(&f[1]) : -1;
        if (idx < 0) {
            sendErr(ERR_CODE_NOT_FOUND);
            break;
        }
        contacts[idx].outPathLen = 0xFF;
        contactTouch();
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_REMOVE_CONTACT: {
        int idx = (len >= 33) ? contactFind(&f[1]) : -1;
        if (idx < 0) {
            sendErr(ERR_CODE_NOT_FOUND);
            break;
        }
        for (int k = idx; k + 1 < contactCount; k++) contacts[k] = contacts[k + 1];
        contactCount--;
        contactTouch();
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_GET_ADVERT_PATH: {
        // [42][резерв][ключ] -> [22][когда слышали 4][длина пути][хэши ретрансляторов]
        //
        // РАНЬШЕ ЗДЕСЬ ТРЕБОВАЛИСЬ ВСЕ 32 БАЙТА КЛЮЧА И СРАВНИВАЛИСЬ ВСЕ 32 — и из-за этого
        // приложение не показывало маршрут ни для одного узла, слышанного через нашу
        // прошивку. Приложение спрашивает путь по НАЧАЛУ ключа: у хопа в маршруте полного
        // ключа у него нет и быть не может, в пути передаются хэши ретрансляторов. Любой
        // такой запрос мы отвергали как «не найдено». Оригинал держит для этого отдельную
        // таблицу, ключуемую семью байтами, и отвечает.
        //
        // Рядом, в CMD_GET_CONTACT_BY_KEY, префикс обрабатывался правильно с самого начала —
        // то есть про это поведение приложения мы знали, просто не применили здесь.
        int idx = (len >= 2) ? contactFindPrefix(&f[2], len - 2) : -1;
        if (idx < 0) {
            sendErr(ERR_CODE_NOT_FOUND);
            break;
        }
        const Contact& c = contacts[idx];
        // Длина пришла из NVS: запись могла остаться от другой версии, а в кадр влезает
        // ограниченно. advertPathBytes() отдаёт 0 вместо невлезающего пути, проверяется
        // на хосте на всех 256 значениях байта.
        uint16_t bytes = advertPathBytes(c.advPathLen, sizeof(c.advPath));
        out[i++] = RESP_CODE_ADVERT_PATH;
        memcpy(&out[i], &c.lastmod, 4); i += 4;
        out[i++] = c.advPathLen;
        memcpy(&out[i], c.advPath, bytes); i += bytes;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_CONTACT_BY_KEY: {
        // Приложение может прислать только начало ключа. Поиск общий с CMD_GET_ADVERT_PATH:
        // правило «сколько прислали, столько и сравниваем» должно быть одно на оба запроса.
        int idx = (len >= 2) ? contactFindPrefix(&f[1], len - 1) : -1;
        if (idx < 0) {
            sendErr(ERR_CODE_NOT_FOUND);
        } else {
            sendFrameToApp(out, contactFrame(RESP_CODE_CONTACT, contacts[idx], out));
        }
        break;
    }
    case CMD_SET_CHANNEL: {
        // [32][номер][имя 32][ключ 16]
        if (len < 2 + 32 + 16) {
            sendErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        char nm[33];
        memset(nm, 0, sizeof(nm));
        memcpy(nm, &f[2], 32);
        // Каналы из настроек устройства приложению не отдаём: сохранить такую правку
        // мы не можем (их держит конфиг), и после перезагрузки она молча откатится.
        if (f[1] < appChanBase) {
            Serial.printf("[CH] канал %u задан настройками устройства, из приложения не меняем\n", f[1]);
            sendErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        int idx = channelSetSlot(f[1], nm, &f[2 + 32]);
        if (idx < 0) {
            sendErr(ERR_CODE_TABLE_FULL);
            break;
        }
        appChannelsSave();
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_SET_ADVERT_NAME: {
        if (len < 2) {
            sendErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        String nn;
        // Лимит тот же, что у консоли и страницы (CFG_NAME_MAX). Раньше здесь брались 32
        // символа, а cfgLoad при следующем старте обрезал имя до 31 — узел возвращался в
        // сеть под другим именем, а вместе с именем менялась и его личность.
        for (size_t k = 1; k < len && (int)nn.length() < CFG_NAME_MAX; k++) nn += (char)f[k];
        cfg.name = nn;
        cfgSave();
        // Личность к имени больше не привязана (seed лежит в NVS), пересчитывать нечего.
        // А вот сети о новом имени сказать стоит сразу, не дожидаясь планового адверта.
        sendAdvert(ADV_ROUTE_FLOOD);
        Serial.printf("[BLE] имя узла изменено на «%s»\n", cfg.name.c_str());
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_GET_CHANNEL: {
        uint8_t idx = (len >= 2) ? f[1] : 0;
        // Приложение перебирает каналы подряд, пока не получит ошибку. Если отвечать
        // описанием канала на любой индекс, перебор не кончается никогда: телефон
        // бесконечно спрашивает следующий канал, а мы бесконечно отвечаем.
        if (idx >= numChannels) {
            sendErr(ERR_CODE_NOT_FOUND);
            break;
        }
        out[i++] = RESP_CODE_CHANNEL_INFO;
        out[i++] = idx;
        memset(&out[i], 0, 32);
        strncpy((char*)&out[i], channels[idx].name, 31);
        i += 32;
        memcpy(&out[i], channels[idx].secret, 16);
        i += 16;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SEND_TXT_MSG: {
        // [02][тип][попытка][время 4][начало ключа 6][текст]
        if (len < 14) {
            sendErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        uint8_t txtType = f[1];
        uint8_t attempt = f[2];               // номер попытки, 2 бита — входит в хэш ACK
        uint32_t msgTs = 0;
        memcpy(&msgTs, &f[3], 4);             // метка времени приложения — тоже в хэш ACK
        // Приложение адресует личное сообщение шестью байтами ключа. Поиск общий со всеми
        // остальными запросами: именно расхождение между соседними местами, которые ищут
        // контакт каждый по-своему, дважды за день приводило к поломке — сначала маршрут не
        // показывался, потом контакт не находился.
        int idx = contactFindPrefix(&f[7], 6);

        String text;
        for (size_t k = 13; k < len; k++) text += (char)f[k];
        if (text.length() > DM_TEXT_MAX) {
            text.remove(DM_TEXT_MAX);
            Serial.printf("[BLE] текст личного сообщения обрезан до %u байт\n",
                          (unsigned)DM_TEXT_MAX);
        }

        bool sent = false;
        uint8_t expAck[4];
        memset(expAck, 0, sizeof(expAck));
        int fl = 0;
        if (idx >= 0 && txtType == 0 && text.length() > 0) {   // 0 — обычный текст
            uint8_t frame[256];
            // Хэш подтверждения запоминаем здесь же: ждать его будем мы, а посчитать его
            // может только тот, кто собрал кадр, — по тому же открытому тексту. Метку
            // времени и номер попытки ИЗ КОМАНДЫ ПРИЛОЖЕНИЯ отдаём в сборщик кадра: они
            // входят в открытый текст, а значит и в хэш ACK. Раньше кадр собирался с
            // time(NULL)/0 — подтверждение приходило на другой хэш, и метка в ответе ниже
            // была «вечный ноль», по которому приложение не могло сопоставить ответ.
            fl = buildPrivateTextFrame(contacts[idx].pub[0], contacts[idx].pub,
                                       text, frame, sizeof(frame), expAck, msgTs, attempt);
            if (fl > 0) ackExpect(expAck);
            // floodSendQueued, а не floodSend: приложение ждёт подтверждения этой команды,
            // и выходить в эфир ДО ответа значит задержать ответ на ожидание канала плюс
            // время кадра — до двух секунд. Снаружи это «приложение притормаживает».
            if (fl > 0) { floodSendQueued(-1, frame, fl); sent = true; }
        }
        if (!sent) {
            sendErr((idx < 0) ? ERR_CODE_NOT_FOUND : ERR_CODE_ILLEGAL_ARG);
            break;
        }
        // Метка подтверждения — НАСТОЯЩИЙ хэш, который придёт на это сообщение: приложение
        // ждёт подтверждение ровно по этой метке. Оценка времени — как в оригинале: база
        // плюс копии флуда, каждая по длине кадра в эфире. Раньше здесь стояли ноль и 3000 —
        // приложение ждало подтверждение по метке «всегда ноль», которое не приходит никогда.
        out[i++] = RESP_CODE_SENT;
        out[i++] = 1;                        // ушло флудом
        memcpy(&out[i], expAck, 4); i += 4;
        uint32_t est = SEND_TIMEOUT_BASE_MS + (uint32_t)(FLOOD_SEND_TIMEOUT_FACTOR *
                                                         (float)radioAirtimeMs(fl));
        memcpy(&out[i], &est, 4); i += 4;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SEND_CHANNEL_TXT_MSG: {
        // [03][00][канал][время 4][текст]
        if (len < 7) {
            sendErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        uint8_t ch = f[2];
        // Метка времени приложения из команды. Её же приложение ищет в сыром журнале 0x88,
        // чтобы показать маршрут собственного сообщения, поэтому в кадр обязана лечь
        // именно она, а не часы узла (оригинал: MyMesh -> sendGroupMessage(msg_timestamp)).
        uint32_t chMsgTs = 0;
        memcpy(&chMsgTs, &f[3], 4);
        // Кадр не оканчивается нулём, поэтому длину текста берём из длины кадра:
        // иначе в сообщение попадал бы мусор, оставшийся в буфере от прошлой команды.
        String text;
        for (size_t k = 7; k < len; k++) text += (char)f[k];
        bool sent = false;
        if (ch < numChannels && text.length() > 0) {
            uint8_t frame[256];
            int fl = buildGroupFrameFlood(ch, text, frame, sizeof(frame), NULL, chMsgTs);
            // См. CMD_SEND_TXT_MSG: ответ приложению не должен ждать эфира.
            if (fl > 0) { floodSendQueued(ch, frame, fl); sent = true; }
        }
        if (sent) {
            // Оригинал отвечает одним байтом согласия. Мы отвечали кадром «отправлено»
            // с оценкой времени доставки — такого подтверждения приложение не ждёт, и
            // сообщение навсегда оставалось у него в состоянии «отправляется».
            sendRes(RESP_CODE_OK);
        } else {
            sendErr(ERR_CODE_NOT_FOUND);
        }
        break;
    }
    case CMD_SYNC_NEXT_MESSAGE: {
        if (msgCount == 0) {
            sendRes(RESP_CODE_NO_MORE_MESSAGES);
            break;
        }
        QueuedMsg& m = msgQueue[msgHead];
        const bool dm = (m.kind == MSG_KIND_CONTACT);
        // До версии 3 приложение не понимает полей качества связи — шлём короткий кадр
        if (appVer >= 3) {
            out[i++] = dm ? RESP_CODE_CONTACT_MSG_RECV_V3 : RESP_CODE_CHANNEL_MSG_RECV_V3;
            out[i++] = (uint8_t)m.snr4;
            out[i++] = 0; out[i++] = 0;      // зарезервировано
        } else {
            out[i++] = dm ? RESP_CODE_CONTACT_MSG_RECV : RESP_CODE_CHANNEL_MSG_RECV;
        }
        // Личку приложение раскладывает по переписке с конкретным узлом, и узнаёт его по
        // началу ключа — шести байтам. Групповое отличается от неё только этим полем:
        // вместо ключа номер канала.
        if (dm) { memcpy(&out[i], m.pub6, 6); i += 6; }
        else    { out[i++] = m.channelIdx; }
        out[i++] = m.pathLen;
        out[i++] = m.txtType;
        memcpy(&out[i], &m.ts, 4); i += 4;
        // Текст занимает хвост кадра целиком, поэтому копируется с пределом: putTextBounded
        // не даёт длинному сообщению дописать себя поверх следующего кадра. Предел здесь
        // MAX_FRAME_SIZE, а не sizeof(out) — в лишние восемь байт кадр не должен вылезать.
        i = (int)putTextBounded(out, (size_t)i, MAX_FRAME_SIZE, m.text, strlen(m.text));
        sendFrameToApp(out, i);
        msgHead = (msgHead + 1) % MSG_QUEUE_MAX;
        msgCount--;
        break;
    }
    case CMD_SEND_SELF_ADVERT: {
        // Второй байт: 1 — разослать по всей сети, иначе только ближайшим соседям
        sendAdvert((len >= 2 && f[1] == 1) ? ADV_ROUTE_FLOOD : ADV_ROUTE_DIRECT);
        sendRes(RESP_CODE_OK);
        break;
    }
    case CMD_GET_BATT_AND_STORAGE: {
        out[i++] = RESP_CODE_BATT_AND_STORAGE;
        uint16_t mv = (uint16_t)(batteryVoltage() * 1000.0f);
        memcpy(&out[i], &mv, 2); i += 2;
        uint32_t used = 0, total = 0;
        memcpy(&out[i], &used, 4); i += 4;
        memcpy(&out[i], &total, 4); i += 4;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_DEFAULT_FLOOD_SCOPE: {
        // Областей рассылки у нас нет. В оригинале ответ из одного байта, без имени и
        // ключа, как раз и означает «область не задана»; приложение переспрашивало эту
        // команду по кругу, пока получало отказ.
        out[i++] = RESP_CODE_DEFAULT_FLOOD_SCOPE;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SET_FLOOD_SCOPE_KEY: {
        // Команда просит либо сбросить область, либо слать без неё — мы всегда так и
        // делаем, так что согласие здесь не обещает ничего, чего мы не выполняем.
        sendRes(RESP_CODE_OK);
        break;
    }
    default:
        Serial.printf("[BLE] команда %u пока не поддержана\n", f[0]);
        sendErr(ERR_CODE_UNSUPPORTED_CMD);
        break;
    }
}

void companionTick() {
    // Сводка счётчиков потока 0x88 раз в 30 с: по ней видно, сколько сырых кадров приняло
    // радио, сколько ушло приложению, сколько потеряно и сколько раз эхом вернулся наш
    // собственный пакет (ретранслятор его переиздал). Строку на каждый кадр не печатаем —
    // она и так есть там, где кадр печатается.
    static unsigned long diagLastMs = 0;
    if ((long)(millis() - diagLastMs) >= 30000) {
        diagLastMs = millis();
        Serial.printf("[DIAG] 0x88: принято %lu, ушло в BLE %lu, потерь %lu, эхо своих %lu\n",
                      (unsigned long)diagLogRxRecv, (unsigned long)diagLogRxPushed,
                      (unsigned long)diagLogRxLost, (unsigned long)diagEchoOwn);
    }

    // Частая реклама держится ограниченное время: подключения уже не ждут, а объявляться
    // каждые 30 мс сутками незачем.
    if (bleAdvFastUntil != 0 && !bleConnected
        && (long)(millis() - bleAdvFastUntil) >= 0) {
        bleAdvFast(false);
    }

    if (contactsDirty && millis() - contactsDirtyMs > CONTACTS_SAVE_DELAY_MS) contactsSave();

    // ЗА ОДИН ПРОХОД ОБРАБАТЫВАЕТСЯ НЕСКОЛЬКО КОМАНД, а не ровно одна. Проход главного цикла
    // занимает до двух секунд (передача ждёт тишины в канале и держит кадр в эфире), а
    // приложение после подключения шлёт команды десятками: список контактов, каналы,
    // сообщения. Одна команда за проход превращала синхронизацию в минуты — снаружи это и
    // есть «компаньон не быстрый».
    //
    // Предел по ВРЕМЕНИ, а не по числу команд: команды разной цены, и отдать радио мы обязаны
    // не позже чем через COMPANION_TICK_BUDGET_MS, иначе отзывчивость приложения куплена
    // задержкой эфира.
    const unsigned long started = millis();
    static uint8_t frame[MAX_FRAME_SIZE];
    for (;;) {
        uint8_t n = 0;
        portENTER_CRITICAL(&inMux);
        if (inCount > 0) {
            n = inQueueLen[inHead];
            memcpy(frame, inQueue[inHead], n);
            inHead = (inHead + 1) % IN_QUEUE_MAX;
            inCount--;
        }
        uint32_t dropped = inDropped;
        inDropped = 0;
        portEXIT_CRITICAL(&inMux);

        if (dropped) {
            Serial.printf("[BLE] очередь команд переполнена, потеряно кадров: %u\n", dropped);
        }
        if (n == 0) break;
        Serial.printf("[BLE] <- команда %u, %u байт\n", frame[0], (unsigned)n);
        handleFrame(frame, n);
        if ((long)(millis() - started) >= COMPANION_TICK_BUDGET_MS) break;
    }

    // Список контактов тоже отдаётся порциями, а не по одной записи за проход: у узла в живой
    // сети их под сотню, и по одной за проход список уезжал к приложению секундами.
    while (contactIterIdx >= 0 && blePaired
           && (long)(millis() - started) < COMPANION_TICK_BUDGET_MS) {
        contactsIterStep();
    }
}

#endif // FEATURE_COMPANION
