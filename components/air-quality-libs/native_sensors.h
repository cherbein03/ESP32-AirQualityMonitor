#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float humidity_percent;
    float eco2_ppm;
    float tvoc_ppb;
    float aqi;
    bool aht_ok;
    bool ens_ok;
} air_quality_readings_t;

esp_err_t air_quality_sensors_init(void);
void air_quality_sensors_read(air_quality_readings_t *readings);

#ifdef __cplusplus
}
#endif

