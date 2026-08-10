// SPDX-License-Identifier: MIT
// dw_portal — Web portal integration implementation
#include "dw_portal.h"
#include "dw_device.h"
#include "dw_aht20.h"
#include "dc_portal.h"
#include "dc_evlog.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>

static const char *TAG = "dw_portal";

static cJSON *describe_product(void *ctx)
{
    (void)ctx;
    dw_device_state_t st = dw_device_get_state();
    dw_aht20_stats_t aht_stats = dw_aht20_get_stats();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product", "DragonWheeze");
    cJSON_AddStringToObject(root, "model", "Sovol SH01 Filament Dryer");
    cJSON_AddBoolToObject(root, "power_status", st.power_status);
    cJSON_AddBoolToObject(root, "active_status", st.active_status);
    cJSON_AddStringToObject(root, "screen_state", dw_screen_state_to_str(st.screen_state));
    cJSON_AddNumberToObject(root, "set_temperature", st.set_temperature);
    cJSON_AddNumberToObject(root, "set_time_hours", st.set_time_hours);
    cJSON_AddNumberToObject(root, "elapsed_sec", st.elapsed_sec);
    cJSON_AddNumberToObject(root, "remaining_sec", st.remaining_sec);
    cJSON_AddStringToObject(root, "preset", dw_preset_to_str(st.preset));
    cJSON_AddNumberToObject(root, "ambient_temp_c", st.ambient_temp_c);
    cJSON_AddNumberToObject(root, "ambient_humidity_rh", st.ambient_humidity_rh);
    cJSON_AddBoolToObject(root, "physical_power_pressed", st.physical_power_pressed);

    cJSON *stats = cJSON_CreateObject();
    cJSON_AddNumberToObject(stats, "aht20_total_reads", aht_stats.total_reads);
    cJSON_AddNumberToObject(stats, "aht20_success_reads", aht_stats.successful_reads);
    cJSON_AddNumberToObject(stats, "aht20_bus_errors", aht_stats.bus_errors);
    cJSON_AddNumberToObject(stats, "aht20_crc_errors", aht_stats.crc_errors);
    cJSON_AddNumberToObject(stats, "aht20_polling_interval_sec", dw_aht20_get_polling_interval());
    cJSON_AddItemToObject(root, "i2c_diagnostics", stats);

    return root;
}

static esp_err_t apply_product(const cJSON *values, void *ctx, char *message, size_t message_size)
{
    (void)ctx;
    if (!values) return ESP_ERR_INVALID_ARG;

    cJSON *action = cJSON_GetObjectItem(values, "action");
    if (!cJSON_IsString(action)) {
        snprintf(message, message_size, "Missing 'action' string parameter");
        return ESP_ERR_INVALID_ARG;
    }

    const char *act = action->valuestring;
    if (strcmp(act, "power_toggle") == 0) {
        dw_device_toggle_power();
    } else if (strcmp(act, "power_on") == 0) {
        dw_device_set_power(true);
    } else if (strcmp(act, "power_off") == 0) {
        dw_device_set_power(false);
    } else if (strcmp(act, "start") == 0) {
        dw_device_start_drying();
    } else if (strcmp(act, "stop") == 0) {
        dw_device_stop_drying();
    } else if (strcmp(act, "reset") == 0) {
        dw_device_reset_sequence();
    } else if (strcmp(act, "press_m") == 0) {
        dw_device_press_m();
    } else if (strcmp(act, "press_a") == 0) {
        dw_device_press_a();
    } else if (strcmp(act, "set_target") == 0) {
        cJSON *temp = cJSON_GetObjectItem(values, "temperature");
        cJSON *time_h = cJSON_GetObjectItem(values, "time_hours");
        if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(time_h)) {
            snprintf(message, message_size, "set_target requires 'temperature' and 'time_hours' numbers");
            return ESP_ERR_INVALID_ARG;
        }
        dw_device_set_target((uint8_t)temp->valueint, (uint8_t)time_h->valueint);
    } else if (strcmp(act, "set_preset") == 0) {
        cJSON *preset = cJSON_GetObjectItem(values, "preset");
        if (!cJSON_IsString(preset)) {
            snprintf(message, message_size, "set_preset requires 'preset' string (PLA, PETG, TPU, ABS)");
            return ESP_ERR_INVALID_ARG;
        }
        dw_device_apply_preset(dw_preset_from_str(preset->valuestring));
    } else {
        snprintf(message, message_size, "Unknown action: %s", act);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(message, message_size, "Action %s applied successfully", act);
    return ESP_OK;
}

static esp_err_t get_status_handler(httpd_req_t *req)
{
    cJSON *json = describe_product(NULL);
    char *response = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    return ESP_OK;
}

static esp_err_t post_control_handler(httpd_req_t *req)
{
    char buf[256] = { 0 };
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    char msg[128] = { 0 };
    esp_err_t err = apply_product(json, NULL, msg, sizeof(msg));
    cJSON_Delete(json);

    cJSON *resp_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp_json, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp_json, "message", msg);
    char *resp_str = cJSON_PrintUnformatted(resp_json);
    cJSON_Delete(resp_json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    free(resp_str);
    return ESP_OK;
}

static const httpd_uri_t uri_get_status = {
    .uri = "/api/v1/dw/status",
    .method = HTTP_GET,
    .handler = get_status_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_post_control = {
    .uri = "/api/v1/dw/control",
    .method = HTTP_POST,
    .handler = post_control_handler,
    .user_ctx = NULL
};

static esp_err_t register_dw_routes(httpd_handle_t server, void *ctx)
{
    (void)ctx;
    httpd_register_uri_handler(server, &uri_get_status);
    httpd_register_uri_handler(server, &uri_post_control);
    ESP_LOGI(TAG, "Registered /api/v1/dw/status and /api/v1/dw/control web routes");
    return ESP_OK;
}

esp_err_t dw_portal_start(void)
{
    ESP_LOGI(TAG, "Starting dw_portal Web Service...");

    dc_portal_config_t cfg = {
        .product = "dragonwheeze",
        .display_name = "DragonWheeze (Sovol SH01)",
        .register_product_routes = register_dw_routes,
        .describe_product = describe_product,
        .apply_product = apply_product,
        .ctx = NULL,
    };

    return dc_portal_start(&cfg);
}

esp_err_t dw_portal_stop(void)
{
    return dc_portal_stop();
}
