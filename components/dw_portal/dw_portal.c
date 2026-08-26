// SPDX-License-Identifier: MIT
// dw_portal — Web portal integration implementation
#include "dw_portal.h"
#include "dw_device.h"
#include "dw_touch.h"
#include "dw_aht20.h"
#include "dw_mqtt.h"
#include "dc_portal.h"
#include "dc_evlog.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dc_wifi.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dw_portal";

// Monotonic revision bumped on every accepted command so the SPA can detect a
// state change. Mirrors dv_portal / pb_httpd.
static uint32_t s_api_revision = 1;

// ── Control token (family-consistent with DragonBreath / DragonVent) ─────────
// token configured -> the header must match it exactly
// no token         -> any non-empty header value passes (CSRF gate)
#define DW_NVS_NS             "app_nvs"
#define DW_NVS_TOKEN          "ctl_token"
#define DW_TOKEN_MAX          64
#define DW_AUTH_HEADER        "X-Dragon-Auth"
#define DW_AUTH_HEADER_LEGACY "X-DragonBreath-Auth"

static void ctl_token(char *out, size_t outsz)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(DW_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = outsz;
    nvs_get_str(h, DW_NVS_TOKEN, out, &sz);   // leaves out="" on any error
    nvs_close(h);
}

static bool auth_header(httpd_req_t *req, char *out, size_t outsz)
{
    const char *names[] = { DW_AUTH_HEADER, DW_AUTH_HEADER_LEGACY };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t len = httpd_req_get_hdr_value_len(req, names[i]);
        if (len == 0 || len >= outsz) continue;
        if (httpd_req_get_hdr_value_str(req, names[i], out, outsz) == ESP_OK && out[0])
            return true;
    }
    out[0] = '\0';
    return false;
}

static bool auth_ok(httpd_req_t *req)
{
    char token[DW_TOKEN_MAX + 1];
    ctl_token(token, sizeof token);
    char value[DW_TOKEN_MAX + 1] = {0};
    if (!auth_header(req, value, sizeof value)) return false;
    if (token[0]) return strcmp(value, token) == 0;   // configured -> exact match
    return true;                                      // else presence-only CSRF gate
}

// dc_portal calls this for its own routes (provisioning, logs, OTA, factory
// reset). The wheeze product routes gate themselves via auth_reject() below.
static bool authorize(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    return auth_ok(req);
}

static bool auth_reject(httpd_req_t *req)
{
    if (auth_ok(req)) return false;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"ok\":false,\"error\":\"missing or invalid " DW_AUTH_HEADER " header\"}");
    return true;
}

// ── JSON send/recv helpers ───────────────────────────────────────────────────
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t api_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "error", "invalid_request");
    cJSON_AddStringToObject(root, "message", message);
    return send_json(req, root);
}

static cJSON *recv_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *text = malloc((size_t)req->content_len + 1);
    if (!text) return NULL;
    int offset = 0;
    while (offset < req->content_len) {
        int got = httpd_req_recv(req, text + offset, req->content_len - offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { free(text); return NULL; }
        offset += got;
    }
    text[offset] = 0;
    cJSON *root = cJSON_Parse(text);
    free(text);
    return root;
}

