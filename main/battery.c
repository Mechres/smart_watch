#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "Battery";

// GPIO 4 is ADC1 Channel 4
#define BATTERY_GPIO        GPIO_NUM_4
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_4
#define BATTERY_ADC_UNIT    ADC_UNIT_1
#define BATTERY_ATTEN       ADC_ATTEN_DB_12

// Voltage divider: 100k/100k = ratio of 2.0
#define VOLTAGE_DIVIDER_RATIO 2.0f

// ESP32-C3 with 12dB attenuation: ~0-2500mV measurable range
#define ADC_MAX_VOLTAGE_MV 2500

// Li-ion voltage range
#define BATTERY_MIN_MV 3000
#define BATTERY_MAX_MV 4200

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;

void battery_init(void) {
    // Configure GPIO4 as input with no pull resistors BEFORE ADC init
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BATTERY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BATTERY_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config));
    
    // Initialize ADC calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .atten = BATTERY_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration initialized");
    } else {
        ESP_LOGW(TAG, "ADC calibration failed, using raw values");
        adc1_cali_handle = NULL;
    }
    
    ESP_LOGI(TAG, "Battery ADC initialized on GPIO4 (100k/100k divider, ratio=2.0)");
}

int battery_get_voltage_mv(void) {
    int adc_raw;
    if (adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &adc_raw) == ESP_OK) {
        int gpio_voltage_mv;
        
        // Use calibrated reading if available, otherwise use raw conversion
        if (adc1_cali_handle != NULL) {
            adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &gpio_voltage_mv);
        } else {
            // Fallback: ESP32-C3 with 12dB attenuation: approx 0-2500mV range
            gpio_voltage_mv = (adc_raw * ADC_MAX_VOLTAGE_MV) / 4095;
        }
        
        // Apply voltage divider ratio to get actual battery voltage
        // With 100k/100k resistors, the ratio is exactly 2.0
        // 4.2V Battery -> 2.1V at GPIO4 (within 2.5V limit)
        int battery_voltage_mv = (int)(gpio_voltage_mv * VOLTAGE_DIVIDER_RATIO);
        
        // Saturation check
        if (adc_raw >= 4090) {  // Leave small margin
            ESP_LOGW(TAG, "ADC near saturation! Raw: %d, GPIO4: %d mV", adc_raw, gpio_voltage_mv);
            ESP_LOGW(TAG, "Maximum measurable battery voltage: ~5000mV with current divider");
        }
        
        // ESP_LOGI(TAG, "ADC raw: %d, GPIO4: %d mV, Battery: %d mV", 
        //          adc_raw, gpio_voltage_mv, battery_voltage_mv);
        return battery_voltage_mv;
    }
    return 0;
}

int battery_get_percentage(void) {
    int mv = battery_get_voltage_mv();
    
    // Li-ion battery voltage range: 3.0V (0%) to 4.2V (100%)
    if (mv <= BATTERY_MIN_MV) return 0;
    if (mv >= BATTERY_MAX_MV) return 100;
    
    // Linear interpolation between min and max
    int percentage = ((mv - BATTERY_MIN_MV) * 100) / (BATTERY_MAX_MV - BATTERY_MIN_MV);
    
    return percentage;
}
