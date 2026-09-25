#include "power.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "ble_manager.h"
#include "display.h"
#include "sensors.h"
#include "pedometer.h"

static const char *TAG = "power";

static power_mode_t current_mode = POWER_ACTIVE;

/* Inactivity thresholds (seconds) */
#define LIGHT_SLEEP_AFTER_S    30   /* Light sleep after 30s idle (I2C on, poll 500ms) */
#define DEEP_SLEEP_AFTER_S     1800 /* Deep sleep after 30 min idle (I2C off, timer wakeup) */

power_mode_t power_get_mode(void) {
    return current_mode;
}

void power_enter_deep_sleep(void) {
    ESP_LOGI(TAG, "Entering deep sleep: turning off display and configuring wakeup...");
    
    // 1. Turn off OLED display to prevent battery drain and burn-in
    sh1106_display_off();

    // 2. Stop radios so they don't stay on across sleep
    wifi_stop();
    ble_manager_stop_adv();
    
    // 3. Flush pending step count to NVS (steps since last periodic save)
    pedometer_save();

    // 3. Clear any lingering tap interrupt on ADXL345
    sensors_clear_tap_interrupt();
    
    // 4. Enable wakeup on GPIO 1 (ADXL345 tap, active LOW) and GPIO 5 (OK Button, active LOW)
    // Note: timer wakeup intentionally not enabled; wake sources are tap + OK button.
    uint64_t wake_mask = (1ULL << 1) | (1ULL << 5);
    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(wake_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable deep sleep GPIO wakeup: %s", esp_err_to_name(err));
    }
    
    // 5. Enter deep sleep
    esp_deep_sleep_start();
}

void power_update_mode(uint32_t inactivity_secs) {
    power_mode_t new_mode = POWER_ACTIVE;
    
    if (inactivity_secs > DEEP_SLEEP_AFTER_S) {
        new_mode = POWER_DEEP_SLEEP;
    } else if (inactivity_secs > LIGHT_SLEEP_AFTER_S) {
        new_mode = POWER_LIGHT_SLEEP;
    } else {
        new_mode = POWER_ACTIVE;
    }
    
    if (new_mode != current_mode) {
        ESP_LOGI(TAG, "Transitioning from mode %d to %d (inactivity=%u s)", current_mode, new_mode, inactivity_secs);

        current_mode = new_mode;
        
        if (current_mode == POWER_DEEP_SLEEP) {
            power_enter_deep_sleep();
        }
    }
}

uint32_t power_get_poll_interval_ms(void) {
    switch (current_mode) {
        case POWER_ACTIVE:
            return 100;   /* Normal: 100ms, responsive motion detection */
        case POWER_LIGHT_SLEEP:
            return 500;   /* Light sleep: 500ms (I2C stays on for motion detection) */
        case POWER_DEEP_SLEEP:
            return 100;   /* Unreachable: power_update_mode() enters deep sleep immediately */
        default:
            return 100;
    }
}
