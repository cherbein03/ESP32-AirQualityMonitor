#include <math.h>
#include <stdio.h>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_endpoint.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_secur.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zcl/esp_zigbee_zcl_analog_input.h"
#include "zcl/esp_zigbee_zcl_carbon_dioxide_measurement.h"
#include "zcl/esp_zigbee_zcl_pm2_5_measurement.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"

#include "native_display.h"
#include "native_sensors.h"
#include "project_config.h"

static const char *TAG = "air_quality";

static air_quality_readings_t readings;
static float pm25 = NAN;
static float pm;
static esp_zb_ep_list_t *sensor_endpoints = nullptr;
static volatile bool zigbee_stack_ready = false;
static volatile bool zigbee_startup_resolved = false;
static volatile bool zigbee_joined = false;
static volatile bool zigbee_pairing = false;
static volatile bool zigbee_data_live = false;
static volatile uint16_t display_update_interval_seconds = 60;
static volatile bool display_temperature_fahrenheit = false;
static volatile bool display_settings_changed = true;
static volatile bool settings_save_pending = false;
static uint16_t report_interval_attribute = 60;
static uint8_t temperature_unit_attribute = 0;
static constexpr int16_t DISABLED_TEMP_ALERT = INT16_MIN;
static constexpr uint16_t DISABLED_U16_ALERT = UINT16_MAX;
static constexpr uint32_t DISABLED_U32_ALERT = 0x7FFFFFFFU;
static volatile int16_t alert_temperature_tenths = DISABLED_TEMP_ALERT;
static volatile uint16_t alert_humidity_tenths = DISABLED_U16_ALERT;
static volatile uint16_t alert_pm25_tenths = DISABLED_U16_ALERT;
static volatile uint32_t alert_eco2_ppm = DISABLED_U32_ALERT;
static volatile uint32_t alert_tvoc_ppb = DISABLED_U32_ALERT;
static volatile uint16_t alert_aqi_tenths = DISABLED_U16_ALERT;
static int16_t temperature_alert_attribute = DISABLED_TEMP_ALERT;
static uint16_t humidity_alert_attribute = DISABLED_U16_ALERT;
static uint16_t pm25_alert_attribute = DISABLED_U16_ALERT;
static uint32_t eco2_alert_attribute = DISABLED_U32_ALERT;
static uint32_t tvoc_alert_attribute = DISABLED_U32_ALERT;
static uint16_t aqi_alert_attribute = DISABLED_U16_ALERT;

static constexpr uint16_t ATTR_REPORT_INTERVAL_SECONDS = 0xF000;
static constexpr uint16_t ATTR_TEMPERATURE_UNIT = 0xF001;
static constexpr uint16_t ATTR_ALERT_TEMPERATURE = 0xF002;
static constexpr uint16_t ATTR_ALERT_HUMIDITY = 0xF003;
static constexpr uint16_t ATTR_ALERT_PM25 = 0xF004;
static constexpr uint16_t ATTR_ALERT_ECO2 = 0xF005;
static constexpr uint16_t ATTR_ALERT_TVOC = 0xF006;
static constexpr uint16_t ATTR_ALERT_AQI = 0xF007;

static void load_display_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("air_quality", NVS_READONLY, &handle) != ESP_OK)
        return;

    uint16_t interval = 60;
    uint8_t unit = 0;
    if (nvs_get_u16(handle, "report_secs", &interval) == ESP_OK &&
        (interval == 10 || interval == 30 || interval == 60 || interval == 300 || interval == 900))
        display_update_interval_seconds = report_interval_attribute = interval;
    if (nvs_get_u8(handle, "temp_unit", &unit) == ESP_OK && unit <= 1)
        display_temperature_fahrenheit = (temperature_unit_attribute = unit) == 1;
    nvs_get_i16(handle, "alert_temp", &temperature_alert_attribute);
    nvs_get_u16(handle, "alert_hum", &humidity_alert_attribute);
    nvs_get_u16(handle, "alert_pm", &pm25_alert_attribute);
    nvs_get_u32(handle, "alert_eco2", &eco2_alert_attribute);
    nvs_get_u32(handle, "alert_tvoc", &tvoc_alert_attribute);
    nvs_get_u16(handle, "alert_aqi", &aqi_alert_attribute);
    alert_temperature_tenths = temperature_alert_attribute;
    alert_humidity_tenths = humidity_alert_attribute;
    alert_pm25_tenths = pm25_alert_attribute;
    alert_eco2_ppm = eco2_alert_attribute;
    alert_tvoc_ppb = tvoc_alert_attribute;
    alert_aqi_tenths = aqi_alert_attribute;
    nvs_close(handle);
}

