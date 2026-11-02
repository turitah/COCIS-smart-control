cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared/room_monitor")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(room2_esp32)
