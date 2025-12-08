#include "menu.h"
#include "display.h"
#include "watchfaces.h"
#include "settings.h"
#include "esp_log.h"
#include <stdio.h>
#include <sys/time.h>
#include "pedometer.h"
#include "weather.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "wifi_manager.h"
#include "ble_manager.h"

static const char *TAG = "Menu";

/* Menu states */
typedef enum {
    MENU_WATCH,           // Main watch display
    MENU_ROOT,            // Top-level category menu
    MENU_SETTINGS,        // Settings menu
    MENU_SENSOR_DATA,     // Detailed sensor readings
    MENU_WATCHFACE,       // Watchface selection
    MENU_WEATHER,         // Weather display
    MENU_STOPWATCH,       // Stopwatch feature
    MENU_SYSTEM_INFO,     // System Info
    MENU_FLASHLIGHT,      // Flashlight
    MENU_NOTIFICATION,    // Notification display
    MENU_SYNC_WAIT,       // Waiting for WiFi Sync
    MENU_FIND_PHONE,      // Find My Phone
    MENU_MUSIC_CONTROL,   // Music Control
    MENU_COUNT
} menu_mode_t;

typedef enum {
    WATCHFACE_DIGITAL = 0,
    WATCHFACE_ANALOG_STYLE,
    WATCHFACE_MINIMAL,
    WATCHFACE_COMPACT,
    WATCHFACE_TERMINAL,
    WATCHFACE_MATRIX,
    WATCHFACE_CATS,
    WATCHFACE_COUNT
} watchface_t;

typedef enum {
    SETTINGS_MOTION_THRESHOLD = 0,
    SETTINGS_SCREEN_TIMEOUT,
    SETTINGS_BRIGHTNESS,
    SETTINGS_TIME_SYNC,
    SETTINGS_REBOOT,
    SETTINGS_POWER_OFF,
    SETTINGS_BACK,
    SETTINGS_COUNT
} settings_item_t;

typedef enum {
    SENSOR_TEMP = 0,
    SENSOR_HUMIDITY,
    SENSOR_STEPS,
    SENSOR_ACCEL_X,
    SENSOR_ACCEL_Y,
    SENSOR_ACCEL_Z,
    SENSOR_BATTERY,
    SENSOR_BACK,
    SENSOR_COUNT
} sensor_item_t;

/* State variables */
static menu_mode_t current_menu = MENU_WATCH;
static int32_t menu_last_activity_s = 0;
static settings_item_t current_setting = SETTINGS_MOTION_THRESHOLD;
static sensor_item_t current_sensor = SENSOR_TEMP;
static int root_selection = 0; // 0 = Sensors, 1 = Weather, 2 = Settings, 3 = Watchface
static watchface_t current_watchface = WATCHFACE_DIGITAL;
static int watchface_selection = 0;
static int weather_selection = 0; // 0 = Refresh, 1 = Back
static int music_selection = 0; // 0=Play/Pause, 1=Next, 2=Prev, 3=Back
static int find_phone_selection = 0; // 0=Ring, 1=Stop, 2=Back
static bool editing_mode = false;

static uint8_t saved_brightness = 128;

/* Notification state */
static char notif_title[32];
static char notif_body[128];

/* Stopwatch state */
static bool stopwatch_running = false;
static int64_t stopwatch_start_time = 0;
static int64_t stopwatch_elapsed_time = 0;

/* Local copies of settings (loaded from settings module) */
static int16_t motion_threshold_editable = 100;
static int16_t screen_timeout_editable = 5;
static int16_t brightness_editable = 128;

void menu_init(void) {
    // Load initial settings
    settings_load(&motion_threshold_editable, &screen_timeout_editable, (int*)&current_watchface, &brightness_editable);
    // Apply loaded brightness
    sh1106_set_contrast((uint8_t)brightness_editable);
}

int16_t menu_get_motion_threshold(void) {
    return motion_threshold_editable;
}

int16_t menu_get_screen_timeout(void) {
    return screen_timeout_editable;
}

bool menu_is_watch_mode(void) {
    return (current_menu == MENU_WATCH);
}

