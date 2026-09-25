#include "menu.h"
#include "display.h"
#include "watchfaces.h"
#include "settings.h"
#include "alarm.h"
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
#include "power.h"

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
    SETTINGS_TIME_FORMAT,
    SETTINGS_TEMP_UNIT,
    SETTINGS_ALARM_TOGGLE,
    SETTINGS_ALARM_HOUR,
    SETTINGS_ALARM_MIN,
    SETTINGS_TIME_SYNC,
    SETTINGS_RESET,
    SETTINGS_REBOOT,
    SETTINGS_POWER_OFF,
    SETTINGS_BACK,
    SETTINGS_COUNT
} settings_item_t;

typedef enum {
    SENSOR_TEMP = 0,
    SENSOR_HUMIDITY,
    SENSOR_STEPS,
    SENSOR_DISTANCE,
    SENSOR_CALORIES,
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
// weather_selection removed
static int music_selection = 0; // 0=Play/Pause, 1=Next, 2=Prev, 3=Back
static int find_phone_selection = 0; // 0=Ring, 1=Stop, 2=Back
static bool editing_mode = false;

static uint8_t saved_brightness = 128;

/* Notification state */
static char notif_title[32];
static char notif_body[128];
static int notif_scroll = 0;
static time_t notif_time = 0;

/* Word-wrap body into 21-char lines. Returns line count. */
#define NOTIF_MAX_LINES 16
static int notif_wrap_lines(char lines[][22]) {
    int n = 0;
    const char *p = notif_body;
    while (*p && n < NOTIF_MAX_LINES) {
        while (*p == ' ') p++;
        if (!*p) break;
        size_t remaining = strlen(p);
        size_t take = remaining > 21 ? 21 : remaining;
        if (remaining > 21) {
            size_t ws = take;
            while (ws > 0 && p[ws - 1] != ' ') ws--;
            if (ws > 8) take = ws;
        }
        memcpy(lines[n], p, take);
        lines[n][take] = '\0';
        n++;
        p += take;
    }
    return n;
}

static int notif_max_scroll(void) {
    char lines[NOTIF_MAX_LINES][22];
    int n = notif_wrap_lines(lines);
    return (n > 3) ? n - 3 : 0;
}

/* Stopwatch state */
static bool stopwatch_running = false;
static int64_t stopwatch_start_time = 0;
static int64_t stopwatch_elapsed_time = 0;

/* Local copies of settings (loaded from settings module) */
static int16_t motion_threshold_editable = 100;
static int16_t screen_timeout_editable = 3;
static int16_t brightness_editable = 128;
static int time_format_editable = 0; /* 0=24h, 1=12h */
static int temp_unit_editable = 0;   /* 0=C, 1=F */
static int alarm_hour_editable = 7;
static int alarm_min_editable = 0;
static bool alarm_enabled_editable = false;

int menu_get_time_format(void) {
    return time_format_editable;
}

int menu_get_temp_unit(void) {
    return temp_unit_editable;
}

void menu_init(void) {
    // Load initial settings
    settings_load(&motion_threshold_editable, &screen_timeout_editable, (int*)&current_watchface, &brightness_editable);
    settings_load_units(&time_format_editable, &temp_unit_editable);
    alarm_init();
    alarm_get(&alarm_hour_editable, &alarm_min_editable, &alarm_enabled_editable);
    // Apply loaded brightness
    esp_err_t err = sh1106_set_contrast((uint8_t)brightness_editable);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_contrast failed: %s", esp_err_to_name(err));
    }
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

int menu_get_min_refresh_ms(void) {
    if (current_menu == MENU_STOPWATCH && stopwatch_running) {
        return 100; /* 1/10 s display resolution */
    }
    if (current_menu == MENU_WATCH) {
        /* Throttle animations in light sleep to save power + I2C contention. */
        bool low_power = (power_get_mode() == POWER_LIGHT_SLEEP);
        switch (current_watchface) {
            case WATCHFACE_TERMINAL:
                return low_power ? 2000 : 1000; /* seconds field + cursor blink */
            case WATCHFACE_DIGITAL:
                return low_power ? 1000 : 500; /* seconds + blinking colon */
            case WATCHFACE_ANALOG_STYLE:
                return low_power ? 1000 : 500; /* second hand */
            case WATCHFACE_MINIMAL:
                return low_power ? 2000 : 1000; /* blinking colon */
            case WATCHFACE_MATRIX:
            case WATCHFACE_CATS:
                return low_power ? 1000 : 200;  /* animation tick */
            default:
                return 0;    /* static: render only on content change */
        }
    }
    return 0;
}

void menu_check_timeout(int32_t current_time_s) {
    if (current_menu == MENU_NOTIFICATION) {
        if ((current_time_s - menu_last_activity_s) > 10) {
             ESP_LOGI(TAG, "Notification timeout - returning to watch");
             current_menu = MENU_WATCH;
        }
    } else if (current_menu != MENU_WATCH && (current_time_s - menu_last_activity_s) > 10) {
        ESP_LOGI(TAG, "Menu timeout - returning to watch");
        if (editing_mode && current_setting != SETTINGS_RESET) {
            settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
            settings_save_units(time_format_editable, temp_unit_editable);
            alarm_set(alarm_hour_editable, alarm_min_editable, alarm_enabled_editable);
        }
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
    notif_scroll = 0;
    notif_time = time(NULL);
    
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
    
    fb_draw_header("SENSORS");
    
    int start_idx = current_sensor - 2;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > SENSOR_COUNT - 5) start_idx = SENSOR_COUNT - 5;
    if (start_idx < 0) start_idx = 0;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 5 && i < SENSOR_COUNT; i++) {
        bool is_selected = (i == current_sensor);
        
        switch (i) {
            case SENSOR_TEMP: {
                float t = temp_unit_editable ? (temp * 9.0f / 5.0f + 32.0f) : temp;
                snprintf(buf, sizeof(buf), "Temp: %.2f %c", t, temp_unit_editable ? 'F' : 'C');
                break;
            }
            case SENSOR_HUMIDITY:
                snprintf(buf, sizeof(buf), "Humidity: %.1f%%", hum);
                break;
            case SENSOR_STEPS:
                snprintf(buf, sizeof(buf), "Steps: %d", pedometer_get_steps());
                break;
            case SENSOR_DISTANCE:
                snprintf(buf, sizeof(buf), "Dist: %.2f km", pedometer_get_distance_m() / 1000.0f);
                break;
            case SENSOR_CALORIES:
                snprintf(buf, sizeof(buf), "Kcal: %.1f", pedometer_get_calories_kcal());
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
    
    fb_draw_header("SETTINGS");
    
    int start_idx = current_setting - 2;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > SETTINGS_COUNT - 5) start_idx = SETTINGS_COUNT - 5;
    if (start_idx < 0) start_idx = 0;

    int y_pos = 12;

    for (int i = start_idx; i < start_idx + 5 && i < SETTINGS_COUNT; i++) {
        bool is_selected = (i == current_setting);

        switch (i) {
            case SETTINGS_MOTION_THRESHOLD:
                if (editing_mode && is_selected) {
                    snprintf(buf, sizeof(buf), "Motion: [%d]", motion_threshold_editable);
                    fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
                    fb_draw_text_ex(2, y_pos + 1, buf, 0, -1);
                    fb_draw_text(100, y_pos + 1, "<>");
                } else {
                    snprintf(buf, sizeof(buf), "Motion: %d", motion_threshold_editable);
                    draw_menu_item(y_pos, buf, is_selected);
                }
                break;
            case SETTINGS_SCREEN_TIMEOUT:
                if (editing_mode && is_selected) {
                    snprintf(buf, sizeof(buf), "Timeout: [%d]s", screen_timeout_editable);
                    fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
                    fb_draw_text_ex(2, y_pos + 1, buf, 0, -1);
                    fb_draw_text(100, y_pos + 1, "<>");
                } else {
                    snprintf(buf, sizeof(buf), "Timeout: %d s", screen_timeout_editable);
                    draw_menu_item(y_pos, buf, is_selected);
                }
                break;
            case SETTINGS_BRIGHTNESS:
                if (editing_mode && is_selected) {
                    snprintf(buf, sizeof(buf), "Bright: [%d]", brightness_editable);
                    fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
                    fb_draw_text_ex(2, y_pos + 1, buf, 0, -1);
                    fb_draw_text(100, y_pos + 1, "<>");
                } else {
                    snprintf(buf, sizeof(buf), "Bright: %d", brightness_editable);
                    draw_menu_item(y_pos, buf, is_selected);
                }
                break;
            case SETTINGS_TIME_FORMAT:
                snprintf(buf, sizeof(buf), "Time: %s", time_format_editable ? "12h" : "24h");
                draw_menu_item(y_pos, buf, is_selected);
                break;
            case SETTINGS_TEMP_UNIT:
                snprintf(buf, sizeof(buf), "Temp: %s", temp_unit_editable ? "F" : "C");
                draw_menu_item(y_pos, buf, is_selected);
                break;
            case SETTINGS_ALARM_TOGGLE:
                snprintf(buf, sizeof(buf), "Alarm: %s", alarm_enabled_editable ? "ON" : "OFF");
                draw_menu_item(y_pos, buf, is_selected);
                break;
            case SETTINGS_ALARM_HOUR:
                if (editing_mode && is_selected) {
                    snprintf(buf, sizeof(buf), "Alm Hr: [%02d]", alarm_hour_editable);
                    fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
                    fb_draw_text_ex(2, y_pos + 1, buf, 0, -1);
                    fb_draw_text(100, y_pos + 1, "<>");
                } else {
                    snprintf(buf, sizeof(buf), "Alm Hr: %02d", alarm_hour_editable);
                    draw_menu_item(y_pos, buf, is_selected);
                }
                break;
            case SETTINGS_ALARM_MIN:
                if (editing_mode && is_selected) {
                    snprintf(buf, sizeof(buf), "Alm Min: [%02d]", alarm_min_editable);
                    fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
                    fb_draw_text_ex(2, y_pos + 1, buf, 0, -1);
                    fb_draw_text(100, y_pos + 1, "<>");
                } else {
                    snprintf(buf, sizeof(buf), "Alm Min: %02d", alarm_min_editable);
                    draw_menu_item(y_pos, buf, is_selected);
                }
                break;
            case SETTINGS_TIME_SYNC:
                draw_menu_item(y_pos, "Time Sync", is_selected);
                break;
            case SETTINGS_RESET:
                if (editing_mode && is_selected) {
                    fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
                    fb_draw_text_ex(2, y_pos + 1, "Reset? [OK]=Yes", 0, -1);
                } else {
                    draw_menu_item(y_pos, "Reset Settings", is_selected);
                }
                break;
            case SETTINGS_REBOOT:
                draw_menu_item(y_pos, "Reboot", is_selected);
                break;
            case SETTINGS_POWER_OFF:
                draw_menu_item(y_pos, "Power Off", is_selected);
                break;
            case SETTINGS_BACK:
                draw_menu_item(y_pos, "[Back]", is_selected);
                break;
        }
        /* Mini value bar for ranged settings (2px, row bottom; inverted with selection). */
        if (i == SETTINGS_MOTION_THRESHOLD || i == SETTINGS_SCREEN_TIMEOUT || i == SETTINGS_BRIGHTNESS) {
            int pct = 0;
            if (i == SETTINGS_MOTION_THRESHOLD)
                pct = (motion_threshold_editable - 10) * 100 / 490;
            else if (i == SETTINGS_SCREEN_TIMEOUT)
                pct = (screen_timeout_editable - 1) * 100 / 59;
            else
                pct = brightness_editable * 100 / 255;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            int bar_w = (124 * pct) / 100;
            if (bar_w > 0) fb_fill_rect(2, y_pos + 8, bar_w, 2, is_selected ? 0 : 1);
        }
        y_pos += 10;
    }

    // Scroll indicators
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 5 < SETTINGS_COUNT) fb_draw_text(120, 54, "v");
}

