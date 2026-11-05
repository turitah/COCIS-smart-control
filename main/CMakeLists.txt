idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver fatfs sdmmc esp_driver_sdspi esp_driver_spi
                                esp_driver_uart esp_driver_ledc esp_driver_gpio
                                esp_timer nvs_flash espressif__cjson)
