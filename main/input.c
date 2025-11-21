// input.c - ISR-based button handling with debouncing and FreeRTOS queue

#include <stdlib.h>
#include "input.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Button GPIO pins */
#define BUTTON_UP_GPIO   3
#define BUTTON_DOWN_GPIO 4
#define BUTTON_OK_GPIO   5

/* Debounce time (ms) */
#define DEBOUNCE_MS 50

static const char *TAG = "input";
static QueueHandle_t button_queue = NULL;

/* ISR handler for button presses */
static void IRAM_ATTR gpio_isr_handler(void *arg) {
    button_event_t event = (button_event_t)(uintptr_t)arg;
    xQueueSendFromISR(button_queue, &event, NULL);
}

/* Debounce task: monitors GPIO and posts events after debounce delay */
static void debounce_task(void *arg) {
    bool last_up = false, last_down = false, last_ok = false;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        
        // Read current GPIO states
        bool up_now = gpio_get_level(BUTTON_UP_GPIO) == 0;    // Active low
        bool down_now = gpio_get_level(BUTTON_DOWN_GPIO) == 0;
        bool ok_now = gpio_get_level(BUTTON_OK_GPIO) == 0;
        
        // Detect rising edge (press: idle→pressed after debounce)
        if (up_now && !last_up) {
            button_event_t event = BTN_UP_PRESS;
            xQueueSend(button_queue, &event, portMAX_DELAY);
            ESP_LOGI(TAG, "UP button pressed (debounced)");
        }
        if (down_now && !last_down) {
            button_event_t event = BTN_DOWN_PRESS;
            xQueueSend(button_queue, &event, portMAX_DELAY);
            ESP_LOGI(TAG, "DOWN button pressed (debounced)");
        }
        if (ok_now && !last_ok) {
            button_event_t event = BTN_OK_PRESS;
            xQueueSend(button_queue, &event, portMAX_DELAY);
            ESP_LOGI(TAG, "OK button pressed (debounced)");
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
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_UP_GPIO) | (1ULL << BUTTON_DOWN_GPIO) | (1ULL << BUTTON_OK_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE  // Use debounce task instead of ISR for reliability
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create debounce task (runs periodically to detect button presses)
    xTaskCreate(debounce_task, "debounce_task", 2048, NULL, 10, NULL);
    
    ESP_LOGI(TAG, "Input module initialized");
    return ESP_OK;
}

bool input_get_event(button_event_t *event) {
    if (!button_queue) return false;
    return xQueueReceive(button_queue, event, 0) == pdTRUE;  // Non-blocking
}

/* Legacy polling APIs (for compatibility with existing menu code) */
bool input_button_up_pressed(void) {
    return gpio_get_level(BUTTON_UP_GPIO) == 0;
}

bool input_button_down_pressed(void) {
    return gpio_get_level(BUTTON_DOWN_GPIO) == 0;
}

bool input_button_ok_pressed(void) {
    return gpio_get_level(BUTTON_OK_GPIO) == 0;
}