static void add_device_id(cJSON *root)
{
    uint8_t mac[6] = {0};
    char id[28] = "dragonwheeze";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(id, sizeof(id), "dragonwheeze-%02x%02x%02x", mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device_id", id);
}

// Coarse operating mode for the SPA's status indicator.
static const char *wheeze_mode_str(const dw_device_state_t *st)
{
    if (!st->power_status) return "off";
    return st->active_status ? "drying" : "idle";
}

static const char *wifi_wire(dc_wifi_state_t state)
{
    switch (state) {
    case DC_WIFI_STATE_INIT:           return "starting";
    case DC_WIFI_STATE_STA_CONNECTING: return "connecting";
    case DC_WIFI_STATE_STA_CONNECTED:  return "station";
    case DC_WIFI_STATE_AP_PORTAL:      return "setup_ap";
    }
    return "unknown";
}

// Short, stable preset code for the SPA (dw_preset_to_str is a display string
// like "PLA (45°C/6h)"; the UI needs a bare token to match its preset buttons).
static const char *wheeze_preset_id(dw_preset_profile_t preset)
{
    switch (preset) {
    case DW_PRESET_PLA:  return "PLA";
    case DW_PRESET_PETG: return "PETG";
    case DW_PRESET_TPU:  return "TPU";
    case DW_PRESET_ABS:  return "ABS";
    default:             return "CUSTOM";
    }
}

// Flat status snapshot for the legacy /api/v1/dw/status route (NOT the setup
// descriptor — that's setup_describe below).
static cJSON *dw_status_json(void *ctx)
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

// Legacy action verbs for the /api/v1/dw/control route (NOT the setup values
// route — that's setup_apply below).
static esp_err_t dw_apply_control(const cJSON *values, void *ctx, char *message, size_t message_size)
{
    (void)ctx;
    if (!values) return ESP_ERR_INVALID_ARG;

    cJSON *action = cJSON_GetObjectItem(values, "action");
    if (!cJSON_IsString(action)) {
        snprintf(message, message_size, "Missing 'action' string parameter");
        return ESP_ERR_INVALID_ARG;
    }

    // All actuations go through the single background worker queue so button
    // presses serialize and never contend for the touch mutex (dropped pulses).
    const char *act = action->valuestring;
    esp_err_t qe = ESP_OK;
    if (strcmp(act, "power_toggle") == 0) {
        qe = dw_device_enqueue(DW_ACT_POWER_TOGGLE, 0, 0);
    } else if (strcmp(act, "power_on") == 0) {
        qe = dw_device_enqueue(DW_ACT_POWER_ON, 0, 0);
    } else if (strcmp(act, "power_off") == 0) {
        qe = dw_device_enqueue(DW_ACT_POWER_OFF, 0, 0);
    } else if (strcmp(act, "start") == 0) {
        qe = dw_device_enqueue(DW_ACT_START, 0, 0);
    } else if (strcmp(act, "stop") == 0) {
        qe = dw_device_enqueue(DW_ACT_STOP, 0, 0);
    } else if (strcmp(act, "reset") == 0) {
        qe = dw_device_enqueue(DW_ACT_RESET, 0, 0);
    } else if (strcmp(act, "press_m") == 0) {
        qe = dw_device_enqueue(DW_ACT_PRESS_M, 0, 0);
    } else if (strcmp(act, "press_a") == 0) {
        qe = dw_device_enqueue(DW_ACT_PRESS_A, 0, 0);
    } else if (strcmp(act, "pulse") == 0) {
        // Bench diagnostic: fire ONE raw pulse of a caller-chosen width straight
        // at a button's optocoupler (bypasses the queue/state machine), to sweep
        // pulse duration and find the reliable capacitive-touch threshold.
        cJSON *btn = cJSON_GetObjectItem(values, "button");
        cJSON *ms  = cJSON_GetObjectItem(values, "ms");
        if (!cJSON_IsString(btn)) {
            snprintf(message, message_size, "pulse requires 'button' ('m','a','p') and optional 'ms'");
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t dur = cJSON_IsNumber(ms) ? (uint32_t)ms->valueint : 200;
        if (dur < 10)   dur = 10;
        if (dur > 3000) dur = 3000;
        dw_button_t b;
        char c = btn->valuestring[0];
        if (c == 'm' || c == 'M')      b = DW_BUTTON_MODE;
        else if (c == 'a' || c == 'A') b = DW_BUTTON_ADJUST;
        else if (c == 'p' || c == 'P') b = DW_BUTTON_POWER;
        else { snprintf(message, message_size, "pulse 'button' must be m, a, or p"); return ESP_ERR_INVALID_ARG; }
        esp_err_t pe = dw_touch_pulse(b, dur);
        snprintf(message, message_size, "pulsed %c for %lums: %s", c, (unsigned long)dur, esp_err_to_name(pe));
        return pe;
    } else if (strcmp(act, "set_target") == 0) {
        cJSON *temp = cJSON_GetObjectItem(values, "temperature");
        cJSON *time_h = cJSON_GetObjectItem(values, "time_hours");
        if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(time_h)) {
            snprintf(message, message_size, "set_target requires 'temperature' and 'time_hours' numbers");
            return ESP_ERR_INVALID_ARG;
        }
        qe = dw_device_enqueue(DW_ACT_SET_TARGET, (uint8_t)temp->valueint, (uint8_t)time_h->valueint);
    } else if (strcmp(act, "set_preset") == 0) {
        cJSON *preset = cJSON_GetObjectItem(values, "preset");
        if (!cJSON_IsString(preset)) {
            snprintf(message, message_size, "set_preset requires 'preset' string (PLA, PETG, TPU, ABS)");
            return ESP_ERR_INVALID_ARG;
        }
        dw_preset_profile_t p = dw_preset_from_str(preset->valuestring);
        uint8_t pt = 45, ph = 6;
        if (p == DW_PRESET_PETG) { pt = 50; ph = 8; }
        else if (p == DW_PRESET_TPU) { pt = 50; ph = 10; }
        else if (p == DW_PRESET_ABS) { pt = 50; ph = 12; }
        qe = dw_device_enqueue(DW_ACT_SET_TARGET, pt, ph);
    } else {
        snprintf(message, message_size, "Unknown action: %s", act);
        return ESP_ERR_INVALID_ARG;
    }
    if (qe != ESP_OK) {
        snprintf(message, message_size, "Busy — action queue full, try again");
        return qe;
    }

    snprintf(message, message_size, "Action %s applied successfully", act);
    return ESP_OK;
}

// ── /api/v2 surface (family-consistent envelope, consumed by the shared SPA) ──

// Build the api_version:2 state envelope. Top-level `wheeze` object is what the
// SPA's apply(s) dispatcher keys on to route to the DragonWheeze view.
static cJSON *make_state(void)
{
    dw_device_state_t st = dw_device_get_state();
    dw_aht20_stats_t aht = dw_aht20_get_stats();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "project", "dragonwheeze");
    cJSON_AddNumberToObject(root, "state_revision", s_api_revision);
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    add_device_id(root);
    cJSON_AddStringToObject(root, "mode", wheeze_mode_str(&st));

    cJSON *w = cJSON_AddObjectToObject(root, "wheeze");
    cJSON_AddBoolToObject(w, "power", st.power_status);
    cJSON_AddBoolToObject(w, "active", st.active_status);
    // Best available "heating right now" signal: the dryer is powered and a
    // cycle is running. (No discrete element-on telemetry from the SH01 board.)
    cJSON_AddBoolToObject(w, "heating", st.power_status && st.active_status);
    cJSON_AddStringToObject(w, "screen", dw_screen_state_to_str(st.screen_state));
    // Active profile = the editable profile whose temp/time matches the current
    // setpoint (first match); -1/"Custom" when the setpoint matches none.
    dw_profile_t profiles[DW_PROFILE_COUNT];
    size_t np = dw_device_get_profiles(profiles, DW_PROFILE_COUNT);
    int active = -1;
    for (size_t i = 0; i < np; ++i) {
        if (profiles[i].temp_c == st.set_temperature &&
            profiles[i].time_hours == st.set_time_hours) { active = (int)i; break; }
    }
    cJSON_AddNumberToObject(w, "profile_index", active);
    cJSON_AddStringToObject(w, "preset", active >= 0 ? profiles[active].name : "Custom");
    cJSON_AddStringToObject(w, "preset_id", active >= 0 ? profiles[active].name : "Custom");
    cJSON_AddNumberToObject(w, "set_temperature_c", st.set_temperature);
    cJSON_AddNumberToObject(w, "set_time_hours", st.set_time_hours);
    cJSON_AddNumberToObject(w, "elapsed_sec", st.elapsed_sec);
    cJSON_AddNumberToObject(w, "remaining_sec", st.remaining_sec);
    cJSON_AddNumberToObject(w, "ambient_temp_c", st.ambient_temp_c);
    cJSON_AddNumberToObject(w, "ambient_humidity_rh", st.ambient_humidity_rh);
    cJSON_AddBoolToObject(w, "physical_power_pressed", st.physical_power_pressed);

    cJSON *diag = cJSON_AddObjectToObject(w, "sensor");
    cJSON_AddNumberToObject(diag, "aht20_total_reads", aht.total_reads);
    cJSON_AddNumberToObject(diag, "aht20_success_reads", aht.successful_reads);
    cJSON_AddNumberToObject(diag, "aht20_bus_errors", aht.bus_errors);
    cJSON_AddNumberToObject(diag, "aht20_crc_errors", aht.crc_errors);
    cJSON_AddNumberToObject(diag, "aht20_polling_interval_sec", dw_aht20_get_polling_interval());

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "state", wifi_wire(dc_wifi_state()));
    if (dc_wifi_state() == DC_WIFI_STATE_STA_CONNECTED) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {0};
        if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
            char ip[20];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
            cJSON_AddStringToObject(wifi, "ip", ip);
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    }

    return root;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "project", "dragonwheeze");
    add_device_id(root);
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    const char *values[] = { "dryer", "presets", "polling", "provisioning" };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        cJSON_AddItemToArray(caps, cJSON_CreateString(values[i]));
    cJSON *ui = cJSON_AddObjectToObject(root, "ui");
    cJSON_AddNumberToObject(ui, "schema", 1);
    cJSON_AddStringToObject(ui, "product", "dragonwheeze");
    cJSON_AddStringToObject(ui, "display_name", "DragonWheeze");
    cJSON *update = cJSON_AddObjectToObject(root, "update");
    cJSON_AddStringToObject(update, "repo", "justinh-rahb/DragonWheeze");
    cJSON_AddStringToObject(update, "asset_prefix", "dragonwheeze-");
    return send_json(req, root);
}

