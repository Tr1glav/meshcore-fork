#ifndef ST7789_H
#define ST7789_H

// Компактный драйвер ST7789 (панель 240x320, ландшафт через MADCTL) поверх
// Adafruit_GFX. API совместим с SysOled/Adafruit_SSD1306 для наших вызовов
// (begin/dim/setPower/setBrightness/clearDisplay/display/всё из Adafruit_GFX).
//
// Пиксели сначала пишутся в фреймбуфер RGB565 в PSRAM (у T-Deck 8 МБ), затем
// одна передача всей области по SPI — так цена перерисовки не зависит от числа
// примитивов, а рисовать можно сколько угодно без мерцания.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include "cyrillic.h"

#define ST7789_TFTWIDTH   240
#define ST7789_TFTHEIGHT  320

// Регистры команды (нужные для инициализации)
#define ST7789_SWRESET    0x01
#define ST7789_SLPOUT     0x11
#define ST7789_NORON      0x13
#define ST7789_INVON      0x21
#define ST7789_GAMSET     0x26
#define ST7789_DISPON     0x29
#define ST7789_CASET      0x2A
#define ST7789_RASET      0x2B
#define ST7789_RAMWR      0x2C
#define ST7789_COLMOD     0x3A
#define ST7789_MADCTL     0x36
#define ST7789_MADCTL_MY  0x80
#define ST7789_MADCTL_MX  0x40
#define ST7789_MADCTL_MV  0x20
// BGR→RGB переключение цвета (0x08): не ставим, панель T-Deck собрана RGB-порядком

class RusST7789 : public Adafruit_GFX {
public:
  RusST7789(int16_t w, int16_t h, int8_t csPin, int8_t dcPin, int8_t rstPin,
            int8_t blPin)
      : Adafruit_GFX(w, h), _cs(csPin), _dc(dcPin), _rst(rstPin), _bl(blPin) {}

  // vcc/addr игнорируются — это SPI-панель. Настраивает пины, СПИ-бус уже
  // инициализирован main.cpp (те же SCK/MOSI/MISO, что и для SX1262), аллоцирует
  // фреймбуфер в PSRAM и запускает панель.
  bool begin(uint8_t vcc, uint8_t addr) {
    (void)vcc; (void)addr;
    if (_cs >= 0) { pinMode(_cs, OUTPUT); digitalWrite(_cs, HIGH); }
    if (_dc >= 0) pinMode(_dc, OUTPUT);
    if (_rst >= 0) { pinMode(_rst, OUTPUT); }
    if (_bl  >= 0) {
      // ШИМ для подсветки: GPIO42 у T-Deck выводит 3.3..0 В (активный HIGH).
      // Канал 0, частота 5 кГц — достаточно для подсветки без жужжания.
      ledcSetup(0, 5000, 8);
      ledcAttachPin(_bl, 0);
      ledcWrite(0, _brightness);
    }

    _buf = (uint16_t*)heap_caps_malloc((size_t)_width * _height * 2, MALLOC_CAP_SPIRAM);
    if (!_buf) _buf = (uint16_t*)malloc((size_t)_width * _height * 2);
    if (!_buf) return false;

    if (_rst >= 0) {
      digitalWrite(_rst, LOW);
      delay(10);
      digitalWrite(_rst, HIGH);
    } else {
      // Без пина RESET сбрасываем контроллер командой SWRESET
    }
    delay(20);
    command(ST7789_SWRESET);
    delay(120);
    command(ST7789_SLPOUT);            // выходим из sleep
    delay(120);

    uint8_t madctl = _madctlValue();
    command(ST7789_MADCTL);
    spiData(&madctl, 1);
    command(ST7789_COLMOD);            // 65K, 16 бит на пиксель
    uint8_t cm = 0x55;
    spiData(&cm, 1);
    command(ST7789_INVON);
    command(ST7789_NORON);
    command(ST7789_DISPON);

    _initialized = true;
    clearDisplay();
    display();
    return true;
  }

