// SPDX-License-Identifier: Apache-2.0
// Minimal ES8311 codec init for the Waveshare ESP32-C5-Touch-LCD-3.5.
// DAC playback only: 16 kHz, 16-bit, I2S slave, NO external MCLK (the codec is
// clocked from SCLK/BCLK on this board). I2C control @ 0x18 on the shared bus
// (SDA 27 / SCL 26). Codec enable (Codec_CE) is hardware-tied, no toggle needed.
//
// Register sequence ported from Espressif esp_codec_dev es8311.c
// (open + config_sample + set_bits + config_fmt + start), reduced to the
// fixed 16 kHz / 16-bit / DAC / slave / no-MCLK path. Coefficient row used:
// {mclk=4096000 (=16000*256), rate=16000}: pre_div 1, pre_multi 1, adc/dac_div 1,
// fs_mode 0, lrck_h 0, lrck_l 0xff, bclk_div 4, adc/dac_osr 0x10.
//
// IMPORTANT: start the I2S master (BCLK/LRCK running) BEFORE calling this —
// the ES8311 derives its internal clock from SCLK.
#pragma once
#ifndef PORKCHOP_ES8311_H
#define PORKCHOP_ES8311_H

#include <Wire.h>

#ifndef CTP_SDA
#define CTP_SDA 27
#endif
#ifndef CTP_SCL
#define CTP_SCL 26
#endif

#define ES8311_ADDR 0x18

static inline bool es8311_w(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}
static inline uint8_t es8311_rd(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom((int)ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

// Returns true if the codec answered with its expected chip ID (0x83 0x11).
static bool es8311_init_dac() {
  Wire.end(); Wire.begin(CTP_SDA, CTP_SCL, 100000U);  // fresh bus (C5 read quirk)
  uint8_t v;

  // ── open ───────────────────────────────────────────────────────────────
  if (es8311_rd(0x0D) != 0xFA) es8311_w(0x0D, 0xFA);
  es8311_w(0x44, 0x08); es8311_w(0x44, 0x08);   // I2C noise immunity (x2 per vendor)
  es8311_w(0x01, 0x30); es8311_w(0x02, 0x00); es8311_w(0x03, 0x10);
  es8311_w(0x16, 0x24); es8311_w(0x04, 0x10); es8311_w(0x05, 0x00);
  es8311_w(0x0B, 0x00); es8311_w(0x0C, 0x00); es8311_w(0x10, 0x1F);
  es8311_w(0x11, 0x7F); es8311_w(0x00, 0x80);
  v = es8311_rd(0x00); v &= 0xBF; es8311_w(0x00, v);     // slave mode
  es8311_w(0x01, 0xBF);                                  // 0x3F|0x80: clock from SCLK, no invert
  v = es8311_rd(0x06); v &= ~0x20; es8311_w(0x06, v);    // SCLK not inverted
  es8311_w(0x13, 0x10); es8311_w(0x1B, 0x0A); es8311_w(0x1C, 0x6A);
  es8311_w(0x44, 0x58);                                  // internal reference (ADCL+DACR)

  // ── config_sample: 16 kHz coeff {4096000,16000}, use_mclk=false → datmp=3 ──
  v = es8311_rd(0x02); v &= 0x07; v |= (0 << 5); v |= (3 << 3); es8311_w(0x02, v);
  es8311_w(0x05, 0x00);                                  // adc_div/dac_div = 1
  v = es8311_rd(0x03); v &= 0x80; v |= 0x10; es8311_w(0x03, v);   // fs_mode 0, adc_osr 0x10
  v = es8311_rd(0x04); v &= 0x80; v |= 0x10; es8311_w(0x04, v);   // dac_osr 0x10
  v = es8311_rd(0x07); v &= 0xC0; v |= 0x00; es8311_w(0x07, v);   // lrck_h
  es8311_w(0x08, 0xFF);                                            // lrck_l
  v = es8311_rd(0x06); v &= 0xE0; v |= (4 - 1); es8311_w(0x06, v); // bclk_div 4

  // ── 16-bit + I2S normal format ─────────────────────────────────────────
  v = es8311_rd(0x09); v |= 0x0C; v &= 0xFC; es8311_w(0x09, v);
  v = es8311_rd(0x0A); v |= 0x0C; v &= 0xFC; es8311_w(0x0A, v);

  // ── start: power up DAC ────────────────────────────────────────────────
  es8311_w(0x00, 0x80); es8311_w(0x01, 0xBF);
  v = es8311_rd(0x09); v &= ~0x40; es8311_w(0x09, v);    // enable DAC serial-in path
  es8311_w(0x17, 0xBF);
  es8311_w(0x0E, 0x02);
  es8311_w(0x12, 0x00);                                  // power up DAC
  es8311_w(0x14, 0x1A);
  v = es8311_rd(0x14); v &= ~0x40; es8311_w(0x14, v);    // no digital mic
  es8311_w(0x0D, 0x01);
  es8311_w(0x15, 0x40);
  es8311_w(0x37, 0x08);
  es8311_w(0x45, 0x00);

  // ── volume + unmute ────────────────────────────────────────────────────
  es8311_w(0x32, 0xE0);                                  // DAC volume (0x00..0xFF, higher=louder)
  v = es8311_rd(0x31); v &= 0x9F; es8311_w(0x31, v);     // unmute DAC

  uint8_t id1 = es8311_rd(0xFD), id2 = es8311_rd(0xFE);
  Serial.printf("[ES8311] init done, chip ID 0x%02X%02X (expect 8311)\n", id1, id2);
  return (id1 == 0x83 && id2 == 0x11);
}

// Set DAC volume register directly (0x00 min .. 0xFF max).
static inline void es8311_set_volume(uint8_t reg0x32) { es8311_w(0x32, reg0x32); }

#endif // PORKCHOP_ES8311_H
