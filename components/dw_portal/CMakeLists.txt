idf_component_register(
    SRCS "dw_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES dw_device dw_aht20 dw_lcd dw_touch dw_mqtt dc_portal dc_evlog cjson esp_http_server
             esp_app_format esp_hw_support nvs_flash dc_wifi esp_wifi esp_netif
)
