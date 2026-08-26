// SPDX-License-Identifier: MIT
// dw_mqtt — Home Assistant MQTT Discovery & Telemetry service implementation
#include "dw_mqtt.h"
#include "dw_device.h"
#include "dw_aht20.h"
#include "dc_evlog.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "dw_mqtt";

#define DW_MQTT_NVS_NS "app_nvs"

static dc_mqtt_client_t *s_mqtt_client = NULL;
static dw_mqtt_config_t s_config = { 0 };
static char s_state_topic[128] = { 0 };
static char s_cmd_topic_prefix[128] = { 0 };
static char s_topic_root[48] = "dragonwheeze";  // base prefix (configurable)
static bool s_connected = false;
// Backing storage so the const char* fields handed to dc_mqtt stay valid.
static dw_mqtt_settings_t s_settings = { 0 };
static char s_broker_uri[96] = { 0 };

static void on_device_state_changed(const dw_device_state_t *state, void *user_data)
{
    (void)user_data;
    if (s_connected && s_mqtt_client) {
        dw_mqtt_publish_state();
    }
}

static void publish_ha_discovery(void)
{
    if (!s_mqtt_client || !s_connected) return;

    char disc_topic[160];
    char payload[1024];

    const char *dev_name = s_config.device_name ? s_config.device_name : "DragonWheeze";
    const char *dev_id = s_config.device_id ? s_config.device_id : "dragonwheeze";

    // Common device block JSON
    cJSON *device_json = cJSON_CreateObject();
    cJSON_AddStringToObject(device_json, "identifiers", dev_id);
    cJSON_AddStringToObject(device_json, "name", dev_name);
    cJSON_AddStringToObject(device_json, "model", "Sovol SH01 Filament Dryer");
    cJSON_AddStringToObject(device_json, "manufacturer", "Dragon Series");
    cJSON_AddStringToObject(device_json, "sw_version", "1.0.0");
    char *dev_str = cJSON_PrintUnformatted(device_json);
    cJSON_Delete(device_json);

    // 1. Ambient Temperature Sensor
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/sensor/%s/temperature/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Temperature\",\"unique_id\":\"%s_temp\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.temperature }}\","
        "\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"state_class\":\"measurement\",\"dev\":%s}",
        dev_id, s_state_topic, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 2. Ambient Humidity Sensor
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/sensor/%s/humidity/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Humidity\",\"unique_id\":\"%s_humidity\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.humidity }}\","
        "\"unit_of_meas\":\"%%\"," "\"dev_cla\":\"humidity\",\"state_class\":\"measurement\",\"dev\":%s}",
        dev_id, s_state_topic, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 3. Power Switch / Binary Sensor
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/switch/%s/power/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Power\",\"unique_id\":\"%s_power\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.power }}\","
        "\"cmd_t\":\"%s/power/set\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"dev\":%s}",
        dev_id, s_state_topic, s_cmd_topic_prefix, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 4. Active Drying Binary Sensor
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/binary_sensor/%s/active/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Active Drying\",\"unique_id\":\"%s_active\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.active }}\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"dev_cla\":\"running\",\"dev\":%s}",
        dev_id, s_state_topic, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 5. Target Temperature Select
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/select/%s/target_temp/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Target Temperature\",\"unique_id\":\"%s_target_temp\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.target_temp }}\","
        "\"cmd_t\":\"%s/target_temp/set\",\"options\":[\"40\",\"45\",\"50\"],\"dev\":%s}",
        dev_id, s_state_topic, s_cmd_topic_prefix, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 6. Target Time Select (6..48 h)
    char time_opts[256] = { 0 };
    {
        size_t off = 0;
        for (int h = 6; h <= 48 && off < sizeof(time_opts) - 6; h += 2)
            off += snprintf(time_opts + off, sizeof(time_opts) - off, "%s\"%d\"", h > 6 ? "," : "", h);
    }
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/select/%s/target_time/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Target Duration\",\"unique_id\":\"%s_target_time\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.target_time }}\","
        "\"cmd_t\":\"%s/target_time/set\",\"options\":[%s],\"dev\":%s}",
        dev_id, s_state_topic, s_cmd_topic_prefix, time_opts, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 7. Drying Profile Select — options come from the editable profiles.
    char prof_opts[192] = { 0 };
    {
        dw_profile_t profs[DW_PROFILE_COUNT];
        size_t np = dw_device_get_profiles(profs, DW_PROFILE_COUNT);
        size_t off = 0;
        for (size_t i = 0; i < np && off < sizeof(prof_opts) - 2; ++i)
            off += snprintf(prof_opts + off, sizeof(prof_opts) - off, "%s\"%s\"", i ? "," : "", profs[i].name);
    }
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/select/%s/profile/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Drying Profile\",\"unique_id\":\"%s_profile\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.profile }}\","
        "\"cmd_t\":\"%s/profile/set\",\"options\":[%s],\"dev\":%s}",
        dev_id, s_state_topic, s_cmd_topic_prefix, prof_opts, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 8. Start Button
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/button/%s/start/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Start Drying\",\"unique_id\":\"%s_start\",\"cmd_t\":\"%s/start/set\",\"payload_press\":\"PRESS\",\"dev\":%s}",
        dev_id, s_cmd_topic_prefix, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 9. Stop Button
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/button/%s/stop/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Stop Drying\",\"unique_id\":\"%s_stop\",\"cmd_t\":\"%s/stop/set\",\"payload_press\":\"PRESS\",\"dev\":%s}",
        dev_id, s_cmd_topic_prefix, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    // 10. Reset Sequence Button
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/button/%s/reset/config", dev_id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Reset Sequence\",\"unique_id\":\"%s_reset\",\"cmd_t\":\"%s/reset/set\",\"payload_press\":\"PRESS\",\"dev\":%s}",
        dev_id, s_cmd_topic_prefix, dev_str);
    dc_mqtt_publish(s_mqtt_client, disc_topic, payload, strlen(payload), 1, true);

    free(dev_str);
    dc_evlog_add("MQTT: Published HA auto-discovery entities for %s", dev_id);
}