void menu_check_timeout(int32_t current_time_s) {
    if (current_menu == MENU_NOTIFICATION) {
        if ((current_time_s - menu_last_activity_s) > 10) {
             ESP_LOGI(TAG, "Notification timeout - returning to watch");
             current_menu = MENU_WATCH;
        }
    } else if (current_menu != MENU_WATCH && (current_time_s - menu_last_activity_s) > 10) {
        ESP_LOGI(TAG, "Menu timeout - returning to watch");
        current_menu = MENU_WATCH;
        editing_mode = false;
    }
}

void menu_show_notification(const char *title, const char *body) {
    if (title) strncpy(notif_title, title, sizeof(notif_title) - 1);
    else notif_title[0] = '\0';
    
    if (body) strncpy(notif_body, body, sizeof(notif_body) - 1);
    else notif_body[0] = '\0';
    
    // Ensure null termination
    notif_title[sizeof(notif_title) - 1] = '\0';
    notif_body[sizeof(notif_body) - 1] = '\0';
    
    current_menu = MENU_NOTIFICATION;
    // Reset activity timer handled by caller (main.c updates last_motion_time_s, 
    // but we also need to update menu_last_activity_s to prevent immediate timeout)
    // We use monotonic time to match main.c's time base
    menu_last_activity_s = (int32_t)(esp_timer_get_time() / 1000000);
}

/* Render helpers */
static void draw_menu_item(int y, const char *text, bool selected) {
    if (selected) {
        fb_fill_rect(0, y, DISP_WIDTH, 10, 1);
        fb_draw_text_ex(2, y + 1, text, 0, -1);
    } else {
        fb_draw_text(2, y + 1, text);
    }
}

static void render_sensor_menu_list(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===SENSORS===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    int start_idx = current_sensor - 2;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > SENSOR_COUNT - 5) start_idx = SENSOR_COUNT - 5;
    if (start_idx < 0) start_idx = 0;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 5 && i < SENSOR_COUNT; i++) {
        bool is_selected = (i == current_sensor);
        
        switch (i) {
            case SENSOR_TEMP:
                snprintf(buf, sizeof(buf), "Temp: %.2f C", temp);
                break;
            case SENSOR_HUMIDITY:
                snprintf(buf, sizeof(buf), "Humidity: %.1f%%", hum);
                break;
            case SENSOR_STEPS:
                snprintf(buf, sizeof(buf), "Steps: %d", pedometer_get_steps());
                break;
            case SENSOR_ACCEL_X:
                snprintf(buf, sizeof(buf), "AccelX: %d", ax);
                break;
            case SENSOR_ACCEL_Y:
                snprintf(buf, sizeof(buf), "AccelY: %d", ay);
                break;
            case SENSOR_ACCEL_Z:
                snprintf(buf, sizeof(buf), "AccelZ: %d", az);
                break;
            case SENSOR_BATTERY:
                snprintf(buf, sizeof(buf), "Batt: %d%% %dmV", batt_pct, batt_mv);
                break;
            case SENSOR_BACK:
                snprintf(buf, sizeof(buf), "[Back]");
                break;
        }
        
        draw_menu_item(y_pos, buf, is_selected);
        y_pos += 10;
    }
    
    // Scroll indicators
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 5 < SENSOR_COUNT) fb_draw_text(120, 54, "v");
}

static void render_settings_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===SETTINGS===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    // Motion Threshold
    if (current_setting == SETTINGS_MOTION_THRESHOLD && editing_mode) {
        snprintf(buf, sizeof(buf), "Motion: [%d]", motion_threshold_editable);
        fb_fill_rect(0, 12, DISP_WIDTH, 10, 1);
        fb_draw_text_ex(2, 13, buf, 0, -1);
        fb_draw_text(100, 13, "<>");
    } else {
        snprintf(buf, sizeof(buf), "Motion: %d", motion_threshold_editable);
        draw_menu_item(12, buf, current_setting == SETTINGS_MOTION_THRESHOLD);
    }
    
    // Screen Timeout
    if (current_setting == SETTINGS_SCREEN_TIMEOUT && editing_mode) {
        snprintf(buf, sizeof(buf), "Timeout: [%d]s", screen_timeout_editable);
        fb_fill_rect(0, 24, DISP_WIDTH, 10, 1);
        fb_draw_text_ex(2, 25, buf, 0, -1);
        fb_draw_text(100, 25, "<>");
    } else {
        snprintf(buf, sizeof(buf), "Timeout: %d s", screen_timeout_editable);
        draw_menu_item(24, buf, current_setting == SETTINGS_SCREEN_TIMEOUT);
    }

    // Brightness
    if (current_setting == SETTINGS_BRIGHTNESS && editing_mode) {
        snprintf(buf, sizeof(buf), "Bright: [%d]", brightness_editable);
        fb_fill_rect(0, 36, DISP_WIDTH, 10, 1);
        fb_draw_text_ex(2, 37, buf, 0, -1);
        fb_draw_text(100, 37, "<>");
    } else {
        snprintf(buf, sizeof(buf), "Bright: %d", brightness_editable);
        draw_menu_item(36, buf, current_setting == SETTINGS_BRIGHTNESS);
    }

    // Time Sync
    draw_menu_item(48, "Time Sync", current_setting == SETTINGS_TIME_SYNC);

    // Reboot
    draw_menu_item(60, "Reboot", current_setting == SETTINGS_REBOOT);

    // Power Off
    draw_menu_item(72, "Power Off", current_setting == SETTINGS_POWER_OFF);

    // Back
    draw_menu_item(84, "[Back]", current_setting == SETTINGS_BACK);
}

