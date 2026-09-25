// input.h - Debounced button input handling with event queue

#ifndef INPUT_H
#define INPUT_H

#include "esp_err.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Button event types */
typedef enum {
    BTN_UP_PRESS = 0,
    BTN_DOWN_PRESS,
    BTN_OK_PRESS,   /* short press, sent on release */
    BTN_OK_LONG     /* OK held >= LONG_PRESS_MS, sent once while held */
} button_event_t;

/* Initialize button inputs and debounce task */
esp_err_t input_init(void);

/* Optional: register main task handle to wake immediately upon button press */
void input_register_notify_task(TaskHandle_t task_handle);

/* Check if a button event is available (non-blocking) */
bool input_get_event(button_event_t *event);

/* Enable GPIO wakeup for buttons (Light Sleep) */
void input_enable_wakeup(void);

#endif // INPUT_H
