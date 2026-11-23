#include "native_display.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "project_config.h"

/* The ILI9488 is driven directly over SPI in RGB666 (three bytes per pixel).
 * Small row chunks keep the transfer buffer in internal RAM. */
static spi_device_handle_t lcd;
DMA_ATTR static uint8_t transfer_buffer[DISPLAY_HORIZONTAL_PIXELS * 8 * 3];
DMA_ATTR static uint8_t glyph_buffer[12 * 14 * 3];

static void send_command(uint8_t value)
{
    gpio_set_level(PIN_TFT_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &value };
    spi_device_transmit(lcd, &t);
}

static void send_data(const uint8_t *value, size_t length)
{
    gpio_set_level(PIN_TFT_DC, 1);
    spi_transaction_t t = { .length = length * 8, .tx_buffer = value };
    spi_device_transmit(lcd, &t);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t args[4];
    send_command(0x2A);
    args[0] = x0 >> 8; args[1] = x0;
    args[2] = x1 >> 8; args[3] = x1;
    send_data(args, sizeof(args));
    send_command(0x2B);
    args[0] = y0 >> 8; args[1] = y0;
    args[2] = y1 >> 8; args[3] = y1;
    send_data(args, sizeof(args));
    send_command(0x2C);
}

static void color_bytes(uint16_t color, uint8_t *out)
{
    out[0] = (uint8_t)((((color >> 11) & 0x1f) * 255) / 31);
    out[1] = (uint8_t)((((color >> 5) & 0x3f) * 255) / 63);
    out[2] = (uint8_t)(((color & 0x1f) * 255) / 31);
}

static void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!lcd || !w || !h || x >= DISPLAY_HORIZONTAL_PIXELS || y >= DISPLAY_VERTICAL_PIXELS) return;
    if (x + w > DISPLAY_HORIZONTAL_PIXELS) w = DISPLAY_HORIZONTAL_PIXELS - x;
    if (y + h > DISPLAY_VERTICAL_PIXELS) h = DISPLAY_VERTICAL_PIXELS - y;
    uint8_t rgb[3]; color_bytes(color, rgb);
    for (size_t i = 0; i < sizeof(transfer_buffer); i += 3) {
        transfer_buffer[i] = rgb[0]; transfer_buffer[i + 1] = rgb[1]; transfer_buffer[i + 2] = rgb[2];
    }
    set_window(x, y, x + w - 1, y + h - 1);
    const size_t row_bytes = (size_t)w * 3;
    uint16_t rows_left = h;
    while (rows_left) {
        uint16_t rows = rows_left > 8 ? 8 : rows_left;
        send_data(transfer_buffer, row_bytes * rows);
        rows_left -= rows;
    }
}

