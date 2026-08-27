// SPDX-License-Identifier: MIT
// dw_board — GPIO initialization and physical sense input reading
#include "dw_board.h"
#include "esp_log.h"

static const char *TAG = "dw_board";

esp_err_t dw_board_init(void)
{
    ESP_LOGI(TAG, "Initializing hardware GPIO pinout...");

    // Optocouplers are ACTIVE-HIGH (GPIO HIGH -> LED lit -> opto fires). Idle
    // LOW = released; a pull-down keeps them released through the boot window
    // (before this runs) so the pads aren't held down at startup.
    gpio_config_t opto_cfg = {
        .pin_bit_mask = (1ULL << DW_GPIO_OPTO_M) | (1ULL << DW_GPIO_OPTO_A) | (1ULL << DW_GPIO_OPTO_P),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&opto_cfg);
    if (ret != ESP_OK) return ret;

    gpio_set_level(DW_GPIO_OPTO_M, DW_OPTO_LEVEL_RELEASED);
    gpio_set_level(DW_GPIO_OPTO_A, DW_OPTO_LEVEL_RELEASED);
    gpio_set_level(DW_GPIO_OPTO_P, DW_OPTO_LEVEL_RELEASED);

    // Power-sense on GPIO10 is DEFUNCT: GPIO10 is now the LCD /WR sniffer tap
    // (dw_lcd owns its config). We deliberately do NOT configure it here — the
    // old power-sense config collided with the WR tap and the WR clock read as a
    // storm of phantom "power button" taps. Physical power detection is gone;
    // the LCD decoder tells us the real state instead.
    ESP_LOGI(TAG, "GPIO initialization complete.");
    return ESP_OK;
}

bool dw_board_read_power_btn_pressed(void)
{
    // Defunct: GPIO10 repurposed as the LCD /WR tap. Never report a phantom tap.
    return false;
}
