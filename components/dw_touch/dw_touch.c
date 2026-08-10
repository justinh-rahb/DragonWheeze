// SPDX-License-Identifier: MIT
// dw_touch — Optocoupler touch-emulation driver implementation
#include "dw_touch.h"
#include "dw_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "dw_touch";

static SemaphoreHandle_t s_touch_mutex = NULL;

static gpio_num_t get_gpio_for_button(dw_button_t btn)
{
    switch (btn) {
        case DW_BUTTON_MODE:   return DW_GPIO_OPTO_M;
        case DW_BUTTON_ADJUST: return DW_GPIO_OPTO_A;
        case DW_BUTTON_POWER:  return DW_GPIO_OPTO_P;
        default:               return GPIO_NUM_NC;
    }
}

static const char *get_button_name(dw_button_t btn)
{
    switch (btn) {
        case DW_BUTTON_MODE:   return "MODE (M)";
        case DW_BUTTON_ADJUST: return "ADJUST (A)";
        case DW_BUTTON_POWER:  return "POWER (P)";
        default:               return "UNKNOWN";
    }
}

esp_err_t dw_touch_pulse(dw_button_t button, uint32_t pulse_duration_ms)
{
    if (s_touch_mutex == NULL) {
        s_touch_mutex = xSemaphoreCreateMutex();
    }

    gpio_num_t gpio = get_gpio_for_button(button);
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "Invalid button specified: %d", button);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_touch_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire touch mutex for button %s", get_button_name(button));
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGD(TAG, "Pulsing touch button %s for %lu ms", get_button_name(button), (unsigned long)pulse_duration_ms);

    // Drive optocoupler LED active HIGH
    gpio_set_level(gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(pulse_duration_ms));
    gpio_set_level(gpio, 0);

    // Inter-press delay to allow capacitive controller to settle
    vTaskDelay(pdMS_TO_TICKS(DW_TOUCH_INTER_PRESS_DELAY_MS));

    xSemaphoreGive(s_touch_mutex);
    return ESP_OK;
}

esp_err_t dw_touch_press(dw_button_t button)
{
    return dw_touch_pulse(button, DW_TOUCH_DEFAULT_PULSE_MS);
}
