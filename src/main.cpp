#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_AHTX0.h>
#include <SparkFun_ENS160.h>
#include <Arduino_GFX_Library.h>
#include <Zigbee.h>

#include "project_config.h"

#ifndef ZIGBEE_MODE_ED
#error "Build this device as a Zigbee End Device (ZIGBEE_MODE_ED)."
#endif

// The ILI9488 is an 18-bit SPI display. This module has 3.3 V logic even when
// its VCC pin is powered from USB VBUS/5 V.
Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
    PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI, PIN_TFT_MISO);
Arduino_GFX *display = new Arduino_ILI9488_18bit(
    displayBus, PIN_TFT_RESET, 1 /* landscape */, false /* IPS */);

Adafruit_AHTX0 aht;
SparkFun_ENS160 ens160;
HardwareSerial pmSerial(1);

ZigbeeTempSensor envEndpoint(EP_ENVIRONMENT);
ZigbeePM25Sensor pmEndpoint(EP_PM25);
ZigbeeCarbonDioxideSensor co2Endpoint(EP_ECO2);
ZigbeeAnalog tvocEndpoint(EP_TVOC);
ZigbeeAnalog aqiEndpoint(EP_AQI);

struct Measurements {
  float temperatureC = NAN;
  float humidity = NAN;
  float pm25 = NAN;
  float eco2 = NAN;
  float tvoc = NAN;
  float aqi = NAN;
  bool ahtOk = false;
  bool ensOk = false;
  bool pmOk = false;
};

Measurements values;
bool useFahrenheit = DEFAULT_USE_FAHRENHEIT;
uint32_t lastSampleMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t buttonPressedAt = 0;
bool buttonHandled = false;

static void printValue(int16_t x, int16_t y, const char *label, float value,
                       const char *unit, uint8_t decimals = 1) {
  display->setCursor(x, y);
  display->setTextColor(WHITE, BLACK);
  display->setTextSize(2);
  display->print(label);
  if (isnan(value)) {
    display->print("--");
  } else {
    display->print(value, decimals);
  }
  display->print(unit);
}

static void drawScreen() {
  display->fillScreen(BLACK);
  display->setTextColor(CYAN, BLACK);
  display->setTextSize(3);
  display->setCursor(12, 10);
  display->print("AIR QUALITY");

  char status[28];
  snprintf(status, sizeof(status), "Zigbee: %s", Zigbee.connected() ? "connected" : "pairing...");
  display->setTextColor(Zigbee.connected() ? GREEN : YELLOW, BLACK);
  display->setTextSize(1);
  display->setCursor(350, 21);
  display->print(status);

  const float temp = values.temperatureC;
  printValue(12, 65, "Temp: ", useFahrenheit && !isnan(temp) ? temp * 9.0f / 5.0f + 32.0f : temp,
             useFahrenheit ? " F" : " C", 1);
  printValue(12, 100, "Humidity: ", values.humidity, " %", 1);
  printValue(12, 145, "PM2.5: ", values.pm25, " ug/m3", 1);
  printValue(12, 190, "eCO2: ", values.eco2, " ppm", 0);
  printValue(12, 235, "TVOC: ", values.tvoc, " ppb", 0);
  printValue(12, 280, "AQI (UBA 1-5): ", values.aqi, "", 0);

  display->setTextSize(1);
  display->setTextColor(0xC618, BLACK);
  display->setCursor(350, 300);
  display->print("Report interval: set in Hubitat | Hold D6 3s to re-pair");
}

// PMS5003 active-mode frames are 32 bytes, start 0x42 0x4D, and carry a
// 16-bit additive checksum. Concentration bytes 12..13 are PM2.5 atmospheric.
static bool readPms5003(float &pm25) {
  static uint8_t frame[32];
  static size_t used = 0;
  while (pmSerial.available()) {
    const uint8_t byteIn = static_cast<uint8_t>(pmSerial.read());
    if (used == 0 && byteIn != 0x42) continue;
    if (used == 1 && byteIn != 0x4D) {
      used = byteIn == 0x42 ? 1 : 0;
      frame[0] = 0x42;
      continue;
    }
    frame[used++] = byteIn;
    if (used == sizeof(frame)) {
      used = 0;
      const uint16_t length = (uint16_t(frame[2]) << 8) | frame[3];
      uint16_t sum = 0;
      for (size_t i = 0; i < 30; ++i) sum += frame[i];
      const uint16_t supplied = (uint16_t(frame[30]) << 8) | frame[31];
      if (length != 28 || sum != supplied) return false;
      pm25 = (uint16_t(frame[12]) << 8) | frame[13];
      return true;
    }
  }
  return false;
}