  // Подсветка: dim гасит до 10%, включая обратно к сохранённой яркости.
  void dim(bool d) {
    if (_bl < 0) return;
    ledcWrite(0, d ? (uint8_t)(_brightness * 0.10f) : _brightness);
  }

  // Выключить/включить панель (сильнее, чем подсветка): отключаем LED-питание и
  // память контроллера, чтобы TFT не потреблял мА при простое.
  void setPower(bool on) {
    if (!_initialized) return;
    command(on ? ST7789_DISPON : ST7789_SLPOUT);
    if (_bl >= 0) ledcWrite(0, on ? _brightness : 0);
  }

  void invertDisplay(bool i) { command(i ? ST7789_INVON : 0x20); }

  void setBrightness(uint8_t v) {
    _brightness = v;
    if (_bl >= 0) ledcWrite(0, v);
  }

  void clearDisplay(void) {
    if (_buf) memset(_buf, 0, (size_t)_width * _height * 2);
  }

  void display(void) {
    if (!_initialized || !_buf) return;
    _setAddrWindow(0, 0, _width - 1, _height - 1);
    command(ST7789_RAMWR);
    SPI.beginTransaction(_spi);
    digitalWrite(_cs, LOW);
    digitalWrite(_dc, HIGH);
    // Область до 320x240: одна передача без порезки на чанки — фреймбуфер в PSRAM,
    // контроллер SPI работает с DMA.
    SPI.writeBytes((uint8_t*)_buf, (size_t)_width * _height * 2);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (!_buf) return;
    if ((x < 0) || (y < 0) || (x >= _width) || (y >= _height)) return;
    // Код рисует монохромными константами SSD1306_WHITE/BLACK (0/1):
    // превращаем их в настоящий белый/чёрный RGB565, а прямые 16-бит цвета — как есть.
    if (color == 1)       color = 0xFFFF;
    else if (color == 0)  color = 0x0000;
    _buf[(size_t)y * _width + x] = color;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!_buf || w <= 0 || h <= 0) return;
    if (color == 1)       color = 0xFFFF;
    else if (color == 0)  color = 0x0000;
    for (int16_t r = 0; r < h; r++) {
      if (y + r < 0 || y + r >= _height) continue;
      for (int16_t c = 0; c < w; c++) {
        if (x + c < 0 || x + c >= _width) continue;
        _buf[(size_t)(y + r) * _width + (x + c)] = color;
      }
    }
  }

  // UTF-8 -> CP866 decode hook: ASCII -> classic font, Cyrillic -> 6x8 table.
  size_t write(uint8_t c) override {
    return utf8cp866::processByte(*this, _u8, c, textcolor, 1, 1);
  }

private:
  const SPISettings _spi{40000000, MSBFIRST, SPI_MODE0};

  utf8cp866::Decoder _u8;
  int8_t _cs, _dc, _rst, _bl;
  uint16_t* _buf = nullptr;
  uint8_t _brightness = 255;
  bool _initialized = false;

  // Ландшафт 320x240: MADCTL с MV+MY поворачивает физическую панель 240x320.
  // Портрет (w<h) — без поворота.
  uint8_t _madctlValue() {
    if (_width > _height) return ST7789_MADCTL_MY | ST7789_MADCTL_MV;
    return 0x00;
  }

  void command(uint8_t c) {
    SPI.beginTransaction(_spi);
    digitalWrite(_cs, LOW);
    digitalWrite(_dc, LOW);
    SPI.transfer(c);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  void spiData(const uint8_t* p, size_t n) {
    SPI.beginTransaction(_spi);
    digitalWrite(_cs, LOW);
    digitalWrite(_dc, HIGH);
    SPI.writeBytes(p, n);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  void _setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    command(ST7789_CASET);
    uint8_t d[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    spiData(d, 4);
    command(ST7789_RASET);
    uint8_t e[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    spiData(e, 4);
  }
};

#endif