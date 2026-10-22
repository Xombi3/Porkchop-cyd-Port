// SPDX-License-Identifier: MIT
// FT6336 capacitive touch driver for the Waveshare ESP32-C5-Touch-LCD-3.5.
// I2C-only (addr 0x38) on the shared bus (SDA 27 / SCL 26). Touch reset and
// panel power are on the I/O expander (ws_c5_exio.h), so ft6336_init() brings
// the expander up first. Verified on this board during the Marauder C5 port.
#pragma once
#ifndef PORKCHOP_FT6336_H
#define PORKCHOP_FT6336_H

#include <Wire.h>
#include "ws_c5_exio.h"
#include "i2c_lock.h"

#define FT6336_ADDR      0x38
#define FT6336_TD_STATUS 0x02
#define FT6336_T1_XH     0x03

static bool _ft6336_read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)FT6336_ADDR, (int)len);
  for (uint8_t i = 0; i < len; i++)
    buf[i] = Wire.available() ? Wire.read() : 0;
  return true;
}

// Brings up the I/O expander (power + resets + backlight) and configures the
// FT6336. Must run before the first touch read; call once in setup().
static void ft6336_init() {
  // Panel power, LCD reset and touch reset are on the expander (0x24), which
  // also opens the shared I2C bus. Must run before tft.init() too — setup()
  // calls ws_c5_exio_begin() ahead of tft.begin(); calling again here is safe.
  ws_c5_exio_begin();

  uint8_t chipId = 0;
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0xA3);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)FT6336_ADDR, 1);
    if (Wire.available()) chipId = Wire.read();
  }
  Serial.printf("[Touch] FT6336U ID: 0x%02X%s\n",
                chipId, chipId == 0x64 ? " (OK)" : " (unexpected)");

  // Touch threshold (reg 0x80 = IDTHRESHOLD). LOWER = more sensitive. The
  // FT6336U default is 22; 40 (panel-in-case) made a bare panel need hard
  // presses and miss light taps, so use 12 for a light, responsive bare-glass feel.
  // ft6336_read_raw() validates each read (1-2 points, on-panel coords), so a
  // lower threshold won't let noise register as a tap. Raise toward 30-40 if a
  // case/film is added and phantom touches appear.
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0x80);
  Wire.write(12);
  Wire.endTransmission();
}

// Reads raw panel coordinates (panel native portrait space: 0..319 x 0..479).
// Returns 1 on a valid single/double touch, 0 otherwise.
static uint8_t _ft6336_read_raw_nolock(uint16_t *raw_x, uint16_t *raw_y) {
  // On the C5, the early Wire.begin() does not leave the I2C controller able
  // to complete reads, so re-open the bus each read. Validate the result
  // (1-2 points, on-panel coords) so a noisy read cannot register as a tap.
  // This was the single hardest bug in the Marauder port on this board.
  Wire.end(); Wire.begin(CTP_SDA, CTP_SCL, 100000U);
  uint8_t data[7];
  if (!_ft6336_read(FT6336_TD_STATUS, data, 7)) return 0;
  const uint8_t points = data[0] & 0x0F;
  if (points == 0 || points > 2) return 0;
  const uint16_t rx = ((uint16_t)(data[1] & 0x0F) << 8) | data[2];
  const uint16_t ry = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];
  if (rx >= TFT_WIDTH || ry >= TFT_HEIGHT) return 0;
  *raw_x = rx;
  *raw_y = ry;
  return 1;
}

// Bus-locked wrapper: the GPS task shares this bus (see i2c_lock.h).
static uint8_t ft6336_read_raw(uint16_t *raw_x, uint16_t *raw_y) {
  i2c_lock();
  uint8_t r = _ft6336_read_raw_nolock(raw_x, raw_y);
  i2c_unlock();
  return r;
}

#endif // PORKCHOP_FT6336_H