static void render_weather_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===WEATHER===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    if (weather_is_fetching()) {
        fb_draw_text(0, 20, "Loading...");
    } else {
        weather_data_t w = weather_get_current();
        if (w.valid) {
            snprintf(buf, sizeof(buf), "Temp: %.1f C", w.temp_c);
            fb_draw_text(0, 15, buf);
            
            const char *desc = weather_get_desc(w.weather_code);
            snprintf(buf, sizeof(buf), "%s", desc);
            fb_draw_text(0, 27, buf);
        } else {
            fb_draw_text(0, 15, "No Data");
            fb_draw_text(0, 27, "Connect WiFi");
        }
    }
    
    // Draw actions
    draw_menu_item(42, "Refresh", weather_selection == 0);
    draw_menu_item(53, "[Back]", weather_selection == 1);
}

static void render_stopwatch_menu(void) {
    fb_clear();
    char buf[64];

    fb_draw_text(0, 0, "===STOPWATCH===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);

    // Calculate current elapsed time
    int64_t current_elapsed = stopwatch_elapsed_time;
    if (stopwatch_running) {
        int64_t now = esp_timer_get_time();
        current_elapsed += (now - stopwatch_start_time);
    }

    // Format time: MM:SS.ms
    int total_ms = current_elapsed / 1000;
    int ms = (total_ms % 1000) / 100; // 1/10th of a second
    int total_seconds = total_ms / 1000;
    int seconds = total_seconds % 60;
    int minutes = total_seconds / 60;

    snprintf(buf, sizeof(buf), "%02d:%02d.%d", minutes, seconds, ms);
    
    // Large centered text
    int len = strlen(buf);
    int char_width = 6 * 2;
    int x = (DISP_WIDTH - (len * char_width)) / 2;
    fb_draw_text_scaled(x, 25, buf, 2);

    // Instructions
    if (stopwatch_running) {
        fb_draw_text(10, 50, "[OK] Stop");
    } else {
        fb_draw_text(10, 50, "[OK] Start");
        fb_draw_text(70, 50, "[v] Reset");
    }
}

static void render_system_info_menu(void) {
    fb_clear();
    char buf[64];

    fb_draw_text(0, 0, "===SYS INFO===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);

    // Uptime
    int64_t uptime_us = esp_timer_get_time();
    int uptime_s = uptime_us / 1000000;
    int h = uptime_s / 3600;
    int m = (uptime_s % 3600) / 60;
    int s = uptime_s % 60;
    snprintf(buf, sizeof(buf), "Up: %02d:%02d:%02d", h, m, s);
    fb_draw_text(0, 12, buf);

    // Heap
    uint32_t free_heap = esp_get_free_heap_size();
    snprintf(buf, sizeof(buf), "Heap: %lu B", free_heap);
    fb_draw_text(0, 22, buf);

    // IP
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_ip_info(netif, &ip_info);
        snprintf(buf, sizeof(buf), "IP: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, sizeof(buf), "IP: Unknown");
    }
    fb_draw_text(0, 32, buf);

    // MAC
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, sizeof(buf), "MAC:%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    fb_draw_text(0, 42, buf);

    fb_draw_text(0, 54, "[Back]");
}

static void render_flashlight_menu(void) {
    fb_fill_rect(0, 0, DISP_WIDTH, DISP_HEIGHT, 1);
}

static void render_notification_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===NOTIFY===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    // Title
    fb_draw_text(0, 12, notif_title);
    
    // Body (simple multi-line rendering)
    int y = 24;
    const char *p = notif_body;
    char line[22]; // ~21 chars fit on 128px wide screen with 6px font
    
    while (*p && y < DISP_HEIGHT) {
        strncpy(line, p, 21);
        line[21] = '\0';
        fb_draw_text(0, y, line);
        y += 10;
        if (strlen(p) > 21) p += 21;
        else break;
    }
    
    fb_draw_text(0, 54, "[Any Key] Close");
}

static void render_sync_wait(void) {
    fb_clear();
    fb_draw_text(0, 0, "=== SYNC ===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    int status = wifi_get_sync_status();
    if (status == 1) {
        fb_draw_text(0, 20, "Syncing...");
        fb_draw_text(0, 35, "Please wait");
    } else if (status == 2) {
        fb_draw_text(0, 20, "Updated!");
    } else if (status == 3) {
        fb_draw_text(0, 20, "Failed!");
        fb_draw_text(0, 35, "Check WiFi");
    }
}

static void render_find_phone_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===FIND PHONE===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    draw_menu_item(30, "Ring Phone", find_phone_selection == 0);
    draw_menu_item(42, "Stop ring", find_phone_selection == 1);
    draw_menu_item(54, "[Back]", find_phone_selection == 2);
}

static void render_music_control_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===MUSIC===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    const char *items[] = {
        "Play/Pause",
        "Next Track",
        "Prev Track",
        "[Back]"
    };
    
    int y_pos = 20;
    for (int i = 0; i < 4; i++) {
        draw_menu_item(y_pos, items[i], i == music_selection);
        y_pos += 10;
    }
}

