idf_component_register(
    SRCS
        "native_sensors.c"
        "native_display.c"

    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/include"

    REQUIRES
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_lcd
)
