#include "menu.h"
#include "display.h"
#include "watchfaces.h"
#include "settings.h"
#include "esp_log.h"
#include <stdio.h>
#include <sys/time.h>

static const char *TAG = "Menu";

/* Menu states */
typedef enum {
    MENU_WATCH,           // Main watch display
    MENU_ROOT,            // Top-level category menu
    MENU_SETTINGS,        // Settings menu
    MENU_SENSOR_DATA,     // Detailed sensor readings
    MENU_WATCHFACE,       // Watchface selection
    MENU_COUNT
} menu_mode_t;

typedef enum {
    WATCHFACE_DIGITAL = 0,
    WATCHFACE_ANALOG_STYLE,
    WATCHFACE_MINIMAL,
    WATCHFACE_COMPACT,
    WATCHFACE_COUNT
} watchface_t;

typedef enum {
    SETTINGS_MOTION_THRESHOLD = 0,
    SETTINGS_SCREEN_TIMEOUT,
    SETTINGS_COUNT
} settings_item_t;

typedef enum {
    SENSOR_TEMP = 0,
    SENSOR_HUMIDITY,
    SENSOR_ACCEL_X,
    SENSOR_ACCEL_Y,
    SENSOR_ACCEL_Z,
    SENSOR_BATTERY,
    SENSOR_COUNT
} sensor_item_t;

/* State variables */
static menu_mode_t current_menu = MENU_WATCH;
static int32_t menu_last_activity_s = 0;
static settings_item_t current_setting = SETTINGS_MOTION_THRESHOLD;
static sensor_item_t current_sensor = SENSOR_TEMP;
static int root_selection = 0; // 0 = Sensors, 1 = Settings, 2 = Watchface
static watchface_t current_watchface = WATCHFACE_DIGITAL;
static int watchface_selection = 0;
static bool editing_mode = false;

/* Local copies of settings (loaded from settings module) */
static int16_t motion_threshold_editable = 100;
static int16_t screen_timeout_editable = 5;

void menu_init(void) {
    // Load initial settings
    settings_load(&motion_threshold_editable, &screen_timeout_editable, (int*)&current_watchface);
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
    if (current_menu != MENU_WATCH && (current_time_s - menu_last_activity_s) > 10) {
        ESP_LOGI(TAG, "Menu timeout - returning to watch");
        current_menu = MENU_WATCH;
        editing_mode = false;
    }
}

/* Render helpers */
static void render_sensor_menu_list(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===SENSORS===");
    
    int start_idx = current_sensor - 1;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > SENSOR_COUNT - 3) start_idx = SENSOR_COUNT - 3;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 3 && i < SENSOR_COUNT; i++) {
        bool is_selected = (i == current_sensor);
        char prefix = is_selected ? '>' : ' ';
        
        switch (i) {
            case SENSOR_TEMP:
                snprintf(buf, sizeof(buf), "%c Temp: %.2f C", prefix, temp);
                break;
            case SENSOR_HUMIDITY:
                snprintf(buf, sizeof(buf), "%c Humidity: %.1f%%", prefix, hum);
                break;
            case SENSOR_ACCEL_X:
                snprintf(buf, sizeof(buf), "%c AccelX: %d", prefix, ax);
                break;
            case SENSOR_ACCEL_Y:
                snprintf(buf, sizeof(buf), "%c AccelY: %d", prefix, ay);
                break;
            case SENSOR_ACCEL_Z:
                snprintf(buf, sizeof(buf), "%c AccelZ: %d", prefix, az);
                break;
            case SENSOR_BATTERY:
                snprintf(buf, sizeof(buf), "%c Batt: %d%% %dmV", prefix, batt_pct, batt_mv);
                break;
        }
        
        fb_draw_text(0, y_pos, buf);
        y_pos += 10;
    }
    
    if (start_idx > 0) fb_draw_text(0, 52, "  [up]");
    if (start_idx + 3 < SENSOR_COUNT) fb_draw_text(70, 52, "[dn]");
}