static void render_root_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===MENU===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);

    const char *items[] = {
        "Sensors",
        "Weather",
        "Settings",
        "Watchface",
        "Stopwatch",
        "Find Phone",
        "Music Control",
        "System Info",
        "Flashlight",
        "[Back]"
    };
    int item_count = 10;

    int start_idx = root_selection - 2;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > item_count - 5) start_idx = item_count - 5;
    if (start_idx < 0) start_idx = 0; // Safety if count < 5

    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 5 && i < item_count; i++) {
        bool is_selected = (i == root_selection);
        draw_menu_item(y_pos, items[i], is_selected);
        y_pos += 10;
    }

    // Scroll indicators
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 5 < item_count) fb_draw_text(120, 54, "v");
}

static void render_watchface_menu(void) {
    fb_clear();
    
    fb_draw_text(0, 0, "===WATCHFACE===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
    const char *names[WATCHFACE_COUNT] = {
        "Digital",
        "Analog Style",
        "Minimal",
        "Compact",
        "Terminal",
        "Matrix",
        "Cats"
    };
    
    int start_idx = watchface_selection - 2;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > WATCHFACE_COUNT - 5) start_idx = WATCHFACE_COUNT - 5;
    if (start_idx < 0) start_idx = 0;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 5 && i < WATCHFACE_COUNT; i++) {
        bool is_selected = (i == watchface_selection);
        draw_menu_item(y_pos, names[i], is_selected);
        y_pos += 10;
    }
    
    // Show Back option at the end if scrolled there
    if (watchface_selection == WATCHFACE_COUNT) {
         draw_menu_item(y_pos, "[Back]", true);
    } else if (start_idx + 5 >= WATCHFACE_COUNT) {
         draw_menu_item(y_pos, "[Back]", false);
    }
    
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 5 < WATCHFACE_COUNT) fb_draw_text(120, 54, "v");
}