/* Five-column bitmap font, one byte per column; bit zero is the top row. */
static const uint8_t *glyph(char c)
{
    static const uint8_t blank[5] = {0};
    static const uint8_t digits[10][5] = {
        {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},
        {0x62,0x51,0x49,0x49,0x46},{0x22,0x49,0x49,0x49,0x36},
        {0x18,0x14,0x12,0x7f,0x10},{0x2f,0x49,0x49,0x49,0x31},
        {0x3e,0x49,0x49,0x49,0x32},{1,0x71,9,5,3},
        {0x36,0x49,0x49,0x49,0x36},{0x26,0x49,0x49,0x49,0x3e}};
    static const uint8_t letters[26][5] = {
        {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
        {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
        {0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
        {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},
        {0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,1},
        {0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
        {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},
        {0x3e,0x41,0x41,0x41,0x3e},{0x7f,9,9,9,6},
        {0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},
        {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
        {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
        {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}};
    if (c == 'e') { static const uint8_t lowercase_e[5] = {0x38,0x54,0x54,0x7c,0x1c}; return lowercase_e; }
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c == '.') { static const uint8_t dot[5] = {0,0x60,0x60,0,0}; return dot; }
    if (c == '-') { static const uint8_t dash[5] = {8,8,8,8,8}; return dash; }
    if (c == ':') { static const uint8_t colon[5] = {0,0x36,0x36,0,0}; return colon; }
    if (c == '%') { static const uint8_t pct[5] = {0x63,0x13,8,0x64,0x63}; return pct; }
    return blank;
}

static void draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *bits = glyph(c);
    uint8_t f[3], b[3]; color_bytes(fg, f); color_bytes(bg, b);
    /* 6x7 cell, 1px spacing; rendering at 2x scale for readability. */
    for (int yy = 0; yy < 14; ++yy) for (int xx = 0; xx < 12; ++xx) {
        bool on = xx < 10 && yy < 14 && (bits[xx / 2] & (1u << (yy / 2)));
        const uint8_t *p = on ? f : b;
        size_t at = ((size_t)yy * 12 + xx) * 3;
        glyph_buffer[at] = p[0]; glyph_buffer[at + 1] = p[1]; glyph_buffer[at + 2] = p[2];
    }
    set_window(x, y, x + 11, y + 13);
    send_data(glyph_buffer, sizeof(glyph_buffer));
}

static void draw_text(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg)
{
    while (*text && x + 12 <= DISPLAY_HORIZONTAL_PIXELS) {
        char c = *text++;
        if (c >= 'a' && c <= 'z' && c != 'e') c -= 'a' - 'A';
        draw_char(x, y, c, fg, bg);
        x += 14;
    }
}

static void fill_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t radius, uint16_t color)
{
    if (!w || !h) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    fill_rect(x + radius, y, w - radius * 2, h, color);
    if (h > radius * 2) {
        fill_rect(x, y + radius, radius, h - radius * 2, color);
        fill_rect(x + w - radius, y + radius, radius, h - radius * 2, color);
    }
    for (uint8_t row = 0; row < radius; ++row) {
        float dy = (float)(radius - row);
        uint16_t inset = radius - (uint16_t)sqrtf((float)radius * radius - dy * dy);
        uint16_t span = w - inset * 2;
        fill_rect(x + inset, y + row, span, 1, color);
        fill_rect(x + inset, y + h - 1 - row, span, 1, color);
    }
}

static void draw_centered_text(uint16_t x, uint16_t y, uint16_t w, const char *text,
                               uint16_t fg, uint16_t bg, bool bold)
{
    size_t chars = strlen(text);
    uint16_t text_width = chars ? (uint16_t)(chars * 14 - 2) : 0;
    uint16_t text_x = text_width < w ? x + (w - text_width) / 2 : x;
    draw_text(text_x, y, text, fg, bg);
    if (bold)
        draw_text(text_x + 1, y, text, fg, bg);
}

static void draw_connection_badge(bool zigbee_joined, bool zigbee_pairing, bool zigbee_data_live)
{
    const uint16_t white = 0xffff;
    const uint16_t green = 0x07e0, yellow = 0xffe0, red = 0xf800, cyan = 0x07ff;
    const uint16_t badge_bg = 0x02b0;
    uint16_t status_color = zigbee_joined ? (zigbee_data_live ? green : cyan) : (zigbee_pairing ? yellow : red);
    const char *status_text = zigbee_joined ? (zigbee_data_live ? "LIVE" : "LINK") : (zigbee_pairing ? "PAIR" : "OFF");

    fill_round_rect(231, 14, 76, 31, 10, badge_bg);
    fill_rect(240, 34, 3, 5, status_color);
    fill_rect(245, 29, 3, 10, status_color);
    fill_rect(250, 23, 3, 16, status_color);
    draw_text(258, 23, status_text, white, badge_bg);
}

static void draw_initial_layout(bool temperature_fahrenheit)
{
    const uint16_t bg = 0x0841, white = 0xffff, muted = 0x9cf3, cyan = 0x07ff;
    const uint16_t card_border = 0x2945, card_bg = 0x10a2, header_bg = 0x018c;
    const char *labels[] = { temperature_fahrenheit ? "TEMP F" : "TEMP C", "HUMIDITY", "PM2.5", "eCO2 PPM", "TVOC PPB", "AQI" };

    fill_rect(0, 0, DISPLAY_HORIZONTAL_PIXELS, DISPLAY_VERTICAL_PIXELS, bg);
    fill_rect(0, 0, DISPLAY_HORIZONTAL_PIXELS, 58, header_bg);
    fill_rect(12, 52, 52, 3, cyan);
    draw_text(12, 9, "AIR QUALITY", white, header_bg);
    draw_text(12, 31, "INDOOR MONITOR", muted, header_bg);

    for (int i = 0; i < 6; ++i) {
        uint16_t col = (uint16_t)(i % 2);
        uint16_t row = (uint16_t)(i / 2);
        uint16_t x = (uint16_t)(8 + col * 156);
        uint16_t y = (uint16_t)(68 + row * 130);
        fill_round_rect(x, y, 148, 116, 12, card_border);
        fill_round_rect(x + 1, y + 1, 146, 114, 11, card_bg);
        fill_round_rect(x + 12, y + 15, 4, 13, 2, cyan);
        draw_text(x + 24, y + 15, labels[i], muted, card_bg);
    }
}

esp_err_t native_display_init(void)
{
    gpio_config_t pins = { .pin_bit_mask = (1ULL << PIN_TFT_DC) | (1ULL << PIN_TFT_RESET), .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&pins));
    gpio_set_level(PIN_TFT_RESET, 0); esp_rom_delay_us(10000);
    gpio_set_level(PIN_TFT_RESET, 1); esp_rom_delay_us(120000);
    spi_bus_config_t bus = { .mosi_io_num = PIN_TFT_MOSI, .miso_io_num = PIN_TFT_MISO,
        .sclk_io_num = PIN_TFT_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(transfer_buffer) };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "display", "SPI bus init failed");
    spi_device_interface_config_t dev = { .clock_speed_hz = 40000000, .mode = 0,
        .spics_io_num = PIN_TFT_CS, .queue_size = 1 };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &lcd), "display", "SPI device init failed");
    send_command(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    send_command(0x36); uint8_t madctl = 0x48; send_data(&madctl, 1);
    send_command(0x3A); uint8_t format = 0x66; send_data(&format, 1);
    send_command(0x29);
    fill_rect(0, 0, DISPLAY_HORIZONTAL_PIXELS, DISPLAY_VERTICAL_PIXELS, 0x0000);
    return ESP_OK;
}

