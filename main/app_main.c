// SPDX-License-Identifier: MIT
// DragonWheeze — open firmware for the Sovol SH01 Filament Dryer (ESP32-C3 Super Mini)
//
// Native ESP-IDF application using dragon-core components:
// - dc_wifi: Wi-Fi setup & captive portal fallback
// - dc_portal: Local web server & OTA updates
// - dc_mqtt: Transport layer for Home Assistant discovery & remote control
// - dc_evlog: Diagnostic logging for I2C bus error tracking
// - dw_device: On-device state machine and optocoupler sequence manager

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "dw_board.h"
#include "dw_touch.h"
#include "dw_aht20.h"
#include "dw_device.h"
#include "dw_mqtt.h"
#include "dw_portal.h"
#include "dc_evlog.h"
#include "dc_wifi.h"

// Optional local dev config (gitignored) for WiFi / MQTT credentials
#if defined(__has_include)
#  if __has_include("dev_config.h")
#    include "dev_config.h"
#  endif
#endif

static const char *TAG = "dragonwheeze";

static void tick_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Main 1-second state tick task started.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        dw_device_tick_1s();
    }
}

void app_main(void)
{
    // Mark app image valid after successful boot to prevent OTA rollback
    esp_ota_mark_app_valid_cancel_rollback();

    // 1. Console log capture & event ring buffer
    dc_evlog_console_init();
    dc_evlog_init();
    dc_evlog_add("DragonWheeze firmware booting (ESP32-C3)...");

    // 2. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 3. Hardware board pinout initialization
    ESP_ERROR_CHECK(dw_board_init());

    // 4. AHT20 Temperature/Humidity sensor initialization
    dw_aht20_init();

    // 5. State machine initialization
    ESP_ERROR_CHECK(dw_device_init());

    // 6. Wi-Fi networking bringup via dragon-core dc_wifi
    const dc_wifi_identity_t wifi_identity = {
        .hostname = "dragonwheeze",
        .instance_name = "DragonWheeze",
        .ap_ssid_prefix = "DragonWheeze_",
        .ap_password = DC_WIFI_DEFAULT_AP_PASSWORD,
    };
    esp_err_t wifi_err = dc_wifi_set_identity(&wifi_identity);
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "dc_wifi_set_identity: %s (using family defaults)", esp_err_to_name(wifi_err));
    }
    ESP_LOGI(TAG, "Starting dc_wifi...");
    dc_wifi_start();

    // 7. Web Portal bringup via dragon-core dc_portal
    ESP_LOGI(TAG, "Starting web portal...");
    dw_portal_start();

    // 8. MQTT & Home Assistant integration (if broker URI configured)
#if defined(DEV_MQTT_BROKER_URI)
    dw_mqtt_config_t mqtt_cfg = {
        .broker_uri = DEV_MQTT_BROKER_URI,
#  if defined(DEV_MQTT_USER)
        .username = DEV_MQTT_USER,
#  endif
#  if defined(DEV_MQTT_PASS)
        .password = DEV_MQTT_PASS,
#  endif
        .device_name = "DragonWheeze SH01",
        .device_id = "dragonwheeze_sh01",
    };
    dw_mqtt_init(&mqtt_cfg);
#endif

    // 9. Start periodic state machine tick task
    xTaskCreate(tick_task, "dw_tick", 4096, NULL, 5, NULL);

    dc_evlog_add("DragonWheeze boot sequence complete. System ready.");
    ESP_LOGI(TAG, "DragonWheeze boot complete!");
}
