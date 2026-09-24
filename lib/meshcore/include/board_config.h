#pragma once

// ============================================================================
// BOARD CONFIGURATION (multi-board support)
//
// All per-board differences live in platformio.ini build_flags and are mapped
// here onto the app-level macros used by main.cpp. Sensible defaults are the
// Heltec WiFi LoRa 32 V4.3 so a bare `pio run` just works.
//
// Per-board switches:
//   -DHAS_OLED=0        board has no display (serial-only build)
//   -DHAS_FEM=0         board has no external FEM/LNA module
//   -DOLED_DRIVER_SH1106=0   use Adafruit_SSD1306 instead of built-in SH1106 driver
//
// Pin flags (all optional, defaults = Heltec V4.3):
//   -DP_LORA_NSS/-DP_LORA_DIO_1/-DP_LORA_RESET/-DP_LORA_BUSY   SX1262 SPI
//   -DP_LORA_SCLK/-DP_LORA_MISO/-DP_LORA_MOSI                  SPI bus
//   -DPIN_BOARD_SDA/-DPIN_BOARD_SCL                            I2C
//   -DPIN_OLED_RESET                                           OLED reset GPIO
//   -DPIN_USER_BTN                                             user button (-1 = none)
//   -DPIN_VEXT_EN + -DPIN_VEXT_EN_ACTIVE                       peripheral power rail
//   -DP_LORA_PA_POWER/-DP_LORA_KCT8103L_PA_CSD/-DP_LORA_KCT8103L_PA_CTX  FEM pins
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// SX1262 SPI pins
// ---------------------------------------------------------------------------
#ifndef LORA_CS
  #if defined(P_LORA_NSS)
    #define LORA_CS   P_LORA_NSS
  #else
    #define LORA_CS   8
  #endif
#endif

#ifndef LORA_DIO1
  #if defined(P_LORA_DIO_1)
    #define LORA_DIO1 P_LORA_DIO_1
  #else
    #define LORA_DIO1 14
  #endif
#endif

#ifndef LORA_RST
  #if defined(P_LORA_RESET)
    #define LORA_RST  P_LORA_RESET
  #else
    #define LORA_RST  12
  #endif
#endif

#ifndef LORA_BUSY
  #if defined(P_LORA_BUSY)
    #define LORA_BUSY P_LORA_BUSY
  #else
    #define LORA_BUSY 13
  #endif
#endif

#ifndef LORA_SCK
  #if defined(P_LORA_SCLK)
    #define LORA_SCK  P_LORA_SCLK
  #else
    #define LORA_SCK  9
  #endif
#endif

#ifndef LORA_MISO
  #if defined(P_LORA_MISO)
    #define LORA_MISO P_LORA_MISO
  #else
    #define LORA_MISO 11
  #endif
#endif

#ifndef LORA_MOSI
  #if defined(P_LORA_MOSI)
    #define LORA_MOSI P_LORA_MOSI
  #else
    #define LORA_MOSI 10
  #endif
#endif

// ---------------------------------------------------------------------------
// Radio parameters
// ---------------------------------------------------------------------------
#ifndef LORA_FREQ
  #define LORA_FREQ      868.731018
#endif
#ifndef LORA_BW
  #define LORA_BW        62.5
#endif
#ifndef LORA_SF
  #define LORA_SF        8
#endif
#ifndef LORA_CR
  #define LORA_CR        7
#endif
#ifndef LORA_SYNC_WORD
  #define LORA_SYNC_WORD 0x12
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER  10
#endif
#ifndef LORA_PREAMBLE
  #define LORA_PREAMBLE  16
#endif

// ---------------------------------------------------------------------------
// FEM (external power amplifier / LNA board)
// ---------------------------------------------------------------------------
#ifdef P_LORA_PA_POWER
  #define FEM_VCC_PIN P_LORA_PA_POWER
#endif
#ifdef P_LORA_KCT8103L_PA_CSD
  #define FEM_EN_PIN  P_LORA_KCT8103L_PA_CSD
#endif
#ifdef P_LORA_KCT8103L_PA_CTX
  #define FEM_TX_PIN  P_LORA_KCT8103L_PA_CTX
#endif

