#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "Battery";

// GPIO 2 is ADC1 Channel 2
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_ADC_UNIT    ADC_UNIT_1

static adc_oneshot_unit_handle_t adc1_handle;

void battery_init(void) {
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12, // 12dB for up to ~2.5V-3.1V input
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config));
    ESP_LOGI(TAG, "Battery ADC initialized on GPIO 2");
}

int battery_get_voltage_mv(void) {
    int adc_raw;
    if (adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &adc_raw) == ESP_OK) {
        // Rough conversion:
        // 12dB attenuation range is approx 0 to 2500mV (on C3 it might be slightly different but ~2500 is safe bet for calculation)
        // Let's assume 2500mV max for 4095 counts.
        // Voltage at pin = raw * 2500 / 4095
        // Battery voltage = pin_voltage * 2 (divider)
        // So: raw * 5000 / 4095
        return (adc_raw * 5000) / 4095;
    }
    return 0;
}

int battery_get_percentage(void) {
    int mv = battery_get_voltage_mv();
    // Simple linear map 3.0V (0%) to 4.2V (100%)
    if (mv < 2000) return 0;
    if (mv > 3000) return 100;
    return (mv - 2000) * 100 / 1000;
}
