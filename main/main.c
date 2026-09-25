#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#include "power.h"
#include "input.h"
#include "sensors.h"
#include "gesture.h"
#include "wifi_manager.h"
#include "menu.h"
#include "battery.h"
#include "pedometer.h"
#include "weather.h"
#include "ble_manager.h"
#include "ota_updater.h"

static const char *TAG = "SmartWatch";

/* ---------- I2C / pins ---------- */
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SDA_IO           8   // ESP32-C3 SuperMini SDA
#define I2C_MASTER_SCL_IO           9   // ESP32-C3 SuperMini SCL
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0

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

/* Motion and gesture-based screen control */
static bool screen_on = true;
static bool display_available = true;
static int32_t last_motion_time_s = 0;
static uint32_t s_ble_dropped_events = 0;

/* ---------- BLE event queue (NimBLE task → main_task) ----------
 * BLE callbacks run on the NimBLE host task. Shared UI state (screen_on,
 * menu, weather) must only be touched from main_task, so callbacks enqueue
 * copies of the payloads and main_task drains them each loop. */
typedef enum {
    BLE_EVT_NOTIFICATION = 0,
    BLE_EVT_COMMAND,
} ble_evt_type_t;

typedef struct {
    ble_evt_type_t type;
    char a[256]; /* notification title or command string (BLE_CMD_MAX_LEN) */
    char b[160]; /* notification body (unused for commands) */
} ble_evt_t;

static QueueHandle_t s_ble_evt_queue = NULL;

static bool ble_evt_enqueue(ble_evt_type_t type, const char *a, const char *b) {
    if (!s_ble_evt_queue) return false;
    ble_evt_t evt = {0};
    evt.type = type;
    snprintf(evt.a, sizeof(evt.a), "%s", a ? a : "");
    if (b) {
        snprintf(evt.b, sizeof(evt.b), "%s", b);
    }
    return xQueueSend(s_ble_evt_queue, &evt, 0) == pdTRUE;
}

/* Update screen state based on wrist tilt gesture, viewing position, and timeout */
static void update_screen_state(int32_t current_time_s) {
    bool in_menu = !menu_is_watch_mode();
    static bool was_viewing = false;
    bool viewing = gesture_is_in_viewing_position();

    /* Debounced viewing-edge timeout reset: only when the user deliberately
     * raises the watch into view after it was NOT in view (gesture.c debounces
     * s_is_currently_viewing, so desk/typing jitter does not re-arm the timer). */
    if (screen_on && viewing && !was_viewing) {
        last_motion_time_s = current_time_s;
    }
    was_viewing = viewing;

    if (!screen_on) {
        // Screen is OFF: check for wrist-tilt raise-to-wake gesture
        if (gesture_has_raised_to_wake()) {
            ESP_LOGD(TAG, "Wrist raised to face - turning screen ON");
            sh1106_display_on();
            screen_on = true;
            gesture_notify_screen_state(true);
            last_motion_time_s = current_time_s;
        }
    } else {
        if (!in_menu) {
            // In watch mode: check for lower-to-sleep gesture or timeout
            if (gesture_should_lower_to_sleep()) {
                ESP_LOGD(TAG, "Wrist lowered - turning screen OFF immediately");
                sh1106_display_off();
                screen_on = false;
                gesture_notify_screen_state(false);
            } else if ((current_time_s - last_motion_time_s) >= menu_get_screen_timeout()) {
                ESP_LOGD(TAG, "Screen timeout (%d s) - turning screen OFF", menu_get_screen_timeout());
                sh1106_display_off();
                screen_on = false;
                gesture_notify_screen_state(false);
            }
        }
    }
}

/* NimBLE host-task callbacks: copy payload and hand off to main_task */
static void handle_ble_notification(const char *title, const char *body) {
    if (!ble_evt_enqueue(BLE_EVT_NOTIFICATION, title, body)) {
        s_ble_dropped_events++;
        ESP_LOGW(TAG, "BLE notification dropped (queue full, total dropped=%u)", s_ble_dropped_events);
    }
}

static void handle_ble_command(const char *command) {
    if (!command) return;
    if (!ble_evt_enqueue(BLE_EVT_COMMAND, command, NULL)) {
        s_ble_dropped_events++;
        ESP_LOGW(TAG, "BLE command dropped (queue full, total dropped=%u)", s_ble_dropped_events);
    }
}