static esp_err_t state_get(httpd_req_t *req) { return send_json(req, make_state()); }

static esp_err_t command_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *body = recv_json(req);
    cJSON *command = body ? cJSON_GetObjectItemCaseSensitive(body, "command") : NULL;
    cJSON *name = command ? cJSON_GetObjectItemCaseSensitive(command, "name") : NULL;
    if (!cJSON_IsString(name)) {
        cJSON_Delete(body);
        return api_error(req, "400 Bad Request", "missing command name");
    }

    const char *cmd = name->valuestring;
    char cmd_name[24];
    strncpy(cmd_name, cmd, sizeof(cmd_name) - 1);
    cmd_name[sizeof(cmd_name) - 1] = '\0';
    esp_err_t err = ESP_OK;
    if (!strcmp(cmd, "set_preset")) {
        cJSON *preset = cJSON_GetObjectItemCaseSensitive(command, "preset");
        if (!cJSON_IsString(preset)) {
            cJSON_Delete(body);
            return api_error(req, "400 Bad Request", "set_preset requires a preset string");
        }
        dw_preset_profile_t p = dw_preset_from_str(preset->valuestring);
        uint8_t pt = 45, ph = 6;
        if (p == DW_PRESET_PETG) { pt = 50; ph = 8; }
        else if (p == DW_PRESET_TPU) { pt = 50; ph = 10; }
        else if (p == DW_PRESET_ABS) { pt = 50; ph = 12; }
        err = dw_device_enqueue(DW_ACT_SET_TARGET, pt, ph);
    } else if (!strcmp(cmd, "apply_profile")) {
        cJSON *idx = cJSON_GetObjectItemCaseSensitive(command, "index");
        if (!cJSON_IsNumber(idx)) {
            cJSON_Delete(body);
            return api_error(req, "400 Bad Request", "apply_profile requires an index");
        }
        err = dw_device_enqueue(DW_ACT_APPLY_PROFILE, (uint8_t)idx->valueint, 0);
    } else if (!strcmp(cmd, "set_target")) {
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(command, "temperature");
        cJSON *time_h = cJSON_GetObjectItemCaseSensitive(command, "time_hours");
        if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(time_h)) {
            cJSON_Delete(body);
            return api_error(req, "400 Bad Request", "set_target requires temperature and time_hours");
        }
        err = dw_device_enqueue(DW_ACT_SET_TARGET, (uint8_t)temp->valueint, (uint8_t)time_h->valueint);
    } else if (!strcmp(cmd, "power_toggle")) {
        err = dw_device_enqueue(DW_ACT_POWER_TOGGLE, 0, 0);
    } else if (!strcmp(cmd, "power_on")) {
        err = dw_device_enqueue(DW_ACT_POWER_ON, 0, 0);
    } else if (!strcmp(cmd, "power_off")) {
        err = dw_device_enqueue(DW_ACT_POWER_OFF, 0, 0);
    } else if (!strcmp(cmd, "start")) {
        err = dw_device_enqueue(DW_ACT_START, 0, 0);
    } else if (!strcmp(cmd, "stop")) {
        err = dw_device_enqueue(DW_ACT_STOP, 0, 0);
    } else if (!strcmp(cmd, "reset")) {
        err = dw_device_enqueue(DW_ACT_RESET, 0, 0);
    } else {
        cJSON_Delete(body);
        return api_error(req, "400 Bad Request", "unknown command");
    }
    cJSON_Delete(body);
    if (err != ESP_OK) return api_error(req, "409 Conflict", esp_err_to_name(err));

    ++s_api_revision;
    dc_evlog_add("api: cmd=%s", cmd_name);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "state", make_state());
    return send_json(req, reply);
}

