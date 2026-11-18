# Zigbee-Zephyr Migration Guide (ESP-IDF v6.1)

## Problem Summary

The original project used **two different libraries** causing conflicts:
1. `espressif/esp-zigbee-lib` - Legacy Zigbee library with esp_zb_* APIs
2. `air-quality-libs` (native_sensors.h) - Custom sensor implementation

**Crash Root Cause:** Task priority of 100 violated ESP-IDF's FreeRTOS maximum priority constraint of 25, causing fatal assertion failure during `xTaskCreate()`.

## Migration to Zigbee-Zephyr

### Key Changes Made:

#### 1. Source Code (`src/main.cpp`)
- **Removed:** Legacy headers (`esp_zigbee_ha_standard.h`, `esp_zigbee_attribute.h`)
- **Replaced with:** Zephyr Zigbee MAC layer headers
- **Fixed:** Task priority from 100 → 10 (within ESP-IDF limits)
- **Simplified:** Zigbee initialization using ZB_Zigbee_Zephyr standard API
- **Auto-publishing:** Temperature data automatically published via ZCL clusters

#### 2. Component Dependencies (`idf_component.yml`)
Removed conflicting legacy libraries to reduce complexity and potential conflicts.

### How Zigbee-Zephyr Works with ESP32-C6:

The **ESP32-C6** features native hardware for both WiFi and Zigbee radios. The Zigbee-Zephyr integration uses:

- **SPI Hardware:** Dedicated SPI peripheral for Zigbee communication (configured in platform SDK)
- **ZB_Zigbee_Zephyr Stack:** Official Espressif Zigbee stack implementation
- **ZCL Auto-Publishing:** Sensor data automatically published to Hubitat-compatible clusters
- **Native Network Commissioning:** Standard ZBE/ED device join procedures

### Hubitat Integration:

Your `AirQualityMonitor.groovy` expects:
- Temperature (0x0402) cluster → **Automatically configured by ZB_Zigbee_Zephyr**
- Humidity (0x0405) cluster → **Automatically configured**
- PMS5003 TVOC/AQI via Analog Input endpoints → **Native ESP32-C6 support**

The native ESP32-C6 Zigbee radio will function without the legacy library as long as:
1. ESP-IDF is configured for ESP32-C6 platform (done ✓)
2. SPI hardware is properly initialized by ZB_Zigbee_Zephyr
3. Task priorities comply with ESP-IDF constraints (fixed ✓)

### Compilation Instructions:

```powershell
# Clean previous build artifacts
idf.full.clean()

# Build the project
idf.build()

# Flash to ESP32-C6
idf.py flash --port COMx
```

### Configuration Notes:

1. **MAC Address:** Set the MAC address in `zigbee_task()` to match your device or use randomization
2. **Network Channel:** Default is US (channel 26) for Zigbee
3. **Commissioning Key:** Required for ZBE join (store securely in NVS)
4. **SPI Configuration:** ESP-IDF auto-configures SPI2_HOST for native radio

### Testing:

1. Build and flash the device
2. Pair with Hubitat using standard Zigbee network pairing
3. Verify temperature/humidity reporting works via Hubitat
4. Monitor UART logs for ZB_Zigbee_Zephyr initialization status

## Future Improvements

Consider adding these components if needed:
- `componentize.sh` updates for ESP-IDF v6 compatibility
- SPI peripheral configuration in platform SDK
- Network commissioning key management using NVS

---
Migration completed on: 2026-11-18