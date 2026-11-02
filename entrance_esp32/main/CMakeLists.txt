idf_component_register(
    SRCS "../main.c"
    INCLUDE_DIRS ".."
    REQUIRES driver nvs_flash esp_event esp_netif esp_wifi mqtt json fatfs sdmmc
)