static void handle_mqtt_command(const char *topic, const char *data, int data_len)
{
    char payload[64] = { 0 };
    if (data_len >= (int)sizeof(payload)) data_len = sizeof(payload) - 1;
    memcpy(payload, data, data_len);
    payload[data_len] = '\0';

    ESP_LOGI(TAG, "Received command on topic %s: %s", topic, payload);

    char subtopic[64] = { 0 };
    size_t prefix_len = strlen(s_cmd_topic_prefix);
    if (strncmp(topic, s_cmd_topic_prefix, prefix_len) == 0 && topic[prefix_len] == '/') {
        strncpy(subtopic, topic + prefix_len + 1, sizeof(subtopic) - 1);
    }

    // Enqueue (never run the multi-second button dance on the MQTT client task).
    if (strcmp(subtopic, "power/set") == 0) {
        bool on = (strcasecmp(payload, "ON") == 0 || strcmp(payload, "1") == 0);
        dw_device_enqueue(on ? DW_ACT_POWER_ON : DW_ACT_POWER_OFF, 0, 0);
    } else if (strcmp(subtopic, "start/set") == 0) {
        dw_device_enqueue(DW_ACT_START, 0, 0);
    } else if (strcmp(subtopic, "stop/set") == 0) {
        dw_device_enqueue(DW_ACT_STOP, 0, 0);
    } else if (strcmp(subtopic, "target_temp/set") == 0) {
        dw_device_state_t cur = dw_device_get_state();
        dw_device_enqueue(DW_ACT_SET_TARGET, (uint8_t)atoi(payload), cur.set_time_hours);
    } else if (strcmp(subtopic, "target_time/set") == 0) {
        dw_device_state_t cur = dw_device_get_state();
        dw_device_enqueue(DW_ACT_SET_TARGET, cur.set_temperature, (uint8_t)atoi(payload));
    } else if (strcmp(subtopic, "profile/set") == 0) {
        dw_profile_t profs[DW_PROFILE_COUNT];
        size_t np = dw_device_get_profiles(profs, DW_PROFILE_COUNT);
        for (size_t i = 0; i < np; ++i) {
            if (strcmp(profs[i].name, payload) == 0) { dw_device_enqueue(DW_ACT_APPLY_PROFILE, (uint8_t)i, 0); break; }
        }
    } else if (strcmp(subtopic, "reset/set") == 0) {
        dw_device_enqueue(DW_ACT_RESET, 0, 0);
    }
}

