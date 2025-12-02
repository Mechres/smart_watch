#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/* WiFi credentials - configure these for your network */
#define WIFI_SSID      "SUPERONLINE_WiFi_C8AF"
#define WIFI_PASS      "4UFRYY9EAXMN"

/* Initialize WiFi in Station mode and connect */
void wifi_init_sta(void);

void wifi_stop(void);
void wifi_start(void);
bool wifi_is_connected(void);
bool wifi_ensure_connection(int timeout_ms);

/* Async Time Sync */
void wifi_sync_time_async(void);
int wifi_get_sync_status(void); // 0=Idle, 1=Syncing, 2=Success, 3=Failed

#endif // WIFI_MANAGER_H
