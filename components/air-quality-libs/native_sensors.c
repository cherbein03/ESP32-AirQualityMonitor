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

static uint8_t aht20_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
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
    // The AHT20 needs up to 20 ms after power-on before it accepts commands.
    vTaskDelay(pdMS_TO_TICKS(40));
    if (aht20) {
        uint8_t status = 0;
        if (i2c_master_receive(aht20, &status, 1, 100) != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 status read failed");
        } else if (!(status & 0x08)) {
            const uint8_t init[] = {0xBE, 0x08, 0x00};
            if (write_bytes(aht20, init, sizeof(init)) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                ESP_LOGW(TAG, "AHT20 initialization failed");
            }
        }
    }
    if (ens160) {
        const uint8_t part_id_reg = 0x00;
        uint8_t part_id[2] = {0};
        const esp_err_t part_id_err = i2c_master_transmit_receive(
            ens160, &part_id_reg, 1, part_id, sizeof(part_id), 100);
        if (part_id_err != ESP_OK || part_id[0] != 0x60 || part_id[1] != 0x01) {
            ESP_LOGW(
                TAG,
                "ENS160 not detected at 0x53 (PART_ID=%02X%02X, err=%s)",
                (unsigned)part_id[1],
                (unsigned)part_id[0],
                esp_err_to_name(part_id_err));
            ens160 = NULL;
        } else {
            const uint8_t op_mode[] = {0x10, 0x02};
            if (write_bytes(ens160, op_mode, sizeof(op_mode)) != ESP_OK) {
                ESP_LOGW(TAG, "ENS160 failed to enter standard mode");
            } else {
                ESP_LOGI(TAG, "ENS160 detected at 0x53 and in standard mode");
            }
        }
    }
    return ESP_OK;
}

static bool read_aht20(float *temperature, float *humidity) {
    if (!aht20) return false;
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    if (write_bytes(aht20, trigger, sizeof(trigger)) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(80));
    bool complete = false;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        uint8_t status = 0;
        if (i2c_master_receive(aht20, &status, 1, 100) != ESP_OK) return false;
        if (!(status & 0x80)) {
            complete = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!complete) return false;
    uint8_t data[7];
    if (i2c_master_receive(aht20, data, sizeof(data), 100) != ESP_OK) return false;
    if ((data[0] & 0x80) || aht20_crc8(data, 6) != data[6]) return false;
    const uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    const uint32_t raw_t = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *humidity = raw_h * 100.0f / 1048576.0f;
    *temperature = raw_t * 200.0f / 1048576.0f - 50.0f;
    return true;
}

static bool read_ens160(float *eco2, float *tvoc, float *aqi, uint8_t *status) {
    if (!ens160) return false;
    // Read DEVICE_STATUS and the contiguous AQI/TVOC/eCO2 data block.
    uint8_t reg = 0x20;
    uint8_t data[6];
    if (i2c_master_transmit_receive(ens160, &reg, 1, data, sizeof(data), 100) != ESP_OK) return false;
    *status = data[0];

    // Bits 2:3 encode output validity: normal, warm-up, initial start-up,
    // or invalid. Do not publish estimates until the sensor reports normal.
    const uint8_t validity = (data[0] >> 2) & 0x03;
    if (!(data[0] & 0x80) || (data[0] & 0x40) || validity != 0) return false;

    *aqi = (float)(data[1] & 0x07);
    *tvoc = (float)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    *eco2 = (float)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
    return true;
}

static esp_err_t write_ens160_compensation(float temperature_c, float humidity_percent) {
    if (!ens160) return ESP_ERR_INVALID_STATE;

    // ENS160 host inputs: Kelvin * 64 and %RH * 512, little-endian.
    const uint16_t temp = (uint16_t)((temperature_c + 273.15f) * 64.0f + 0.5f);
    const uint16_t humidity = (uint16_t)(humidity_percent * 512.0f + 0.5f);
    const uint8_t input[] = {
        0x13,
        (uint8_t)(temp & 0xFF), (uint8_t)(temp >> 8),
        (uint8_t)(humidity & 0xFF), (uint8_t)(humidity >> 8)};
    return write_bytes(ens160, input, sizeof(input));
}

void air_quality_sensors_read(air_quality_readings_t *readings) {
    readings->aht_ok = read_aht20(&readings->temperature_c, &readings->humidity_percent);
    readings->ens_status = 0;
    if (readings->aht_ok &&
        write_ens160_compensation(readings->temperature_c, readings->humidity_percent) != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 temperature/humidity compensation write failed");
    }
    readings->ens_ok = read_ens160(&readings->eco2_ppm, &readings->tvoc_ppb, &readings->aqi, &readings->ens_status);
    if (!readings->aht_ok) readings->temperature_c = readings->humidity_percent = NAN;
    if (!readings->ens_ok) readings->eco2_ppm = readings->tvoc_ppb = readings->aqi = NAN;
}
