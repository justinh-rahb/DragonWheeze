idf_component_register(
    SRCS "dw_peer.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_peer dw_device esp_app_format esp_netif esp_hw_support
)