void native_display_show_readings(float temp, float humidity, float eco2, float tvoc, float aqi, float pm25,
                                  bool temperature_fahrenheit, const bool alert_values[6],
                                  bool zigbee_joined, bool zigbee_pairing, bool zigbee_data_live)
{
    static bool layout_drawn = false;
    static bool previous_fahrenheit = false;
    static bool previous_status_valid = false;
    static bool previous_joined = false, previous_pairing = false, previous_live = false;
    static float previous_values[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
    static bool previous_alerts[6] = {false, false, false, false, false, false};
    static bool previous_values_drawn[6] = {false, false, false, false, false, false};
    const uint16_t card_bg = 0x10a2, muted = 0x9cf3, cyan = 0x07ff, red = 0xf800;
    const char *labels[] = { temperature_fahrenheit ? "TEMP F" : "TEMP C", "HUMIDITY", "PM2.5", "eCO2 PPM", "TVOC PPB", "AQI" };

    if (!layout_drawn) {
        draw_initial_layout(temperature_fahrenheit);
        layout_drawn = true;
        previous_fahrenheit = temperature_fahrenheit;
    } else if (previous_fahrenheit != temperature_fahrenheit) {
        draw_text(32, 83, labels[0], muted, card_bg);
        previous_fahrenheit = temperature_fahrenheit;
    }

    if (!previous_status_valid || previous_joined != zigbee_joined ||
        previous_pairing != zigbee_pairing || previous_live != zigbee_data_live) {
        draw_connection_badge(zigbee_joined, zigbee_pairing, zigbee_data_live);
        previous_joined = zigbee_joined;
        previous_pairing = zigbee_pairing;
        previous_live = zigbee_data_live;
        previous_status_valid = true;
    }

    float shown_temp = temperature_fahrenheit ? temp * 9.0f / 5.0f + 32.0f : temp;
    float values[] = { shown_temp, humidity, pm25, eco2, tvoc, aqi };
    const char *units[] = { temperature_fahrenheit ? "F" : "C", "%", "UG/M3", "PPM", "PPB", "" };
    for (int i = 0; i < 6; ++i) {
        uint16_t col = (uint16_t)(i % 2);
        uint16_t row = (uint16_t)(i / 2);
        uint16_t x = (uint16_t)(8 + col * 156);
        uint16_t y = (uint16_t)(68 + row * 130);
        bool alert = alert_values && alert_values[i];
        bool valid_now = isfinite(values[i]);
        bool valid_before = isfinite(previous_values[i]);
        bool value_changed = !previous_values_drawn[i] || valid_now != valid_before ||
            (valid_now && fabsf(values[i] - previous_values[i]) > 0.001f);
        bool alert_changed = alert != previous_alerts[i];

        if (alert_changed)
            fill_round_rect(x + 12, y + 15, 4, 13, 2, alert ? red : cyan);
        if (!value_changed && !alert_changed)
            continue;

        char value[24];
        if (!isfinite(values[i])) snprintf(value, sizeof(value), "--");
        else if (i == 0 || i == 1) snprintf(value, sizeof(value), "%.1f %s", values[i], units[i]);
        else if (i == 5) snprintf(value, sizeof(value), "%.0f", values[i]);
        else snprintf(value, sizeof(value), "%.0f %s", values[i], units[i]);
        fill_rect(x + 4, y + 54, 140, 48, card_bg);
        uint16_t value_color = alert ? red : cyan;
        draw_centered_text(x + 4, y + 61, 140, value, value_color, card_bg,
            alert);
        previous_values[i] = values[i];
        previous_alerts[i] = alert;
        previous_values_drawn[i] = true;
    }
}