/* Process a notification on main_task: show it and wake the screen */
static void process_ble_notification(const char *title, const char *body) {
    ESP_LOGI(TAG, "BLE notification: %s | %s", title ? title : "", body ? body : "");

    menu_show_notification(title, body);

    int32_t now_s = (int32_t)(esp_timer_get_time() / 1000000);
    last_motion_time_s = now_s;
    if (!screen_on) {
        sh1106_display_on();
        screen_on = true;
        gesture_notify_screen_state(true);
    }
}

/* Process a control command on main_task */
static void process_ble_command(const char *command) {
    if (!command || !command[0]) return;

    if (strcmp(command, "wifi_on") == 0) {
        wifi_start();
    } else if (strcmp(command, "wifi_off") == 0) {
        wifi_stop();
    } else if (strcmp(command, "screen_on") == 0) {
        sh1106_display_on();
        screen_on = true;
        gesture_notify_screen_state(true);
        // Reset inactivity timer so it doesn't immediately turn off
        last_motion_time_s = (int32_t)(esp_timer_get_time() / 1000000);
    } else if (strcmp(command, "screen_off") == 0) {
        sh1106_display_off();
        screen_on = false;
        gesture_notify_screen_state(false);
        // Force inactivity timer to expire so it can enter light sleep immediately
        // Subtracting 60s ensures we are well past the light sleep threshold (30s)
        last_motion_time_s = (int32_t)(esp_timer_get_time() / 1000000) - 60;
    } else if (strncmp(command, "time=", 5) == 0) {
        long long timestamp = atoll(command + 5);
        /* Valid range: 2020-01-01 .. 2100-01-01. Rejects garbage/negative/overflow. */
        if (timestamp >= 1577836800LL && timestamp <= 4102444800LL) {
            struct timeval tv;
            tv.tv_sec = (time_t)timestamp;
            tv.tv_usec = 0;
            if (settimeofday(&tv, NULL) == 0) {
                ESP_LOGI(TAG, "Time updated via BLE to: %lld", timestamp);
            } else {
                ESP_LOGW(TAG, "settimeofday failed for timestamp %lld", timestamp);
            }
        } else {
            ESP_LOGW(TAG, "Rejected invalid BLE timestamp: %s", command + 5);
        }
    } else if (strncmp(command, "weather=", 8) == 0) {
        // Format: weather=TEMP,CODE (e.g. weather=24.5,1)
        float temp = 0.0f;
        int code = 0;
        if (sscanf(command + 8, "%f,%d", &temp, &code) == 2 &&
            temp >= -100.0f && temp <= 100.0f && code >= 0 && code <= 99) {
            weather_set_data(temp, code);
            // Refresh screen to show new weather immediately
            if (!screen_on) {
                sh1106_display_on();
                screen_on = true;
                gesture_notify_screen_state(true);
            }
            last_motion_time_s = (int32_t)(esp_timer_get_time() / 1000000);
        } else {
            ESP_LOGW(TAG, "Invalid weather command format");
        }
    } else if (strncmp(command, "ota=", 4) == 0) {
        ota_updater_request(command + 4);
    }
    ESP_LOGI(TAG, "BLE control command handled: %s", command);
}

/* Drain pending BLE events (called from main_task) */
static void process_ble_events(void) {
    ble_evt_t evt;
    while (xQueueReceive(s_ble_evt_queue, &evt, 0) == pdTRUE) {
        switch (evt.type) {
            case BLE_EVT_NOTIFICATION:
                process_ble_notification(evt.a, evt.b);
                break;
            case BLE_EVT_COMMAND:
                process_ble_command(evt.a);
                break;
        }
    }
}

