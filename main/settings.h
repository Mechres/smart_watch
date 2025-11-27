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

#endif // SETTINGS_H