static void render_watch_display(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    switch (current_watchface) {
        case WATCHFACE_DIGITAL:
            render_watchface_digital(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case WATCHFACE_ANALOG_STYLE:
            render_watchface_analog(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case WATCHFACE_MINIMAL:
            render_watchface_minimal(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case WATCHFACE_COMPACT:
            render_watchface_compact(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case WATCHFACE_TERMINAL:
            render_watchface_terminal(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case WATCHFACE_MATRIX:
            render_watchface_matrix(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case WATCHFACE_CATS:
            render_watchface_cats(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        default:
            render_watchface_digital(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
    }
}

void menu_render(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    switch (current_menu) {
        case MENU_WATCH:
            render_watch_display(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
        case MENU_ROOT:
            render_root_menu();
            break;
        case MENU_SENSOR_DATA:
            render_sensor_menu_list(temp, hum, ax, ay, az, batt_mv, batt_pct);
            break;
        case MENU_WEATHER:
            render_weather_menu();
            break;
        case MENU_STOPWATCH:
            render_stopwatch_menu();
            break;
        case MENU_SYSTEM_INFO:
            render_system_info_menu();
            break;
        case MENU_FLASHLIGHT:
            render_flashlight_menu();
            break;
        case MENU_SETTINGS:
            render_settings_menu();
            break;
        case MENU_WATCHFACE:
            render_watchface_menu();
            break;
        case MENU_NOTIFICATION:
            render_notification_menu();
            break;
        case MENU_SYNC_WAIT:
            render_sync_wait();
            break;
        case MENU_FIND_PHONE:
            render_find_phone_menu();
            break;
        case MENU_MUSIC_CONTROL:
            render_music_control_menu();
            break;
        default:
            render_watch_display(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
    }
}

bool menu_handle_button(button_event_t event, int32_t current_time_s) {
    bool activity = true;
    menu_last_activity_s = current_time_s; // Update activity time

    if (current_menu == MENU_SYNC_WAIT) {
        // Any button press exits sync wait
        current_menu = MENU_SETTINGS;
        return true;
    }

    if (event == BTN_UP_PRESS) {
        if (current_menu == MENU_ROOT) {
            if (root_selection > 0) root_selection--;
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection > 0) watchface_selection--;
        } else if (current_menu == MENU_WEATHER) {
            if (weather_selection > 0) weather_selection--;
        } else if (current_menu == MENU_FIND_PHONE) {
            if (find_phone_selection > 0) find_phone_selection--;
        } else if (current_menu == MENU_MUSIC_CONTROL) {
            if (music_selection > 0) music_selection--;
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                motion_threshold_editable += 10;
                if (motion_threshold_editable > 500) motion_threshold_editable = 500;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                screen_timeout_editable += 1;
                if (screen_timeout_editable > 60) screen_timeout_editable = 60;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            } else if (current_setting == SETTINGS_BRIGHTNESS) {
                brightness_editable += 10;
                if (brightness_editable > 255) brightness_editable = 255;
                sh1106_set_contrast((uint8_t)brightness_editable);
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            }
        } else if (current_menu == MENU_SETTINGS) {
            if (current_setting > 0) current_setting--;
        } else if (current_menu == MENU_SENSOR_DATA) {
            if (current_sensor > 0) current_sensor--;
        }
    } else if (event == BTN_DOWN_PRESS) {
        if (current_menu == MENU_ROOT) {
            if (root_selection < 9) root_selection++;
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection < WATCHFACE_COUNT) watchface_selection++;
        } else if (current_menu == MENU_WEATHER) {
            if (weather_selection < 1) weather_selection++;
        } else if (current_menu == MENU_FIND_PHONE) {
            if (find_phone_selection < 2) find_phone_selection++;
        } else if (current_menu == MENU_MUSIC_CONTROL) {
            if (music_selection < 3) music_selection++;
        } else if (current_menu == MENU_STOPWATCH) {
            if (!stopwatch_running) {
                stopwatch_elapsed_time = 0; // Reset
            }
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                motion_threshold_editable -= 10;
                if (motion_threshold_editable < 10) motion_threshold_editable = 10;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                screen_timeout_editable -= 1;
                if (screen_timeout_editable < 1) screen_timeout_editable = 1;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            } else if (current_setting == SETTINGS_BRIGHTNESS) {
                brightness_editable -= 10;
                if (brightness_editable < 0) brightness_editable = 0;
                sh1106_set_contrast((uint8_t)brightness_editable);
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            }
        } else if (current_menu == MENU_SETTINGS) {
            if (current_setting < SETTINGS_COUNT - 1) current_setting++;
        } else if (current_menu == MENU_SENSOR_DATA) {
            if (current_sensor < SENSOR_COUNT - 1) current_sensor++;
        }
    } else if (event == BTN_OK_PRESS) {
        if (current_menu == MENU_WATCH) {
            current_menu = MENU_ROOT;
            root_selection = 0;
        } else if (current_menu == MENU_ROOT) {
            if (root_selection == 0) current_menu = MENU_SENSOR_DATA;
            else if (root_selection == 1) {
                current_menu = MENU_WEATHER;
                weather_selection = 0; // Reset to Refresh
            }
            else if (root_selection == 2) current_menu = MENU_SETTINGS;
            else if (root_selection == 3) {
                current_menu = MENU_WATCHFACE;
                watchface_selection = current_watchface;
            } else if (root_selection == 4) {
                current_menu = MENU_STOPWATCH;
            } else if (root_selection == 5) {
                current_menu = MENU_FIND_PHONE;
            } else if (root_selection == 6) {
                current_menu = MENU_MUSIC_CONTROL;
                music_selection = 0;
            } else if (root_selection == 7) {
                current_menu = MENU_SYSTEM_INFO;
            } else if (root_selection == 8) {
                current_menu = MENU_FLASHLIGHT;
                saved_brightness = (uint8_t)brightness_editable;
                sh1106_set_contrast(255);
            } else if (root_selection == 9) {
                current_menu = MENU_WATCH; // Back to watch
            }
        } else if (current_menu == MENU_FIND_PHONE) {
            if (find_phone_selection == 0) {
                ble_manager_send_command("find_phone");
            } else if (find_phone_selection == 1) {
                ble_manager_send_command("find_phone_stop");
            }else {
                current_menu = MENU_ROOT;
            }
        } else if (current_menu == MENU_MUSIC_CONTROL) {
            if (music_selection == 0) ble_manager_send_command("music_toggle");
            else if (music_selection == 1) ble_manager_send_command("music_next");
            else if (music_selection == 2) ble_manager_send_command("music_prev");
            else current_menu = MENU_ROOT;

        } else if (current_menu == MENU_FLASHLIGHT) {
             sh1106_set_contrast(saved_brightness);
             current_menu = MENU_ROOT;
        } else if (current_menu == MENU_SYSTEM_INFO) {
            current_menu = MENU_ROOT; // Back
        } else if (current_menu == MENU_STOPWATCH) {
            if (stopwatch_running) {
                // Stop
                int64_t now = esp_timer_get_time();
                stopwatch_elapsed_time += (now - stopwatch_start_time);
                stopwatch_running = false;
            } else {
                // Start
                stopwatch_start_time = esp_timer_get_time();
                stopwatch_running = true;
            }
        } else if (current_menu == MENU_WEATHER) {
            if (weather_selection == 0) {
                ESP_LOGI(TAG, "Manual weather refresh requested");
                weather_fetch_async();
            } else {
                current_menu = MENU_ROOT; // Back
            }
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection == WATCHFACE_COUNT) {
                current_menu = MENU_ROOT; // Back
            } else {
                current_watchface = watchface_selection;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
                current_menu = MENU_WATCH;
                editing_mode = false;
            }
        } else if (current_menu == MENU_SETTINGS && !editing_mode) {
            if (current_setting == SETTINGS_BACK) {
                current_menu = MENU_ROOT; // Back
            } else if (current_setting == SETTINGS_TIME_SYNC) {
                wifi_sync_time_async();
                current_menu = MENU_SYNC_WAIT;
            } else if (current_setting == SETTINGS_REBOOT) {
                ESP_LOGI(TAG, "Reboot requested");
                esp_restart();
            } else if (current_setting == SETTINGS_POWER_OFF) {
                ESP_LOGI(TAG, "Power Off requested");
                fb_clear();
                fb_draw_text(10, 30, "Powering Off...");
                sh1106_render();
                vTaskDelay(pdMS_TO_TICKS(1000));
                sh1106_display_off();
                input_enable_deep_sleep_wakeup();
                esp_deep_sleep_start();
            } else {
                editing_mode = true;
            }
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            editing_mode = false;
            settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
        } else if (current_menu == MENU_SENSOR_DATA) {
            if (current_sensor == SENSOR_BACK) {
                current_menu = MENU_ROOT;
            }
        } else if (current_menu == MENU_NOTIFICATION) {
             // Any key dismisses notification
             current_menu = MENU_WATCH;
        } else if (current_menu == MENU_SYNC_WAIT) {
             // Allow exit from sync wait
             current_menu = MENU_SETTINGS;
        } else {
            current_menu = MENU_WATCH;
            editing_mode = false;
        }
    }

    return activity;
}
