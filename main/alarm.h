// alarm.h - Daily alarm (NVS persisted, edge-triggered)

#ifndef ALARM_H
#define ALARM_H

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/* Load persisted alarm (defaults: disabled, 07:00). */
void alarm_init(void);

/* hour 0-23, min 0-59. enabled gates triggering. Persists to NVS. */
esp_err_t alarm_set(int hour, int min, bool enabled);
void alarm_get(int *hour, int *min, bool *enabled);

/* Returns true once when the alarm time is reached (same minute, edge-triggered).
 * Call once per main loop with RTC time. Returns false if disabled or time invalid. */
bool alarm_check(const struct tm *timeinfo);

#endif // ALARM_H
