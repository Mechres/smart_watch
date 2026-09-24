// gesture.h - Wrist-tilt raise-to-wake and lower-to-sleep gesture engine

#ifndef GESTURE_H
#define GESTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Initialize the gesture engine */
void gesture_init(void);

/* Register main task handle to wake immediately upon raise/lower events */
void gesture_register_notify_task(TaskHandle_t task_handle);

/* Process a new 25 Hz accelerometer reading (ax, ay, az in LSB, ~256 LSB = 1g)
 * Called from pedometer_sampling_task at 25 Hz.
 */
void gesture_process(int16_t ax, int16_t ay, int16_t az);

/* Check if a deliberate raise-to-wake gesture occurred (clears flag on read) */
bool gesture_has_raised_to_wake(void);

/* Returns true if the watch is currently held in the viewing position (screen facing user) */
bool gesture_is_in_viewing_position(void);

/* Check if the user lowered their arm / tilted watch away while screen was on (clears flag on read) */
bool gesture_should_lower_to_sleep(void);

/* Inform the gesture engine of screen on/off state changes */
void gesture_notify_screen_state(bool is_screen_on);

/* Set sensitivity (used from Settings menu motion threshold) */
void gesture_set_sensitivity(int16_t threshold);

#endif // GESTURE_H
