#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t native_display_init(void);

void native_display_show_readings(
    float temp,
    float humidity,
    float eco2,
    float tvoc,
    float aqi,
    float pm25,
    bool temperature_fahrenheit,
    const bool alert_values[6],
    bool zigbee_joined,
    bool zigbee_pairing,
    bool zigbee_data_live);

#ifdef __cplusplus
}
#endif
