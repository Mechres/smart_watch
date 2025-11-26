// input.h - ISR-based button input handling with debouncing

#ifndef INPUT_H
#define INPUT_H

#include "esp_err.h"
#include <stdbool.h>

/* Button event types */
typedef enum {
    BTN_UP_PRESS = 0,
    BTN_DOWN_PRESS,
    BTN_OK_PRESS
} button_event_t;

/* Initialize GPIO ISRs and event queue */
esp_err_t input_init(void);

/* Check if a button event is available (non-blocking) */
bool input_get_event(button_event_t *event);

/* Poll for button press states (legacy API, used during menu/settings) */
bool input_button_up_pressed(void);
bool input_button_down_pressed(void);
bool input_button_ok_pressed(void);

/* Enable GPIO wakeup for buttons */
void input_enable_wakeup(void);

#endif // INPUT_H
