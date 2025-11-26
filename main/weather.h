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

void weather_init(void);
esp_err_t weather_fetch(void);
void weather_fetch_async(void);
bool weather_is_fetching(void);
weather_data_t weather_get_current(void);
const char* weather_get_desc(int code);

#endif // WEATHER_H