static void save_display_settings_if_pending(void)
{
    if (!settings_save_pending)
        return;
    settings_save_pending = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("air_quality", NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_u16(handle, "report_secs", display_update_interval_seconds);
        if (err == ESP_OK)
            err = nvs_set_u8(handle, "temp_unit", display_temperature_fahrenheit ? 1 : 0);
        if (err == ESP_OK)
            err = nvs_set_i16(handle, "alert_temp", alert_temperature_tenths);
        if (err == ESP_OK)
            err = nvs_set_u16(handle, "alert_hum", alert_humidity_tenths);
        if (err == ESP_OK)
            err = nvs_set_u16(handle, "alert_pm", alert_pm25_tenths);
        if (err == ESP_OK)
            err = nvs_set_u32(handle, "alert_eco2", alert_eco2_ppm);
        if (err == ESP_OK)
            err = nvs_set_u32(handle, "alert_tvoc", alert_tvoc_ppb);
        if (err == ESP_OK)
            err = nvs_set_u16(handle, "alert_aqi", alert_aqi_tenths);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Saved Hubitat display preferences to NVS");
    else
        ESP_LOGE(TAG, "Could not save Hubitat display preferences: %s", esp_err_to_name(err));
}

static esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id != ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID || !message)
        return ESP_OK;

    const auto *set_message = static_cast<const esp_zb_zcl_set_attr_value_message_t *>(message);
    if (set_message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_BASIC ||
        set_message->info.dst_endpoint != EP_ENVIRONMENT)
        return ESP_OK;

    const auto *data = &set_message->attribute.data;
    if (!data->value)
        return ESP_OK;

    if (set_message->attribute.id == ATTR_REPORT_INTERVAL_SECONDS &&
        data->type == ESP_ZB_ZCL_ATTR_TYPE_U16 && data->size == sizeof(uint16_t))
    {
        uint16_t interval = *static_cast<const uint16_t *>(data->value);
        if (interval == 10 || interval == 30 || interval == 60 || interval == 300 || interval == 900)
        {
            report_interval_attribute = interval;
            display_update_interval_seconds = interval;
            settings_save_pending = true;
            display_settings_changed = true;
            ESP_LOGI(TAG, "Hubitat set display/report update interval to %u seconds", interval);
        }
        else
            ESP_LOGW(TAG, "Ignoring unsupported Hubitat reporting interval: %u seconds", interval);
        return ESP_OK;
    }
    else if (set_message->attribute.id == ATTR_TEMPERATURE_UNIT &&
             data->type == ESP_ZB_ZCL_ATTR_TYPE_U8 && data->size == sizeof(uint8_t))
    {
        uint8_t unit = *static_cast<const uint8_t *>(data->value);
        if (unit <= 1)
        {
            temperature_unit_attribute = unit;
            display_temperature_fahrenheit = unit == 1;
            settings_save_pending = true;
            display_settings_changed = true;
            ESP_LOGI(TAG, "Hubitat set display temperature unit to %s", unit == 1 ? "F" : "C");
        }
        else
            ESP_LOGW(TAG, "Ignoring unsupported Hubitat temperature unit: %u", unit);
        return ESP_OK;
    }
    else if (set_message->attribute.id == ATTR_ALERT_TEMPERATURE && data->type == ESP_ZB_ZCL_ATTR_TYPE_S16 && data->size == sizeof(int16_t))
        alert_temperature_tenths = *static_cast<const int16_t *>(data->value);
    else if (set_message->attribute.id == ATTR_ALERT_HUMIDITY && data->type == ESP_ZB_ZCL_ATTR_TYPE_U16 && data->size == sizeof(uint16_t))
        alert_humidity_tenths = *static_cast<const uint16_t *>(data->value);
    else if (set_message->attribute.id == ATTR_ALERT_PM25 && data->type == ESP_ZB_ZCL_ATTR_TYPE_U16 && data->size == sizeof(uint16_t))
        alert_pm25_tenths = *static_cast<const uint16_t *>(data->value);
    else if (set_message->attribute.id == ATTR_ALERT_ECO2 && data->type == ESP_ZB_ZCL_ATTR_TYPE_U32 && data->size == sizeof(uint32_t))
        alert_eco2_ppm = *static_cast<const uint32_t *>(data->value);
    else if (set_message->attribute.id == ATTR_ALERT_TVOC && data->type == ESP_ZB_ZCL_ATTR_TYPE_U32 && data->size == sizeof(uint32_t))
        alert_tvoc_ppb = *static_cast<const uint32_t *>(data->value);
    else if (set_message->attribute.id == ATTR_ALERT_AQI && data->type == ESP_ZB_ZCL_ATTR_TYPE_U16 && data->size == sizeof(uint16_t))
        alert_aqi_tenths = *static_cast<const uint16_t *>(data->value);
    else
        return ESP_OK;

    settings_save_pending = true;
    display_settings_changed = true;
    ESP_LOGI(TAG, "Hubitat updated one or more screen alert thresholds");
    return ESP_OK;
}

