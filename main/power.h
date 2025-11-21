// power.h - Power management and sleep modes

#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include "esp_err.h"

/* Sleep modes */
typedef enum {
    POWER_ACTIVE = 0,       /* Full polling (100ms), responsive */
    POWER_LIGHT_SLEEP,      /* Reduced polling (500ms), I2C on, detect motion */
    POWER_DEEP_SLEEP        /* Minimal polling (5s), timer wakeup */
} power_mode_t;

/* Get current power mode */
power_mode_t power_get_mode(void);

/* Update power mode based on inactivity (seconds) */
void power_update_mode(uint32_t inactivity_secs);

/* Get polling interval (ms) for current power mode */
uint32_t power_get_poll_interval_ms(void);

#endif // POWER_H
