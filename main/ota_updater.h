// ota_updater.h - HTTPS OTA firmware update triggered via BLE command

#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include "esp_err.h"
#include <stdbool.h>

/* Request an OTA update from an HTTPS URL (non-blocking; runs in its own task).
 * Returns ESP_ERR_INVALID_STATE if an update is already in progress,
 * ESP_ERR_INVALID_ARG for bad URL, or ESP_OK if the task was started. */
esp_err_t ota_updater_request(const char *https_url);

/* True while an OTA download/flash is in progress */
bool ota_updater_in_progress(void);

#endif // OTA_UPDATER_H