static void mqtt_event_handler(void *ctx, const dc_mqtt_event_t *event)
{
    (void)ctx;
    switch (event->type) {
        case DC_MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to broker");
            s_connected = true;
            dc_evlog_add("MQTT: Connected to broker");

            // Subscribe to all command topics under command prefix
            char sub_topic[160];
            snprintf(sub_topic, sizeof(sub_topic), "%s/#", s_cmd_topic_prefix);
            dc_mqtt_subscribe(s_mqtt_client, sub_topic, 1);

            publish_ha_discovery();
            dw_mqtt_publish_state();
            break;

        case DC_MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected");
            s_connected = false;
            dc_evlog_add("MQTT: Disconnected");
            break;

        case DC_MQTT_EVENT_MESSAGE:
            if (event->topic && event->topic_len > 0) {
                char topic_buf[160] = { 0 };
                if (event->topic_len < (int)sizeof(topic_buf)) {
                    memcpy(topic_buf, event->topic, event->topic_len);
                    handle_mqtt_command(topic_buf, event->data, event->data_len);
                }
            }
            break;

        case DC_MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error event");
            dc_evlog_add("MQTT: Client error encountered");
            break;
    }
}

esp_err_t dw_mqtt_init(const dw_mqtt_config_t *config)
{
    if (!config || !config->broker_uri) return ESP_ERR_INVALID_ARG;

    s_config = *config;
    const char *dev_id = config->device_id ? config->device_id : "dragonwheeze";
    const char *root = s_topic_root[0] ? s_topic_root : "dragonwheeze";

    snprintf(s_state_topic, sizeof(s_state_topic), "%s/%s/state", root, dev_id);
    snprintf(s_cmd_topic_prefix, sizeof(s_cmd_topic_prefix), "%s/%s", root, dev_id);

    dc_mqtt_config_t cfg = {
        .broker_uri = config->broker_uri,
        .username = config->username,
        .password = config->password,
        .event_cb = mqtt_event_handler,
        .event_ctx = NULL,
    };

    esp_err_t ret = dc_mqtt_start(&cfg, &s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dc_mqtt_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    dw_device_register_state_cb(on_device_state_changed, NULL);
    return ESP_OK;
}

esp_err_t dw_mqtt_stop(void)
{
    if (s_mqtt_client) {
        dc_mqtt_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_connected = false;
    }
    return ESP_OK;
}

static void load_str(nvs_handle_t h, const char *key, char *out, size_t sz, const char *dflt)
{
    size_t n = sz;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
        if (dflt) { strncpy(out, dflt, sz - 1); out[sz - 1] = '\0'; }
    }
}

