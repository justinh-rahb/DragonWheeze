// SPDX-License-Identifier: MIT
// dw_device — Sovol SH01 Filament Dryer state machine & on-device control logic
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DW_SCREEN_INFO = 0,
    DW_SCREEN_TEMP,
    DW_SCREEN_TIME,
} dw_screen_state_t;

typedef enum {
    DW_PRESET_PLA = 0,   // 45°C / 6h
    DW_PRESET_PETG,      // 50°C / 8h
    DW_PRESET_TPU,       // 50°C / 10h
    DW_PRESET_ABS,       // 50°C / 12h
    DW_PRESET_CUSTOM,
} dw_preset_profile_t;

// Editable drying profiles. The SH01 only accepts temp ∈ {40,45,50} and
// time 6..12h, so a profile is just a named shortcut to one of those combos.
#define DW_PROFILE_COUNT     4
#define DW_PROFILE_NAME_MAX  16
typedef struct {
    char name[DW_PROFILE_NAME_MAX];  // user-set material/type label
    uint8_t temp_c;                  // 40 / 45 / 50
    uint8_t time_hours;              // 6..12
} dw_profile_t;

typedef struct {
    bool power_status;           // Dryer main power ON/OFF
    bool active_status;          // Active drying cycle running
    dw_screen_state_t screen_state;
    uint8_t set_temperature;     // Target temp °C (40, 45, 50)
    uint8_t set_time_hours;      // Target time hours (6..12)
    uint32_t elapsed_sec;        // Seconds elapsed in current drying cycle
    uint32_t remaining_sec;      // Seconds remaining in current drying cycle
    dw_preset_profile_t preset;  // Legacy fixed preset (v1/MQTT compatibility)
    int8_t active_profile;       // Index into the editable profiles, -1 = none
    float ambient_temp_c;        // Sensor temp °C
    float ambient_humidity_rh;   // Sensor humidity %RH
    bool physical_power_pressed; // True if physical power button currently held
} dw_device_state_t;

typedef void (*dw_device_state_cb_t)(const dw_device_state_t *state, void *user_data);

// Initialize state machine and read saved NVS defaults
esp_err_t dw_device_init(void);

// Register state change listener callback
esp_err_t dw_device_register_state_cb(dw_device_state_cb_t cb, void *user_data);

// Get current state snapshot
dw_device_state_t dw_device_get_state(void);

// Power control
esp_err_t dw_device_toggle_power(void);
esp_err_t dw_device_set_power(bool power_on);

// Drying cycle control
esp_err_t dw_device_start_drying(void);
esp_err_t dw_device_stop_drying(void);

// Set target temperature (40/45/50) and duration (6..12)
esp_err_t dw_device_set_target(uint8_t temp_c, uint8_t time_hours);

// Apply a material preset (PLA, PETG, TPU, ABS)
esp_err_t dw_device_apply_preset(dw_preset_profile_t preset);

// Editable drying profiles (NVS-persisted). get returns the count written.
size_t dw_device_get_profiles(dw_profile_t *out, size_t max);
esp_err_t dw_device_set_profiles(const dw_profile_t *in, size_t count);
esp_err_t dw_device_apply_profile(uint8_t index);

// Actuations that drive the physical buttons take several seconds (power cycle +
// M/A pulses). Callers on latency-sensitive threads (the HTTP server, the MQTT
// client) must NOT run them inline or they starve that thread. Enqueue instead:
// the request is validated and returns immediately; a background worker runs the
// button dance. Returns ESP_ERR_INVALID_ARG on bad params, ESP_ERR_NO_MEM if the
// queue is full.
typedef enum {
    DW_ACT_POWER_TOGGLE = 0,
    DW_ACT_POWER_ON,
    DW_ACT_POWER_OFF,
    DW_ACT_START,
    DW_ACT_STOP,
    DW_ACT_RESET,
    DW_ACT_PRESS_M,
    DW_ACT_PRESS_A,
    DW_ACT_SET_TARGET,     // a = temp_c, b = time_hours
    DW_ACT_APPLY_PROFILE,  // a = profile index
} dw_action_type_t;
esp_err_t dw_device_enqueue(dw_action_type_t type, uint8_t a, uint8_t b);

// Execute reset sequence (Power OFF -> ON -> M/A button pulses)
esp_err_t dw_device_reset_sequence(void);

// Manual button presses (emulating front-panel M or A tap)
esp_err_t dw_device_press_m(void);
esp_err_t dw_device_press_a(void);

// 1-second periodic tick handler (updates runtimes, timeouts, and sensors)
void dw_device_tick_1s(void);

// Helpers for screen & preset strings
const char *dw_screen_state_to_str(dw_screen_state_t screen);
const char *dw_preset_to_str(dw_preset_profile_t preset);
dw_preset_profile_t dw_preset_from_str(const char *str);

#ifdef __cplusplus
}
#endif
