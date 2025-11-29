// watchfaces.h - prototypes for watchface renderers

#ifndef WATCHFACES_H
#define WATCHFACES_H

#include <stdint.h>
#include <time.h>

void render_watchface_digital(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);
void render_watchface_analog(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);
void render_watchface_minimal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);
void render_watchface_compact(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);
void render_watchface_terminal(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);
void render_watchface_matrix(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);
void render_watchface_cats(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);

#endif // WATCHFACES_H
