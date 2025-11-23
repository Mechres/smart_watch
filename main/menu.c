#include "menu.h"
#include "display.h"
#include "watchfaces.h"
#include "settings.h"
#include "esp_log.h"
#include <stdio.h>
#include <sys/time.h>
#include "pedometer.h"

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
    
    int start_idx = current_sensor - 1;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > SENSOR_COUNT - 3) start_idx = SENSOR_COUNT - 3;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 3 && i < SENSOR_COUNT; i++) {
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
        y_pos += 11;
    }
    
    // Scroll indicators
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 3 < SENSOR_COUNT) fb_draw_text(120, 34, "v");
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

    // Back
    draw_menu_item(36, "[Back]", current_setting == SETTINGS_BACK);
}

static void render_root_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===MENU===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);

    draw_menu_item(12, "Sensors", root_selection == 0);
    draw_menu_item(24, "Settings", root_selection == 1);
    draw_menu_item(36, "Watchface", root_selection == 2);
    draw_menu_item(48, "[Back]", root_selection == 3);
}

static void render_watchface_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===WATCHFACE===");
    fb_draw_line(0, 9, DISP_WIDTH, 9, 1);
    
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
        draw_menu_item(y_pos, names[i], is_selected);
        y_pos += 11;
    }
    
    // Show Back option at the end if scrolled there
    if (watchface_selection == WATCHFACE_COUNT) {
         draw_menu_item(y_pos, "[Back]", true);
    } else if (start_idx + 3 >= WATCHFACE_COUNT) {
         draw_menu_item(y_pos, "[Back]", false);
    }
    
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 3 < WATCHFACE_COUNT) fb_draw_text(120, 34, "v");
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
            if (root_selection < 3) root_selection++;
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection < WATCHFACE_COUNT) watchface_selection++;
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
            } else if (root_selection == 3) {
                current_menu = MENU_WATCH; // Back to watch
            }
        } else if (current_menu == MENU_WATCHFACE) {
            if (watchface_selection == WATCHFACE_COUNT) {
                current_menu = MENU_ROOT; // Back
            } else {
                current_watchface = watchface_selection;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                current_menu = MENU_WATCH;
                editing_mode = false;
            }
        } else if (current_menu == MENU_SETTINGS && !editing_mode) {
            if (current_setting == SETTINGS_BACK) {
                current_menu = MENU_ROOT; // Back
            } else {
                editing_mode = true;
            }
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            editing_mode = false;
            settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
        } else if (current_menu == MENU_SENSOR_DATA) {
            if (current_sensor == SENSOR_BACK) {
                current_menu = MENU_ROOT;
            }
        } else {
            current_menu = MENU_WATCH;
            editing_mode = false;
        }
    }

    return activity;
}