static void render_weather_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_header("WEATHER");
    
    weather_data_t w = weather_get_current();
    if (w.valid) {
        float wt = temp_unit_editable ? (w.temp_c * 9.0f / 5.0f + 32.0f) : w.temp_c;
        snprintf(buf, sizeof(buf), "Temp: %.1f %c", wt, temp_unit_editable ? 'F' : 'C');
        fb_draw_text(0, 15, buf);

        fb_draw_wx_icon(112, 14, w.weather_code);

        const char *desc = weather_get_desc(w.weather_code);
        snprintf(buf, sizeof(buf), "%s", desc);
        fb_draw_text(0, 27, buf);
    } else {
        fb_draw_text(0, 15, "No Data");
        fb_draw_text(0, 27, "Sync via App");
    }
    
    // Draw actions
    draw_menu_item(53, "[Back]", true);
}

static void render_stopwatch_menu(void) {
    fb_clear();
    char buf[64];

    fb_draw_header("STOPWATCH");

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

    // Large 7-seg display, centered
    int bw = fb_big_text_width(buf);
    fb_draw_big_text((DISP_WIDTH - bw) / 2, 16, buf, 1);

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

    fb_draw_header("SYS INFO");

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
    snprintf(buf, sizeof(buf), "Heap: %u B", (unsigned)free_heap);
    fb_draw_text(0, 22, buf);

    // IP (only valid when WiFi connected; otherwise show dashes)
    if (wifi_is_connected()) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(buf, sizeof(buf), "IP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            snprintf(buf, sizeof(buf), "IP: ---");
        }
    } else {
        snprintf(buf, sizeof(buf), "IP: (offline)");
    }
    fb_draw_text(0, 32, buf);

    // MAC (may fail if WiFi never initialized)
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(buf, sizeof(buf), "MAC:%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(buf, sizeof(buf), "MAC: ------");
    }
    fb_draw_text(0, 42, buf);

    fb_draw_text(0, 54, "[Back]");
}

