// SPDX-License-Identifier: MIT
// Shared-bus lock for the Waveshare ESP32-C5-Touch-LCD-3.5 I2C bus (SDA 27 /
// SCL 26). The touch task polls the bus at ~100Hz while the GPS task runs a
// multi-frame LC76G transaction that must not be interleaved with any other
// slave (Quectel), so both take this lock around their bus work.
#pragma once
#ifndef PORKCHOP_I2C_LOCK_H
#define PORKCHOP_I2C_LOCK_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t g_i2cMux = nullptr;

// Call once in setup(), before any task that uses the bus is started.
static inline void i2c_lock_init() {
  if (!g_i2cMux) g_i2cMux = xSemaphoreCreateMutex();
}
static inline void i2c_lock()   { if (g_i2cMux) xSemaphoreTake(g_i2cMux, portMAX_DELAY); }
static inline void i2c_unlock() { if (g_i2cMux) xSemaphoreGive(g_i2cMux); }

#endif // PORKCHOP_I2C_LOCK_H