static esp_err_t profiles_get(httpd_req_t *req)
{
    dw_profile_t p[DW_PROFILE_COUNT];
    size_t n = dw_device_get_profiles(p, DW_PROFILE_COUNT);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    for (size_t i = 0; i < n; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "index", (double)i);
        cJSON_AddStringToObject(e, "name", p[i].name);
        cJSON_AddNumberToObject(e, "temp_c", p[i].temp_c);
        cJSON_AddNumberToObject(e, "time_hours", p[i].time_hours);
        cJSON_AddItemToArray(arr, e);
    }
    // Discrete values the SH01 accepts, so the UI can constrain its inputs.
    cJSON *temps = cJSON_AddArrayToObject(root, "allowed_temps");
    cJSON_AddItemToArray(temps, cJSON_CreateNumber(40));
    cJSON_AddItemToArray(temps, cJSON_CreateNumber(45));
    cJSON_AddItemToArray(temps, cJSON_CreateNumber(50));
    cJSON_AddNumberToObject(root, "time_min", 6);
    cJSON_AddNumberToObject(root, "time_max", 48);
    cJSON_AddNumberToObject(root, "time_step", 2);
    return send_json(req, root);
}

static esp_err_t profiles_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *body = recv_json(req);
    cJSON *arr = body ? cJSON_GetObjectItemCaseSensitive(body, "profiles") : NULL;
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != DW_PROFILE_COUNT) {
        cJSON_Delete(body);
        return api_error(req, "400 Bad Request", "profiles must be an array of 4");
    }
    dw_profile_t p[DW_PROFILE_COUNT];
    memset(p, 0, sizeof(p));
    for (int i = 0; i < DW_PROFILE_COUNT; ++i) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(e, "name");
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(e, "temp_c");
        cJSON *time_h = cJSON_GetObjectItemCaseSensitive(e, "time_hours");
        if (!cJSON_IsString(name) || !name->valuestring[0] ||
            !cJSON_IsNumber(temp) || !cJSON_IsNumber(time_h)) {
            cJSON_Delete(body);
            return api_error(req, "400 Bad Request", "each profile needs name, temp_c, time_hours");
        }
        strncpy(p[i].name, name->valuestring, DW_PROFILE_NAME_MAX - 1);
        p[i].temp_c = (uint8_t)temp->valueint;
        p[i].time_hours = (uint8_t)time_h->valueint;
    }
    esp_err_t err = dw_device_set_profiles(p, DW_PROFILE_COUNT);
    cJSON_Delete(body);
    if (err != ESP_OK)
        return api_error(req, "400 Bad Request",
                         "invalid values (temp 40/45/50, time 6-12, name required)");
    ++s_api_revision;
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "state", make_state());
    return send_json(req, reply);
}

