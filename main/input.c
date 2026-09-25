// input.c - Polling button handling with debouncing and FreeRTOS queue

#include <stdlib.h>
#include "input.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "power.h"

/* Button GPIO pins */
#define BUTTON_UP_GPIO   7
#define BUTTON_DOWN_GPIO 6
#define BUTTON_OK_GPIO   5

/* Debounce polling interval (ms) */
#define DEBOUNCE_POLL_MS_ACTIVE 20
#define DEBOUNCE_POLL_MS_IDLE   100

static const char *TAG = "input";
static QueueHandle_t button_queue = NULL;
static TaskHandle_t s_notify_task_handle = NULL;
static uint32_t s_input_dropped = 0;

void input_register_notify_task(TaskHandle_t task_handle) {
    s_notify_task_handle = task_handle;
}

/* Debounce task: monitors GPIO and posts events on edge transition.
 * Polls at 20 ms while active; backs off to 100 ms in light sleep. */
static void debounce_task(void *arg) {
    bool last_up = false, last_down = false, last_ok = false;

    while (1) {
        uint32_t poll_ms = (power_get_mode() == POWER_ACTIVE)
                               ? DEBOUNCE_POLL_MS_ACTIVE
                               : DEBOUNCE_POLL_MS_IDLE;
        vTaskDelay(pdMS_TO_TICKS(poll_ms));

        // Read current GPIO states (active-low: pressed = 0)
        bool up_now = (gpio_get_level(BUTTON_UP_GPIO) == 0);
        bool down_now = (gpio_get_level(BUTTON_DOWN_GPIO) == 0);
        bool ok_now = (gpio_get_level(BUTTON_OK_GPIO) == 0);

        bool event_generated = false;

        // Detect press transitions (idle -> pressed)
        if (up_now && !last_up) {
            button_event_t event = BTN_UP_PRESS;
            if (xQueueSend(button_queue, &event, 0) == pdTRUE) {
                ESP_LOGD(TAG, "UP button pressed");
                event_generated = true;
            } else {
                s_input_dropped++;
                ESP_LOGW(TAG, "UP press dropped (queue full, total=%u)", s_input_dropped);
            }
        }
        if (down_now && !last_down) {
            button_event_t event = BTN_DOWN_PRESS;
            if (xQueueSend(button_queue, &event, 0) == pdTRUE) {
                ESP_LOGD(TAG, "DOWN button pressed");
                event_generated = true;
            } else {
                s_input_dropped++;
                ESP_LOGW(TAG, "DOWN press dropped (queue full, total=%u)", s_input_dropped);
            }
        }
        if (ok_now && !last_ok) {
            button_event_t event = BTN_OK_PRESS;
            if (xQueueSend(button_queue, &event, 0) == pdTRUE) {
                ESP_LOGD(TAG, "OK button pressed");
                event_generated = true;
            } else {
                s_input_dropped++;
                ESP_LOGW(TAG, "OK press dropped (queue full, total=%u)", s_input_dropped);
            }
        }

        // Notify main task to wake immediately instead of waiting for full loop delay
        if (event_generated && s_notify_task_handle) {
            xTaskNotifyGive(s_notify_task_handle);
        }

        last_up = up_now;
        last_down = down_now;
        last_ok = ok_now;
    }
}

esp_err_t input_init(void) {
    // Create queue for button events
    button_queue = xQueueCreate(10, sizeof(button_event_t));
    if (!button_queue) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return ESP_ERR_NO_MEM;
    }

    // Configure GPIO pins as inputs with pull-ups
    // No ISRs here to avoid level-interrupt storms with gpio_wakeup_enable
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_UP_GPIO) | (1ULL << BUTTON_DOWN_GPIO) | (1ULL << BUTTON_OK_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create debounce task (runs periodically at 20ms to detect button presses)
    xTaskCreate(debounce_task, "debounce_task", 2048, NULL, 10, NULL);

    ESP_LOGI(TAG, "Input module initialized (debounced polling)");
    return ESP_OK;
}

bool input_get_event(button_event_t *event) {
    if (!button_queue) return false;
    return xQueueReceive(button_queue, event, 0) == pdTRUE;  // Non-blocking
}

void input_enable_wakeup(void) {
    // Enable wakeup on low level for button pins (Light Sleep)
    gpio_wakeup_enable(BUTTON_UP_GPIO, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BUTTON_DOWN_GPIO, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BUTTON_OK_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}
