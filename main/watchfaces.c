#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "display.h"
#include "watchfaces.h"
#include "menu.h"
#include "pedometer.h"
#include "weather.h"
#include "alarm.h"
#include "esp_timer.h"
#include "ble_manager.h"

/* Display prefs helpers (backed by menu settings). */
static int disp_hour(int hour24) {
    if (menu_get_time_format() == 1) {
        int h = hour24 % 12;
        return (h == 0) ? 12 : h;
    }
    return hour24;
}

static float disp_temp(float temp_c) {
    return (menu_get_temp_unit() == 1) ? (temp_c * 9.0f / 5.0f + 32.0f) : temp_c;
}

static char temp_unit_char(void) {
    return (menu_get_temp_unit() == 1) ? 'F' : 'C';
}

/* ---------- Shared status bar + icons (9px inverted header) ---------- */
/* 8x8 MSB-first bitmaps, drawn black-on-white in the status bar. */
static const uint8_t ICON_BT[8]   = {0x10,0x18,0x28,0x48,0x48,0x28,0x18,0x10};
static const uint8_t ICON_BELL[8] = {0x10,0x38,0x44,0x44,0x44,0x7C,0x10,0x00};
static const uint8_t ICON_MAIL[8] = {0x7E,0x41,0x5A,0x55,0x55,0x5A,0x41,0x7E};

static void draw_cloud(int x, int y) {
    fb_fill_circle(x + 3, y + 4, 3, 1);
    fb_fill_circle(x + 8, y + 3, 4, 1);
    fb_fill_rect(x + 3, y + 4, 9, 4, 1);
}

/* 12x10 weather glyph drawn procedurally (no font needed). */
static void draw_wx_icon(int x, int y, int code) {
    if (code <= 1) { /* Clear: sun */
        fb_fill_circle(x + 5, y + 4, 3, 1);
        fb_draw_line(x + 5, y - 1, x + 5, y + 1, 1);
        fb_draw_line(x + 5, y + 7, x + 5, y + 9, 1);
        fb_draw_line(x, y + 4, x + 2, y + 4, 1);
        fb_draw_line(x + 8, y + 4, x + 10, y + 4, 1);
    } else if (code <= 3) { /* Cloudy */
        draw_cloud(x, y);
    } else if (code == 45 || code == 48) { /* Fog */
        draw_cloud(x, y - 1);
        fb_draw_line(x + 1, y + 8, x + 10, y + 8, 1);
        fb_draw_line(x + 3, y + 10, x + 10, y + 10, 1);
    } else if (code >= 95) { /* Storm */
        draw_cloud(x, y - 1);
        fb_draw_line(x + 6, y + 7, x + 4, y + 9, 1);
        fb_draw_line(x + 4, y + 9, x + 7, y + 9, 1);
        fb_draw_line(x + 7, y + 9, x + 5, y + 11, 1);
    } else if (code >= 71 && code <= 75) { /* Snow */
        draw_cloud(x, y - 1);
        fb_set_pixel(x + 3, y + 9, 1);
        fb_set_pixel(x + 6, y + 10, 1);
        fb_set_pixel(x + 9, y + 9, 1);
    } else if ((code >= 51 && code <= 65) || (code >= 80 && code <= 82)) { /* Rain/drizzle */
        draw_cloud(x, y - 1);
        fb_draw_line(x + 3, y + 8, x + 2, y + 10, 1);
        fb_draw_line(x + 6, y + 8, x + 5, y + 10, 1);
        fb_draw_line(x + 9, y + 8, x + 8, y + 10, 1);
    } else {
        draw_cloud(x, y);
    }
}

/* Unified 9px header: HH:MM left, BT/alarm/mail icons, battery right.
 * All faces call this first, then lay out content below y=10. */
static void draw_status_bar(struct tm *timeinfo, int batt_pct) {
    fb_fill_rect(0, 0, DISP_WIDTH, 9, 1);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    fb_draw_text_ex(2, 1, tbuf, 0, -1);

    int ix = 34;
    if (ble_manager_is_connected()) {
        fb_draw_bitmap(ix, 0, 8, 8, ICON_BT, 0);
        ix += 10;
    }
    int ah = 0, am = 0;
    bool aen = false;
    alarm_get(&ah, &am, &aen);
    if (aen) {
        fb_draw_bitmap(ix, 0, 8, 8, ICON_BELL, 0);
        ix += 10;
    }
    ble_notification_t n;
    if (ble_manager_get_last_notification(&n, false) && n.has_data) {
        fb_draw_bitmap(ix, 0, 8, 8, ICON_MAIL, 0);
        if (n.has_unread) fb_fill_rect(ix + 6, 0, 3, 3, 0);
        ix += 10;
    }

    /* Battery right: icon at 109 + right-aligned pct text. Blink when low. */
    bool low = (batt_pct <= 15);
    bool blink_off = low && (timeinfo->tm_sec % 2 == 1);
    const int bx = DISP_WIDTH - 14 - 2;
    if (!blink_off) {
        fb_draw_battery_icon(bx, 1, batt_pct, 0, 1);
        char bbuf[8];
        snprintf(bbuf, sizeof(bbuf), "%d%%", batt_pct);
        int blen = strlen(bbuf);
        fb_draw_text_ex(bx - blen * 6 - 2, 1, bbuf, 0, -1);
    } else {
        fb_draw_battery_icon(bx, 1, 0, 0, 1);
    }
}




