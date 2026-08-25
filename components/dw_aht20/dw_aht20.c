// SPDX-License-Identifier: MIT
// dw_aht20 — I2C driver implementation for AHT20 sensor
#include "dw_aht20.h"
#include "dw_board.h"
#include "dc_evlog.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "dw_aht20";

#define DW_AHT20_NVS_NS   "app_nvs"
#define DW_AHT20_NVS_KEY  "aht_poll"

#define AHT20_I2C_ADDR         0x38
#define AHT20_CMD_INIT         0xBE
#define AHT20_CMD_MEASURE      0xAC
#define AHT20_CMD_SOFT_RESET   0xBA
#define AHT20_STATUS_BUSY      0x80
#define AHT20_STATUS_CALIBRATED 0x08

static dw_aht20_data_t s_latest_data = { .temperature_c = 0.0f, .humidity_rh = 0.0f, .valid = false, .timestamp_ms = 0 };
static dw_aht20_stats_t s_stats = { 0 };
// The AHT20 sits on the I2C bus the SH01 mainboard also drives. Every read is a
// chance to collide with the mainboard and trip its E0 error, so we poll slowly
// by default (a filament dryer changes slowly) and let the user tune it.
static uint32_t s_polling_interval_sec = 120;
static bool s_initialized = false;
// The I2C driver is a one-time global install; only the sensor handshake below
// is retried. Re-installing an already-installed driver returns ESP_FAIL.
static bool s_driver_installed = false;

