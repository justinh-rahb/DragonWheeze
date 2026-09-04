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
#include "dw_lcd.h"
#include "dw_device.h"
#include "dw_mqtt.h"
#include "dw_portal.h"
#include "dw_peer.h"
#include "dc_evlog.h"
#include "dc_wifi.h"

// Optional local dev config (gitignored) for WiFi / MQTT credentials
#if defined(__has_include)
#  if __has_include("dev_config.h")
#    include "dev_config.h"
#  endif
#endif

static const char *TAG = "dragonwheeze";

// Bench convenience: if dev_config.h defines WiFi creds, seed them into NVS on
// boot when nothing is provisioned yet. User/portal creds always win (we only
// seed when the ssid key is empty), and it makes the device auto-join even
// after a hard-boot NVS wipe — no re-entering creds every time.
static void seed_dev_config(void)
{
#ifdef DW_WIFI_SSID
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READWRITE, &h) != ESP_OK) return;
    size_t sz = 0;
    if (nvs_get_str(h, "ssid", NULL, &sz) == ESP_OK && sz > 1) {
        nvs_close(h);
        return;   // already provisioned — don't override
    }
    nvs_set_str(h, "ssid", DW_WIFI_SSID);
    nvs_set_str(h, "password", DW_WIFI_PASS);
    // Default this board to FALLBACK AP mode (3): STA-only while connected (no
    // concurrent AP fighting the C3's radio → reliable joins), portal only if
    // STA fails. Only seeded on a fresh NVS; a user's later choice wins.
    nvs_set_u8(h, "ap_mode", 3);   // DC_WIFI_AP_FALLBACK
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Seeded WiFi creds from dev_config");
#endif
}

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

    // FIRST: drive the optocoupler pins to their idle (off) state ASAP so the
    // dryer's touch pads aren't held down during the boot window. (They float
    // until this runs; doing it before NVS/logging minimizes that flash.)
    ESP_ERROR_CHECK(dw_board_init());

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

    // 4. LCD sniffer — passively decode the mainboard's TM1621C to read the
    // display (and the ambient temp/RH it shows). Replaces the I2C AHT20 path:
    // GPIO3/4 are now CS/DATA taps and we no longer master the shared I2C bus,
    // so the E0 contention is gone for good. (dw_aht20_init() intentionally NOT
    // called — it would grab GPIO3/4 for I2C and conflict with the taps.)
    ESP_ERROR_CHECK(dw_lcd_init(DW_GPIO_LCD_CS, DW_GPIO_LCD_WR, DW_GPIO_LCD_DATA));

    // 5. State machine initialization
    ESP_ERROR_CHECK(dw_device_init());

    // 6. Wi-Fi networking bringup via dragon-core dc_wifi
    seed_dev_config();   // bench: auto-provision creds if none saved
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
    // The ESP32-C3 SuperMini's weak PCB antenna + mesh-DHCP flakiness need the
    // hardened STA path (modem power-save off, no-DHCP-IP watchdog, longer retries).
    // Other Dragon products run on healthier radios and stay on the STANDARD default.
    ESP_ERROR_CHECK(dc_wifi_set_radio_profile(DC_WIFI_RADIO_CONSTRAINED));
    ESP_LOGI(TAG, "Starting dc_wifi...");
    dc_wifi_start();

    // 7. Web Portal bringup via dragon-core dc_portal
    ESP_LOGI(TAG, "Starting web portal...");
    dw_portal_start();

    // 8. Home Assistant / MQTT integration — runtime-configured from the web
    // setup page (persisted in NVS). No-op until a broker is enabled there.
    dw_mqtt_start("DragonWheeze SH01", "dragonwheeze_sh01");

    // 8b. ESP-NOW discovery: broadcast the ANNOUNCE descriptor + dryer status so a
    // console (DragonTouch) can find and display this dryer. Status only; non-fatal.
    esp_err_t peer_err = dw_peer_start();
    if (peer_err != ESP_OK)
        ESP_LOGW(TAG, "dw_peer_start failed: %s (continuing)", esp_err_to_name(peer_err));

    // 9. Start periodic state machine tick task
    xTaskCreate(tick_task, "dw_tick", 4096, NULL, 5, NULL);

    dc_evlog_add("DragonWheeze boot sequence complete. System ready.");
    ESP_LOGI(TAG, "DragonWheeze boot complete!");
}
