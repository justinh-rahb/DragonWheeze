idf_component_register(
    SRCS "dw_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES dw_device dw_aht20 dw_touch dc_portal dc_evlog cjson esp_http_server
)
