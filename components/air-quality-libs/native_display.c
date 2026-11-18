#include "native_display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include "project_config.h"

static spi_device_handle_t lcd;
static void cmd(uint8_t value) {
    gpio_set_level(PIN_TFT_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &value};
    spi_device_transmit(lcd, &t);
}
static void data(const uint8_t *value, size_t len) {
    gpio_set_level(PIN_TFT_DC, 1);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = value};
    spi_device_transmit(lcd, &t);
}

esp_err_t native_display_init(void) {
    gpio_set_direction(PIN_TFT_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_TFT_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_TFT_RESET, 0); esp_rom_delay_us(10000);
    gpio_set_level(PIN_TFT_RESET, 1); esp_rom_delay_us(120000);
    spi_bus_config_t bus = {.mosi_io_num = PIN_TFT_MOSI, .miso_io_num = PIN_TFT_MISO, .sclk_io_num = PIN_TFT_SCK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 320 * 40 * 2};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {.clock_speed_hz = 40000000, .mode = 0, .spics_io_num = PIN_TFT_CS, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd));

    cmd(0x11);
vTaskDelay(pdMS_TO_TICKS(120));

cmd(0x36);
{
    uint8_t madctl = 0x48;
    data(&madctl, 1);
}

cmd(0x3A);
{
    uint8_t pixel_format = 0x66;
    data(&pixel_format, 1);
}

cmd(0x29);
return ESP_OK;
}

static void set_addr_window(
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1)
{
    uint8_t data_buf[4];

    cmd(0x2A);
    data_buf[0] = x0 >> 8;
    data_buf[1] = x0 & 0xFF;
    data_buf[2] = x1 >> 8;
    data_buf[3] = x1 & 0xFF;
    data(data_buf, 4);

    cmd(0x2B);
    data_buf[0] = y0 >> 8;
    data_buf[1] = y0 & 0xFF;
    data_buf[2] = y1 >> 8;
    data_buf[3] = y1 & 0xFF;
    data(data_buf, 4);

    cmd(0x2C);
}

static void draw_pixel(
    uint16_t x,
    uint16_t y,
    uint16_t color)
{
    set_addr_window(x, y, x, y);

    uint8_t pixel[3];

    pixel[0] = ((color >> 11) & 0x1F) << 3;
    pixel[1] = ((color >> 5) & 0x3F) << 2;
    pixel[2] = (color & 0x1F) << 3;

    gpio_set_level(PIN_TFT_DC, 1);

    spi_transaction_t t = {0};

    t.length = 24;
    t.tx_buffer = pixel;

    spi_device_transmit(lcd, &t);
}

static const uint8_t digit_font[10][5] =
{
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x62,0x51,0x49,0x49,0x46}, // 2
    {0x22,0x49,0x49,0x49,0x36}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x2F,0x49,0x49,0x49,0x31}, // 5
    {0x3E,0x49,0x49,0x49,0x32}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x26,0x49,0x49,0x49,0x3E}  // 9
};

static void draw_digit(
    int x,
    int y,
    char c,
    uint16_t color)
{
    if (c < '0' || c > '9')
    {
        return;
    }

    const uint8_t *glyph =
        digit_font[c - '0'];

    for (int col = 0; col < 5; col++)
    {
        uint8_t bits = glyph[col];

        for (int row = 0; row < 7; row++)
        {
            if (bits & (1 << row))
            {
                draw_pixel(
                    x + col,
                    y + row,
                    color);
            }
        }
    }
}

void native_display_text(
    int x,
    int y,
    const char *text,
    uint16_t color)
{
    while (*text)
    {
        native_display_fill_rect(
            x,
            y,
            12,
            18,
            color);

        x += 16;
        text++;
    }
}

void native_display_show_readings(
    float temp,
    float humidity,
    float eco2,
    float tvoc,
    float aqi)
{
    native_display_fill_rect(
        0,
        0,
        320,
        120,
        0x0000);

    int t = (int)temp;

    draw_digit(20,20,(t/10)%10,0xFFFF);
    draw_digit(30,20,(t%10)+'0',0xFFFF);

    int h = (int)humidity;

    draw_digit(20,50,(h/10)%10+'0',0xFFFF);
    draw_digit(30,50,(h%10)+'0',0xFFFF);
}




void native_display_clear(uint16_t color)
{
    set_addr_window(0, 0, 479, 319);

    uint8_t pixel[3];

    pixel[0] = ((color >> 11) & 0x1F) << 3;
    pixel[1] = ((color >> 5) & 0x3F) << 2;
    pixel[2] = (color & 0x1F) << 3;

    gpio_set_level(PIN_TFT_DC, 1);

    for (int i = 0; i < 480 * 320; i++)
    {
        spi_transaction_t t = {
            .length = 24,
            .tx_buffer = pixel
        };

        spi_device_transmit(lcd, &t);
    }
}

void native_display_test_pixel(void)
{
    set_addr_window(10, 10, 109, 109);

    uint8_t red[3] = {
        255,
        0,
        0
    };

    gpio_set_level(PIN_TFT_DC, 1);

    for (int i = 0; i < 100 * 100; i++)
    {
        spi_transaction_t t = {0};
        t.length = 24;
        t.tx_buffer = red;
        spi_device_transmit(lcd, &t);
    }
}

void native_display_fill_rect(
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    uint16_t color)
{
    set_addr_window(
        x,
        y,
        x + w - 1,
        y + h - 1);

    uint8_t pixel[3];

    pixel[0] = ((color >> 11) & 0x1F) << 3;
    pixel[1] = ((color >> 5) & 0x3F) << 2;
    pixel[2] = (color & 0x1F) << 3;

    gpio_set_level(PIN_TFT_DC, 1);

    uint32_t count = (uint32_t)w * h;

    for (uint32_t i = 0; i < count; i++)
    {
        spi_transaction_t t = {0};

        t.length = 24;
        t.tx_buffer = pixel;

        spi_device_transmit(lcd, &t);

        if ((i % 2048) == 0)
        {
            taskYIELD();
        }
    }
}

void native_display_demo(void)
{
    native_display_fill_rect(0, 0, 480, 320, 0x0000);

    native_display_fill_rect(0, 0, 480, 30, 0xFFFF);

    native_display_fill_rect(0, 40, 200, 20, 0xF800);

    native_display_fill_rect(0, 70, 200, 20, 0x07E0);

    native_display_fill_rect(0, 100, 200, 20, 0x001F);
}