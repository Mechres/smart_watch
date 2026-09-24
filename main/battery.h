#pragma once

void battery_init(void);
int battery_get_voltage_mv(void);
int battery_get_percentage(void);
int battery_mv_to_percentage(int mv);
