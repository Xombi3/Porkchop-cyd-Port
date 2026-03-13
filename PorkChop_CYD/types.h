// types.h - All enums and structs for PorkChop CYD
// Arduino IDE processes .h files before .ino, solving declaration order issues
#pragma once
#include <Arduino.h>
#include <WiFi.h>

enum PigMood  { PM_CHILL, PM_HYPE, PM_HUNGRY, PM_SAD, PM_SCAN };
enum AppScr   { SC_SPLASH, SC_MENU, SC_SCAN, SC_DETAIL, SC_CAPTURE,
                SC_WARDRIVE, SC_SPECTRUM, SC_SETTINGS, SC_GPS, SC_DIAG };
enum FiltMode { FM_ALL, FM_VULN, FM_SOFT, FM_HIDDEN };

struct TouchPt { int x, y; bool pressed; };

struct WNet {
  String ssid, bssid;
  int32_t rssi;
  uint8_t ch;
  wifi_auth_mode_t auth;
  bool hidden, hasPMF, hsGot;
  float lat, lon;
  uint8_t mac[6];
  unsigned long seen;
};

struct WDLog {
  String ssid, bssid;
  float lat, lon;
  int32_t rssi;
  wifi_auth_mode_t auth;
};

struct HSCap { uint8_t bssid[6]; int frames; };
