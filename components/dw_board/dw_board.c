// SPDX-License-Identifier: MIT
// dw_board — GPIO initialization and physical sense input reading
#include "dw_board.h"
#include "esp_log.h"

static const char *TAG = "dw_board";

esp_err_t dw_board_init(void)
{
    ESP_LOGI(TAG, "Initializing hardware GPIO pinout...");

    // Optocouplers are ACTIVE-HIGH (LED lit when driven HIGH). Idle LOW = off;
    // a pull-down keeps them off (and holds them low through the boot window
    // once configured) so the pads aren't held down at startup.
    gpio_config_t opto_cfg = {
        .pin_bit_mask = (1ULL << DW_GPIO_OPTO_M) | (1ULL << DW_GPIO_OPTO_A) | (1ULL << DW_GPIO_OPTO_P),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&opto_cfg);
    if (ret != ESP_OK) return ret;

    gpio_set_level(DW_GPIO_OPTO_M, 0);
    gpio_set_level(DW_GPIO_OPTO_A, 0);
    gpio_set_level(DW_GPIO_OPTO_P, 0);

    // Configure power sense pin (BS813A-1 pin 4 tap: idle HIGH, active LOW)
    gpio_config_t sense_cfg = {
        .pin_bit_mask = (1ULL << DW_GPIO_POWER_SENSE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&sense_cfg);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "GPIO initialization complete.");
    return ESP_OK;
}

bool dw_board_read_power_btn_pressed(void)
{
    // Pin 4 of BS813A-1 is idle HIGH, active LOW when physically touched
    return gpio_get_level(DW_GPIO_POWER_SENSE) == 0;
}
