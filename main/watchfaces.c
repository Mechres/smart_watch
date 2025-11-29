#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "display.h"
#include "watchfaces.h"
#include "pedometer.h"
#include "weather.h"
#include "esp_timer.h"




/* Digital watchface - big time display (original) */
void render_watchface_digital(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    fb_clear();
    char buf[64];

    // Time: HH:MM big using scaled text (scale 2)
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    int len = strlen(buf);
    int char_width = 6 * 2; // 5x7 font + 1 spacing * scale 2
    int x = (DISP_WIDTH - (len * char_width)) / 2;
    fb_draw_text_scaled(x, 5, buf, 2);

    // Date: Day Mon Year in Turkish
    const char *months_tr[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};
    snprintf(buf, sizeof(buf), "%02d %s %04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    len = strlen(buf);
    x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 25, buf);

    // Separator line
    fb_draw_line(10, 35, 118, 35, 1);

    // Sensors lower area
    snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", temp, hum);
    fb_draw_text(0, 40, buf);
    
    // Steps instead of raw accel
    int steps = pedometer_get_steps();
    snprintf(buf, sizeof(buf), "Steps: %d", steps);
    fb_draw_text(0, 50, buf);



    // Battery top right
    snprintf(buf, sizeof(buf), "%d%%", batt_pct);
    fb_draw_text(100, 0, buf);
}

/* Analog-inspired watchface */
void render_watchface_analog(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    
    // Draw clock face (simple markers)
    int cx = 64, cy = 32;
    // 12, 3, 6, 9 markers
    fb_draw_line(cx, cy - 30, cx, cy - 25, 1); // 12
    fb_draw_line(cx + 30, cy, cx + 25, cy, 1); // 3
    fb_draw_line(cx, cy + 30, cx, cy + 25, 1); // 6
    fb_draw_line(cx - 30, cy, cx - 25, cy, 1); // 9

    // Hands (simple math, approximate)
    // Hour hand
    float h_angle = (timeinfo->tm_hour % 12 + timeinfo->tm_min / 60.0) * 30.0; // 360/12
    // Convert to radians: deg * PI / 180. PI ~ 3.14159
    float rad = (h_angle - 90) * 3.14159 / 180.0;
    int hx = cx + (int)(15 * cos(rad));
    int hy = cy + (int)(15 * sin(rad));
    fb_draw_line(cx, cy, hx, hy, 1);

    // Minute hand
    float m_angle = timeinfo->tm_min * 6.0; // 360/60
    rad = (m_angle - 90) * 3.14159 / 180.0;
    int mx = cx + (int)(25 * cos(rad));
    int my = cy + (int)(25 * sin(rad));
    fb_draw_line(cx, cy, mx, my, 1);
    
    // Digital time small at bottom
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    fb_draw_text(90, 54, buf);

    // Date top left
    snprintf(buf, sizeof(buf), "%02d/%02d", timeinfo->tm_mday, timeinfo->tm_mon + 1);
    fb_draw_text(0, 0, buf);

    // Steps bottom left
    int steps = pedometer_get_steps();
    snprintf(buf, sizeof(buf), "Stp:%d", steps);
    fb_draw_text(0, 54, buf);
/*
    // Weather info
    weather_data_t w = weather_get_current();
    if (w.valid) {
        const char *desc = weather_get_desc(w.weather_code);
        snprintf(buf, sizeof(buf), "%.1fC %s", w.temp_c, desc);
        fb_draw_text(0, 60, buf);
    }
        */
}

/* Minimal watchface - time only */
void render_watchface_minimal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    const char *months_tr[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};

    // Extra large time (scale 3)

    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    int len = strlen(buf);
    int char_width = 6 * 3;
    int x = (DISP_WIDTH - (len * char_width)) / 2;
    fb_draw_text_scaled(x, 10, buf, 3);

    // Date at bottom
    snprintf(buf, sizeof(buf), "%02d %s %04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    len = strlen(buf);
    x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 50, buf);
}

