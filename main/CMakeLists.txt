idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES dw_board dw_touch dw_aht20 dw_device dw_mqtt dw_portal dc_wifi dc_portal dc_evlog nvs_flash esp_wifi
)
