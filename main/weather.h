#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float temp_c;
    int weather_code; // WMO code
    bool valid;
} weather_data_t;

/* Set weather data manually (e.g. from BLE) */
void weather_set_data(float temp_c, int weather_code);

/* Get current weather data */
weather_data_t weather_get_current(void);

/* Get text description for WMO weather code */
const char *weather_get_desc(int code);

#endif // WEATHER_H
