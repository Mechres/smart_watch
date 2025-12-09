#include "pedometer.h"
#include <math.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "Pedometer";

// 1g is approx 256 LSBs (3.9mg/LSB)
// Threshold of 80 is approx 0.3g
#define STEP_THRESHOLD 80 
#define MIN_STEP_INTERVAL_MS 300

static int step_count = 0;
static int64_t last_step_time = 0;
static float avg_mag = 256.0f; // Initial guess for 1g
static int last_day_saved = -1;

void pedometer_init(void) {
    step_count = 0;
    last_step_time = 0;
    avg_mag = 256.0f;
}

void pedometer_process(int16_t ax, int16_t ay, int16_t az) {
    // Calculate magnitude
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    
    // Low-pass filter for gravity estimation (slowly track baseline)
    avg_mag = avg_mag * 0.95f + mag * 0.05f;
    
    // Step detection
    // We look for a significant deviation from the average (impact)
    // This is a very simple peak detection.
    if (mag > avg_mag + STEP_THRESHOLD) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_step_time > MIN_STEP_INTERVAL_MS) {
            step_count++;
            last_step_time = now;
        }
    }
}

int pedometer_get_steps(void) {
    return step_count;
}

void pedometer_reset(void) {
    step_count = 0;
    pedometer_save();
}

void pedometer_load(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }

    int32_t saved_steps = 0;
    err = nvs_get_i32(my_handle, "steps", &saved_steps);
    if (err == ESP_OK) {
        step_count = saved_steps;
        ESP_LOGI(TAG, "Steps loaded from NVS: %d", step_count);
    } else {
        ESP_LOGW(TAG, "No saved steps found in NVS");
    }
    
    // Also load last day to know if we missed a midnight reset
    int32_t saved_day = -1;
    err = nvs_get_i32(my_handle, "step_day", &saved_day);
    if (err == ESP_OK) {
        last_day_saved = saved_day;
    }

    nvs_close(my_handle);
}

void pedometer_save(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(my_handle, "steps", step_count);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save steps");
    
    if (last_day_saved != -1) {
        nvs_set_i32(my_handle, "step_day", last_day_saved);
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to commit NVS");

    nvs_close(my_handle);
    ESP_LOGI(TAG, "Steps saved: %d", step_count);
}

void pedometer_check_midnight(struct tm *timeinfo) {
    if (last_day_saved == -1) {
        // First run or not loaded yet, just initialize
        last_day_saved = timeinfo->tm_mday;
        return;
    }

    if (timeinfo->tm_mday != last_day_saved) {
        ESP_LOGI(TAG, "Midnight detected! Resetting steps. (Old: %d, New: %d)", last_day_saved, timeinfo->tm_mday);
        step_count = 0;
        last_day_saved = timeinfo->tm_mday;
        pedometer_save(); // Save the reset state and new day immediately
    }
}
