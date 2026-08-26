// SPDX-License-Identifier: MIT
// dw_board — hardware pin mappings for DragonWheeze on Sovol SH01 Filament Dryer.
#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Touch optocouplers (4N35). Per the Simply Maker schematic: GPIO -> 200R ->
// LED anode (pin1), LED cathode (pin2) -> GND, so drive is ACTIVE-HIGH (GPIO
// HIGH lights the LED / fires the opto; idle LOW = released). Physical wiring
// confirmed at the bench: pin5=Power, pin6=M, pin7=A.
// NOTE: M/A (U3/U4) only work with their pin2 LED cathode grounded like U2's.
#define DW_GPIO_OPTO_P       GPIO_NUM_5
#define DW_GPIO_OPTO_M       GPIO_NUM_6
#define DW_GPIO_OPTO_A       GPIO_NUM_7

// Opto drive polarity: active-HIGH (LED cathode grounded).
#define DW_OPTO_LEVEL_PRESSED   1
#define DW_OPTO_LEVEL_RELEASED  0

// Power-state sense input: ESPHome "Power Touch" binary_sensor on GPIO10
// (inverted -> touch IC pulls it LOW when powered).
#define DW_GPIO_POWER_SENSE  GPIO_NUM_10

// AHT sensor I2C bus (J2 connector) per the schematic.
#define DW_GPIO_I2C_SDA      GPIO_NUM_4
#define DW_GPIO_I2C_SCL      GPIO_NUM_3
#define DW_I2C_PORT          I2C_NUM_0

// Initialize board GPIOs (sets up optocoupler outputs & power sense input)
esp_err_t dw_board_init(void);

// Read current raw state of the physical power button (true = physically pressed)
bool dw_board_read_power_btn_pressed(void);

#ifdef __cplusplus
}
#endif
