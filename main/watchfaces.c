// watchfaces.c - implementations for various watchfaces

#include <stdio.h>
#include <string.h>
#include "display.h"
#include "watchfaces.h"

/* Digital watchface - big time display (original) */
void render_watchface_digital(float temp, float hum, int16_t ax, int16_t ay, int16_t az, struct tm *timeinfo) {
    fb_clear();
    char buf[64];

    // Time: HH:MM big by printing twice vertically
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    int len = strlen(buf);
    int x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 0, buf);
    fb_draw_text(x, 8, buf);

    // Date: Day Mon Year in Turkish
    const char *months_tr[12] = {"Oca","Şub","Mar","Nis","May","Haz","Tem","Ağu","Eyl","Eki","Kas","Ara"};
    snprintf(buf, sizeof(buf), "%02d %s %04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    len = strlen(buf);
    x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 20, buf);

    // Sensors lower area
    snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", temp, hum);
    fb_draw_text(0, 34, buf);
    snprintf(buf, sizeof(buf), "X:%d Y:%d", ax, ay);
    fb_draw_text(0, 44, buf);
    snprintf(buf, sizeof(buf), "Z:%d", az);
    fb_draw_text(80, 44, buf);
}

/* Analog-inspired watchface */
void render_watchface_analog(float temp, float hum, int16_t ax, int16_t ay, int16_t az, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    const char *months_tr[12] = {"Oca","Şub","Mar","Nis","May","Haz","Tem","Ağu","Eyl","Eki","Kas","Ara"};

    // Large time in center
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    int len = strlen(buf);
    int x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 8, buf);

    // Date below
    snprintf(buf, sizeof(buf), "%s %02d, %04d", months_tr[timeinfo->tm_mon], timeinfo->tm_mday, 1900 + timeinfo->tm_year);
    len = strlen(buf);
    x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 22, buf);

    // Day of week
    const char *days[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(buf, sizeof(buf), "%s", days[timeinfo->tm_wday]);
    x = (DISP_WIDTH - (strlen(buf)*6)) / 2;
    fb_draw_text(x, 32, buf);

    // Temperature
    snprintf(buf, sizeof(buf), "Temp: %.1fC  Hum: %.0f%%", temp, hum);
    fb_draw_text(0, 45, buf);
}

/* Minimal watchface - time only */
void render_watchface_minimal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    const char *months_tr[12] = {"Oca","Şub","Mar","Nis","May","Haz","Tem","Ağu","Eyl","Eki","Kas","Ara"};

    // Extra large time (triple)
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    int len = strlen(buf);
    int x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 5, buf);
    fb_draw_text(x, 13, buf);
    fb_draw_text(x, 21, buf);

    // Date at bottom
    snprintf(buf, sizeof(buf), "%02d %s %04d", timeinfo->tm_mday, months_tr[timeinfo->tm_mon], 1900 + timeinfo->tm_year);
    len = strlen(buf);
    x = (DISP_WIDTH - (len*6)) / 2;
    fb_draw_text(x, 50, buf);
}

/* Compact watchface - all info compact */
void render_watchface_compact(float temp, float hum, int16_t ax, int16_t ay, int16_t az, struct tm *timeinfo) {
    fb_clear();
    char buf[64];
    const char *months_tr[12] = {"Oca","Şub","Mar","Nis","May","Haz","Tem","Ağu","Eyl","Eki","Kas","Ara"};

    // Time and date on first line
    snprintf(buf, sizeof(buf), "%02d:%02d %02d-%s", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_mday, months_tr[timeinfo->tm_mon]);
    fb_draw_text(0, 0, buf);

    // Temperature and humidity
    snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", temp, hum);
    fb_draw_text(0, 12, buf);

    // Acceleration
    snprintf(buf, sizeof(buf), "X:%d Y:%d Z:%d", ax, ay, az);
    fb_draw_text(0, 24, buf);

    // Day of week and week number
    const char *days[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    snprintf(buf, sizeof(buf), "%s", days[timeinfo->tm_wday]);
    fb_draw_text(0, 36, buf);

    // Motion status (simple)
    snprintf(buf, sizeof(buf), "Motion: Active");
    fb_draw_text(0, 48, buf);
}