static void render_flashlight_menu(void) {
    fb_fill_rect(0, 0, DISP_WIDTH, DISP_HEIGHT, 1);
}

static void render_notification_menu(void) {
    fb_clear();
    /* Header carries the received time when the clock is set. */
    char hbuf[24];
    struct tm rtm;
    localtime_r(&notif_time, &rtm);
    if (rtm.tm_year + 1900 >= 2024)
        snprintf(hbuf, sizeof(hbuf), "NOTIFY %02d:%02d", rtm.tm_hour, rtm.tm_min);
    else
        snprintf(hbuf, sizeof(hbuf), "NOTIFY");
    fb_draw_header(hbuf);

    // Title (truncate to bar width)
    char tbuf[22];
    snprintf(tbuf, sizeof(tbuf), "%.21s", notif_title);
    fb_draw_text(0, 12, tbuf);

    // Body: 3 visible lines from scroll offset
    char lines[NOTIF_MAX_LINES][22];
    int n = notif_wrap_lines(lines);
    int y = 24;
    for (int i = notif_scroll; i < n && i < notif_scroll + 3; i++) {
        fb_draw_text(0, y, lines[i]);
        y += 10;
    }

    if (notif_max_scroll() > 0) {
        fb_draw_text(0, 54, "[OK]Close");
        char sbuf[12];
        int page = notif_scroll + 1;
        int pages = n - 2;
        if (page < 1) page = 1;
        if (page > 99) page = 99;
        if (pages < 1) pages = 1;
        if (pages > 99) pages = 99;
        snprintf(sbuf, sizeof(sbuf), "%d/%d", page, pages);
        int slen = strlen(sbuf);
        fb_draw_text(DISP_WIDTH - slen * 6, 54, sbuf);
    } else {
        fb_draw_text(0, 54, "[OK] Close");
    }
}

