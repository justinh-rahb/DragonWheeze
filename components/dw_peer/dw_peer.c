// SPDX-License-Identifier: MIT
#include "dw_peer.h"

#include "dc_peer.h"
#include "dw_device.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "dw_peer";

#define STATUS_MS    2000
#define ANNOUNCE_MS  6000

static void fill_ip(uint8_t ip[4])
{
    memset(ip, 0, 4);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        uint32_t a = info.ip.addr;
        ip[0] = a & 0xff; ip[1] = (a >> 8) & 0xff; ip[2] = (a >> 16) & 0xff; ip[3] = (a >> 24) & 0xff;
    }
}

static void publish_announce(void)
{
    dc_peer_announce_t a = {0};
    a.kind  = DC_PEER_KIND_WHEEZE;
    a.caps  = DC_PEER_CAP_BIT(DC_PEER_CAP_ANNOUNCE) | DC_PEER_CAP_BIT(DC_PEER_CAP_DRYER);
    fill_ip(a.ip);
    strlcpy(a.name, "DragonWheeze", sizeof(a.name));
    strlcpy(a.fw, esp_app_get_description()->version, sizeof(a.fw));
    dc_peer_publish(DC_PEER_CAP_ANNOUNCE, &a, sizeof(a));
}

static void publish_status(void)
{
    dw_device_state_t s = dw_device_get_state();

    dc_peer_dryer_t d = {0};
    d.mode  = !s.power_status ? 0 : (s.active_status ? 2 : 1);   // off / idle / drying
    d.flags = (s.power_status ? DC_PEER_DRYER_POWER : 0)
            | (s.active_status ? DC_PEER_DRYER_ACTIVE : 0);
    d.set_temp_c = s.set_temperature;
    d.set_time_h = s.set_time_hours;
    d.ambient_dc = (isfinite(s.ambient_temp_c) && s.ambient_temp_c > 0.0f)
                 ? (int16_t)(s.ambient_temp_c * 10.0f) : DC_PEER_TEMP_UNKNOWN;
    if (isfinite(s.ambient_humidity_rh) && s.ambient_humidity_rh > 0.0f) {
        int rh = (int)(s.ambient_humidity_rh + 0.5f);
        d.humidity_pct = rh > 100 ? 100 : (uint8_t)rh;
    } else {
        d.humidity_pct = DC_PEER_RH_UNKNOWN;
    }
    d.remaining_sec = s.remaining_sec;
    d.elapsed_sec   = s.elapsed_sec;
    dc_peer_publish(DC_PEER_CAP_DRYER, &d, sizeof(d));
}

static void peer_task(void *arg)
{
    (void)arg;
    int since_announce = ANNOUNCE_MS;   // announce on the first tick
    for (;;) {
        publish_status();
        since_announce += STATUS_MS;
        if (since_announce >= ANNOUNCE_MS) { publish_announce(); since_announce = 0; }
        vTaskDelay(pdMS_TO_TICKS(STATUS_MS));
    }
}

esp_err_t dw_peer_start(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char self_id[DC_PEER_ID_MAX];
    snprintf(self_id, sizeof(self_id), "dragonwheeze-%02x%02x%02x", mac[3], mac[4], mac[5]);

    esp_err_t err = dc_peer_start(self_id);
    if (err != ESP_OK) return err;
    if (xTaskCreate(peer_task, "dw_peer", 3072, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "dryer capability provider up as '%s'", self_id);
    return ESP_OK;
}