static esp_err_t get_status_handler(httpd_req_t *req)
{
    cJSON *json = dw_status_json(NULL);
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
    esp_err_t err = dw_apply_control(json, NULL, msg, sizeof(msg));
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

// Product routes handed to dc_portal via the product_routes[] array (NOT the
// register_product_routes callback): dc_portal sizes the httpd URI-handler
// budget as 16 + product_route_count, so declaring them here keeps the table
// from overflowing. (The callback path leaves product_route_count at 0, which
// caps the budget at 16 and silently drops routes past the 13 built-ins + 2.)
static const httpd_uri_t s_product_routes[] = {
    // v2 surface consumed by the shared SPA (matches dv_portal / pb_httpd).
    { .uri = "/api/v2/info",       .method = HTTP_GET,  .handler = info_get,            .user_ctx = NULL },
    { .uri = "/api/v2/state",      .method = HTTP_GET,  .handler = state_get,           .user_ctx = NULL },
    { .uri = "/api/v2/command",    .method = HTTP_POST, .handler = command_post,        .user_ctx = NULL },
    { .uri = "/api/v2/profiles",   .method = HTTP_GET,  .handler = profiles_get,        .user_ctx = NULL },
    { .uri = "/api/v2/profiles",   .method = HTTP_POST, .handler = profiles_post,       .user_ctx = NULL },
    // Legacy flat v1 routes, retained for scripts / backward compatibility.
    { .uri = "/api/v1/dw/status",  .method = HTTP_GET,  .handler = get_status_handler,  .user_ctx = NULL },
    { .uri = "/api/v1/dw/control", .method = HTTP_POST, .handler = post_control_handler, .user_ctx = NULL },
};

// ── Web setup schema (dc_portal describe/apply hooks) ────────────────────────
// The shared SPA renders whatever {sections:[{title,fields:[...]}]} we return
// here and POSTs the field values back to setup_apply.
static cJSON *add_setup_section(cJSON *sections, const char *title, const char *desc)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "title", title);
    if (desc) cJSON_AddStringToObject(s, "description", desc);
    cJSON_AddItemToObject(s, "fields", cJSON_CreateArray());
    cJSON_AddItemToArray(sections, s);
    return s;
}
static cJSON *add_field(cJSON *section, const char *key, const char *label, const char *type)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "key", key);
    cJSON_AddStringToObject(f, "label", label);
    cJSON_AddStringToObject(f, "type", type);
    cJSON_AddItemToArray(cJSON_GetObjectItem(section, "fields"), f);
    return f;
}