/* ---------- Main task: sensors + display + time ---------- */
static void main_task(void *arg) {
    ESP_LOGI(TAG, "main_task starting: init sensors & display");

    menu_init();

    sensors_init();
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "Battery monitor unavailable - using defaults");
    }
    pedometer_init();
    pedometer_load();
    pedometer_start_task();
    
    // Configure GPIO 1 for tap interrupt (deep-sleep wake pin)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 1),
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = 0,
        .pull_up_en = 1, // Pull up for active-low INT
    };
    esp_err_t gpio_err = gpio_config(&io_conf);
    if (gpio_err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO1 tap-wake config failed: %s", esp_err_to_name(gpio_err));
    }
    
    // Configure ADXL345 for tap wakeup
    esp_err_t tap_err = sensors_config_tap_wakeup();
    if (tap_err != ESP_OK) {
        ESP_LOGW(TAG, "Tap wakeup config failed: %s - deep-sleep wake via tap unavailable",
                 esp_err_to_name(tap_err));
    }

    if (sh1106_init() != ESP_OK) {
        ESP_LOGE(TAG, "SH1106 init failed - continuing without display");
        display_available = false;
    } else {
        ESP_LOGI(TAG, "Display ready");
    }
    
    // Initialize event-driven button input
    if (input_init() != ESP_OK) {
        ESP_LOGE(TAG, "Input module init failed - continuing without buttons");
    } else {
        // Configure buttons for wakeup from light sleep
        input_enable_wakeup();
        input_register_notify_task(xTaskGetCurrentTaskHandle());
    }

    // Initialize wrist-tilt gesture engine
    gesture_init();
    gesture_register_notify_task(xTaskGetCurrentTaskHandle());
    gesture_set_sensitivity(menu_get_motion_threshold());
    gesture_notify_screen_state(screen_on);

    // Initialize time tracking (use monotonic time for timeouts to avoid SNTP jumps)
    int64_t now_mono_us = esp_timer_get_time();
    last_motion_time_s = (int32_t)(now_mono_us / 1000000);

    // main loop: adaptive polling based on power mode
    while (1) {
        // Drain BLE events queued from the NimBLE host task
        process_ble_events();

        // get current time
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // Use monotonic time for timeouts
        int64_t current_mono_us = esp_timer_get_time();
        int32_t current_mono_s = (int32_t)(current_mono_us / 1000000);

        // Check for midnight reset (uses RTC time)
        pedometer_check_midnight(&timeinfo);

        // Calculate inactivity time and update power mode
        uint32_t inactivity_secs = (current_mono_s - last_motion_time_s);
        power_update_mode(inactivity_secs);
        uint32_t poll_interval_ms = power_get_poll_interval_ms();

        // Only read sensors in active/idle modes (skip in deep sleep to save power)
        power_mode_t mode = power_get_mode();
        // Static cached readings to prevent aggressive polling and I2C blocking
        static float cached_temp = 25.0f;
        static float cached_hum = 50.0f;
        static int32_t last_th_read_s = -100;
        static int32_t th_pending_since_s = -1;
        static int cached_batt_mv = 3800;
        static int cached_batt_pct = 50;
        static int32_t last_batt_read_s = -100;

        // Periodic battery reading: 5 s while active, 30 s otherwise
        int32_t batt_interval_s = (mode == POWER_ACTIVE && screen_on) ? 5 : 30;
        if (current_mono_s - last_batt_read_s >= batt_interval_s || last_batt_read_s < 0) {
            cached_batt_mv = battery_get_voltage_mv();
            cached_batt_pct = battery_mv_to_percentage(cached_batt_mv);
            last_batt_read_s = current_mono_s;
            ble_manager_update_battery((uint8_t)cached_batt_pct);
        }
        int batt_mv = cached_batt_mv;
        int batt_pct = cached_batt_pct;

        // Update BLE characteristics only while connected
        if (ble_manager_is_connected()) {
            ble_manager_update_steps((uint32_t)pedometer_get_steps());
        }

        int16_t ax = 0, ay = 0, az = 0;
        if (mode != POWER_DEEP_SLEEP) {
            // Non-blocking AHT10: kick a measurement every 20s, collect >=80ms later
            if (th_pending_since_s < 0 && (current_mono_s - last_th_read_s >= 20 || last_th_read_s < 0)) {
                if (sensors_start_temp_hum() == ESP_OK) {
                    th_pending_since_s = current_mono_s;
                } else {
                    last_th_read_s = current_mono_s; /* retry next interval */
                }
            } else if (th_pending_since_s >= 0 && (current_mono_s - th_pending_since_s) >= 1) {
                float t = 0.0f, h = 0.0f;
                esp_err_t th_err = sensors_poll_temp_hum(&t, &h);
                if (th_err == ESP_OK) {
                    cached_temp = t;
                    cached_hum = h;
                    th_pending_since_s = -1;
                    last_th_read_s = current_mono_s;
                } else if (th_err != ESP_ERR_INVALID_STATE) {
                    th_pending_since_s = -1;
                    last_th_read_s = current_mono_s;
                } else if ((current_mono_s - th_pending_since_s) > 2) {
                    th_pending_since_s = -1; /* timed out waiting for conversion */
                    last_th_read_s = current_mono_s;
                }
            }
            pedometer_get_latest_accel(&ax, &ay, &az);
        }
        float temp = cached_temp;
        float hum = cached_hum;

        // Update gesture sensitivity and screen state (only in active/idle modes)
        if (mode != POWER_DEEP_SLEEP) {
            gesture_set_sensitivity(menu_get_motion_threshold());
            update_screen_state(current_mono_s);
        }

        // BLE advertising housekeeping: stop after idle timeout, restart on activity
        ble_manager_housekeeping(screen_on);

        // Handle event-driven button inputs from queue
        button_event_t btn_event;
        bool button_activity = false;
        
        // Process button events from queue
        while (input_get_event(&btn_event)) {
            button_activity = true;
            last_motion_time_s = current_mono_s;
            // Wake screen if it's off
            if (!screen_on) {
            ESP_LOGD(TAG, "Button pressed - waking screen");
                sh1106_display_on();
                screen_on = true;
                gesture_notify_screen_state(true);
                // Discard any other presses buffered while screen was off
                while (input_get_event(&btn_event));
                break;
            } else {
                // Screen is already on, pass event to menu system
                menu_handle_button(btn_event, current_mono_s);
            }
        }
        
        // Check menu timeout
        menu_check_timeout(current_mono_s);

        // Smart redraw gating: only render when display content has actually changed
        static int last_rendered_min = -1;
        static int last_rendered_steps = -1;
        static int last_rendered_batt = -1;
        static int last_rendered_temp = -100;
        static bool last_screen_on = false;
        static int64_t last_fast_render_us = 0;
        int min_refresh_ms = menu_get_min_refresh_ms();

        if (screen_on) {
            bool needs_render = false;

            if (!display_available) {
                needs_render = false;
            } else if (!last_screen_on) {
                // Screen just turned on: force render immediately
                needs_render = true;
            } else if (!menu_is_watch_mode()) {
                // In menu: render on button events or throttled continuous refresh (stopwatch)
                if (button_activity) {
                    needs_render = true;
                } else if (min_refresh_ms > 0 &&
                           (current_mono_us - last_fast_render_us) >= (int64_t)min_refresh_ms * 1000) {
                    needs_render = true;
                }
            } else if (min_refresh_ms > 0) {
                // Animated watchface: continuous but throttled refresh
                if ((current_mono_us - last_fast_render_us) >= (int64_t)min_refresh_ms * 1000) {
                    needs_render = true;
                }
            } else {
                // Static watchface: render on minute change, step change, battery change, or button press
                int cur_steps = pedometer_get_steps();
                if (timeinfo.tm_min != last_rendered_min ||
                    cur_steps != last_rendered_steps ||
                    batt_pct != last_rendered_batt ||
                    (int)temp != last_rendered_temp ||
                    button_activity) {
                    needs_render = true;
                }
            }

            if (needs_render) {
                menu_render(temp, hum, ax, ay, az, batt_mv, batt_pct, &timeinfo);
                if (sh1106_render() != ESP_OK) {
                    ESP_LOGW(TAG, "render failed");
                }

                last_rendered_min = timeinfo.tm_min;
                last_rendered_steps = pedometer_get_steps();
                last_rendered_batt = batt_pct;
                last_rendered_temp = (int)temp;
                last_fast_render_us = current_mono_us;
            }
        }
        last_screen_on = screen_on;

        // Periodic Save (e.g. every 30 minutes = 1800 seconds)
        static int32_t last_save_s = 0;
        if ((current_mono_s - last_save_s) > 1800) {
             pedometer_save();
             last_save_s = current_mono_s;
        }

        // Sleep for adaptive interval based on power mode, or wake immediately on button press
        // FreeRTOS automatic light sleep (esp_pm_configure) handles power saving during delay
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_interval_ms));
    }
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(erase_err));
            return;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s - aborting", esp_err_to_name(ret));
        return;
    }

    // Set timezone to UTC+3 (Istanbul)
    setenv("TZ", "TRT-3", 1);
    tzset();

    // Queue for BLE → main_task handoff (must exist before ble_manager_init)
    s_ble_evt_queue = xQueueCreate(8, sizeof(ble_evt_t));
    if (!s_ble_evt_queue) {
        ESP_LOGE(TAG, "Failed to create BLE event queue");
        return;
    }

    ESP_LOGI(TAG, "Configuring Power Management");
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true
    };
    ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s - continuing without PM", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Init I2C");
    if (i2c_master_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return;
    }

    // Quick probe of known devices only (full 127-addr scan cost >= 380 ms on
    // every deep-sleep wake; known buses are checked during their init instead)
    ESP_LOGD(TAG, "I2C probe: display=0x%02X accel=0x%02X env=0x%02X",
             0x3C, 0x53, 0x38);

    // WiFi is lazily initialized on first wifi_start() (e.g. Time Sync menu)
    ESP_LOGI(TAG, "Init BLE");
    if (ble_manager_init(handle_ble_notification, handle_ble_command) != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed");
    }

    // create main task
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}