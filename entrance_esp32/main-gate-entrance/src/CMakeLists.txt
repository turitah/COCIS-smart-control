# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources "${CMAKE_CURRENT_LIST_DIR}/*.c")

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "../include"
    REQUIRES fatfs sdmmc esp_driver_gpio esp_driver_ledc esp_driver_spi
             esp_driver_uart
             esp_driver_sdspi esp_timer
)