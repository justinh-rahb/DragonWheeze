// SPDX-License-Identifier: MIT
// dw_mqtt — Home Assistant MQTT Discovery & Telemetry service for DragonWheeze
#pragma once

#include "esp_err.h"
#include "dc_mqtt.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;
    const char *device_name;  // e.g. "DragonWheeze SH01"
    const char *device_id;    // e.g. "dragonwheeze_a1b2"
} dw_mqtt_config_t;

// Runtime, NVS-persisted Home Assistant / MQTT settings (set from the web setup
// page). When `enabled` and `host` are set, the wheeze connects to the broker as
// both a watcher (telemetry + discovery) and a controller (command topics).
typedef struct {
    bool enabled;
    char host[64];
    uint16_t port;     // default 1883
    char user[32];
    char pass[64];
    char topic[48];    // base topic prefix, default "dragonwheeze"
} dw_mqtt_settings_t;

// Load/save the persisted settings (NVS namespace "app_nvs").
esp_err_t dw_mqtt_get_settings(dw_mqtt_settings_t *out);
esp_err_t dw_mqtt_set_settings(const dw_mqtt_settings_t *in);

// Start MQTT client, register HA auto-discovery, and begin telemetry sync
esp_err_t dw_mqtt_init(const dw_mqtt_config_t *config);

// Load settings from NVS and (re)start the client if enabled+host set. Safe to
// call repeatedly — it stops any existing client first. No-op when disabled.
esp_err_t dw_mqtt_start(const char *device_name, const char *device_id);

// Stop MQTT service
esp_err_t dw_mqtt_stop(void);

// Publish state updates immediately
esp_err_t dw_mqtt_publish_state(void);

#ifdef __cplusplus
}
#endif
