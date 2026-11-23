#pragma once

#include <stdint.h>

// XIAO ESP32-C6 header labels and SoC GPIOs.
#define PIN_I2C_SDA 22
#define PIN_I2C_SCL 23
#define PIN_PM_RX 0
#define PIN_PM_TX 1
#define PIN_BUTTON 16

// Hosyond 14-pin SPI module. MISO is shared by LCD, touch, and SD if used.
#define PIN_TFT_SCK 19
#define PIN_TFT_MISO 20
#define PIN_TFT_MOSI 18
#define PIN_TFT_CS 21
#define PIN_TFT_DC 2
#define PIN_TFT_RESET 17
#define PIN_TOUCH_CS 4

#define SPI_CLOCK PIN_TFT_SCK
#define SPI_MOSI  PIN_TFT_MOSI
#define SPI_MISO  PIN_TFT_MISO
#define TFT_CS    PIN_TFT_CS
#define TFT_DC    PIN_TFT_DC
#define TFT_RESET PIN_TFT_RESET

#define DISPLAY_HORIZONTAL_PIXELS 320
#define DISPLAY_VERTICAL_PIXELS   480

// These are presentation defaults. Hubitat preferences can change display units
// and the Zigbee reporting interval; see hubitat/AirQualityMonitor.groovy.
#define DEFAULT_USE_FAHRENHEIT false
#define SENSOR_SAMPLE_MS 2000
#define DISPLAY_REFRESH_MS 1000
#define BUTTON_HOLD_RESET_MS 3000

#define EP_ENVIRONMENT 1
#define EP_PM25 2
#define EP_ECO2 3
#define EP_TVOC 4
#define EP_AQI 5

