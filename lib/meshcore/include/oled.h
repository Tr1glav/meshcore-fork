#pragma once

// ============================================================================
// OLED / display обьект платы. Только для файлов прошивки — mesh-network-core
// сюда не заглядывает (у него чисто макросные board_config.h, а рендеринг идёт
// через mcUi*-хуки). Подключён из display.h.
//
// Механизм инстанцирования: тот translation unit, где должен жить объект display,
// #define DISPLAY_DEFINE_HERE ПЕРЕД #include "config.h" (см. display.cpp);
// везде же это просто extern. Нужен ровно один объект: любой второй инстанс дал
// бы дубликат символа на линковке.
// ============================================================================

#include "config.h"

#include <Adafruit_GFX.h>
#include "cyrillic.h"

#ifndef HAS_OLED
  #define HAS_OLED 1
#endif

#if HAS_OLED

  #ifndef OLED_DRIVER_SH1106
    #define OLED_DRIVER_SH1106 1
  #endif
  #if OLED_DRIVER_SH1106
    #include "sysoled.h"          // own compact SH1106 driver (Heltec V4 panels)
  #else
    #include <Adafruit_SSD1306.h>
    // Same UTF-8 -> CP866 hook, applied to the Adafruit SSD1306 driver.
    class RusSSD1306 : public Adafruit_SSD1306 {
    public:
      using Adafruit_SSD1306::Adafruit_SSD1306;
      size_t write(uint8_t c) override {
        return utf8cp866::processByte(*this, _u8, c, 1, 1, 1);
      }
      void setPower(bool on) { ssd1306_command(on ? 0xAF : 0xAE); }
      // Яркость: контраст (0x81) и уровень отключения VCOMH (0xDB). Штатный dim(false)
      // ставит контраст всего 0xCF, а 0xDB по умолчанию 0x20 — панель светит заметно
      // слабее, чем может.
      void setBrightness(uint8_t v) {
        // Порядок и формула как в библиотеке, которую используют Meshtastic и MeshCore:
        // яркость раскладывается на контраст, предзаряд и уровень VCOMH, а в конце
        // обязательно идут resume/normal/display-on — без них панель после записи
        // регистров может остаться погашенной.
        uint8_t contrast  = (v < 128) ? (uint8_t)(v * 1.171f) : (uint8_t)(v * 1.171f - 43);
        uint8_t precharge = (v == 0) ? 0x00 : 0xF1;
        uint8_t comdetect = v / 8;
        ssd1306_command(0xD9); ssd1306_command(precharge);
        ssd1306_command(0x81); ssd1306_command(contrast);
        ssd1306_command(0xDB); ssd1306_command(comdetect);
        ssd1306_command(0xA4);   // выводить содержимое ОЗУ
        ssd1306_command(0xA6);   // без инверсии
        ssd1306_command(0xAF);   // экран включён
      }
    private:
      utf8cp866::Decoder _u8;
    };
  #endif

#else // no display -> stub with the same API

  class StubDisplay : public Adafruit_GFX {
  public:
    StubDisplay(int16_t w, int16_t h) : Adafruit_GFX(w, h) {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
      (void)x; (void)y; (void)color;
    }
    bool begin(uint8_t vcc, uint8_t addr) { (void)vcc; (void)addr; return true; }
    void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void clearDisplay(void) {}
    void display(void) {}
    void dim(bool) {}
    void setBrightness(uint8_t) {}
    void setPower(bool) {}
  };

#endif // HAS_OLED

// Some draw calls use these SSD1306 constants; define them for the
// SH1106/stub paths where the Adafruit_SSD1306 header is not included.
#ifndef SSD1306_WHITE
  #define SSD1306_WHITE 1
#endif
#ifndef SSD1306_BLACK
  #define SSD1306_BLACK 0
#endif
#ifndef SSD1306_SWITCHCAPVCC
  #define SSD1306_SWITCHCAPVCC 1
#endif

// Instantiate the display object exactly once (see top-of-file comment).
#ifdef DISPLAY_DEFINE_HERE
  #if HAS_OLED
    #if OLED_DRIVER_SH1106
      SysOled display(SCREEN_WIDTH, SCREEN_HEIGHT);
    #else
      RusSSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
    #endif
  #else
    StubDisplay display(SCREEN_WIDTH, SCREEN_HEIGHT);
  #endif
#else
  #if HAS_OLED
    #if OLED_DRIVER_SH1106
      extern SysOled display;
    #else
      extern RusSSD1306 display;
    #endif
  #else
    extern StubDisplay display;
  #endif
#endif