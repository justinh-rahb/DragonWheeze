// SPDX-License-Identifier: MIT
// dw_aht20 — I2C driver for AHT20 sensor with error logging & polling mitigation
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float humidity_rh;
    bool valid;
    uint32_t timestamp_ms;
} dw_aht20_data_t;

typedef struct {
    uint32_t total_reads;
    uint32_t successful_reads;
    uint32_t bus_errors;
    uint32_t crc_errors;
} dw_aht20_stats_t;

// Initialize I2C bus and AHT20 sensor
esp_err_t dw_aht20_init(void);

// Read current temperature (°C) and humidity (%RH)
esp_err_t dw_aht20_read(float *out_temp_c, float *out_humidity_rh);

// Get cached reading from latest measurement
dw_aht20_data_t dw_aht20_get_latest(void);

// Get current I2C bus error statistics
dw_aht20_stats_t dw_aht20_get_stats(void);

// Set polling interval in seconds (default: 10s to minimize I2C contention)
void dw_aht20_set_polling_interval(uint32_t interval_sec);

// Get current polling interval in seconds
uint32_t dw_aht20_get_polling_interval(void);

#ifdef __cplusplus
}
#endif
