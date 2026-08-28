idf_component_register(
    SRCS "dw_device.c"
    INCLUDE_DIRS "include"
    REQUIRES dw_board dw_touch dw_aht20 dw_lcd dc_evlog dc_source freertos nvs_flash
)