static const char *bdb_status_name(esp_zb_bdb_commissioning_status_t status)
{
    switch (status)
    {
    case ESP_ZB_BDB_STATUS_SUCCESS: return "success";
    case ESP_ZB_BDB_STATUS_IN_PROGRESS: return "in progress";
    case ESP_ZB_BDB_STATUS_NOT_AA_CAPABLE: return "not address-assignment capable";
    case ESP_ZB_BDB_STATUS_NO_NETWORK: return "no open network found";
    case ESP_ZB_BDB_STATUS_TARGET_FAILURE: return "target failed to join";
    case ESP_ZB_BDB_STATUS_FORMATION_FAILURE: return "network formation failed";
    case ESP_ZB_BDB_STATUS_NO_IDENTIFY_QUERY_RESPONSE: return "no identify-query response";
    case ESP_ZB_BDB_STATUS_BINDING_TABLE_FULL: return "binding table full";
    case ESP_ZB_BDB_STATUS_NO_SCAN_RESPONSE: return "no scan response";
    case ESP_ZB_BDB_STATUS_NOT_PERMITTED: return "not permitted";
    case ESP_ZB_BDB_STATUS_TCLK_EX_FAILURE: return "trust-center key exchange failed";
    case ESP_ZB_BDB_STATUS_NOT_ON_A_NETWORK: return "not on a network";
    case ESP_ZB_BDB_STATUS_ON_A_NETWORK: return "already on a network";
    case ESP_ZB_BDB_STATUS_CANCELLED: return "cancelled";
    case ESP_ZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE: return "device announcement failed";
    default: return "unknown";
    }
}

