idf_component_register(
    SRCS "dw_aht20.c"
    INCLUDE_DIRS "include"
    REQUIRES dw_board driver dc_evlog freertos nvs_flash
)
