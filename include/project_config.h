#pragma once

#include <Arduino.h>

// XIAO ESP32-C6 header labels and SoC GPIOs.
constexpr int PIN_I2C_SDA = 22;   // D4
constexpr int PIN_I2C_SCL = 23;   // D5
constexpr int PIN_PM_RX = 0;      // D0: PMS5003 TX -> ESP32 RX
constexpr int PIN_PM_TX = 1;      // D1: PMS5003 RX <- ESP32 TX (optional)
constexpr int PIN_BUTTON = 16;    // D6, active-low button to GND

// Hosyond 14-pin SPI module. MISO is shared by LCD, touch, and SD if used.
constexpr int PIN_TFT_SCK = 19;   // D8
constexpr int PIN_TFT_MISO = 20;  // D9
constexpr int PIN_TFT_MOSI = 18;  // D10
constexpr int PIN_TFT_CS = 21;    // D3
constexpr int PIN_TFT_DC = 2;     // D2
constexpr int PIN_TFT_RESET = 17; // D7
constexpr int PIN_TOUCH_CS = 4;   // GPIO4/MTMS pad; button owns D6/GPIO16

// These are presentation defaults. Hubitat preferences can change display units
// and the Zigbee reporting interval; see hubitat/AirQualityMonitor.groovy.
constexpr bool DEFAULT_USE_FAHRENHEIT = false;
constexpr uint32_t SENSOR_SAMPLE_MS = 2000;
constexpr uint32_t DISPLAY_REFRESH_MS = 1000;
constexpr uint32_t BUTTON_HOLD_RESET_MS = 3000;

constexpr uint8_t EP_ENVIRONMENT = 1;
constexpr uint8_t EP_PM25 = 2;
constexpr uint8_t EP_ECO2 = 3;
constexpr uint8_t EP_TVOC = 4;
constexpr uint8_t EP_AQI = 5;