/* Compact watchface - all info compact */
void render_watchface_compact(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    const char *months_tr[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};

    // Header bar
    fb_fill_rect(0, 0, DISP_WIDTH, 9, 1);
    snprintf(buf, sizeof(buf), "%02d:%02d  %d%%", timeinfo->tm_hour, timeinfo->tm_min, batt_pct);
    fb_draw_text_ex(2, 1, buf, 0, -1);

    // Date
    snprintf(buf, sizeof(buf), "%02d-%s-%04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    fb_draw_text(0, 12, buf);

    // Temperature and humidity
    snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", temp, hum);
    fb_draw_text(0, 24, buf);

    // Steps
    int steps = pedometer_get_steps();
    snprintf(buf, sizeof(buf), "Steps: %d", steps);
    fb_draw_text(0, 36, buf);



    // Day of week
    const char *days[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(buf, sizeof(buf), "%s", days[timeinfo->tm_wday]);
    fb_draw_text(0, 48, buf);
}

/* Terminal watchface - retro command line style */
void render_watchface_terminal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    
    // Line 1: Time
    snprintf(buf, sizeof(buf), "> TIME: %02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    fb_draw_text(0, 0, buf);
    
    // Line 2: Date
    snprintf(buf, sizeof(buf), "> DATE: %04d-%02d-%02d", 1900 + timeinfo->tm_year, timeinfo->tm_mon + 1, timeinfo->tm_mday);
    fb_draw_text(0, 12, buf);
    
    // Line 3: Battery
    snprintf(buf, sizeof(buf), "> BATT: %d%% [%dmV]", batt_pct, batt_mv);
    fb_draw_text(0, 24, buf);
    
    // Line 4: Env
    snprintf(buf, sizeof(buf), "> ENV : %.1fC %.0f%%", temp, hum);
    fb_draw_text(0, 36, buf);

    // Line 5: Status with blinking cursor
    if (timeinfo->tm_sec % 2 == 0) {
        fb_draw_text(0, 48, "> STAT: ONLINE_");
    } else {
        fb_draw_text(0, 48, "> STAT: ONLINE ");
    }
}

/* Matrix watchface - digital rain inspired */
void render_watchface_matrix(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    
    // Draw random characters background
    // Draw columns of random chars
    for (int y = 0; y < DISP_HEIGHT; y += 10) {
        for (int x = 0; x < DISP_WIDTH; x += 12) {
             // Simple pseudo-random pattern based on time and position to avoid static noise
             if (((x + y + timeinfo->tm_sec) / 10) % 2 == 0) { 
                 char c = 33 + (rand() % 90); // Random printable char
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
    
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    int len = strlen(buf);
    int char_width = 6 * 2;
    int tx = box_x + (box_w - (len * char_width)) / 2;
    int ty = box_y + 5;
    fb_draw_text_scaled(tx, ty, buf, 2);
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
    fb_clear();
    char buf[64];
    
    // Time top center
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    int len = strlen(buf);
    int char_width = 6 * 2;
    int tx = (DISP_WIDTH - (len * char_width)) / 2;
    fb_draw_text_scaled(tx, 5, buf, 2);
    
    // Date bottom center
    snprintf(buf, sizeof(buf), "%02d-%02d-%04d", timeinfo->tm_mday, timeinfo->tm_mon + 1, 1900 + timeinfo->tm_year);
    len = strlen(buf);
    int dx = (DISP_WIDTH - (len * 6)) / 2;
    fb_draw_text(dx, 50, buf);
    
    // Animated Cat
    // Move across screen every 5 seconds
    int64_t now = esp_timer_get_time() / 1000; // ms
    int cycle_ms = 5000;
    int pos_ms = now % cycle_ms;
    
    // X position: -16 to 128
    int cat_x = ((pos_ms * (DISP_WIDTH + 32)) / cycle_ms) - 16;
    int cat_y = 28;
    
    // Animation frame: switch every 200ms
    int frame = (now / 200) % 2;
    
    if (frame == 0) {
        draw_bitmap_16x16(cat_x, cat_y, cat_f1);
    } else {
        draw_bitmap_16x16(cat_x, cat_y, cat_f2);
    }
    
    // Battery
    snprintf(buf, sizeof(buf), "%d%%", batt_pct);
    fb_draw_text(100, 0, buf);
}
