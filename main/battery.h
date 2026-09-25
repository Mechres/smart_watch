#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t battery_init(void);
bool battery_is_available(void);
int battery_get_voltage_mv(void);
int battery_mv_to_percentage(int mv);
