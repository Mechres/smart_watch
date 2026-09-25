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

/* Passive AP scanner (SSID/RSSI/channel of nearby networks). Does not
 * associate to anything; only used to populate the WiFi Scan screen. */
#define WIFI_SCAN_MAX_AP 10

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open; // true if the AP has no authentication
} wifi_scan_ap_t;

/* Kicks off a scan on a background task; safe to call again to rescan. */
esp_err_t wifi_scan_start_async(void);
int wifi_scan_get_status(void); // 0=Idle, 1=Scanning, 2=Done, 3=Failed

/* Copies up to max_count cached results (sorted by RSSI desc) into out and
 * returns how many were copied. Pass out=NULL/max_count=0 to just get the
 * count without copying. */
int wifi_scan_get_results(wifi_scan_ap_t *out, int max_count);

#endif // WIFI_MANAGER_H