static cJSON *setup_describe(void *ctx)
{
    (void)ctx;
    dw_mqtt_settings_t m;
    dw_mqtt_get_settings(&m);

    cJSON *root = cJSON_CreateObject();
    cJSON *sections = cJSON_AddArrayToObject(root, "sections");
    cJSON *s = add_setup_section(sections, "Home Assistant (MQTT)",
        "Connect the dryer to your MQTT broker so Home Assistant auto-discovers it "
        "as a device you can watch and control. Leave disabled to keep it local-only.");
    cJSON_AddBoolToObject(add_field(s, "mqtt_enable", "Enable Home Assistant", "boolean"), "value", m.enabled);
    cJSON_AddStringToObject(add_field(s, "mqtt_host", "Broker host", "text"), "value", m.host);
    cJSON_AddNumberToObject(add_field(s, "mqtt_port", "Port", "number"), "value", m.port ? m.port : 1883);
    cJSON_AddStringToObject(add_field(s, "mqtt_user", "Username", "text"), "value", m.user);
    cJSON *pf = add_field(s, "mqtt_pass", "Password (blank keeps current)", "password");
    cJSON_AddBoolToObject(pf, "secret", true);
    cJSON_AddStringToObject(pf, "value", "");
    cJSON_AddStringToObject(add_field(s, "mqtt_topic", "Base topic", "text"), "value", m.topic);

    cJSON *s2 = add_setup_section(sections, "Ambient sensor",
        "How often the ESP reads the AHT20 temperature/humidity sensor. It shares "
        "the I2C bus with the SH01 mainboard, so reading too often can trigger the "
        "dryer's E0 error — 120 seconds or more is recommended.");
    cJSON *pi = add_field(s2, "aht_poll", "Poll interval (seconds, 0 = off)", "number");
    cJSON_AddNumberToObject(pi, "value", dw_aht20_get_polling_interval());
    cJSON_AddNumberToObject(pi, "min", 0);
    cJSON_AddNumberToObject(pi, "max", 600);
    cJSON_AddStringToObject(pi, "hint", "Set to 0 to stop all I2C access if the dryer shows E0 (you lose ambient readings until a dedicated sensor is wired).");
    return root;
}

