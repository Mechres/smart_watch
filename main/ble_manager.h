#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_notification_callback_t)(const char *title, const char *body);
typedef void (*ble_control_callback_t)(const char *command);

/* Max BLE control command length. Must fit OTA URLs (https://... up to 255 chars). */
#define BLE_CMD_MAX_LEN 256

typedef struct {
    char title[32];
    char body[128];
    bool has_data;
    bool has_unread;
} ble_notification_t;

/* Now-playing metadata pushed from the phone (see Media characteristic).
 * has_data=false means nothing is playing, so the watchface can show an
 * idle state rather than stale track info. */
#define BLE_MEDIA_TITLE_MAX  48
#define BLE_MEDIA_ARTIST_MAX 32

typedef struct {
    char title[BLE_MEDIA_TITLE_MAX];
    char artist[BLE_MEDIA_ARTIST_MAX];
    bool playing;
    bool has_data;
} ble_media_t;

esp_err_t ble_manager_init(ble_notification_callback_t notification_cb,
                           ble_control_callback_t control_cb);

bool ble_manager_get_last_notification(ble_notification_t *out, bool clear_unread);
/* Latest now-playing metadata pushed by the phone. Returns false if nothing
 * has been received yet this boot. Thread-safe. */
bool ble_manager_get_media(ble_media_t *out);
bool ble_manager_is_connected(void);
bool ble_manager_is_active(void);
esp_err_t ble_manager_send_command(const char *command);

/* Stop advertising (e.g. before deep sleep). Safe to call when not advertising. */
void ble_manager_stop_adv(void);

/* Periodic housekeeping: stop advertising after idle timeout; restart on user activity.
 * Call from main_task. user_active = screen on, recent button/gesture, etc. */
void ble_manager_housekeeping(bool user_active);

esp_err_t ble_manager_update_battery(uint8_t level);
esp_err_t ble_manager_update_steps(uint32_t steps);
/* Distance in whole meters (uint32 LE). Calories in deci-kcal (kcal*10, uint32 LE). */
esp_err_t ble_manager_update_distance(uint32_t meters);
esp_err_t ble_manager_update_calories(uint32_t deci_kcal);

#ifdef __cplusplus
}
#endif

#endif // BLE_MANAGER_H
