#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/* WiFi credentials - configure these for your network */
#define WIFI_SSID      "SUPERONLINE_WiFi_C8AF"
#define WIFI_PASS      "4UFRYY9EAXMN"

/* Initialize WiFi in Station mode and connect */
void wifi_init_sta(void);

/* Initialize SNTP and wait for sync */
void sntp_initialize_and_wait(void);

#endif // WIFI_MANAGER_H
