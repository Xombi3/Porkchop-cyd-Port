// SPDX-License-Identifier: MIT
// LC76G GNSS over I2C on the shared bus (SDA 27 / SCL 26).
//
// Why I2C: on this board the J3 UART holes (U0RXD/GPIO12) do not reach the
// chip as shipped (verified on two units), but the header's SDA/SCL do, and
// the Waveshare LC76G module exposes I2C.
//
// Protocol from Waveshare's official LC76G I2C demo (LC76G_RPI_I2C, i2c_read.c
// / i2c_register.h). Each step is its own I2C frame, 10 ms before each:
//   0. dummy 1-byte write to 0x50 (wake / multi-slave check)
//   1. write {reg 0xAA510008, len 4} (little-endian) to 0x50  -> "how much?"
//   2. read 4 bytes from 0x54                                 -> length (LE)
//   3. write {reg 0xAA512000, len N} (little-endian) to 0x50  -> "send N"
//   4. read N bytes from 0x54                                 -> NMEA text
// No other slave may be accessed mid-transaction, hence i2c_lock().
#pragma once
#ifndef PORKCHOP_GPS_I2C_H
#define PORKCHOP_GPS_I2C_H

#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include "i2c_lock.h"

#define LC76G_CR_ADDR    0x50
#define LC76G_RD_ADDR    0x54
#define LC76G_CHUNK      4096       // max bytes per read, as in the demo (QL_MAX_DATA_LENGTH)

static StreamBufferHandle_t g_gpsStream = nullptr;
// Link counters (not printed; handy when debugging the I2C link).
static volatile uint32_t g_gpsI2cCycles = 0, g_gpsI2cBytes = 0, g_gpsI2cFails = 0;
static volatile uint32_t g_gpsI2cRecov = 0;   // demo's Recovery_I2C succeeded via 0x54/0x58
static volatile uint32_t g_gpsI2cWakes = 0;   // demo's Ql_Wake_I2C sent (after empty reads)

static void _lc76g_put32le(uint8_t *b, uint32_t v) {
  b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}

static uint8_t _lc76g_write_cr(uint32_t reg, uint32_t len) {   // 0 = ok, else Wire error
  uint8_t b[8];
  _lc76g_put32le(b, reg);
  _lc76g_put32le(b + 4, len);
  Wire.beginTransmission(LC76G_CR_ADDR);
  Wire.write(b, 8);
  return Wire.endTransmission();
}

static size_t _lc76g_read(uint8_t *buf, size_t n) {
  size_t got = Wire.requestFrom((uint8_t)LC76G_RD_ADDR, n);
  for (size_t i = 0; i < got; i++) buf[i] = Wire.read();
  return got;
}

// 1-byte dummy write (demo: Write_Dummy_Addr). 0 = ACKed.
static uint8_t _lc76g_dummy(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)0);
  return Wire.endTransmission();
}

// One full read cycle. Returns bytes pushed to the stream, or -1 on a bus fail.
static int _lc76g_cycle(uint8_t *buf) {
  int out = -1;
  i2c_lock();
  // C5 read quirk (see ft6336.h): re-open the bus ONCE, before the whole
  // transaction, like the touch/codec drivers do. Never between steps — a bus
  // re-init mid-transaction glitches the lines and wedges the LC76G.
  Wire.end(); Wire.begin(CTP_SDA, CTP_SCL, 100000U);
  Wire.setTimeOut(300);             // module clock-stretches up to ~200 ms
  do {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (_lc76g_dummy(LC76G_CR_ADDR) != 0) {
      // Demo's Recovery_I2C: 0x50 silent -> a dummy write to 0x54, then 0x58,
      // releases a module left mid-transaction. Retry the cycle next time.
      if (_lc76g_dummy(LC76G_RD_ADDR) == 0 || _lc76g_dummy(0x58) == 0) g_gpsI2cRecov++;
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    if (_lc76g_write_cr(0xAA510008UL, 4) != 0) break;

    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t lb[4];
    if (_lc76g_read(lb, 4) != 4) break;
    uint32_t len = lb[0] | (lb[1] << 8) | ((uint32_t)lb[2] << 16) | ((uint32_t)lb[3] << 24);
    if (len == 0) { out = 0; break; }
    if (len >= 35 * 1024) break;    // demo treats this as illegal
    if (len > LC76G_CHUNK) len = LC76G_CHUNK;   // demo reads all of it, 4096 at a time

    vTaskDelay(pdMS_TO_TICKS(10));
    if (_lc76g_write_cr(0xAA512000UL, len) != 0) break;

    vTaskDelay(pdMS_TO_TICKS(10));
    size_t got = _lc76g_read(buf, len);
    if (got != len) break;
    out = (int)got;
  } while (0);
  i2c_unlock();
  if (out > 0) xStreamBufferSend(g_gpsStream, buf, out, 0);
  return out;
}

// Demo's Ql_Wake_I2C: a 1-byte (0x00) write through the module's write path.
// The demo sends it after 30 consecutive empty reads; the LC76G I2C transmitter
// can otherwise sleep (Quectel). Sequence from i2c_write.c Write_Data():
//   dummy 0x50 -> write {0xAA510004, 4} to 0x50 -> read 4 (free space) from 0x54
//   -> write {0xAA531000, 1} to 0x50 -> write the byte to 0x58. 10 ms before each.
static bool _lc76g_wake() {
  bool ok = false;
  i2c_lock();
  Wire.end(); Wire.begin(CTP_SDA, CTP_SCL, 100000U);
  Wire.setTimeOut(300);
  do {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (_lc76g_dummy(LC76G_CR_ADDR) != 0) break;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (_lc76g_write_cr(0xAA510004UL, 4) != 0) break;
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t fb[4];
    if (_lc76g_read(fb, 4) != 4) break;
    uint32_t freeLen = fb[0] | (fb[1] << 8) | ((uint32_t)fb[2] << 16) | ((uint32_t)fb[3] << 24);
    if (freeLen < 1) break;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (_lc76g_write_cr(0xAA531000UL, 1) != 0) break;
    vTaskDelay(pdMS_TO_TICKS(10));
    ok = (_lc76g_dummy(0x58) == 0);     // the 1 data byte (0x00) to the write address
  } while (0);
  i2c_unlock();
  return ok;
}

static void gpsI2cTask(void *) {
  static uint8_t buf[LC76G_CHUNK];
  uint32_t empty = 0;
  for (;;) {
    int r = _lc76g_cycle(buf);
    g_gpsI2cCycles++;
    if (r < 0) g_gpsI2cFails++; else g_gpsI2cBytes += r;
    if (r == 0) {
      // The demo polls every ~10 ms and wakes after 30 empties (~1 s); this
      // loop polls slower, so wake after 10 empties (~1.3 s) for the same gap.
      if (++empty >= 10) { empty = 0; if (_lc76g_wake()) g_gpsI2cWakes++; }
    } else {
      empty = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(r == LC76G_CHUNK ? 20 : (r == 0 ? 100 : 250)));
  }
}

// Call in setup() after the bus is up (ft6336_init) and i2c_lock_init().
static void gps_i2c_begin() {
  Wire.setBufferSize(LC76G_CHUNK + 16);  // persists across Wire.end()/begin()
  g_gpsStream = xStreamBufferCreate(8192, 1);
  xTaskCreatePinnedToCore(gpsI2cTask, "gpsi2c", 3072, nullptr, 3, nullptr, 0);
}

#endif // PORKCHOP_GPS_I2C_H