static void sampleSensors() {
  sensors_event_t humidityEvent, temperatureEvent;
  values.ahtOk = aht.getEvent(&humidityEvent, &temperatureEvent);
  if (values.ahtOk) {
    values.temperatureC = temperatureEvent.temperature;
    values.humidity = humidityEvent.relative_humidity;
    // Feed actual ambient compensation back to the ENS160.
    ens160.setTempCompensationCelsius(values.temperatureC);
    ens160.setRHCompensationFloat(values.humidity);
  }

  values.ensOk = ens160.checkDataStatus();
  if (values.ensOk) {
    values.aqi = ens160.getAQI();
    values.tvoc = ens160.getTVOC();
    values.eco2 = ens160.getECO2();
  }

  float pm = NAN;
  if (readPms5003(pm)) {
    values.pm25 = pm;
    values.pmOk = true;
  }

  if (values.ahtOk) {
    envEndpoint.setTemperature(values.temperatureC);
    envEndpoint.setHumidity(values.humidity);
  }
  if (values.pmOk) pmEndpoint.setPM25(values.pm25);
  if (values.ensOk) {
    co2Endpoint.setCarbonDioxide(values.eco2);
    tvocEndpoint.setAnalogInput(values.tvoc);
    aqiEndpoint.setAnalogInput(values.aqi);
  }
}

static void initZigbee() {
  envEndpoint.setManufacturerAndModel("DIY", "ESP32C6-AirQualityMonitor");
  envEndpoint.setMinMaxValue(-20, 80);
  envEndpoint.addHumiditySensor(0, 100, 1);

  pmEndpoint.setManufacturerAndModel("DIY", "ESP32C6-AirQualityMonitor");
  pmEndpoint.setMinMaxValue(0, 1000);

  co2Endpoint.setManufacturerAndModel("DIY", "ESP32C6-AirQualityMonitor");
  co2Endpoint.setMinMaxValue(0, 65000);

  tvocEndpoint.addAnalogInput();
  tvocEndpoint.setManufacturerAndModel("DIY", "ESP32C6-AirQualityMonitor-TVOC");
  tvocEndpoint.setAnalogInputDescription("ENS160 TVOC (ppb)");
  tvocEndpoint.setAnalogInputMinMax(0, 65000);
  tvocEndpoint.setAnalogInputResolution(1);

  aqiEndpoint.addAnalogInput();
  aqiEndpoint.setManufacturerAndModel("DIY", "ESP32C6-AirQualityMonitor-AQI");
  aqiEndpoint.setAnalogInputDescription("ENS160 AQI-UBA (1-5)");
  aqiEndpoint.setAnalogInputMinMax(1, 5);
  aqiEndpoint.setAnalogInputResolution(1);

  Zigbee.addEndpoint(&envEndpoint);
  Zigbee.addEndpoint(&pmEndpoint);
  Zigbee.addEndpoint(&co2Endpoint);
  Zigbee.addEndpoint(&tvocEndpoint);
  Zigbee.addEndpoint(&aqiEndpoint);

  if (!Zigbee.begin()) {
    Serial.println("Zigbee start failed; rebooting.");
    delay(1000);
    ESP.restart();
  }
}

static void handleButton() {
  const bool pressed = digitalRead(PIN_BUTTON) == LOW;
  if (pressed && buttonPressedAt == 0) {
    buttonPressedAt = millis();
    buttonHandled = false;
  } else if (pressed && !buttonHandled && millis() - buttonPressedAt >= BUTTON_HOLD_RESET_MS) {
    buttonHandled = true;
    display->fillScreen(BLACK);
    display->setCursor(16, 30);
    display->setTextSize(2);
    display->setTextColor(YELLOW, BLACK);
    display->print("Clearing Zigbee and re-pairing...");
    Serial.println("Long press: clearing Zigbee network settings.");
    Zigbee.factoryReset(true);
  } else if (!pressed) {
    buttonPressedAt = 0;
    buttonHandled = false;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  SPI.begin(PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI);
  display->begin();
  display->fillScreen(BLACK);
  display->setTextColor(WHITE, BLACK);
  display->setTextSize(2);
  display->setCursor(12, 12);
  display->println("ESP32-C6 Air Quality");
  display->println("Starting sensors...");

  pmSerial.begin(9600, SERIAL_8N1, PIN_PM_RX, PIN_PM_TX);
  values.ahtOk = aht.begin(&Wire);
  values.ensOk = ens160.begin();
  if (values.ensOk) {
    ens160.setOperatingMode(SFE_ENS160_STANDARD);
  }
  Serial.printf("AHT2X: %s | ENS160: %s\n", values.ahtOk ? "OK" : "not found",
                values.ensOk ? "OK" : "not found");

  initZigbee();
  lastSampleMs = millis() - SENSOR_SAMPLE_MS;
  lastDisplayMs = millis() - DISPLAY_REFRESH_MS;
}

void loop() {
  handleButton();
  const uint32_t now = millis();
  if (now - lastSampleMs >= SENSOR_SAMPLE_MS) {
    lastSampleMs = now;
    sampleSensors();
  }
  if (now - lastDisplayMs >= DISPLAY_REFRESH_MS) {
    lastDisplayMs = now;
    drawScreen();
  }
  // Attribute reports are sent on the interval configured by Hubitat's
  // Configure Reporting requests. Do not send on a second, firmware-fixed timer.
  delay(10);
}