#ifndef HAS_FEM
  #if defined(FEM_TX_PIN) && defined(FEM_EN_PIN) && defined(FEM_VCC_PIN)
    #define HAS_FEM 1
  #else
    #define HAS_FEM 0
    #warning "No FEM pins defined, building WITHOUT external FEM control"
  #endif
#endif

// ---------------------------------------------------------------------------
// Peripheral power rail (VEXT)
// ---------------------------------------------------------------------------
#ifdef PIN_VEXT_EN
  #define VEXT_PIN PIN_VEXT_EN
#endif
#ifndef VEXT_EN_ACTIVE
  #if defined(PIN_VEXT_EN_ACTIVE)
    #define VEXT_EN_ACTIVE PIN_VEXT_EN_ACTIVE
  #else
    #define VEXT_EN_ACTIVE LOW
  #endif
#endif

// ---------------------------------------------------------------------------
// Короткий код платы для hello и MQTT: "h43" — Heltec V4.3, "h3" — Heltec V3,
// "gen" — прочие ESP32-S3. По нему видно железо, не раскрывая имя устройства.
// Задаётся build-флагом -DBOARD_CODE, схема — производитель + модель.
// ---------------------------------------------------------------------------
#ifndef BOARD_CODE
  #define BOARD_CODE "esp32"
#endif

// ---------------------------------------------------------------------------
// Батарея: VBAT через делитель на АЦП. У Heltec делитель 390k/100k на ADC1_CH0 (GPIO1)
// и включается управляющим пином ADC_CTRL (GPIO37) — полярность разная: на V4 его тянут
// в HIGH, на V3 в LOW (см. datasheet платы), поэтому задаётся build-флагом.
//
// Управляющий пин необязателен: на части плат делитель припаян намертво и меряется
// всегда. Тогда достаточно -DPIN_VBAT_READ (и -DPIN_VBAT_DIVIDER, если делитель не 4.9).
// ---------------------------------------------------------------------------
#if defined(PIN_VBAT_READ)
  #define HAS_BATTERY 1
  #define VBAT_PIN PIN_VBAT_READ
  #ifdef PIN_VBAT_CTRL
    #define VBAT_CTRL_PIN PIN_VBAT_CTRL
    #ifndef PIN_VBAT_CTRL_ACTIVE
      #define PIN_VBAT_CTRL_ACTIVE HIGH
    #endif
    #define VBAT_CTRL_ACTIVE PIN_VBAT_CTRL_ACTIVE
  #endif
  #ifndef PIN_VBAT_DIVIDER
    #define PIN_VBAT_DIVIDER 4.9f      // (390k + 100k) / 100k
  #endif
  #define VBAT_DIVIDER PIN_VBAT_DIVIDER
#else
  #define HAS_BATTERY 0
#endif

// ---------------------------------------------------------------------------
// User button
// ---------------------------------------------------------------------------
#ifndef BUTTON_PIN
  #if defined(PIN_USER_BTN)
    #define BUTTON_PIN PIN_USER_BTN
  #else
    #define BUTTON_PIN -1
  #endif
#endif

// ---------------------------------------------------------------------------
// I2C pins
// ---------------------------------------------------------------------------
#ifdef PIN_BOARD_SDA
  #define SDA_PIN PIN_BOARD_SDA
#endif
#ifdef PIN_BOARD_SCL
  #define SCL_PIN PIN_BOARD_SCL
#endif

// ---------------------------------------------------------------------------
// Display. HAS_OLED=0 selects a no-op stub so all main.cpp call sites stay as
// they are; it simply does nothing on boards without a screen.
// ---------------------------------------------------------------------------
#ifndef SCREEN_WIDTH
  #define SCREEN_WIDTH 128
#endif
#ifndef SCREEN_HEIGHT
  #define SCREEN_HEIGHT 64
#endif
#ifndef OLED_RESET
  #ifdef PIN_OLED_RESET
    #define OLED_RESET PIN_OLED_RESET
  #else
    #define OLED_RESET 21
  #endif
#endif
#ifndef SCREEN_ADDRESS
  #define SCREEN_ADDRESS 0x3C
#endif

#ifndef HAS_OLED
  #define HAS_OLED 1
#endif

// Display-объект и драйверы живут в oled.h (прошивка) — mesh-network-core включает
// только макросы платы и в дисплей не лезет.