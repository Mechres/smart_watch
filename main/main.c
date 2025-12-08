#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_pm.h"

#include "display.h"
#include "watchfaces.h"
#include "settings.h"
#include "power.h"
#include "input.h"
#include "sensors.h"
#include "wifi_manager.h"
#include "menu.h"
#include "battery.h"
#include "pedometer.h"
#include "weather.h"
#include "ble_manager.h"

static const char *TAG = "SmartWatch";

/* ---------- I2C / pins ---------- */
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SDA_IO           8   // ESP32-C3 SuperMini SDA
#define I2C_MASTER_SCL_IO           9   // ESP32-C3 SuperMini SCL
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_TIMEOUT_MS              1000

/* ---------- low-level I2C helpers ---------- */
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = I2C_MASTER_FREQ_HZ }
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

/* probe address (start + write address only) */
static esp_err_t i2c_probe_addr(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr<<1) | I2C_MASTER_WRITE, 0x1);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* Motion-based screen control */
static bool screen_on = true;
static int32_t last_motion_time_s = 0;
static int16_t last_ax = 0, last_ay = 0, last_az = 0;  // for delta calculation

/* Detect motion from accelerometer readings */
static bool detect_motion(int16_t ax, int16_t ay, int16_t az) {
    // Calculate delta (change) in acceleration from last reading
    int16_t dx = ax - last_ax;
    int16_t dy = ay - last_ay;
    int16_t dz = az - last_az;
    
    // Store current values for next comparison
    last_ax = ax;
    last_ay = ay;
    last_az = az;
    
    // Calculate magnitude of acceleration delta
    int32_t delta_mag_sq = (int32_t)dx*dx + (int32_t)dy*dy + (int32_t)dz*dz;
    
    // Get threshold from settings
    int32_t threshold = menu_get_motion_threshold();
    int32_t threshold_sq = threshold * threshold; 
    
    return delta_mag_sq > threshold_sq;
}

/* Update screen state based on motion and timeout */
static void update_screen_state(bool motion_detected, int32_t current_time_s) {
    // Don't auto-timeout screen if user is navigating menus
    bool in_menu = !menu_is_watch_mode();
    
    if (motion_detected) {
        last_motion_time_s = current_time_s;
        if (!screen_on) {
            ESP_LOGI(TAG, "Motion detected - turning screen ON");
            sh1106_display_on();
            screen_on = true;
        }
    } else if (screen_on && !in_menu && (current_time_s - last_motion_time_s) >= menu_get_screen_timeout()) {
        // Only auto-off in watch mode after timeout, not in menu
        ESP_LOGI(TAG, "No motion for %d seconds - turning screen OFF", menu_get_screen_timeout());
        sh1106_display_off();
        screen_on = false;
    }
}

static void handle_ble_notification(const char *title, const char *body) {
    ESP_LOGI(TAG, "BLE notification: %s | %s", title ? title : "", body ? body : "");

    // Show notification on screen
    menu_show_notification(title, body);

    // Refresh screen and activity timer so alerts are visible
    int32_t now_s = (int32_t)(esp_timer_get_time() / 1000000);
    last_motion_time_s = now_s;
    if (!screen_on) {
        sh1106_display_on();
        screen_on = true;
    }
}

static void handle_ble_command(const char *command) {
    if (!command) return;

    if (strcmp(command, "wifi_on") == 0) {
        wifi_start();
    } else if (strcmp(command, "wifi_off") == 0) {
        wifi_stop();
    } else if (strcmp(command, "screen_on") == 0) {
        sh1106_display_on();
        screen_on = true;
        // Reset inactivity timer so it doesn't immediately turn off
        last_motion_time_s = (int32_t)(esp_timer_get_time() / 1000000);
    } else if (strcmp(command, "screen_off") == 0) {
        sh1106_display_off();
        screen_on = false;
        // Force inactivity timer to expire so it can enter light sleep immediately
        // Subtracting 60s ensures we are well past the light sleep threshold (30s)
        last_motion_time_s = (int32_t)(esp_timer_get_time() / 1000000) - 60;
    } else if (strncmp(command, "time=", 5) == 0) {
        long long timestamp = atoll(command + 5);
        if (timestamp > 0) {
            struct timeval tv;
            tv.tv_sec = (time_t)timestamp;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Time updated via BLE to: %lld", timestamp);
        }
    } else if (strncmp(command, "weather=", 8) == 0) {
        // Format: weather=TEMP,CODE (e.g. weather=24.5,1)
        float temp = 0.0f;
        int code = 0;
        if (sscanf(command + 8, "%f,%d", &temp, &code) == 2) {
            weather_set_data(temp, code);
            // Refresh screen to show new weather immediately
            if (!screen_on) {
                sh1106_display_on();
                screen_on = true;
            }
            last_motion_time_s = (int32_t)(esp_timer_get_time() / 1000000);
        } else {
            ESP_LOGW(TAG, "Invalid weather command format");
        }
    }
    ESP_LOGI(TAG, "BLE control command handled: %s", command);
}

