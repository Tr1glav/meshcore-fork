#include "config.h"
#include "globals.h"
#include "crypto.h"      // fmtFix: печать RSSI/SNR без float-printf
#include "display.h"
#include <WiFi.h>

// ===== Реализация хуков платформы (см. mesh-network-core/include/mc_platform.h) =====
// Экран рисуем на локальном дисплее платы; нет экрана (HAS_OLED=0) — хуки молчат.

void mcUiIncoming(const String& channelName, const String& sender, const String& msg,
                  float rssi, float snr, int hopCount, const String& path) {
    #if HAS_OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(channelName.c_str());
    display.printf("From: %s\n", sender.c_str());
    char r2[12], s2[12];
    display.printf("RSSI:%s SNR:%s\n", fmtFix(rssi, 0, r2, sizeof(r2)),
                   fmtFix(snr, 0, s2, sizeof(s2)));
    if (hopCount > 0) {
        display.drawLine(0, 24, 128, 24, SSD1306_WHITE);
        display.setCursor(0, 26);
        display.print("via: ");
        String pathStr = path;
        if (pathStr.length() > 24) pathStr = pathStr.substring(0, 20) + "..";
        display.println(pathStr);
        display.drawLine(0, 36, 128, 36, SSD1306_WHITE);
        display.setCursor(0, 40);
    } else {
        display.drawLine(0, 32, 128, 32, SSD1306_WHITE);
        display.setCursor(0, 36);
    }
    String showMsg = msg;
    if (showMsg.length() > 26) showMsg = showMsg.substring(0, 24) + "..";
    display.println(showMsg);
    display.display();
    #else
    (void)channelName; (void)sender; (void)msg; (void)rssi; (void)snr;
    (void)hopCount; (void)path;
    #endif
}

void mcUiSensorRx(bool snsPub, float rssi) {
    #if HAS_OLED
    display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
    display.setCursor(0, 50);
    char r3[12];
    if (snsPub) display.printf("SNS -> MQTT RSSI:%s", fmtFix(rssi, 0, r3, sizeof(r3)));
    else        display.printf("SNS RX, MQTT %s", wifiConnected ? "off" : "no-wifi");
    display.display();
    #else
    (void)snsPub; (void)rssi;
    #endif
}

void mcUiHexScreen(int pktLen, float rssi, float snr, const uint8_t* buffer) {
    #if HAS_OLED
    display.setTextSize(1);
    display.clearDisplay();
    display.setCursor(0, 0);
    char dr[12], ds[12];
    display.printf("RX %dB RSSI:%s\n", pktLen, fmtFix(rssi, 0, dr, sizeof(dr)));
    display.printf("SNR:%s pkts:%d\n", fmtFix(snr, 0, ds, sizeof(ds)), packetCount);
    display.printf("hex:");
    for (int i = 0; i < min(pktLen, 21); i++) display.printf("%02X", buffer[i]);
    display.display();
    #else
    (void)pktLen; (void)rssi; (void)snr; (void)buffer;
    #endif
}

void mcUiSetBrightness(uint8_t bri) {
    #if HAS_OLED
    display.setBrightness(bri);
    #else
    (void)bri;
    #endif
}

// ===== Mesh OTA на экране (раздающая сторона) =====
void mcUiOtaAbort(const char* why) {
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA abort");
    display.println(why);
    display.display();
    #else
    (void)why;
    #endif
}

void mcUiOtaProgress(const String& target, uint32_t pct, uint32_t pkts, float rssi, float snr) {
    #if HAS_OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.printf("OTA %s\n", target.c_str());
    if (pct > 100) pct = 100;
    display.printf("%u%%\n", (unsigned)pct);
    int bw = (int)((long)pct * 128 / 100);
    if (bw > 128) bw = 128;
    display.drawRect(0, 28, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 28, bw, 10, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.printf("Pkts: %d", (unsigned)pkts);
    // Качество связи по последнему принятому пакету (ack/nack сенсора)
    display.setCursor(0, 54);
    char pr[12], ps[12];
    display.printf("RSSI:%s SNR:%s", fmtFix(rssi, 0, pr, sizeof(pr)),
                   fmtFix(snr, 0, ps, sizeof(ps)));
    display.display();
    #else
    (void)target; (void)pct; (void)pkts; (void)rssi; (void)snr;
    #endif
}

void mcUiOtaDone(const String& target) {
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA OK");
    display.println(target);
    display.println("reboot sensor");
    display.display();
    #else
    (void)target;
    #endif
}

// ===== Mesh OTA на экране (принимающая сторона) =====
void mcUiOtaSensorProgress(uint32_t got, uint32_t total, uint32_t pkts, float rssi, float snr) {
    #if HAS_OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("OTA update");
    uint32_t pct = total ? (got * 100 / total) : 0;
    if (pct > 100) pct = 100;
    display.printf("%u%%\n", (unsigned)pct);
    int bw = total ? (int)((long)got * 128 / total) : 0;
    if (bw > 128) bw = 128;
    display.drawRect(0, 28, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 28, bw, 10, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.printf("Pkts: %d", (unsigned)pkts);
    // Качество связи: RSSI/SNR последнего принятого чанка
    display.setCursor(0, 54);
    char pr[12], ps[12];
    display.printf("RSSI:%s SNR:%s", fmtFix(rssi, 0, pr, sizeof(pr)),
                   fmtFix(snr, 0, ps, sizeof(ps)));
    display.display();
    #else
    (void)got; (void)total; (void)pkts; (void)rssi; (void)snr;
    #endif
}

void mcUiOtaSensorAbort(const char* why, uint32_t rxFrames, uint32_t rxErr) {
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA ABORT");
    display.println(why);
    display.printf("rx:%u err:%u\n", (unsigned)rxFrames, (unsigned)rxErr);
    display.display();
    #else
    (void)why; (void)rxFrames; (void)rxErr;
    #endif
}

// ===== Сеть (узел-прошивальщик) =====
bool mcWifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String mcLocalIp() {
    return WiFi.localIP().toString();
}