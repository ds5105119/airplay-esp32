# AirPlay 2 Receiver for ESP32-S3
cmake_minimum_required(VERSION 3.16)

if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airplay2-receiver)
