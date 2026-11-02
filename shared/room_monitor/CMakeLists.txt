idf_component_register(
    SRCS "room_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_event esp_netif esp_wifi mqtt
)
