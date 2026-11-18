idf_component_register(
    SRCS "../src/main.cpp"
    INCLUDE_DIRS "../include"
    REQUIRES air-quality-libs espressif__esp-zigbee-lib espressif__esp-zboss-lib esp_driver_i2c esp_driver_spi esp_driver_uart esp_driver_gpio nvs_flash ieee802154
)
