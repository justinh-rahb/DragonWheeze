idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES dw_board dw_touch dw_aht20 dw_lcd dw_device dw_mqtt dw_portal dw_peer dc_wifi dc_portal dc_peer dc_evlog nvs_flash esp_wifi
)