static void render_sync_wait(void) {
    fb_clear();
    fb_draw_header("SYNC");
    
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
    fb_draw_header("FIND PHONE");
    
    draw_menu_item(30, "Ring Phone", find_phone_selection == 0);
    draw_menu_item(42, "Stop ring", find_phone_selection == 1);
    draw_menu_item(54, "[Back]", find_phone_selection == 2);
}

static void render_music_control_menu(void) {
    fb_clear();
    fb_draw_header("MUSIC");
    
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
    fb_draw_header("MENU");

    /* 8x8 MSB-first glyphs, one per root item. */
    static const uint8_t ICONS[10][8] = {
        {0x10,0x10,0x10,0x10,0x38,0x38,0x7C,0x38}, /* Sensors: thermometer */
        {0x00,0x18,0x3C,0x42,0x42,0x3E,0x00,0x00}, /* Weather: cloud */
        {0xA8,0xA8,0xA8,0xA8,0xA8,0xA8,0xA8,0x00}, /* Settings: sliders */
        {0x3C,0x42,0x49,0x49,0x41,0x42,0x3C,0x00}, /* Watchface: clock */
        {0x18,0x24,0x3C,0x42,0x49,0x41,0x42,0x3C}, /* Stopwatch */
        {0x70,0x88,0x88,0x70,0x20,0x50,0x88,0x00}, /* Find Phone: magnifier */
        {0x38,0x28,0x20,0x20,0x20,0x60,0x70,0x00}, /* Music: note */
        {0x10,0x00,0x38,0x10,0x10,0x10,0x38,0x00}, /* System Info: i */
        {0x3C,0x42,0x42,0x42,0x24,0x18,0x18,0x00}, /* Flashlight: bulb */
        {0x10,0x30,0x7E,0x30,0x10,0x00,0x00,0x00}, /* Back: arrow */
    };
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
        if (is_selected) fb_fill_rect(0, y_pos, DISP_WIDTH, 10, 1);
        fb_draw_bitmap(1, y_pos + 1, 8, 8, ICONS[i], is_selected ? 0 : 1);
        fb_draw_text_ex(11, y_pos + 1, items[i], is_selected ? 0 : 1, -1);
        y_pos += 10;
    }

    // Scroll indicators
    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 5 < item_count) fb_draw_text(120, 54, "v");
}

