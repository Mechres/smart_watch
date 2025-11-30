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

typedef struct {
    char title[32];
    char body[128];
    bool has_data;
    bool has_unread;
} ble_notification_t;

esp_err_t ble_manager_init(ble_notification_callback_t notification_cb,
                           ble_control_callback_t control_cb);

bool ble_manager_get_last_notification(ble_notification_t *out, bool clear_unread);
bool ble_manager_has_unread_notification(void);
void ble_manager_mark_notifications_read(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_MANAGER_H
