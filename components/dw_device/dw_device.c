// SPDX-License-Identifier: MIT
// dw_device — Sovol SH01 Filament Dryer state machine implementation
#include "dw_device.h"
#include "dw_board.h"
#include "dw_touch.h"
#include "dw_aht20.h"
#include "dc_evlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "dw_device";

static dw_device_state_t s_state = {
    .power_status = false,
    .active_status = false,
    .screen_state = DW_SCREEN_INFO,
    .set_temperature = 45,
    .set_time_hours = 6,
    .elapsed_sec = 0,
    .remaining_sec = 21600,
    .preset = DW_PRESET_PLA,
    .ambient_temp_c = 0.0f,
    .ambient_humidity_rh = 0.0f,
    .physical_power_pressed = false,
};

static SemaphoreHandle_t s_state_mutex = NULL;
static dw_device_state_cb_t s_cb = NULL;
static void *s_cb_user_data = NULL;

static uint32_t s_idle_timer_sec = 0;
static uint32_t s_screen_timer_sec = 0;
static uint32_t s_sensor_timer_sec = 0;
static bool s_last_phys_btn_state = false;

static void notify_state_changed(void)
{
    if (s_cb) {
        s_cb(&s_state, s_cb_user_data);
    }
}

const char *dw_screen_state_to_str(dw_screen_state_t screen)
{
    switch (screen) {
        case DW_SCREEN_INFO: return "INFO";
        case DW_SCREEN_TEMP: return "TEMPERATURE";
        case DW_SCREEN_TIME: return "TIME";
        default:             return "UNKNOWN";
    }
}

const char *dw_preset_to_str(dw_preset_profile_t preset)
{
    switch (preset) {
        case DW_PRESET_PLA:    return "PLA (45°C/6h)";
        case DW_PRESET_PETG:   return "PETG (50°C/8h)";
        case DW_PRESET_TPU:    return "TPU (50°C/10h)";
        case DW_PRESET_ABS:    return "ABS (50°C/12h)";
        case DW_PRESET_CUSTOM: return "CUSTOM";
        default:               return "UNKNOWN";
    }
}

dw_preset_profile_t dw_preset_from_str(const char *str)
{
    if (strcasecmp(str, "PLA") == 0) return DW_PRESET_PLA;
    if (strcasecmp(str, "PETG") == 0) return DW_PRESET_PETG;
    if (strcasecmp(str, "TPU") == 0) return DW_PRESET_TPU;
    if (strcasecmp(str, "ABS") == 0) return DW_PRESET_ABS;
    return DW_PRESET_CUSTOM;
}

esp_err_t dw_device_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initializing Sovol SH01 state machine...");

    // Read saved targets from NVS if available
    nvs_handle_t h;
    if (nvs_open("dw_state", NVS_READONLY, &h) == ESP_OK) {
        uint8_t temp = 45, time_h = 6;
        if (nvs_get_u8(h, "temp", &temp) == ESP_OK) s_state.set_temperature = temp;
        if (nvs_get_u8(h, "time_h", &time_h) == ESP_OK) s_state.set_time_hours = time_h;
        nvs_close(h);
    }

    s_state.remaining_sec = (uint32_t)s_state.set_time_hours * 3600;

    dc_evlog_add("Device init: target=%u C, time=%u h", s_state.set_temperature, s_state.set_time_hours);
    return ESP_OK;
}

esp_err_t dw_device_register_state_cb(dw_device_state_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_cb_user_data = user_data;
    return ESP_OK;
}

dw_device_state_t dw_device_get_state(void)
{
    dw_device_state_t snapshot;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snapshot = s_state;
    xSemaphoreGive(s_state_mutex);
    return snapshot;
}

esp_err_t dw_device_set_power(bool power_on)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_state.power_status == power_on) {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_state_mutex);

    // Send power touch pulse
    esp_err_t ret = dw_touch_press(DW_BUTTON_POWER);
    if (ret == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.power_status = power_on;
        if (!power_on) {
            s_state.active_status = false;
            s_state.screen_state = DW_SCREEN_INFO;
            s_state.elapsed_sec = 0;
        }
        s_idle_timer_sec = 0;
        s_screen_timer_sec = 0;
        dc_evlog_add("Power state set to %s", power_on ? "ON" : "OFF");
        xSemaphoreGive(s_state_mutex);
        notify_state_changed();
    }
    return ret;
}

esp_err_t dw_device_toggle_power(void)
{
    bool current;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    current = s_state.power_status;
    xSemaphoreGive(s_state_mutex);
    return dw_device_set_power(!current);
}

