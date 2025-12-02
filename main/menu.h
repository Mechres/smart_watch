#ifndef MENU_H
#define MENU_H

#include "input.h"
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

/* Initialize menu state */
void menu_init(void);

/* Handle button input. Returns true if the event was handled/activity occurred. */
bool menu_handle_button(button_event_t event, int32_t current_time_s);

/* Check for menu timeout. Call periodically. */
void menu_check_timeout(int32_t current_time_s);

/* Render the current menu or watchface */
void menu_render(float temp, float hum, int16_t ax, int16_t ay, int16_t az, int batt_mv, int batt_pct, struct tm *timeinfo);

/* Returns true if currently showing the main watchface (not in a menu) */
bool menu_is_watch_mode(void);

/* Getters for settings managed by the menu */
int16_t menu_get_motion_threshold(void);
int16_t menu_get_screen_timeout(void);

/* Show a notification on the screen */
void menu_show_notification(const char *title, const char *body);

#endif // MENU_H
