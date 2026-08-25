// SPDX-License-Identifier: MIT
// dw_touch — Optocoupler touch-emulation driver for Sovol SH01 buttons.
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DW_BUTTON_MODE,    // 'M' button (cycles INFO -> TEMP -> TIME)
    DW_BUTTON_ADJUST,  // 'A' button (advances temperature / time setting)
    DW_BUTTON_POWER,   // 'P' button (turns dryer power on / off)
} dw_button_t;

#define DW_TOUCH_DEFAULT_PULSE_MS 200
// The SH01 power pad ignores a short tap (guards against accidental on/off) and
// needs a press-and-hold, unlike the M/A pads which take a 200ms tap.
#define DW_TOUCH_POWER_PULSE_MS 1500
#define DW_TOUCH_INTER_PRESS_DELAY_MS 400

// Pulse a touch optocoupler for a given duration (ms) and enforce inter-press delay
esp_err_t dw_touch_pulse(dw_button_t button, uint32_t pulse_duration_ms);

// Pulse with default duration (200ms)
esp_err_t dw_touch_press(dw_button_t button);

#ifdef __cplusplus
}
#endif