esp_err_t dw_device_press_m(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_state.power_status) {
        xSemaphoreGive(s_state_mutex);
        ESP_LOGW(TAG, "Cannot press M: device is powered off");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_state_mutex);

    esp_err_t ret = dw_touch_press(DW_BUTTON_MODE);
    if (ret == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        switch (s_state.screen_state) {
            case DW_SCREEN_INFO:
                s_state.screen_state = DW_SCREEN_TEMP;
                break;
            case DW_SCREEN_TEMP:
                s_state.screen_state = DW_SCREEN_TIME;
                break;
            case DW_SCREEN_TIME:
                s_state.screen_state = DW_SCREEN_INFO;
                s_state.active_status = true; // Entering INFO from TIME starts drying
                s_state.elapsed_sec = 0;
                s_state.remaining_sec = (uint32_t)s_state.set_time_hours * 3600;
                dc_evlog_add("Drying started: %u C for %u hours", s_state.set_temperature, s_state.set_time_hours);
                break;
        }
        s_screen_timer_sec = 0;
        dc_evlog_add("M button pressed -> Screen: %s", dw_screen_state_to_str(s_state.screen_state));
        xSemaphoreGive(s_state_mutex);
        notify_state_changed();
    }
    return ret;
}

esp_err_t dw_device_press_a(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_state.power_status) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_state_mutex);

    esp_err_t ret = dw_touch_press(DW_BUTTON_ADJUST);
    if (ret == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_state.screen_state == DW_SCREEN_TEMP) {
            // Cycle temp: 40 -> 45 -> 50 -> 40
            if (s_state.set_temperature == 40) s_state.set_temperature = 45;
            else if (s_state.set_temperature == 45) s_state.set_temperature = 50;
            else s_state.set_temperature = 40;
            dc_evlog_add("A button pressed -> Temp target: %u C", s_state.set_temperature);
        } else if (s_state.screen_state == DW_SCREEN_TIME) {
            // Cycle time: 6 -> 7 -> 8 -> 9 -> 10 -> 11 -> 12 -> 6
            if (s_state.set_time_hours >= 12) s_state.set_time_hours = 6;
            else s_state.set_time_hours++;
            s_state.remaining_sec = (uint32_t)s_state.set_time_hours * 3600;
            dc_evlog_add("A button pressed -> Time target: %u h", s_state.set_time_hours);
        }
        s_screen_timer_sec = 0;
        xSemaphoreGive(s_state_mutex);
        notify_state_changed();
    }
    return ret;
}

esp_err_t dw_device_reset_sequence(void)
{
    dc_evlog_add("Executing reset-before-automation sequence...");
    ESP_LOGI(TAG, "Resetting Sovol dryer state via power cycle...");

    // Pulse Power button to ensure power OFF
    dw_touch_press(DW_BUTTON_POWER);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Pulse Power button to power ON (default state is 40°C / 6h)
    dw_touch_press(DW_BUTTON_POWER);
    vTaskDelay(pdMS_TO_TICKS(1000));

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.power_status = true;
    s_state.active_status = false;
    s_state.screen_state = DW_SCREEN_INFO;
    s_state.set_temperature = 40;
    s_state.set_time_hours = 6;
    s_state.elapsed_sec = 0;
    s_state.remaining_sec = 21600;
    s_idle_timer_sec = 0;
    s_screen_timer_sec = 0;
    xSemaphoreGive(s_state_mutex);

    notify_state_changed();
    return ESP_OK;
}

esp_err_t dw_device_set_target(uint8_t temp_c, uint8_t time_hours)
{
    if (temp_c != 40 && temp_c != 45 && temp_c != 50) {
        ESP_LOGE(TAG, "Invalid temp target: %d. Allowed: 40, 45, 50", temp_c);
        return ESP_ERR_INVALID_ARG;
    }
    if (time_hours < 6 || time_hours > 12) {
        ESP_LOGE(TAG, "Invalid time target: %d. Allowed: 6..12", time_hours);
        return ESP_ERR_INVALID_ARG;
    }

    // Step 1: Force known reset state (Power OFF -> Power ON: 40°C / 6h)
    dw_device_reset_sequence();

    // Calculate required A-button presses for Temperature from 40°C base
    uint8_t temp_presses = 0;
    if (temp_c == 45) temp_presses = 1;
    else if (temp_c == 50) temp_presses = 2;

    // Calculate required A-button presses for Time from 6h base
    uint8_t time_presses = time_hours - 6;

    // Enter Temperature screen (M press 1)
    dw_device_press_m();

    for (uint8_t i = 0; i < temp_presses; i++) {
        dw_device_press_a();
    }

    // Enter Time screen (M press 2)
    dw_device_press_m();

    for (uint8_t i = 0; i < time_presses; i++) {
        dw_device_press_a();
    }

    // Enter Info screen & Start (M press 3)
    dw_device_press_m();

    // Save defaults to NVS
    nvs_handle_t h;
    if (nvs_open("dw_state", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "temp", temp_c);
        nvs_set_u8(h, "time_h", time_hours);
        nvs_commit(h);
        nvs_close(h);
    }

    return ESP_OK;
}

