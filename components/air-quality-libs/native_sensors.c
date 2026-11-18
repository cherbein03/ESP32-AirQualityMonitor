#include "native_sensors.h"
#include <math.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "project_config.h"

static const char *TAG = "sensors";
static i2c_master_dev_handle_t aht20;
static i2c_master_dev_handle_t ens160;

static esp_err_t write_bytes(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len) {
    return i2c_master_transmit(dev, data, len, 100);
}

esp_err_t air_quality_sensors_init(void) {
    // ESP-IDF 6.x uses -1 to request automatic master-port selection.
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = PIN_I2C_SDA;
    bus_cfg.scl_io_num = PIN_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = 0x38;
    cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &cfg, &aht20) != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 not found");
    }
    i2c_device_config_t ens_cfg = {};
    ens_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    ens_cfg.device_address = 0x53;
    ens_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &ens_cfg, &ens160) != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 not found");
    }
    if (aht20) {
        const uint8_t init[] = {0xBE, 0x08, 0x00};
        write_bytes(aht20, init, sizeof(init));
    }
    if (ens160) {
        const uint8_t op_mode[] = {0x10, 0x02};
        write_bytes(ens160, op_mode, sizeof(op_mode));
        ESP_LOGI(TAG, "ENS160 initialized, warming up...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_OK;
}

static bool read_aht20(float *temperature, float *humidity) {
    if (!aht20) return false;
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    if (write_bytes(aht20, trigger, sizeof(trigger)) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(80));
    uint8_t data[6];
    if (i2c_master_receive(aht20, data, sizeof(data), 100) != ESP_OK) return false;
    const uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    const uint32_t raw_t = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *humidity = raw_h * 100.0f / 1048576.0f;
    *temperature = raw_t * 200.0f / 1048576.0f - 50.0f;
    return !(data[0] & 0x80);
}

static bool read_ens160(float *eco2, float *tvoc, float *aqi) {
    if (!ens160) return false;
    uint8_t reg = 0x20;
    uint8_t data[5];
    if (i2c_master_transmit_receive(ens160, &reg, 1, data, sizeof(data), 100) != ESP_OK) return false;
    *aqi = data[0] & 0x07;
    *tvoc = (float)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
    *eco2 = (float)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
    return true;
}

void air_quality_sensors_read(air_quality_readings_t *readings) {
    readings->aht_ok = read_aht20(&readings->temperature_c, &readings->humidity_percent);
    readings->ens_ok = read_ens160(&readings->eco2_ppm, &readings->tvoc_ppb, &readings->aqi);
    if (!readings->aht_ok) readings->temperature_c = readings->humidity_percent = NAN;
    if (!readings->ens_ok) readings->eco2_ppm = readings->tvoc_ppb = readings->aqi = NAN;
}