/* Digital watchface - big time display (original) */
void render_watchface_digital(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    draw_status_bar(timeinfo, batt_pct);
    char buf[64];

    // Time: HH:MM big using scaled text (scale 2), colon blinks each second
    if (timeinfo->tm_sec % 2 == 0)
        snprintf(buf, sizeof(buf), "%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    else
        snprintf(buf, sizeof(buf), "%02d %02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    fb_draw_text_centered_scaled(12, buf, 2);

    // Seconds tucked to the right of the big digits
    char ssec[4];
    snprintf(ssec, sizeof(ssec), "%02d", timeinfo->tm_sec);
    fb_draw_text(98, 19, ssec);

    // Date: Day Mon Year in Turkish
    const char *months_tr[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};
    snprintf(buf, sizeof(buf), "%02d %s %04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    fb_draw_text_centered(30, buf);

    // Separator line
    fb_draw_line(10, 39, 118, 39, 1);

    // Weather icon + temp/hum lower area
    weather_data_t w = weather_get_current();
    if (w.valid) {
        draw_wx_icon(0, 42, w.weather_code);
        float wt = disp_temp(w.temp_c);
        snprintf(buf, sizeof(buf), "%.0f%c H:%.0f%%", wt, temp_unit_char(), hum);
        fb_draw_text(14, 44, buf);
    } else {
        snprintf(buf, sizeof(buf), "T:%.1f%c H:%.0f%%", disp_temp(temp), temp_unit_char(), hum);
        fb_draw_text(0, 44, buf);
    }

    // Steps instead of raw accel
    int steps = pedometer_get_steps();
    snprintf(buf, sizeof(buf), "Steps:%d", steps);
    fb_draw_text(0, 54, buf);

    // Unread notification hint (icon already in status bar)
    ble_notification_t notif;
    if (ble_manager_get_last_notification(&notif, false) && notif.has_unread) {
        snprintf(buf, sizeof(buf), "MSG*");
        fb_draw_text(92, 54, buf);
    }
}

/* Analog-inspired watchface: dial left, digital info right */
void render_watchface_analog(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)temp; (void)hum; (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    draw_status_bar(timeinfo, batt_pct);
    char buf[64];

    const int cx = 34, cy = 38, R = 24;
    fb_draw_circle(cx, cy, R, 1);
    fb_draw_circle(cx, cy, R - 1, 1);

    /* 12 tick marks */
    for (int h = 0; h < 12; h++) {
        float a = (h * 30.0f - 90.0f) * 3.14159f / 180.0f;
        int inner = (h % 3 == 0) ? R - 6 : R - 4;
        int x0 = cx + (int)(inner * cosf(a));
        int y0 = cy + (int)(inner * sinf(a));
        int x1 = cx + (int)((R - 2) * cosf(a));
        int y1 = cy + (int)((R - 2) * sinf(a));
        fb_draw_line(x0, y0, x1, y1, 1);
    }

    /* Hands */
    float h_angle = ((timeinfo->tm_hour % 12) + timeinfo->tm_min / 60.0f) * 30.0f;
    float rad = (h_angle - 90.0f) * 3.14159f / 180.0f;
    fb_draw_line_thick(cx, cy, cx + (int)(12 * cosf(rad)), cy + (int)(12 * sinf(rad)), 3, 1);

    float m_angle = (timeinfo->tm_min + timeinfo->tm_sec / 60.0f) * 6.0f;
    rad = (m_angle - 90.0f) * 3.14159f / 180.0f;
    fb_draw_line_thick(cx, cy, cx + (int)(19 * cosf(rad)), cy + (int)(19 * sinf(rad)), 2, 1);

    float s_angle = timeinfo->tm_sec * 6.0f;
    rad = (s_angle - 90.0f) * 3.14159f / 180.0f;
    int sx = cx + (int)(20 * cosf(rad));
    int sy = cy + (int)(20 * sinf(rad));
    fb_draw_line(cx, cy, sx, sy, 1);
    /* tail for balance */
    fb_draw_line(cx, cy, cx - (int)(5 * cosf(rad)), cy - (int)(5 * sinf(rad)), 1);
    fb_fill_circle(cx, cy, 2, 1);

    /* Right info panel */
    snprintf(buf, sizeof(buf), "%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    fb_draw_text(72, 14, buf);

    snprintf(buf, sizeof(buf), "%02d/%02d", timeinfo->tm_mday, timeinfo->tm_mon + 1);
    fb_draw_text(72, 24, buf);

    int steps = pedometer_get_steps();
    if (steps > 9999) snprintf(buf, sizeof(buf), "%dk", steps / 1000);
    else snprintf(buf, sizeof(buf), "Stp:%d", steps);
    fb_draw_text(72, 34, buf);

    weather_data_t w = weather_get_current();
    if (w.valid) {
        draw_wx_icon(70, 44, w.weather_code);
        float wt = disp_temp(w.temp_c);
        snprintf(buf, sizeof(buf), "%.0f%c", wt, temp_unit_char());
        fb_draw_text(84, 47, buf);
    } else {
        snprintf(buf, sizeof(buf), "%.0f%c", disp_temp(temp), temp_unit_char());
        fb_draw_text(72, 47, buf);
    }
}

/* Minimal watchface - time only */
void render_watchface_minimal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)temp; (void)hum; (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    draw_status_bar(timeinfo, batt_pct);
    char buf[64];
    const char *months_tr[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};

    // Extra large time (scale 3), colon blinks
    if (timeinfo->tm_sec % 2 == 0)
        snprintf(buf, sizeof(buf), "%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    else
        snprintf(buf, sizeof(buf), "%02d %02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    fb_draw_text_centered_scaled(14, buf, 3);

    // Date at bottom
    snprintf(buf, sizeof(buf), "%02d %s %04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    fb_draw_text_centered(52, buf);
}

/* Compact watchface - all info compact */
void render_watchface_compact(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    draw_status_bar(timeinfo, batt_pct);
    char buf[64];
    const char *months_tr[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};

    // Date
    snprintf(buf, sizeof(buf), "%02d-%s-%04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    fb_draw_text(0, 12, buf);

    // Temperature and humidity + weather icon right
    weather_data_t w = weather_get_current();
    if (w.valid) draw_wx_icon(114, 22, w.weather_code);
    snprintf(buf, sizeof(buf), "T:%.1f%c H:%.0f%%", disp_temp(temp), temp_unit_char(), hum);
    fb_draw_text(0, 24, buf);

    // Steps
    int steps = pedometer_get_steps();
    snprintf(buf, sizeof(buf), "Steps:%d", steps);
    fb_draw_text(0, 36, buf);

    // Day of week
    const char *days[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(buf, sizeof(buf), "%s", days[timeinfo->tm_wday]);
    fb_draw_text(0, 48, buf);

    ble_notification_t notif;
    if (ble_manager_get_last_notification(&notif, false) && notif.has_data) {
        char notif_line[16];
        snprintf(notif_line, sizeof(notif_line), "%.15s",
                 notif.title[0] ? notif.title : "New msg");
        fb_draw_text(36, 48, notif_line);
    }
}

/* Terminal watchface - retro command line style */
void render_watchface_terminal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    draw_status_bar(timeinfo, batt_pct);
    char buf[64];

    // Line 1: Time
    snprintf(buf, sizeof(buf), ">TIME %02d:%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min, timeinfo->tm_sec);
    fb_draw_text(0, 12, buf);

    // Line 2: Date
    snprintf(buf, sizeof(buf), ">DATE %04d-%02d-%02d", 1900 + timeinfo->tm_year, timeinfo->tm_mon + 1, timeinfo->tm_mday);
    fb_draw_text(0, 22, buf);

    // Line 3: Env + weather code
    weather_data_t w = weather_get_current();
    if (w.valid)
        snprintf(buf, sizeof(buf), ">ENV %.0f%c %s", disp_temp(w.temp_c), temp_unit_char(), weather_get_desc(w.weather_code));
    else
        snprintf(buf, sizeof(buf), ">ENV %.1f%c %.0f%%", disp_temp(temp), temp_unit_char(), hum);
    buf[21] = '\0'; /* 128px = 21 chars */
    fb_draw_text(0, 32, buf);

    // Line 4: Link status with blinking cursor
    const char *link = ble_manager_is_connected() ? "LINK:BT" : "LINK:--";
    if (timeinfo->tm_sec % 2 == 0)
        snprintf(buf, sizeof(buf), ">%s_", link);
    else
        snprintf(buf, sizeof(buf), ">%s ", link);
    fb_draw_text(0, 42, buf);

    // Line 5: steps
    snprintf(buf, sizeof(buf), ">STP %d", pedometer_get_steps());
    fb_draw_text(0, 52, buf);
}

/* Matrix watchface - digital rain inspired */
void render_watchface_matrix(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)temp; (void)hum; (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    char buf[64];

    /* Deterministic pseudo-random based on position + 200ms tick so identical
     * frames produce identical framebuffers and the dirty check can suppress
     * redundant I2C pushes (rand() would churn every frame). */
    int64_t tick = esp_timer_get_time() / 200000;

    for (int y = 0; y < DISP_HEIGHT; y += 10) {
        for (int x = 0; x < DISP_WIDTH; x += 12) {
             if (((x + y + timeinfo->tm_sec) / 10) % 2 == 0) {
                 uint32_t h = (uint32_t)(x * 73856093u) ^ (uint32_t)(y * 19349663u) ^ (uint32_t)(tick * 83492791u);
                 char c = (char)(33 + (h % 90));
                 char str[2] = {c, 0};
                 fb_draw_text(x, y, str);
             }
        }
    }

    // Draw box for time
    int box_w = 80;
    int box_h = 24;
    int box_x = (DISP_WIDTH - box_w) / 2;
    int box_y = (DISP_HEIGHT - box_h) / 2;

    fb_fill_rect(box_x, box_y, box_w, box_h, 0); // Clear box (black)
    fb_draw_rect(box_x, box_y, box_w, box_h, 1); // White border

    snprintf(buf, sizeof(buf), "%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    int len = strlen(buf);
    int char_width = 6 * 2;
    int tx = box_x + (box_w - (len * char_width)) / 2;
    int ty = box_y + 5;
    fb_draw_text_scaled(tx, ty, buf, 2);

    /* Opaque status bar on top so icons stay readable over rain */
    draw_status_bar(timeinfo, batt_pct);
}

/* Cat Bitmaps 16x16 (Improved) */
// Frame 1
static const uint8_t cat_f1[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x3C, 0x00, 0x3C, 0x10, 0x3C,
    0x18, 0x7C, 0x1F, 0xFC, 0x0F, 0xF8, 0x07, 0xF0, 0x04, 0x10, 0x04, 0x10, 0x00, 0x00, 0x00, 0x00
};
// Frame 2
static const uint8_t cat_f2[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x3C, 0x00, 0x3C, 0x10, 0x3C,
    0x18, 0x7C, 0x1F, 0xFC, 0x0F, 0xF8, 0x07, 0xF0, 0x02, 0x08, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00
};

static void draw_bitmap_16x16(int x, int y, const uint8_t *bitmap) {
    for (int r = 0; r < 16; r++) {
        uint16_t row_data = (bitmap[r*2] << 8) | bitmap[r*2+1];
        for (int c = 0; c < 16; c++) {
            if (row_data & (1 << (15-c))) {
                fb_set_pixel(x + c, y + r, 1);
            }
        }
    }
}

/* Cat watchface - animated cat */
void render_watchface_cats(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    (void)temp; (void)hum; (void)ax; (void)ay; (void)az; (void)batt_mv;
    fb_clear();
    draw_status_bar(timeinfo, batt_pct);
    char buf[64];

    // Time below status bar
    snprintf(buf, sizeof(buf), "%02d:%02d", disp_hour(timeinfo->tm_hour), timeinfo->tm_min);
    fb_draw_text_centered_scaled(12, buf, 2);

    // Date bottom center
    snprintf(buf, sizeof(buf), "%02d-%02d-%04d", timeinfo->tm_mday, timeinfo->tm_mon + 1, 1900 + timeinfo->tm_year);
    fb_draw_text_centered(52, buf);
    
    // Animated Cat
    // Move across screen every 5 seconds
    int64_t now = esp_timer_get_time() / 1000; // ms
    int cycle_ms = 5000;
    int pos_ms = now % cycle_ms;
    
    // X position: -16 to 128
    int cat_x = ((pos_ms * (DISP_WIDTH + 32)) / cycle_ms) - 16;
    int cat_y = 30;

    // Animation frame: switch every 200ms
    int frame = (now / 200) % 2;

    if (frame == 0) {
        draw_bitmap_16x16(cat_x, cat_y, cat_f1);
    } else {
        draw_bitmap_16x16(cat_x, cat_y, cat_f2);
    }
}