esp_err_t dw_device_apply_preset(dw_preset_profile_t preset)
{
    uint8_t temp = 45, time_h = 6;
    switch (preset) {
        case DW_PRESET_PLA:  temp = 45; time_h = 6; break;
        case DW_PRESET_PETG: temp = 50; time_h = 8; break;
        case DW_PRESET_TPU:  temp = 50; time_h = 10; break;
        case DW_PRESET_ABS:  temp = 50; time_h = 12; break;
        default: return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.preset = preset;
    xSemaphoreGive(s_state_mutex);

    dc_evlog_add("Applying preset profile: %s (%u C / %u h)", dw_preset_to_str(preset), temp, time_h);
    return dw_device_set_target(temp, time_h);
}

esp_err_t dw_device_start_drying(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint8_t temp = s_state.set_temperature;
    uint8_t time_h = s_state.set_time_hours;
    xSemaphoreGive(s_state_mutex);

    return dw_device_set_target(temp, time_h);
}

esp_err_t dw_device_stop_drying(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.active_status = false;
    s_state.elapsed_sec = 0;
    dc_evlog_add("Drying cycle manually stopped");
    xSemaphoreGive(s_state_mutex);
    notify_state_changed();
    return dw_device_set_power(false);
}

void dw_device_tick_1s(void)
{
    bool phys_btn = dw_board_read_power_btn_pressed();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    // Detect physical power button press edge
    s_state.physical_power_pressed = phys_btn;
    if (phys_btn && !s_last_phys_btn_state) {
        dc_evlog_add("Physical power button press detected off BS813A-1 pin 4");
        ESP_LOGI(TAG, "Physical power button tap detected");
    }
    s_last_phys_btn_state = phys_btn;

    // Handle sensor updates every polling_interval seconds
    s_sensor_timer_sec++;
    if (s_sensor_timer_sec >= dw_aht20_get_polling_interval()) {
        s_sensor_timer_sec = 0;
        float temp = 0.0f, rh = 0.0f;
        if (dw_aht20_read(&temp, &rh) == ESP_OK) {
            s_state.ambient_temp_c = temp;
            s_state.ambient_humidity_rh = rh;
        }
    }

    if (s_state.power_status) {
        if (s_state.active_status) {
            s_state.elapsed_sec++;
            uint32_t total_sec = (uint32_t)s_state.set_time_hours * 3600;
            if (s_state.elapsed_sec >= total_sec) {
                s_state.remaining_sec = 0;
                s_state.active_status = false;
                s_state.power_status = false;
                dc_evlog_add("Drying cycle complete! Powered down automatically.");
                ESP_LOGI(TAG, "Drying cycle finished!");
            } else {
                s_state.remaining_sec = total_sec - s_state.elapsed_sec;
            }
        } else {
            // Idle timer: 3-minute power-off timeout on Sovol SH01
            s_idle_timer_sec++;
            if (s_idle_timer_sec >= 180) {
                s_state.power_status = false;
                s_idle_timer_sec = 0;
                dc_evlog_add("3-minute idle timeout reached. Powered down automatically.");
                ESP_LOGI(TAG, "3-minute idle power off");
            }
        }

        // Screen setting timeout: 5-second auto return to INFO screen
        if (s_state.screen_state != DW_SCREEN_INFO) {
            s_screen_timer_sec++;
            if (s_screen_timer_sec >= 5) {
                s_state.screen_state = DW_SCREEN_INFO;
                s_screen_timer_sec = 0;
                dc_evlog_add("5-second settings timeout: returned to INFO screen");
            }
        }
    } else {
        s_idle_timer_sec = 0;
        s_screen_timer_sec = 0;
    }

    xSemaphoreGive(s_state_mutex);
    notify_state_changed();
}
