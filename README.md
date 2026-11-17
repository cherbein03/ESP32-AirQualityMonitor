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

1. Install the PlatformIO IDE extension.
2. Open this project folder in VS Code and allow PlatformIO to install the toolchain and libraries.
3. Connect the XIAO by USB. Select its serial port in PlatformIO if it is not detected automatically.
4. Use **PlatformIO: Build**, then **PlatformIO: Upload**. The monitor runs at 115200 baud.

The project uses the pioarduino platform because the maintained upstream PlatformIO Espressif platform does not carry the Arduino Zigbee APIs needed here. Build profile is Zigbee End Device with the `zigbee.csv` partition table.

## Pairing and operation

On first power-up, the device starts Zigbee network steering and should be paired from Hubitat's device discovery flow. Hold the button on D6/GPIO16 for at least three seconds to erase stored Zigbee network settings and reboot into pairing again. Sensor samples refresh every two seconds; the display refreshes each second.

The dashboard currently shows temperature, relative humidity, PM2.5, ENS160 equivalent CO2 (eCO2), TVOC, AQI-UBA (1–5), Zigbee connection state, and the report interval. The ENS160's eCO2 is an estimate from VOC response, not a direct CO2 measurement. The AHT2X on the combo board is the available ambient temperature/humidity sensor; there is no separate probe in this parts list.

Hubitat driver source is in `hubitat/AirQualityMonitor.groovy`. Install it as a Hubitat user driver, pair the device, and choose the reporting interval and displayed temperature scale in the driver's preferences. The temperature scale preference changes the Hubitat temperature event; the LCD currently displays Celsius. Standard measurements use standard Zigbee clusters; TVOC and AQI use analog-input endpoints with descriptions. Reporting interval is configured from Hubitat via Configure Reporting and refreshed after the driver preference is saved.

## Tuning

Pin assignments and initial display/report defaults are in `include/project_config.h`. Sensor sampling and screen placement are in `src/main.cpp`. The screen code is deliberately simple to make moving/resizing the rows easy. ILI9488 is driven in 18-bit SPI mode.

## First power-up checks

- Confirm no module's 5 V rail is connected to an XIAO GPIO.
- Confirm the button shorts GPIO16 to GND only when pressed.
- Confirm the display silk labels match the SPI header mapping above. Touch and SD are not yet read by the firmware; the touch CS wire is reserved for later UI work.
- The firmware logs whether it finds the AHT2X and ENS160 and shows missing values as `--`.

