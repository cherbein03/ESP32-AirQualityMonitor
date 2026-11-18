#include <math.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_attribute.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"

#include "native_display.h"
#include "native_sensors.h"
#include "project_config.h"

static const char *TAG = "air_quality";

static air_quality_readings_t readings;
static float pm25 = NAN;
static float pm;
static esp_zb_ep_list_t *temperature_ep = nullptr;


/* -------------------------------------------------------------------------- */
/* PMS5003                                                                     */
/* -------------------------------------------------------------------------- */

static bool read_pms5003(float *value)
{
    static uint8_t frame[32];
    static size_t used = 0;

    uint8_t byte;

    while (uart_read_bytes(UART_NUM_1, &byte, 1, 0) == 1)
    {
        if (used == 0 && byte != 0x42)
        {
            continue;
        }

        if (used == 1 && byte != 0x4D)
        {
            used = 0;
            continue;
        }

        frame[used++] = byte;

        if (used == sizeof(frame))
        {
            used = 0;

            uint16_t sum = 0;

            for (size_t i = 0; i < 30; ++i)
            {
                sum += frame[i];
            }

            uint16_t supplied =
                ((uint16_t)frame[30] << 8) | frame[31];

            if ((((uint16_t)frame[2] << 8) | frame[3]) != 28 ||
                sum != supplied)
            {
                return false;
            }

            *value =
                (float)(((uint16_t)frame[12] << 8) | frame[13]);

            return true;
        }
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/* Zigbee                                                                       */
/* -------------------------------------------------------------------------- */

static void update_zigbee(void)
{
    if (!temperature_ep || !readings.aht_ok)
    {
        return;
    }

    int16_t temperature =
        (int16_t)lroundf(readings.temperature_c * 100.0f);

    esp_zb_zcl_set_attribute_val(
        EP_ENVIRONMENT,
        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
        &temperature,
        false);
}

static void zigbee_task(void *)
{
    esp_zb_cfg_t network_cfg = {};

    network_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_ED;
    network_cfg.install_code_policy = false;
    network_cfg.nwk_cfg.zed_cfg.ed_timeout =
        ESP_ZB_ED_AGING_TIMEOUT_64MIN;
    network_cfg.nwk_cfg.zed_cfg.keep_alive = 3000;

    esp_zb_init(&network_cfg);

    esp_zb_temperature_sensor_cfg_t sensor_cfg =
        ESP_ZB_DEFAULT_TEMPERATURE_SENSOR_CONFIG();

    temperature_ep =
        esp_zb_temperature_sensor_ep_create(
            EP_ENVIRONMENT,
            &sensor_cfg);

    esp_zb_device_register(temperature_ep);

    esp_zb_set_primary_network_channel_set(0x07FFF800);

    ESP_ERROR_CHECK(esp_zb_start(false));

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void esp_zb_app_signal_handler(
    esp_zb_app_signal_t *signal)
{
    if (!signal)
    {
        return;
    }

    const esp_zb_app_signal_type_t type =
        static_cast<esp_zb_app_signal_type_t>(
            *(signal->p_app_signal));

    if ((type == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP ||
         type == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START) &&
        signal->esp_err_status == ESP_OK)
    {
        esp_zb_bdb_start_top_level_commissioning(
            type == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP
                ? ESP_ZB_BDB_MODE_INITIALIZATION
                : ESP_ZB_BDB_MODE_NETWORK_STEERING);
    }
}

/* -------------------------------------------------------------------------- */
/* Sensor Task                                                                  */
/* -------------------------------------------------------------------------- */

static void sensor_task(void *pvParameters)
{
    while (true)
    {
        air_quality_sensors_read(&readings);

        native_display_show_readings(
            readings.temperature_c,
            readings.humidity_percent,
            readings.eco2_ppm,
            readings.tvoc_ppb,
            readings.aqi);

        if (readings.aht_ok)
        {
            ESP_LOGI(
                TAG,
                "Temp=%.1fC Hum=%.1f%%",
                readings.temperature_c,
                readings.humidity_percent);
        }
        else
        {
            ESP_LOGW(TAG, "AHT20 read failed");
        }

        if (readings.ens_ok)
        {
            ESP_LOGI(
                TAG,
                "eCO2=%.0fppm TVOC=%.0fppb AQI=%.0f",
                readings.eco2_ppm,
                readings.tvoc_ppb,
                readings.aqi);
        }
        else
        {
            ESP_LOGW(TAG, "ENS160 read failed");
        }

        if (read_pms5003(&pm))
            {
                pm25 = pm;
            }

        update_zigbee();

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}



/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};

    ESP_ERROR_CHECK(gpio_config(&button));

    uart_config_t uart = {};

    uart.baud_rate = 9600;
    uart.data_bits = UART_DATA_8_BITS;
    uart.parity = UART_PARITY_DISABLE;
    uart.stop_bits = UART_STOP_BITS_1;
    uart.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(
        uart_driver_install(
            UART_NUM_1,
            1024,
            0,
            0,
            nullptr,
            0));

    ESP_ERROR_CHECK(
        uart_param_config(
            UART_NUM_1,
            &uart));

    ESP_ERROR_CHECK(
        uart_set_pin(
            UART_NUM_1,
            PIN_PM_TX,
            PIN_PM_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(air_quality_sensors_init());
    ESP_ERROR_CHECK(native_display_init());
    
 

    xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        nullptr,
        5,
        nullptr);

    xTaskCreate(
        zigbee_task,
        "zigbee_task",
        8192,
        nullptr,
        20,
        nullptr);

    ESP_LOGI(
        TAG,
        "ESP32-C6 air-quality monitor started");
}