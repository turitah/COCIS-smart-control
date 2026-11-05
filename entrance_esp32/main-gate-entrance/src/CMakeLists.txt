# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(app_sources
    "${CMAKE_CURRENT_LIST_DIR}/main.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/access_logic/access_logic.c"
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "../include" "../../../shared/access_logic"
    REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_sdmmc
        esp_driver_sdspi
        esp_driver_spi
        esp_driver_uart
        esp_event
        esp_timer
        fatfs
        nvs_flash
        sdmmc
        rc522
)