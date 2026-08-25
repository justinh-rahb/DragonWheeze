// SPDX-License-Identifier: MIT
// dw_board — hardware pin mappings for DragonWheeze on Sovol SH01 Filament Dryer.
#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Touch optocouplers (active HIGH to drive 4N35 LED through 200 ohm resistor)
#define DW_GPIO_OPTO_M       GPIO_NUM_4
#define DW_GPIO_OPTO_A       GPIO_NUM_5
#define DW_GPIO_OPTO_P       GPIO_NUM_10   // power opto is wired to GPIO10 (not 6)

// Power-button physical monitoring (tap off pin 4 of BS813A-1 IC, idle HIGH, active LOW)
#define DW_GPIO_POWER_SENSE  GPIO_NUM_7

// AHT20 I2C bus (tapped JST-XH connector)
#define DW_GPIO_I2C_SDA      GPIO_NUM_0
#define DW_GPIO_I2C_SCL      GPIO_NUM_1
#define DW_I2C_PORT          I2C_NUM_0

// Initialize board GPIOs (sets up optocoupler outputs & power sense input)
esp_err_t dw_board_init(void);

// Read current raw state of the physical power button (true = physically pressed)
bool dw_board_read_power_btn_pressed(void);

#ifdef __cplusplus
}
#endif
