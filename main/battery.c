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

// Calibrated ratio based on user report: 3.89V actual / 3.20V read (with 1.66 ratio)
// New ratio = 1.66 * (3.89 / 3.20) = ~2.0179
#define VOLTAGE_DIVIDER_RATIO 2.02f

// ESP32-C3 with 12dB attenuation: ~0-2500mV measurable range
#define ADC_MAX_VOLTAGE_MV 2500

// Li-ion voltage range
#define BATTERY_MIN_MV 3000
#define BATTERY_MAX_MV 4200

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;
static float s_smoothed_voltage = 0.0f;

// LiPo discharge curve lookup table (Voltage -> Percentage)
typedef struct {
    int16_t voltage;
    uint8_t percentage;
} battery_curve_t;

static const battery_curve_t lipo_curve[] = {
    {4200, 100},
    {4100, 90},
    {4000, 80},
    {3900, 70},
    {3800, 60},
    {3700, 50},
    {3600, 30},
    {3500, 15},
    {3400, 5},
    {3300, 0}
};

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
    
    ESP_LOGI(TAG, "Battery ADC initialized on GPIO4 (Ratio=1.66)");
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
        int raw_battery_mv = (int)(gpio_voltage_mv * VOLTAGE_DIVIDER_RATIO);
        
        // Apply EMA smoothing
        // If first reading (0.0f), initialize immediately
        if (s_smoothed_voltage < 1.0f) {
            s_smoothed_voltage = (float)raw_battery_mv;
        } else {
            // Alpha = 0.1 (Heavy smoothing: 10% new, 90% old)
            s_smoothed_voltage = (s_smoothed_voltage * 0.9f) + ((float)raw_battery_mv * 0.1f);
        }
        
        return (int)s_smoothed_voltage;
    }
    return 0;
}

int battery_get_percentage(void) {
    int mv = battery_get_voltage_mv();
    
    if (mv >= lipo_curve[0].voltage) return 100;
    if (mv <= lipo_curve[9].voltage) return 0;

    for (int i = 0; i < 9; i++) {
        if (mv <= lipo_curve[i].voltage && mv > lipo_curve[i+1].voltage) {
            // Linear interpolation between points
            int v_high = lipo_curve[i].voltage;
            int v_low = lipo_curve[i+1].voltage;
            int p_high = lipo_curve[i].percentage;
            int p_low = lipo_curve[i+1].percentage;
            
            return p_low + ((mv - v_low) * (p_high - p_low)) / (v_high - v_low);
        }
    }
    
    return 0;
}
