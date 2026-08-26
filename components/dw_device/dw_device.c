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
#include "freertos/queue.h"
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
    .active_profile = 0,
    .ambient_temp_c = 0.0f,
    .ambient_humidity_rh = 0.0f,
    .physical_power_pressed = false,
};

// Editable drying profiles. Defaults mirror the original fixed presets; users
// override name/temp/time from the settings page. Persisted as an NVS blob.
static dw_profile_t s_profiles[DW_PROFILE_COUNT] = {
    { "PLA",  45, 6  },
    { "PETG", 50, 8  },
    { "TPU",  50, 10 },
    { "ABS",  50, 12 },
};

static SemaphoreHandle_t s_state_mutex = NULL;
static dw_device_state_cb_t s_cb = NULL;
static void *s_cb_user_data = NULL;

// Background actuation worker: the physical button dance blocks for seconds, so
// it must never run on the HTTP/MQTT thread. Requests are queued here.
typedef struct { uint8_t type, a, b; } dw_action_msg_t;
static QueueHandle_t s_action_queue = NULL;

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

static void dw_action_worker(void *arg)
{
    (void)arg;
    dw_action_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_action_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        switch ((dw_action_type_t)msg.type) {
        case DW_ACT_POWER_TOGGLE:  dw_device_toggle_power();            break;
        case DW_ACT_POWER_ON:      dw_device_set_power(true);           break;
        case DW_ACT_POWER_OFF:     dw_device_set_power(false);          break;
        case DW_ACT_START:         dw_device_start_drying();            break;
        case DW_ACT_STOP:          dw_device_stop_drying();             break;
        case DW_ACT_RESET:         dw_device_reset_sequence();          break;
        case DW_ACT_PRESS_M:       dw_device_press_m();                 break;
        case DW_ACT_PRESS_A:       dw_device_press_a();                 break;
        case DW_ACT_SET_TARGET:    dw_device_set_target(msg.a, msg.b);  break;
        case DW_ACT_APPLY_PROFILE: dw_device_apply_profile(msg.a);      break;
        }
    }
}

