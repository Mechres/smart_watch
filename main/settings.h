// settings.h - NVS persistence for editable watch settings

#ifndef SETTINGS_H
#define SETTINGS_H

#include "esp_err.h"

/* Initialize NVS (call once at startup) */
esp_err_t settings_init(void);

/* Load settings from NVS; returns defaults if not found */
esp_err_t settings_load(int16_t *motion_threshold, int16_t *screen_timeout, int *watchface, int16_t *brightness);

/* Save settings to NVS */
esp_err_t settings_save(int16_t motion_threshold, int16_t screen_timeout, int watchface, int16_t brightness);

/* Display units prefs (separate keys, same namespace; defaults 24h + Celsius).
 * time_fmt: 0 = 24h, 1 = 12h. temp_unit: 0 = Celsius, 1 = Fahrenheit. */
esp_err_t settings_load_units(int *time_fmt, int *temp_unit);
esp_err_t settings_save_units(int time_fmt, int temp_unit);

/* Erase saved settings (namespace only; pedometer NVS is separate) */
esp_err_t settings_reset(void);

#endif // SETTINGS_H