/* ---------- Main task: sensors + display + time ---------- */
static void main_task(void *arg) {
    ESP_LOGI(TAG, "main_task starting: init sensors & display");

    // Initialize settings from NVS (also done in menu_init, but good to ensure)
    settings_init();
    menu_init();

    sensors_init();
    battery_init();
    pedometer_init();
    
    // Configure GPIO 1 for tap interrupt
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 1);
    io_conf.pull_down_en = 1; // Pull down if INT is active high
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    // Configure ADXL345 for tap wakeup
    sensors_config_tap_wakeup();

    if (sh1106_init() != ESP_OK) {
        ESP_LOGE(TAG, "SH1106 init failed - aborting");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Display ready");
    
    // Initialize event-driven button input
    if (input_init() != ESP_OK) {
        ESP_LOGE(TAG, "Input module init failed - continuing without buttons");
    } else {
        // Configure buttons for wakeup from light sleep
        input_enable_wakeup();
    }

    // Initialize time tracking (use monotonic time for timeouts to avoid SNTP jumps)
    int64_t now_mono_us = esp_timer_get_time();
    last_motion_time_s = (int32_t)(now_mono_us / 1000000);

    // main loop: adaptive polling based on power mode
    while (1) {
        // get current time
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // Use monotonic time for timeouts
        int64_t current_mono_us = esp_timer_get_time();
        int32_t current_mono_s = (int32_t)(current_mono_us / 1000000);

        // Calculate inactivity time and update power mode
        uint32_t inactivity_secs = (current_mono_s - last_motion_time_s);
        power_update_mode(inactivity_secs);
        uint32_t poll_interval_ms = power_get_poll_interval_ms();

        // Only read sensors in active/idle modes (skip in deep sleep to save power)
        power_mode_t mode = power_get_mode();
        float temp=0.0f, hum=0.0f;
        int16_t ax=0, ay=0, az=0;
        int batt_mv = 0;
        int batt_pct = 0;
        
        // Always read battery (low overhead)
        batt_mv = battery_get_voltage_mv();
        batt_pct = battery_get_percentage();
        
        // Update BLE characteristics
        ble_manager_update_battery((uint8_t)batt_pct);
        ble_manager_update_steps((uint32_t)pedometer_get_steps());
        
        if (mode != POWER_DEEP_SLEEP) {
            // Read sensors normally in active/idle/light-sleep modes
            sensors_read_temp_hum(&temp, &hum);
            sensors_read_accel(&ax, &ay, &az);
            pedometer_process(ax, ay, az);
        } else {
            // In deep sleep, only read motion if motion was detected (via ISR in future)
            // For now, just skip sensor reads to conserve power
            ax=ay=az=0;
            temp=hum=0.0f;
        }

        // Detect motion and update screen state (only in active/idle modes)
        bool motion_detected = false;
        if (mode != POWER_DEEP_SLEEP) {
            motion_detected = detect_motion(ax, ay, az);
        }
        update_screen_state(motion_detected, current_mono_s);

        // Handle event-driven button inputs from queue
        button_event_t btn_event;
        
        // Check if any button event is available (non-blocking)
        if (input_get_event(&btn_event)) {
            // Wake screen if it's off
            if (!screen_on) {
                ESP_LOGI(TAG, "Button pressed - waking screen");
                sh1106_display_on();
                screen_on = true;
                // Update activity time so it doesn't immediately sleep
                last_motion_time_s = current_mono_s;
                // Consume the event (do NOT pass to menu) so the first press only wakes the screen
            } else {
                // Screen is already on, pass event to menu system
                
                // Update both motion and menu activity times to keep screen on
                last_motion_time_s = current_mono_s;
                
                menu_handle_button(btn_event, current_mono_s);
            }
        }
        
        // Check menu timeout
        menu_check_timeout(current_mono_s);

        // Only render if screen is on
        if (screen_on) {
            menu_render(temp, hum, ax, ay, az, batt_mv, batt_pct, &timeinfo);

            // render
            if (sh1106_render() != ESP_OK) {
                ESP_LOGW(TAG, "render failed");
            }
        }

        // Sleep for adaptive interval based on power mode (saves battery)
        if (mode == POWER_LIGHT_SLEEP && ble_manager_is_active()) {
            // Avoid light sleep while BLE is connected/advertising to keep the link alive
            ESP_LOGD(TAG, "Skipping light sleep while BLE is active");
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        } else if (mode == POWER_LIGHT_SLEEP) {
            // In light sleep, use esp_light_sleep_start instead of vTaskDelay
            // This stops the CPU but keeps RAM and peripherals (like I2C/GPIO) active
            esp_sleep_enable_timer_wakeup(poll_interval_ms * 1000);
            esp_light_sleep_start();
            
            // Check wakeup cause
            esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
            if (cause == ESP_SLEEP_WAKEUP_GPIO) {
                ESP_LOGI(TAG, "Woke up from GPIO (Button)");
                // Yield to allow debounce task to run immediately
                taskYIELD();
            }
        } else {
            // Active or Deep Sleep (waiting to enter) -> use standard delay
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        }
    }
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    srand(time(NULL));

    // Set timezone to UTC+3 (Istanbul)
    setenv("TZ", "TRT-3", 1);
    tzset();

    ESP_LOGI(TAG, "Configuring Power Management");
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    ESP_LOGI(TAG, "Init I2C");
    if (i2c_master_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return;
    }

    // quick scan
    int found = 0;
    for (uint8_t a=1; a<127; ++a) {
        if (i2c_probe_addr(a) == ESP_OK) {
            ESP_LOGI(TAG, "Device at 0x%02X", a);
            ++found;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    ESP_LOGI(TAG, "I2C devices found: %d", found);

    // WiFi init
    ESP_LOGI(TAG, "Init WiFi");
    wifi_init_sta();
    wifi_stop(); // Ensure WiFi is off by default to save power

    ESP_LOGI(TAG, "Init BLE");
    if (ble_manager_init(handle_ble_notification, handle_ble_command) != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed");
    }

    // create main task
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}