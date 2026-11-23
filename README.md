# ESP32-C6 Air Quality Monitor

Firmware for Seeed Studio XIAO ESP32-C6 with a Plantower PMS5003, ENS160+AHT2X, and Hosyond 3.5-inch ILI9488 SPI display. It exposes sensor readings to Hubitat over Zigbee and provides a local LCD dashboard.

## Current wiring assumed by the firmware

| Signal | XIAO connection |
| --- | --- |
| ENS160+AHT2X SDA/SCL | D4/GPIO22, D5/GPIO23 |
| PMS5003 TX/RX | D0/GPIO0 (ESP RX), D1/GPIO1 (ESP TX) |
| Display SCK/MISO/MOSI | D8/GPIO19, D9/GPIO20, D10/GPIO18 |
| Display CS/DC/RESET | D3/GPIO21, D2/GPIO2, D7/GPIO17 |
| Touch CS (if fitted/used) | GPIO4/MTMS pad; share SPI lines. If it was wired to D6, move it or leave it disconnected; D6 is the reset button. |
| Re-pair button | D6/GPIO16 to GND; internal pull-up enabled |

Power XIAO by USB. Join all grounds. Feed PMS5003 VCC and display VCC from USB 5 V/VBUS; feed the sensor breakout from 3V3. Display signal pins must remain 3.3 V logic. Check that your USB supply has enough current for the XIAO, display backlight, and PMS fan. The PCA9548A is not required for the single I2C sensor board.

## Build and upload from VS Code

This repository is now an ESP-IDF CMake project. The firmware still uses the
Arduino-ESP32 APIs as an ESP-IDF component, which preserves the Zigbee and
Arduino sensor/display code while using the ESP-IDF toolchain and project
configuration.

1. Install ESP-IDF and the ESP-IDF VS Code extension.
2. Open this project folder and select the `esp32c6` target.
3. Run **ESP-IDF: Build your project**.
4. Connect the XIAO by USB and run **ESP-IDF: Flash your project** or
   **ESP-IDF: Build, Flash and Monitor**. The monitor runs at 115200 baud.

The first build downloads the Arduino-ESP32 component and the local Arduino
library sources under `components/air-quality-libs`. `sdkconfig.defaults`
sets the ESP32-C6 target and the 1000 Hz FreeRTOS tick rate required by the
Arduino component.

## Pairing and operation

On first power-up, the device starts Zigbee network steering and should be paired from Hubitat's device discovery flow. It is a mains-powered end device with its receiver left on so it can answer Hubitat's discovery and interview requests. Hold the button on D6/GPIO16 for at least three seconds, then release it, to erase stored Zigbee network settings and restart pairing. The console logs stack startup, pairing attempts, join/rejoin results, and the BDB failure reason. The screen's signal badge shows `PAIR` while searching, `LINK` when joined but sensor values are not all current, `LIVE` when joined and all sensor attributes update, and `OFF` when not joined or searching. Sensor values are sampled about every five seconds. If you are upgrading from firmware that exposed only temperature, remove the old Hubitat device and pair again so Hubitat discovers the added endpoints.

The dashboard currently shows temperature, relative humidity, PM2.5, ENS160 equivalent CO2 (eCO2), TVOC, AQI-UBA (1–5), Zigbee connection state, and the report interval. The ENS160's eCO2 is an estimate from VOC response, not a direct CO2 measurement. The AHT2X on the combo board is the available ambient temperature/humidity sensor; there is no separate probe in this parts list.

Hubitat driver source is in `hubitat/AirQualityMonitor.groovy`. Install it as a Hubitat user driver, pair the device, and choose the reporting interval and temperature scale in the driver's preferences. The interval configures Zigbee reporting for temperature, humidity, PM2.5, eCO2, TVOC, and AQI; the sensor continues sampling locally every five seconds. The temperature scale preference controls the Hubitat temperature event. Zigbee temperature stays in Celsius, and the LCD currently displays Celsius. Temperature, humidity, PM2.5, and eCO2 use standard measurement clusters; TVOC and AQI use analog-input endpoints.

## Tuning

Pin assignments and initial display/report defaults are in `include/project_config.h`. Sensor sampling and screen placement are in `src/main.cpp`. The screen code is deliberately simple to make moving/resizing the rows easy. ILI9488 is driven in 18-bit SPI mode.

## First power-up checks

- Confirm no module's 5 V rail is connected to an XIAO GPIO.
- Confirm the button shorts GPIO16 to GND only when pressed.
- Confirm the display silk labels match the SPI header mapping above. Touch and SD are not yet read by the firmware; the touch CS wire is reserved for later UI work.
- The firmware logs whether it finds the AHT2X and ENS160 and shows missing values as `--`.