static void add_sensor_endpoint(uint8_t endpoint, esp_zb_attribute_list_t *attribute_list,
                                esp_err_t (*add_cluster)(esp_zb_cluster_list_t *, esp_zb_attribute_list_t *, uint8_t),
                                const char *name)
{
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(clusters ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(attribute_list ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(add_cluster(clusters, attribute_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0};
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(sensor_endpoints, clusters, config));
    ESP_LOGI(TAG, "Added Zigbee endpoint %u (%s)", endpoint, name);
}


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

static bool update_zigbee(void)
{
    if (!sensor_endpoints || !esp_zb_lock_acquire(portMAX_DELAY))
    {
        return false;
    }

    bool environment_updated = false;
    bool particulate_updated = false;
    bool ens_updated = false;
    if (readings.aht_ok)
    {
        int16_t temperature = (int16_t)lroundf(readings.temperature_c * 100.0f);
        uint16_t humidity = (uint16_t)lroundf(readings.humidity_percent * 100.0f);
        esp_zb_zcl_status_t temp_status = esp_zb_zcl_set_attribute_val(EP_ENVIRONMENT, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &temperature, false);
        esp_zb_zcl_status_t humidity_status = esp_zb_zcl_set_attribute_val(EP_ENVIRONMENT, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &humidity, false);
        environment_updated = temp_status == ESP_OK && humidity_status == ESP_OK;
    }
    if (isfinite(pm25))
        particulate_updated = esp_zb_zcl_set_attribute_val(EP_PM25, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID, &pm25, false) == ESP_OK;
    if (readings.ens_ok)
    {
        float co2_fraction = readings.eco2_ppm / 1000000.0f;
        float tvoc = readings.tvoc_ppb;
        float aqi = readings.aqi;
        esp_zb_zcl_status_t co2_status = esp_zb_zcl_set_attribute_val(EP_ECO2, ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID, &co2_fraction, false);
        esp_zb_zcl_status_t tvoc_status = esp_zb_zcl_set_attribute_val(EP_TVOC, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &tvoc, false);
        esp_zb_zcl_status_t aqi_status = esp_zb_zcl_set_attribute_val(EP_AQI, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &aqi, false);
        ens_updated = co2_status == ESP_OK && tvoc_status == ESP_OK && aqi_status == ESP_OK;
    }
    esp_zb_lock_release();
    bool complete = zigbee_joined && environment_updated && particulate_updated && ens_updated;
    if (zigbee_joined && (!environment_updated || !particulate_updated || !ens_updated))
        ESP_LOGW(TAG, "Zigbee local attribute update failed (env=%s pm25=%s ens=%s)",
            environment_updated ? "ok" : "failed", particulate_updated ? "ok" : "failed",
            ens_updated ? "ok" : "failed");
    return complete;
}

static void zigbee_task(void *)
{
    ESP_LOGI(TAG, "Starting Zigbee stack and creating sensor endpoints");
    esp_zb_cfg_t network_cfg = {};

    network_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_ED;
    network_cfg.install_code_policy = false;
    network_cfg.nwk_cfg.zed_cfg.ed_timeout =
        ESP_ZB_ED_AGING_TIMEOUT_64MIN;
    network_cfg.nwk_cfg.zed_cfg.keep_alive = 3000;

    esp_zb_init(&network_cfg);
    esp_zb_core_action_handler_register(zigbee_action_handler);
    // Hubitat can transport the same global key back to the joiner, which
    // causes strict BDB stacks to reject the exchange and leave the network.
    esp_zb_secur_link_key_exchange_required_set(false);
    ESP_LOGW(TAG, "Disabled required Trust Center link-key exchange for Hubitat compatibility");
    esp_zb_set_rx_on_when_idle(true);
    ESP_LOGI(TAG, "Configured mains-powered end device to keep its Zigbee receiver on");

    esp_zb_temperature_sensor_cfg_t sensor_cfg =
        ESP_ZB_DEFAULT_TEMPERATURE_SENSOR_CONFIG();

    sensor_endpoints =
        esp_zb_temperature_sensor_ep_create(
            EP_ENVIRONMENT,
            &sensor_cfg);

    ESP_ERROR_CHECK(sensor_endpoints ? ESP_OK : ESP_ERR_NO_MEM);
    esp_zb_cluster_list_t *environment_clusters = esp_zb_ep_list_get_ep(sensor_endpoints, EP_ENVIRONMENT);
    ESP_ERROR_CHECK(environment_clusters ? ESP_OK : ESP_ERR_INVALID_STATE);
    esp_zb_attribute_list_t *basic_attributes = esp_zb_cluster_list_get_cluster(
        environment_clusters, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_ERROR_CHECK(basic_attributes ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_REPORT_INTERVAL_SECONDS, ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &report_interval_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_TEMPERATURE_UNIT, ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &temperature_unit_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_ALERT_TEMPERATURE, ESP_ZB_ZCL_ATTR_TYPE_S16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &temperature_alert_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_ALERT_HUMIDITY, ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &humidity_alert_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_ALERT_PM25, ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &pm25_alert_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_ALERT_ECO2, ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &eco2_alert_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_ALERT_TVOC, ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &tvoc_alert_attribute));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(basic_attributes, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ATTR_ALERT_AQI, ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &aqi_alert_attribute));
    ESP_LOGI(TAG, "Hubitat settings attributes ready (report interval=%us, temperature=%s)",
        display_update_interval_seconds, display_temperature_fahrenheit ? "F" : "C");
    esp_zb_humidity_meas_cluster_cfg_t humidity_cfg = {.measured_value = 0xFFFF, .min_value = 0, .max_value = 10000};
    esp_zb_attribute_list_t *humidity_attributes = esp_zb_humidity_meas_cluster_create(&humidity_cfg);
    ESP_ERROR_CHECK(humidity_attributes ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_humidity_meas_cluster(environment_clusters,
        humidity_attributes, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_pm2_5_measurement_cluster_cfg_t pm_cfg = {.measured_value = 0, .min_measured_value = 0, .max_measured_value = 1000};
    esp_zb_attribute_list_t *pm_attributes = esp_zb_pm2_5_measurement_cluster_create(&pm_cfg);
    add_sensor_endpoint(EP_PM25, pm_attributes,
        esp_zb_cluster_list_add_pm2_5_measurement_cluster, "PM2.5");
    esp_zb_carbon_dioxide_measurement_cluster_cfg_t co2_cfg = {.measured_value = 0, .min_measured_value = 0, .max_measured_value = 1};
    esp_zb_attribute_list_t *co2_attributes = esp_zb_carbon_dioxide_measurement_cluster_create(&co2_cfg);
    add_sensor_endpoint(EP_ECO2, co2_attributes,
        esp_zb_cluster_list_add_carbon_dioxide_measurement_cluster, "eCO2");

    for (uint8_t endpoint : {EP_TVOC, EP_AQI})
    {
        esp_zb_analog_input_cluster_cfg_t analog_cfg = {.out_of_service = false, .present_value = 0, .status_flags = 0};
        esp_zb_attribute_list_t *analog_attributes = esp_zb_analog_input_cluster_create(&analog_cfg);
        add_sensor_endpoint(endpoint, analog_attributes,
            esp_zb_cluster_list_add_analog_input_cluster, endpoint == EP_TVOC ? "TVOC" : "AQI");
    }

    ESP_ERROR_CHECK(esp_zb_device_register(sensor_endpoints));

    esp_zb_set_primary_network_channel_set(0x07FFF800);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "Zigbee stack started; entering stack event loop");
    esp_zb_stack_main_loop();
}

static void request_network_steering(bool acquire_lock)
{
    if (acquire_lock && !esp_zb_lock_acquire(portMAX_DELAY))
    {
        ESP_LOGE(TAG, "Could not acquire Zigbee lock to start pairing");
        return;
    }

    zigbee_pairing = true;
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);

    if (acquire_lock)
    {
        esp_zb_lock_release();
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Zigbee pairing mode started; open Hubitat device discovery");
    }
    else
    {
        zigbee_pairing = false;
        ESP_LOGW(TAG, "Could not start Zigbee pairing: %s", esp_err_to_name(err));
    }
}

extern "C" void esp_zb_app_signal_handler(
    esp_zb_app_signal_t *signal)
{
    if (!signal || !signal->p_app_signal)
    {
        return;
    }

    const esp_zb_app_signal_type_t type =
        static_cast<esp_zb_app_signal_type_t>(
            *(signal->p_app_signal));

    ESP_LOGD(TAG, "Zigbee signal 0x%02X status=%s", (unsigned)type,
        esp_err_to_name(signal->esp_err_status));

    if (type == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP)
    {
        if (signal->esp_err_status != ESP_OK)
        {
            ESP_LOGE(TAG, "Zigbee stack startup failed: %s", esp_err_to_name(signal->esp_err_status));
            return;
        }
        zigbee_stack_ready = true;
        ESP_LOGI(TAG, "Zigbee stack ready; initializing network commissioning");
        esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Zigbee commissioning initialization failed: %s", esp_err_to_name(err));
    }
    else if (type == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START && signal->esp_err_status == ESP_OK)
    {
        zigbee_startup_resolved = true;
        zigbee_joined = false;
        if (esp_zb_bdb_is_factory_new())
        {
            ESP_LOGI(TAG, "No saved Zigbee network; starting pairing mode");
            request_network_steering(false);
        }
        else
        {
            ESP_LOGI(TAG, "Saved Zigbee network found; restoring that connection without starting pairing");
        }
    }
    else if (type == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START)
    {
        zigbee_startup_resolved = true;
        zigbee_joined = false;
        ESP_LOGE(TAG, "Zigbee initialization did not complete: %s", esp_err_to_name(signal->esp_err_status));
    }
    else if (type == ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT)
    {
        zigbee_startup_resolved = true;
        zigbee_joined = signal->esp_err_status == ESP_OK;
        zigbee_pairing = false;
        zigbee_data_live = false;
        if (zigbee_joined)
            ESP_LOGI(TAG, "Rejoined saved Zigbee network");
        else
        {
            ESP_LOGW(TAG, "Could not rejoin saved Zigbee network yet: %s; will retry saved-network initialization",
                esp_err_to_name(signal->esp_err_status));
        }
    }
    else if (type == ESP_ZB_BDB_SIGNAL_STEERING)
    {
        zigbee_joined = signal->esp_err_status == ESP_OK;
        zigbee_pairing = false;
        zigbee_data_live = false;
        if (zigbee_joined)
            ESP_LOGI(TAG, "Joined Zigbee network (short=0x%04X PAN=0x%04X channel=%u); sensor reports are enabled",
                (unsigned)esp_zb_get_short_address(), (unsigned)esp_zb_get_pan_id(),
                (unsigned)esp_zb_get_current_channel());
        else
        {
            esp_zb_bdb_commissioning_status_t bdb_status = esp_zb_get_bdb_commissioning_status();
            ESP_LOGW(TAG, "Zigbee steering failed: %s (BDB=%u, ESP=%s); will retry",
                bdb_status_name(bdb_status), (unsigned)bdb_status, esp_err_to_name(signal->esp_err_status));
        }
    }
    else if (type == ESP_ZB_ZDO_SIGNAL_LEAVE)
    {
        zigbee_joined = false;
        zigbee_data_live = false;
        ESP_LOGW(TAG, "Left Zigbee network; waiting for the current commissioning attempt to finish before retrying");
    }
    else if (type == ESP_ZB_BDB_SIGNAL_STEERING_CANCELLED)
    {
        zigbee_pairing = false;
        ESP_LOGW(TAG, "Zigbee pairing was cancelled: %s", esp_err_to_name(signal->esp_err_status));
    }
}

static void button_and_pairing_task(void *)
{
    const uint32_t debounce_step_ms = 50;
    const TickType_t retry_period = pdMS_TO_TICKS(15000);
    TickType_t next_retry = xTaskGetTickCount() + retry_period;
    uint32_t held_ms = 0;
    bool reset_handled = false;

    while (true)
    {
        if (zigbee_stack_ready && gpio_get_level((gpio_num_t)PIN_BUTTON) == 0)
        {
            if (held_ms == 0)
                ESP_LOGI(TAG, "Reset button pressed; hold for 3 seconds to clear Zigbee pairing");
            if (held_ms < BUTTON_HOLD_RESET_MS)
                held_ms += debounce_step_ms;

            if (!reset_handled && held_ms >= BUTTON_HOLD_RESET_MS)
            {
                reset_handled = true;
                ESP_LOGW(TAG, "Button held for 3 seconds; release it to clear the saved Zigbee network");
                while (gpio_get_level((gpio_num_t)PIN_BUTTON) == 0)
                    vTaskDelay(pdMS_TO_TICKS(debounce_step_ms));

                ESP_LOGI(TAG, "Erasing saved Zigbee network and restarting in pairing mode");
                if (esp_zb_lock_acquire(portMAX_DELAY))
                {
                    esp_zb_factory_reset();
                    esp_zb_lock_release();
                }
                else
                {
                    ESP_LOGE(TAG, "Could not acquire Zigbee lock to clear saved network");
                }
                held_ms = 0;
                reset_handled = false;
            }
        }
        else
        {
            held_ms = 0;
            reset_handled = false;
        }

        TickType_t now = xTaskGetTickCount();
        if (zigbee_stack_ready && zigbee_startup_resolved && !zigbee_joined && !zigbee_pairing &&
            (int32_t)(now - next_retry) >= 0)
        {
            next_retry = now + retry_period;
            bool factory_new = false;
            if (esp_zb_lock_acquire(portMAX_DELAY))
            {
                factory_new = esp_zb_bdb_is_factory_new();
                esp_zb_lock_release();
            }
            else
            {
                ESP_LOGE(TAG, "Could not acquire Zigbee lock to check saved network state");
                vTaskDelay(pdMS_TO_TICKS(debounce_step_ms));
                continue;
            }

            if (factory_new)
            {
                ESP_LOGI(TAG, "No saved network exists; retrying Zigbee pairing");
                request_network_steering(true);
            }
            else if (esp_zb_lock_acquire(portMAX_DELAY))
            {
                ESP_LOGI(TAG, "Retrying connection to the saved Zigbee network");
                esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
                esp_zb_lock_release();
                if (err != ESP_OK)
                    ESP_LOGW(TAG, "Could not retry saved-network initialization: %s", esp_err_to_name(err));
            }
            else
            {
                ESP_LOGE(TAG, "Could not acquire Zigbee lock to retry saved-network initialization");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(debounce_step_ms));
    }
}

/* -------------------------------------------------------------------------- */
/* Sensor Task                                                                  */
/* -------------------------------------------------------------------------- */

static void sensor_task(void *pvParameters)
{
    TickType_t next_display_update = 0;
    while (true)
    {
        air_quality_sensors_read(&readings);

        if (read_pms5003(&pm))
        {
            pm25 = pm;
        }

        zigbee_data_live = update_zigbee();
        save_display_settings_if_pending();

        TickType_t now = xTaskGetTickCount();
        if (display_settings_changed || next_display_update == 0 || (int32_t)(now - next_display_update) >= 0)
        {
            bool show_fahrenheit = display_temperature_fahrenheit;
            float shown_temperature = show_fahrenheit
                ? readings.temperature_c * 9.0f / 5.0f + 32.0f
                : readings.temperature_c;
            const bool alert_values[6] = {
                alert_temperature_tenths != DISABLED_TEMP_ALERT && shown_temperature >= alert_temperature_tenths / 10.0f,
                readings.aht_ok && alert_humidity_tenths != DISABLED_U16_ALERT && readings.humidity_percent >= alert_humidity_tenths / 10.0f,
                isfinite(pm25) && alert_pm25_tenths != DISABLED_U16_ALERT && pm25 >= alert_pm25_tenths / 10.0f,
                readings.ens_ok && alert_eco2_ppm != DISABLED_U32_ALERT && readings.eco2_ppm >= alert_eco2_ppm,
                readings.ens_ok && alert_tvoc_ppb != DISABLED_U32_ALERT && readings.tvoc_ppb >= alert_tvoc_ppb,
                readings.ens_ok && alert_aqi_tenths != DISABLED_U16_ALERT && readings.aqi >= alert_aqi_tenths / 10.0f};
            native_display_show_readings(
                readings.temperature_c,
                readings.humidity_percent,
                readings.eco2_ppm,
                readings.tvoc_ppb,
                readings.aqi,
                pm25,
                show_fahrenheit,
                alert_values,
                zigbee_joined,
                zigbee_pairing,
                zigbee_data_live);

            if (readings.aht_ok)
                ESP_LOGI(TAG, "Temp=%.1f%s Hum=%.1f%%", shown_temperature,
                    show_fahrenheit ? "F" : "C", readings.humidity_percent);
            else
                ESP_LOGW(TAG, "AHT20 read failed");

            if (readings.ens_ok)
                ESP_LOGI(TAG, "eCO2=%.0fppm TVOC=%.0fppb AQI=%.0f",
                    readings.eco2_ppm, readings.tvoc_ppb, readings.aqi);
            else
            {
                const uint8_t validity = (readings.ens_status >> 2) & 0x03;
                if ((readings.ens_status & 0x80) && !(readings.ens_status & 0x40))
                    ESP_LOGW(TAG, "ENS160 output not valid yet (validity=%u status=0x%02X)",
                        (unsigned)validity, (unsigned)readings.ens_status);
                else
                    ESP_LOGW(TAG, "ENS160 read/mode error (status=0x%02X)", (unsigned)readings.ens_status);
            }

            ESP_LOGI(TAG, "Local sensor values refreshed; Hubitat reporting interval=%u seconds",
                display_update_interval_seconds);
            next_display_update = now + pdMS_TO_TICKS((uint32_t)display_update_interval_seconds * 1000U);
            display_settings_changed = false;
        }

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
    load_display_settings();
    ESP_LOGI(TAG, "Loaded display settings: interval=%u seconds, temperature=%s",
        display_update_interval_seconds, display_temperature_fahrenheit ? "F" : "C");

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};

    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_LOGI(TAG, "Reset button configured on GPIO%d (active low)", PIN_BUTTON);

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
    
    BaseType_t sensor_task_result = xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        nullptr,
        5,
        nullptr);

    BaseType_t zigbee_task_result = xTaskCreate(
        zigbee_task,
        "zigbee_task",
        8192,
        nullptr,
        20,
        nullptr);

    BaseType_t button_task_result = xTaskCreate(
        button_and_pairing_task,
        "button_pairing",
        4096,
        nullptr,
        5,
        nullptr);

    ESP_LOGI(TAG, "Task startup: sensors=%s zigbee=%s button=%s",
        sensor_task_result == pdPASS ? "ok" : "FAILED",
        zigbee_task_result == pdPASS ? "ok" : "FAILED",
        button_task_result == pdPASS ? "ok" : "FAILED");

    ESP_LOGI(
        TAG,
        "ESP32-C6 air-quality monitor started");
}