static void render_settings_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===SETTINGS===");
    
    // Motion Threshold
    if (current_setting == SETTINGS_MOTION_THRESHOLD) {
        fb_draw_text(0, 12, "> Motion Thresh");
    } else {
        fb_draw_text(0, 12, "  Motion Thresh");
    }
    
    if (current_setting == SETTINGS_MOTION_THRESHOLD && editing_mode) {
        snprintf(buf, sizeof(buf), "  [%d]  <OK to exit>", motion_threshold_editable);
    } else {
        snprintf(buf, sizeof(buf), "  Value: %d", motion_threshold_editable);
    }
    fb_draw_text(0, 22, buf);
    
    // Screen Timeout
    if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
        fb_draw_text(0, 37, "> Screen Timeout");
    } else {
        fb_draw_text(0, 37, "  Screen Timeout");
    }
    
    if (current_setting == SETTINGS_SCREEN_TIMEOUT && editing_mode) {
        snprintf(buf, sizeof(buf), "  [%d]s  <OK to exit>", screen_timeout_editable);
    } else {
        snprintf(buf, sizeof(buf), "  Value: %d s", screen_timeout_editable);
    }
    fb_draw_text(0, 47, buf);
}

static void render_root_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===MENU===");
    if (root_selection == 0) fb_draw_text(0, 12, "> Sensors"); else fb_draw_text(0, 12, "  Sensors");
    if (root_selection == 1) fb_draw_text(0, 24, "> Settings"); else fb_draw_text(0, 24, "  Settings");
    if (root_selection == 2) fb_draw_text(0, 36, "> Watchface"); else fb_draw_text(0, 36, "  Watchface");
}

static void render_watchface_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===WATCHFACE===");
    
    const char *names[WATCHFACE_COUNT] = {
        "Digital",
        "Analog Style",
        "Minimal",
        "Compact"
    };
    
    int start_idx = watchface_selection - 1;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > WATCHFACE_COUNT - 3) start_idx = WATCHFACE_COUNT - 3;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 3 && i < WATCHFACE_COUNT; i++) {
        bool is_selected = (i == watchface_selection);
        char prefix = is_selected ? '>' : ' ';
        snprintf(buf, sizeof(buf), "%c %s", prefix, names[i]);
        fb_draw_text(0, y_pos, buf);
        y_pos += 10;
    }
    
    if (start_idx > 0) fb_draw_text(0, 52, "  [up]");
    if (start_idx + 3 < WATCHFACE_COUNT) fb_draw_text(70, 52, "[dn]");
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
        case MENU_SETTINGS:
            render_settings_menu();
            break;
        case MENU_WATCHFACE:
            render_watchface_menu();
            break;
        default:
            render_watch_display(temp, hum, ax, ay, az, batt_mv, batt_pct, timeinfo);
            break;
    }
}

bool menu_handle_button(button_event_t event, int32_t current_time_s) {
    bool activity = true;
    menu_last_activity_s = current_time_s; // Update activity time

    if (event == BTN_UP_PRESS) {
        if (current_menu == MENU_ROOT) {
            if (root_selection > 0) root_selection--;
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection > 0) watchface_selection--;
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                motion_threshold_editable += 10;
                if (motion_threshold_editable > 500) motion_threshold_editable = 500;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
            } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                screen_timeout_editable += 1;
                if (screen_timeout_editable > 60) screen_timeout_editable = 60;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
            }
        } else if (current_menu == MENU_SETTINGS) {
            if (current_setting > 0) current_setting--;
        } else if (current_menu == MENU_SENSOR_DATA) {
            if (current_sensor > 0) current_sensor--;
        }
    } else if (event == BTN_DOWN_PRESS) {
        if (current_menu == MENU_ROOT) {
            if (root_selection < 2) root_selection++;
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection < WATCHFACE_COUNT - 1) watchface_selection++;
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                motion_threshold_editable -= 10;
                if (motion_threshold_editable < 10) motion_threshold_editable = 10;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
            } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                screen_timeout_editable -= 1;
                if (screen_timeout_editable < 1) screen_timeout_editable = 1;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
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
            else if (root_selection == 1) current_menu = MENU_SETTINGS;
            else if (root_selection == 2) {
                current_menu = MENU_WATCHFACE;
                watchface_selection = current_watchface;
            }
        } else if (current_menu == MENU_WATCHFACE) {
            current_watchface = watchface_selection;
            settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
            current_menu = MENU_WATCH;
            editing_mode = false;
        } else if (current_menu == MENU_SETTINGS && !editing_mode) {
            editing_mode = true;
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            editing_mode = false;
            settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
        } else {
            current_menu = MENU_WATCH;
            editing_mode = false;
        }
    }

    return activity;
}