static void render_watchface_menu(void) {
    fb_clear();
    
    fb_draw_header("WATCHFACE");
    
    const char *names[WATCHFACE_COUNT] = {
        "Digital",
        "Analog Style",
        "Minimal",
        "Compact",
        "Terminal",
        "Matrix",
        "Cats"
    };
    
    // Total items = WATCHFACE_COUNT + 1 (for Back)
    int total_items = WATCHFACE_COUNT + 1;
    
    int start_idx = watchface_selection - 2;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > total_items - 5) start_idx = total_items - 5;
    if (start_idx < 0) start_idx = 0;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 5 && i < total_items; i++) {
        bool is_selected = (i == watchface_selection);
        
        if (i < WATCHFACE_COUNT) {
            draw_menu_item(y_pos, names[i], is_selected);
        } else {
            draw_menu_item(y_pos, "[Back]", is_selected);
        }
        y_pos += 10;
    }

    if (start_idx > 0) fb_draw_text(120, 12, "^");
    if (start_idx + 5 < total_items) fb_draw_text(120, 54, "v");
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

    if (event == BTN_OK_LONG) {
        /* Long-press OK: one level up (second press exits edit mode first). */
        switch (current_menu) {
            case MENU_WATCH:
                break;
            case MENU_ROOT:
            case MENU_NOTIFICATION:
                current_menu = MENU_WATCH;
                break;
            case MENU_SYNC_WAIT:
                current_menu = MENU_SETTINGS;
                break;
            case MENU_SETTINGS:
                if (editing_mode) {
                    editing_mode = false;
                    settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
                    settings_save_units(time_format_editable, temp_unit_editable);
                    alarm_set(alarm_hour_editable, alarm_min_editable, alarm_enabled_editable);
                } else {
                    current_menu = MENU_ROOT;
                }
                break;
            case MENU_FLASHLIGHT:
                if (sh1106_set_contrast(saved_brightness) != ESP_OK) {
                    ESP_LOGW(TAG, "flashlight restore failed");
                }
                current_menu = MENU_ROOT;
                break;
            default:
                current_menu = MENU_ROOT;
                break;
        }
        return true;
    }

    if (current_menu == MENU_SYNC_WAIT) {
        // Any button press exits sync wait
        current_menu = MENU_SETTINGS;
        return true;
    }

    if (event == BTN_UP_PRESS) {
        if (current_menu == MENU_ROOT) {
            root_selection = (root_selection > 0) ? root_selection - 1 : 9;
        } else if (current_menu == MENU_WATCHFACE) {
            watchface_selection = (watchface_selection > 0) ? watchface_selection - 1 : WATCHFACE_COUNT;
        } else if (current_menu == MENU_FIND_PHONE) {
            find_phone_selection = (find_phone_selection > 0) ? find_phone_selection - 1 : 2;
        } else if (current_menu == MENU_MUSIC_CONTROL) {
            music_selection = (music_selection > 0) ? music_selection - 1 : 3;
        } else if (current_menu == MENU_NOTIFICATION) {
            if (notif_scroll > 0) notif_scroll--;
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                motion_threshold_editable += 10;
                if (motion_threshold_editable > 500) motion_threshold_editable = 500;
            } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                screen_timeout_editable += 1;
                if (screen_timeout_editable > 60) screen_timeout_editable = 60;
            } else if (current_setting == SETTINGS_BRIGHTNESS) {
                brightness_editable += 10;
                if (brightness_editable > 255) brightness_editable = 255;
                esp_err_t cerr = sh1106_set_contrast((uint8_t)brightness_editable);
                if (cerr != ESP_OK) {
                    ESP_LOGW(TAG, "set_contrast failed: %s", esp_err_to_name(cerr));
                }
            } else if (current_setting == SETTINGS_ALARM_HOUR) {
                alarm_hour_editable = (alarm_hour_editable + 1) % 24;
            } else if (current_setting == SETTINGS_ALARM_MIN) {
                alarm_min_editable = (alarm_min_editable + 1) % 60;
            } else if (current_setting == SETTINGS_RESET) {
                editing_mode = false; // cancel confirm
            }
        } else if (current_menu == MENU_SETTINGS) {
            current_setting = (current_setting > 0) ? current_setting - 1 : SETTINGS_COUNT - 1;
        } else if (current_menu == MENU_SENSOR_DATA) {
            current_sensor = (current_sensor > 0) ? current_sensor - 1 : SENSOR_COUNT - 1;
        }
    } else if (event == BTN_DOWN_PRESS) {
        if (current_menu == MENU_ROOT) {
            root_selection = (root_selection < 9) ? root_selection + 1 : 0;
        } else if (current_menu == MENU_WATCHFACE) {
            watchface_selection = (watchface_selection < WATCHFACE_COUNT) ? watchface_selection + 1 : 0;
        } else if (current_menu == MENU_FIND_PHONE) {
            find_phone_selection = (find_phone_selection < 2) ? find_phone_selection + 1 : 0;
        } else if (current_menu == MENU_MUSIC_CONTROL) {
            music_selection = (music_selection < 3) ? music_selection + 1 : 0;
        } else if (current_menu == MENU_NOTIFICATION) {
            int mx = notif_max_scroll();
            if (notif_scroll < mx) notif_scroll++;
        } else if (current_menu == MENU_STOPWATCH) {
            if (!stopwatch_running) {
                stopwatch_elapsed_time = 0; // Reset
            }
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                motion_threshold_editable -= 10;
                if (motion_threshold_editable < 10) motion_threshold_editable = 10;
            } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                screen_timeout_editable -= 1;
                if (screen_timeout_editable < 1) screen_timeout_editable = 1;
            } else if (current_setting == SETTINGS_BRIGHTNESS) {
                brightness_editable -= 10;
                if (brightness_editable < 0) brightness_editable = 0;
                esp_err_t cerr = sh1106_set_contrast((uint8_t)brightness_editable);
                if (cerr != ESP_OK) {
                    ESP_LOGW(TAG, "set_contrast failed: %s", esp_err_to_name(cerr));
                }
            } else if (current_setting == SETTINGS_ALARM_HOUR) {
                alarm_hour_editable = (alarm_hour_editable + 23) % 24;
            } else if (current_setting == SETTINGS_ALARM_MIN) {
                alarm_min_editable = (alarm_min_editable + 59) % 60;
            } else if (current_setting == SETTINGS_RESET) {
                editing_mode = false; // cancel confirm
            }
        } else if (current_menu == MENU_SETTINGS) {
            if (current_setting < SETTINGS_COUNT - 1) current_setting++;
            else if (!editing_mode) current_setting = 0;
        } else if (current_menu == MENU_SENSOR_DATA) {
            current_sensor = (current_sensor < SENSOR_COUNT - 1) ? current_sensor + 1 : 0;
        }
    } else if (event == BTN_OK_PRESS) {
        if (current_menu == MENU_WATCH) {
            current_menu = MENU_ROOT;
            root_selection = 0;
        } else if (current_menu == MENU_ROOT) {
            if (root_selection == 0) current_menu = MENU_SENSOR_DATA;
            else if (root_selection == 1) {
                current_menu = MENU_WEATHER;
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
                if (sh1106_set_contrast(255) != ESP_OK) {
                    ESP_LOGW(TAG, "flashlight contrast failed");
                }
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
             if (sh1106_set_contrast(saved_brightness) != ESP_OK) {
                 ESP_LOGW(TAG, "flashlight restore failed");
             }
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
            current_menu = MENU_ROOT; // Back
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
            } else if (current_setting == SETTINGS_TIME_FORMAT) {
                time_format_editable = time_format_editable ? 0 : 1;
                settings_save_units(time_format_editable, temp_unit_editable);
            } else if (current_setting == SETTINGS_TEMP_UNIT) {
                temp_unit_editable = temp_unit_editable ? 0 : 1;
                settings_save_units(time_format_editable, temp_unit_editable);
            } else if (current_setting == SETTINGS_ALARM_TOGGLE) {
                alarm_enabled_editable = !alarm_enabled_editable;
                alarm_set(alarm_hour_editable, alarm_min_editable, alarm_enabled_editable);
            } else if (current_setting == SETTINGS_REBOOT) {
                ESP_LOGI(TAG, "Reboot requested");
                esp_restart();
            } else if (current_setting == SETTINGS_POWER_OFF) {
                ESP_LOGI(TAG, "Power Off requested");
                fb_clear();
                fb_draw_text(10, 30, "Powering Off...");
                sh1106_render();
                vTaskDelay(pdMS_TO_TICKS(300));
                power_enter_deep_sleep();
            } else if (current_setting == SETTINGS_RESET) {
                // First OK enters confirm; second OK applies
                editing_mode = true;
            } else {
                editing_mode = true;
            }
        } else if (current_menu == MENU_SETTINGS && editing_mode) {
            if (current_setting == SETTINGS_RESET) {
                if (settings_reset() == ESP_OK) {
                    settings_load(&motion_threshold_editable, &screen_timeout_editable,
                                  (int*)&current_watchface, &brightness_editable);
                    settings_load_units(&time_format_editable, &temp_unit_editable);
                    alarm_set(7, 0, false);
                    alarm_get(&alarm_hour_editable, &alarm_min_editable, &alarm_enabled_editable);
                    if (sh1106_set_contrast((uint8_t)brightness_editable) != ESP_OK) {
                        ESP_LOGW(TAG, "contrast restore failed");
                    }
                    ESP_LOGI(TAG, "Settings restored to defaults");
                }
                editing_mode = false;
            } else {
                editing_mode = false;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface, brightness_editable);
                settings_save_units(time_format_editable, temp_unit_editable);
                alarm_set(alarm_hour_editable, alarm_min_editable, alarm_enabled_editable);
            }
        } else if (current_menu == MENU_SENSOR_DATA) {
            if (current_sensor == SENSOR_BACK) {
                current_menu = MENU_ROOT;
            }
        } else if (current_menu == MENU_NOTIFICATION) {
             // OK dismisses (UP/DN scroll)
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
