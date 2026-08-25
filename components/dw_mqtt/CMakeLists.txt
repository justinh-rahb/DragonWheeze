idf_component_register(
    SRCS "dw_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES dw_device dc_mqtt dc_evlog cjson freertos nvs_flash
)
