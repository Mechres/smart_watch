// power.c - power management: light/deep sleep with smart polling

#include "power.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "wifi_manager.h"

static const char *TAG = "power";

static power_mode_t current_mode = POWER_ACTIVE;

/* Inactivity thresholds (seconds) */
#define LIGHT_SLEEP_AFTER_S    30   /* Light sleep after 30s idle (I2C on, poll 500ms) */
#define DEEP_SLEEP_AFTER_S     1800 /* Deep sleep after 30 min idle (I2C off, timer wakeup) */

power_mode_t power_get_mode(void) {
    return current_mode;
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
        
        // WiFi power is now handled on-demand by weather task
        // if (new_mode == POWER_ACTIVE && current_mode != POWER_ACTIVE) {
        //     wifi_start();
        // } else if (new_mode != POWER_ACTIVE && current_mode == POWER_ACTIVE) {
        //     wifi_stop();
        // }

        current_mode = new_mode;
        
        if (current_mode == POWER_DEEP_SLEEP) {
            ESP_LOGI(TAG, "Entering deep sleep...");
            // Enable wakeup on GPIO 1 (High level)
            // Note: ADXL345 INT pin active high/low depends on config. Default is active HIGH.
            esp_deep_sleep_enable_gpio_wakeup(BIT(1), ESP_GPIO_WAKEUP_GPIO_HIGH);
            esp_deep_sleep_start();
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
            return 5000;  /* Deep sleep: minimal polling before actual deep sleep triggered */
        default:
            return 100;
    }
}