esp_err_t dw_mqtt_get_settings(dw_mqtt_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->port = 1883;
    strcpy(out->topic, "dragonwheeze");
    nvs_handle_t h;
    if (nvs_open(DW_MQTT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0;
        if (nvs_get_u8(h, "mqtt_en", &en) == ESP_OK) out->enabled = en != 0;
        load_str(h, "mqtt_host", out->host, sizeof(out->host), NULL);
        nvs_get_u16(h, "mqtt_port", &out->port);
        load_str(h, "mqtt_user", out->user, sizeof(out->user), NULL);
        load_str(h, "mqtt_pass", out->pass, sizeof(out->pass), NULL);
        load_str(h, "mqtt_topic", out->topic, sizeof(out->topic), "dragonwheeze");
        nvs_close(h);
    }
    if (out->port == 0) out->port = 1883;
    if (!out->topic[0]) strcpy(out->topic, "dragonwheeze");
    return ESP_OK;
}

esp_err_t dw_mqtt_set_settings(const dw_mqtt_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(DW_MQTT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, "mqtt_en", in->enabled ? 1 : 0);
    nvs_set_str(h, "mqtt_host", in->host);
    nvs_set_u16(h, "mqtt_port", in->port ? in->port : 1883);
    nvs_set_str(h, "mqtt_user", in->user);
    nvs_set_str(h, "mqtt_pass", in->pass);
    nvs_set_str(h, "mqtt_topic", in->topic[0] ? in->topic : "dragonwheeze");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t dw_mqtt_start(const char *device_name, const char *device_id)
{
    dw_mqtt_stop();  // idempotent restart

    dw_mqtt_get_settings(&s_settings);
    if (!s_settings.enabled || !s_settings.host[0]) {
        dc_evlog_add("MQTT: disabled (no broker configured)");
        return ESP_OK;
    }

    strncpy(s_topic_root, s_settings.topic[0] ? s_settings.topic : "dragonwheeze",
            sizeof(s_topic_root) - 1);
    s_topic_root[sizeof(s_topic_root) - 1] = '\0';
    snprintf(s_broker_uri, sizeof(s_broker_uri), "mqtt://%s:%u",
             s_settings.host, (unsigned)s_settings.port);

    dw_mqtt_config_t cfg = {
        .broker_uri = s_broker_uri,
        .username = s_settings.user[0] ? s_settings.user : NULL,
        .password = s_settings.pass[0] ? s_settings.pass : NULL,
        .device_name = device_name,
        .device_id = device_id,
    };
    dc_evlog_add("MQTT: connecting to %s", s_broker_uri);
    return dw_mqtt_init(&cfg);
}

esp_err_t dw_mqtt_publish_state(void)
{
    if (!s_mqtt_client || !s_connected) return ESP_ERR_INVALID_STATE;

    dw_device_state_t st = dw_device_get_state();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "power", st.power_status ? "ON" : "OFF");
    cJSON_AddStringToObject(root, "active", st.active_status ? "ON" : "OFF");
    cJSON_AddStringToObject(root, "screen_state", dw_screen_state_to_str(st.screen_state));
    cJSON_AddNumberToObject(root, "target_temp", st.set_temperature);
    cJSON_AddNumberToObject(root, "target_time", st.set_time_hours);
    cJSON_AddNumberToObject(root, "elapsed_mins", st.elapsed_sec / 60);
    cJSON_AddNumberToObject(root, "remaining_mins", st.remaining_sec / 60);
    cJSON_AddStringToObject(root, "preset", dw_preset_to_str(st.preset));
    // Active editable profile (matched by current temp/time) for the HA select.
    {
        dw_profile_t profs[DW_PROFILE_COUNT];
        size_t np = dw_device_get_profiles(profs, DW_PROFILE_COUNT);
        const char *active = "Custom";
        for (size_t i = 0; i < np; ++i)
            if (profs[i].temp_c == st.set_temperature && profs[i].time_hours == st.set_time_hours) {
                active = profs[i].name; break;
            }
        cJSON_AddStringToObject(root, "profile", active);
    }
    cJSON_AddNumberToObject(root, "temperature", (double)((int)(st.ambient_temp_c * 10.0f)) / 10.0);
    cJSON_AddNumberToObject(root, "humidity", (double)((int)(st.ambient_humidity_rh * 10.0f)) / 10.0);
    cJSON_AddStringToObject(root, "physical_power_button", st.physical_power_pressed ? "PRESSED" : "RELEASED");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    int id = dc_mqtt_publish(s_mqtt_client, s_state_topic, json_str, strlen(json_str), 1, true);
    free(json_str);

    return (id >= 0) ? ESP_OK : ESP_FAIL;
}
