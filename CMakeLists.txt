cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig" CACHE FILEPATH "ESP-IDF project configuration" FORCE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_air_quality_monitor)