static esp_err_t setup_apply(const cJSON *values, void *ctx, char *message, size_t message_size)
{
    (void)ctx;
    if (!values) return ESP_ERR_INVALID_ARG;

    // Ambient sensor poll interval — the E0 mitigation knob. Its Save button
    // posts only this field, so apply it and return unless MQTT fields came too.
    cJSON *poll = cJSON_GetObjectItem(values, "aht_poll");
    if (cJSON_IsNumber(poll)) {
        int32_t iv = poll->valueint;
        if (iv < 0) iv = 0;
        if (iv > 600) iv = 600;
        if (iv > 0 && iv < 15) iv = 15;   // 0 = off; otherwise floor at 15s
        uint32_t v = (uint32_t)iv;
        dw_aht20_set_polling_interval(v);
        if (!cJSON_GetObjectItem(values, "mqtt_enable") && !cJSON_GetObjectItem(values, "mqtt_host")) {
            if (v == 0) snprintf(message, message_size, "Ambient sensor disabled (I2C left untouched).");
            else snprintf(message, message_size, "Sensor poll interval set to %us.", (unsigned)v);
            return ESP_OK;
        }
    }

    dw_mqtt_settings_t m;
    dw_mqtt_get_settings(&m);   // start from current so a blank password is kept

    cJSON *en = cJSON_GetObjectItem(values, "mqtt_enable");
    if (en) {
        if (cJSON_IsString(en)) m.enabled = strcmp(en->valuestring, "0") != 0 && en->valuestring[0];
        else if (cJSON_IsBool(en)) m.enabled = cJSON_IsTrue(en);
        else if (cJSON_IsNumber(en)) m.enabled = en->valueint != 0;
    }
    cJSON *host = cJSON_GetObjectItem(values, "mqtt_host");
    if (cJSON_IsString(host)) { strncpy(m.host, host->valuestring, sizeof(m.host) - 1); m.host[sizeof(m.host) - 1] = '\0'; }
    cJSON *port = cJSON_GetObjectItem(values, "mqtt_port");
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535) m.port = (uint16_t)port->valueint;
    cJSON *user = cJSON_GetObjectItem(values, "mqtt_user");
    if (cJSON_IsString(user)) { strncpy(m.user, user->valuestring, sizeof(m.user) - 1); m.user[sizeof(m.user) - 1] = '\0'; }
    cJSON *pass = cJSON_GetObjectItem(values, "mqtt_pass");
    if (cJSON_IsString(pass) && pass->valuestring[0]) { strncpy(m.pass, pass->valuestring, sizeof(m.pass) - 1); m.pass[sizeof(m.pass) - 1] = '\0'; }
    cJSON *topic = cJSON_GetObjectItem(values, "mqtt_topic");
    if (cJSON_IsString(topic) && topic->valuestring[0]) { strncpy(m.topic, topic->valuestring, sizeof(m.topic) - 1); m.topic[sizeof(m.topic) - 1] = '\0'; }

    if (m.enabled && !m.host[0]) {
        snprintf(message, message_size, "Enter a broker host to enable Home Assistant.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = dw_mqtt_set_settings(&m);
    if (err != ESP_OK) { snprintf(message, message_size, "Could not save MQTT settings."); return err; }
    dw_mqtt_start("DragonWheeze SH01", "dragonwheeze_sh01");   // apply live
    snprintf(message, message_size, m.enabled ? "Home Assistant enabled." : "Home Assistant disabled.");
    return ESP_OK;
}

esp_err_t dw_portal_start(void)
{
    ESP_LOGI(TAG, "Starting dw_portal Web Service...");

    dc_portal_config_t cfg = {
        .product = "dragonwheeze",
        .display_name = "DragonWheeze (Sovol SH01)",
        .product_routes = s_product_routes,
        .product_route_count = sizeof(s_product_routes) / sizeof(s_product_routes[0]),
        .describe_product = setup_describe,
        .apply_product = setup_apply,
        .authorize = authorize,
        .ctx = NULL,
    };

    return dc_portal_start(&cfg);
}

esp_err_t dw_portal_stop(void)
{
    return dc_portal_stop();
}
