#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#include "sdkconfig.h"

/* WiFi credentials - configure via 'idf.py menuconfig' -> Smart Watch Configuration */
#ifdef CONFIG_SMARTWATCH_WIFI_SSID
#define WIFI_SSID      CONFIG_SMARTWATCH_WIFI_SSID
#define WIFI_PASS      CONFIG_SMARTWATCH_WIFI_PASSWORD
#else
#define WIFI_SSID      "MySSID"
#define WIFI_PASS      "MyPassword"
#endif

/* Initialize WiFi in Station mode and connect */
esp_err_t wifi_init_sta(void);

esp_err_t wifi_stop(void);
esp_err_t wifi_start(void);
bool wifi_is_connected(void);
bool wifi_ensure_connection(int timeout_ms);

/* Async Time Sync */
void wifi_sync_time_async(void);
int wifi_get_sync_status(void); // 0=Idle, 1=Syncing, 2=Success, 3=Failed

#endif // WIFI_MANAGER_H
