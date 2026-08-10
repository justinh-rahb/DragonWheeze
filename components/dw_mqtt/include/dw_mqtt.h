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

// Start MQTT client, register HA auto-discovery, and begin telemetry sync
esp_err_t dw_mqtt_init(const dw_mqtt_config_t *config);

// Stop MQTT service
esp_err_t dw_mqtt_stop(void);

// Publish state updates immediately
esp_err_t dw_mqtt_publish_state(void);

#ifdef __cplusplus
}
#endif