esp_err_t dw_device_enqueue(dw_action_type_t type, uint8_t a, uint8_t b)
{
    if (!s_action_queue) return ESP_ERR_INVALID_STATE;
    // Cheap up-front validation so the caller still gets a synchronous error.
    if (type == DW_ACT_SET_TARGET) {
        if (a != 40 && a != 45 && a != 50) return ESP_ERR_INVALID_ARG;
        if (b < 6 || b > 48 || (b % 2) != 0) return ESP_ERR_INVALID_ARG;
    } else if (type == DW_ACT_APPLY_PROFILE) {
        if (a >= DW_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    }
    dw_action_msg_t msg = { (uint8_t)type, a, b };
    return xQueueSend(s_action_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t dw_device_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    s_action_queue = xQueueCreate(8, sizeof(dw_action_msg_t));
    xTaskCreate(dw_action_worker, "dw_action", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initializing Sovol SH01 state machine...");

    // Read saved targets from NVS if available
    nvs_handle_t h;
    if (nvs_open("dw_state", NVS_READONLY, &h) == ESP_OK) {
        uint8_t temp = 45, time_h = 6;
        if (nvs_get_u8(h, "temp", &temp) == ESP_OK) s_state.set_temperature = temp;
        if (nvs_get_u8(h, "time_h", &time_h) == ESP_OK) s_state.set_time_hours = time_h;
        // Restore editable profiles blob (falls back to defaults if absent/wrong size).
        dw_profile_t saved[DW_PROFILE_COUNT];
        size_t sz = sizeof(saved);
        if (nvs_get_blob(h, "profiles", saved, &sz) == ESP_OK && sz == sizeof(saved)) {
            memcpy(s_profiles, saved, sizeof(s_profiles));
            for (int i = 0; i < DW_PROFILE_COUNT; ++i)
                s_profiles[i].name[DW_PROFILE_NAME_MAX - 1] = '\0';
        }
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
            // Cycle temp: 50 -> 40 -> 45 -> 50 (Comgrow order)
            if (s_state.set_temperature == 50) s_state.set_temperature = 40;
            else if (s_state.set_temperature == 40) s_state.set_temperature = 45;
            else s_state.set_temperature = 50;
            dc_evlog_add("A button pressed -> Temp target: %u C", s_state.set_temperature);
        } else if (s_state.screen_state == DW_SCREEN_TIME) {
            // Cycle time in 2h steps: 6 -> 8 -> ... -> 48 -> 6
            if (s_state.set_time_hours >= 48) s_state.set_time_hours = 6;
            else s_state.set_time_hours += 2;
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
    // Decide from the MODEL's power state, not GPIO10 — that pin is a momentary
    // touch-detect (only HIGH while a finger/opto is on the pad), not a steady
    // power-state line, so it reads "off" even when the dryer is running.
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool powered = s_state.power_status;
    xSemaphoreGive(s_state_mutex);
    dc_evlog_add("Reset: model power=%d — normalizing to ON @ default", powered);

    if (powered) {
        // Power-cycle to clear settings back to the default, ending ON.
        dw_touch_press(DW_BUTTON_POWER);            // -> off
        vTaskDelay(pdMS_TO_TICKS(1500));
        dw_touch_press(DW_BUTTON_POWER);            // -> on
    } else {
        // Already off: one press powers it on at the default.
        dw_touch_press(DW_BUTTON_POWER);            // -> on
    }
    // Let the panel finish its power-on boot before the M/A dance (it drops
    // presses that arrive during the 88:88 self-test).
    vTaskDelay(pdMS_TO_TICKS(2500));

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.power_status = true;
    s_state.active_status = false;
    s_state.screen_state = DW_SCREEN_INFO;
    s_state.set_temperature = 50;   // SH01 powers on at 50C
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
    if (time_hours < 6 || time_hours > 48 || (time_hours % 2) != 0) {
        ESP_LOGE(TAG, "Invalid time target: %d. Allowed: 6..48 in 2h steps", time_hours);
        return ESP_ERR_INVALID_ARG;
    }

    // Step 1: Force known reset state (Power OFF -> Power ON: 50°C / 6h base)
    dw_device_reset_sequence();

    // A-button presses for Temperature from the 50°C base (cycle 50 -> 40 -> 45)
    uint8_t temp_presses = (temp_c == 40) ? 1 : (temp_c == 45) ? 2 : 0;

    // A-button presses for Time from the 6h base (2h steps: 6,8,...,48)
    uint8_t time_presses = (time_hours - 6) / 2;

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

size_t dw_device_get_profiles(dw_profile_t *out, size_t max)
{
    if (!out) return 0;
    size_t n = max < DW_PROFILE_COUNT ? max : DW_PROFILE_COUNT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(out, s_profiles, n * sizeof(dw_profile_t));
    xSemaphoreGive(s_state_mutex);
    return n;
}

esp_err_t dw_device_set_profiles(const dw_profile_t *in, size_t count)
{
    if (!in || count != DW_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    // Validate every profile before committing any (atomic save).
    for (size_t i = 0; i < count; ++i) {
        if (in[i].name[0] == '\0') return ESP_ERR_INVALID_ARG;
        if (in[i].temp_c != 40 && in[i].temp_c != 45 && in[i].temp_c != 50)
            return ESP_ERR_INVALID_ARG;
        if (in[i].time_hours < 6 || in[i].time_hours > 48 || (in[i].time_hours % 2) != 0)
            return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(s_profiles, in, sizeof(s_profiles));
    for (int i = 0; i < DW_PROFILE_COUNT; ++i)
        s_profiles[i].name[DW_PROFILE_NAME_MAX - 1] = '\0';
    xSemaphoreGive(s_state_mutex);

    nvs_handle_t h;
    if (nvs_open("dw_state", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "profiles", s_profiles, sizeof(s_profiles));
        nvs_commit(h);
        nvs_close(h);
    }
    dc_evlog_add("Drying profiles updated");
    return ESP_OK;
}

esp_err_t dw_device_apply_profile(uint8_t index)
{
    if (index >= DW_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint8_t temp = s_profiles[index].temp_c;
    uint8_t time_h = s_profiles[index].time_hours;
    s_state.active_profile = (int8_t)index;
    char name[DW_PROFILE_NAME_MAX];
    memcpy(name, s_profiles[index].name, sizeof(name));
    xSemaphoreGive(s_state_mutex);

    dc_evlog_add("Applying profile %u: %s (%u C / %u h)", index, name, temp, time_h);
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

    // Handle sensor updates every polling_interval seconds (0 = disabled;
    // never touch the shared I2C bus, e.g. to stop the SH01 E0 error).
    uint32_t poll_iv = dw_aht20_get_polling_interval();
    s_sensor_timer_sec++;
    if (poll_iv > 0 && s_sensor_timer_sec >= poll_iv) {
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