static uint8_t calc_crc8(const uint8_t *ptr, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ptr[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t aht20_soft_reset(void)
{
    uint8_t cmd = AHT20_CMD_SOFT_RESET;
    esp_err_t ret = i2c_master_write_to_device(DW_I2C_PORT, AHT20_I2C_ADDR, &cmd, 1, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(20));
    return ret;
}

esp_err_t dw_aht20_init(void)
{
    ESP_LOGI(TAG, "Initializing AHT20 I2C bus (SDA=%d, SCL=%d)...", DW_GPIO_I2C_SDA, DW_GPIO_I2C_SCL);

    // Restore the persisted poll interval (E0 mitigation knob) if the user set one.
    // A value of 0 means "disabled" — do not touch the shared I2C bus at all.
    nvs_handle_t nh;
    if (nvs_open(DW_AHT20_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint32_t v = 0xFFFFFFFF;
        if (nvs_get_u32(nh, DW_AHT20_NVS_KEY, &v) == ESP_OK) s_polling_interval_sec = v;
        nvs_close(nh);
    }
    if (s_polling_interval_sec == 0) {
        s_initialized = false;
        dc_evlog_add("AHT20: polling disabled — leaving the shared I2C bus untouched");
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DW_GPIO_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = DW_GPIO_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000, // 100kHz standard mode for bus stability
    };

    // Install the bus driver once; later dw_aht20_init() calls (the read-path
    // recovery) skip this and only redo the sensor handshake below.
    if (!s_driver_installed) {
        esp_err_t cfg = i2c_param_config(DW_I2C_PORT, &conf);
        if (cfg != ESP_OK) {
            ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(cfg));
            return cfg;
        }
        esp_err_t inst = i2c_driver_install(DW_I2C_PORT, conf.mode, 0, 0, 0);
        if (inst != ESP_OK && inst != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(inst));
            return inst;
        }
        s_driver_installed = true;
    }

    vTaskDelay(pdMS_TO_TICKS(40));

    // Soft reset sensor
    aht20_soft_reset();

    // Check status
    uint8_t status = 0;
    esp_err_t ret = i2c_master_read_from_device(DW_I2C_PORT, AHT20_I2C_ADDR, &status, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read AHT20 status byte: %s", esp_err_to_name(ret));
        dc_evlog_add("AHT20: Init error reading status (%s)", esp_err_to_name(ret));
        return ret;
    }

    if ((status & AHT20_STATUS_CALIBRATED) == 0) {
        ESP_LOGW(TAG, "AHT20 uncalibrated, sending init command...");
        uint8_t init_cmd[3] = { AHT20_CMD_INIT, 0x08, 0x00 };
        i2c_master_write_to_device(DW_I2C_PORT, AHT20_I2C_ADDR, init_cmd, 3, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_initialized = true;
    dc_evlog_add("AHT20: Sensor initialized successfully (addr 0x%02X)", AHT20_I2C_ADDR);
    ESP_LOGI(TAG, "AHT20 initialized successfully.");
    return ESP_OK;
}

esp_err_t dw_aht20_read(float *out_temp_c, float *out_humidity_rh)
{
    s_stats.total_reads++;

    if (!s_initialized) {
        esp_err_t init_ret = dw_aht20_init();
        if (init_ret != ESP_OK) {
            s_stats.bus_errors++;
            return init_ret;
        }
    }

    // Command: Measure (0xAC, 0x33, 0x00)
    uint8_t measure_cmd[3] = { AHT20_CMD_MEASURE, 0x33, 0x00 };
    esp_err_t ret = i2c_master_write_to_device(DW_I2C_PORT, AHT20_I2C_ADDR, measure_cmd, 3, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        s_stats.bus_errors++;
        dc_evlog_add("AHT20: I2C write error during measurement (%s)", esp_err_to_name(ret));
        ESP_LOGE(TAG, "I2C write error: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(80)); // Wait for measurement

    uint8_t data[7] = { 0 };
    ret = i2c_master_read_from_device(DW_I2C_PORT, AHT20_I2C_ADDR, data, 7, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        s_stats.bus_errors++;
        dc_evlog_add("AHT20: I2C read error during data fetch (%s)", esp_err_to_name(ret));
        ESP_LOGE(TAG, "I2C read error: %s", esp_err_to_name(ret));
        return ret;
    }

    if (data[0] & AHT20_STATUS_BUSY) {
        s_stats.bus_errors++;
        dc_evlog_add("AHT20: Sensor busy timeout");
        ESP_LOGW(TAG, "AHT20 reported busy");
        return ESP_ERR_TIMEOUT;
    }

    // Verify CRC8
    uint8_t crc = calc_crc8(data, 6);
    if (crc != data[6]) {
        s_stats.crc_errors++;
        dc_evlog_add("AHT20: CRC error (expected 0x%02X, got 0x%02X)", crc, data[6]);
        ESP_LOGW(TAG, "CRC error: exp 0x%02X, got 0x%02X", crc, data[6]);
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t raw_humidity = (((uint32_t)data[1]) << 12) | (((uint32_t)data[2]) << 4) | (((uint32_t)data[3]) >> 4);
    uint32_t raw_temp = ((((uint32_t)data[3]) & 0x0F) << 16) | (((uint32_t)data[4]) << 8) | ((uint32_t)data[5]);

    float rh = ((float)raw_humidity / 1048576.0f) * 100.0f;
    float temp = (((float)raw_temp / 1048576.0f) * 200.0f) - 50.0f;

    if (out_temp_c) *out_temp_c = temp;
    if (out_humidity_rh) *out_humidity_rh = rh;

    s_latest_data.temperature_c = temp;
    s_latest_data.humidity_rh = rh;
    s_latest_data.valid = true;
    s_latest_data.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    s_stats.successful_reads++;
    ESP_LOGD(TAG, "AHT20 Read Success: Temp = %.1f C, RH = %.1f %%", temp, rh);

    return ESP_OK;
}

dw_aht20_data_t dw_aht20_get_latest(void)
{
    return s_latest_data;
}

dw_aht20_stats_t dw_aht20_get_stats(void)
{
    return s_stats;
}

void dw_aht20_set_polling_interval(uint32_t interval_sec)
{
    if (interval_sec < 1) interval_sec = 1;
    s_polling_interval_sec = interval_sec;
    nvs_handle_t nh;
    if (nvs_open(DW_AHT20_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u32(nh, DW_AHT20_NVS_KEY, interval_sec);
        nvs_commit(nh);
        nvs_close(nh);
    }
    dc_evlog_add("AHT20: Polling interval set to %lu seconds", (unsigned long)interval_sec);
}

uint32_t dw_aht20_get_polling_interval(void)
{
    return s_polling_interval_sec;
}
