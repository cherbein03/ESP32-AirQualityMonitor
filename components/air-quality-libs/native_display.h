#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t native_display_init(void);
void native_display_clear(uint16_t color);
void native_display_text(int x, int y, const char *text, uint16_t color);
void native_display_test_pixel(void);
void native_display_fill_rect(
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    uint16_t color);
void native_display_demo(void);

void native_display_show_readings(
    float temp,
    float humidity,
    float eco2,
    float tvoc,
    float aqi);
    
#ifdef __cplusplus
}
#endif

