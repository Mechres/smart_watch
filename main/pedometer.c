#include "pedometer.h"
#include <math.h>
#include <stdio.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sensors.h"
#include "power.h"
#include "gesture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Pedometer";

// 1g is approx 256 LSBs (3.9mg/LSB)
// Threshold of 80 is approx 0.3g
#define STEP_THRESHOLD 80 
#define MIN_STEP_INTERVAL_MS 300

static int step_count = 0;
static int64_t last_step_time = 0;
static float avg_mag = 256.0f; // Initial guess for 1g
static int last_day_saved = -1;

/* 7-day history: hist_steps[0] = today (mirrors step_count), [1..6] previous days.
 * hist_day[i] = tm_mday tag for hist_steps[i], -1 = unknown. */
#define PEDO_HIST_DAYS 7
static int32_t hist_steps[PEDO_HIST_DAYS] = {0};
static int32_t hist_day[PEDO_HIST_DAYS] = {-1, -1, -1, -1, -1, -1, -1};

/* Stride / calorie model (documented estimates, 70 kg adult walking). */
#define PEDO_STRIDE_M 0.75f
#define PEDO_KCAL_PER_STEP 0.04f

static int16_t s_latest_ax = 0;
static int16_t s_latest_ay = 0;
static int16_t s_latest_az = 0;

static void pedometer_sampling_task(void *pvParameters) {
    ESP_LOGI(TAG, "Pedometer task started at 25Hz");
    while (1) {
        if (power_get_mode() != POWER_DEEP_SLEEP) {
            int16_t ax = 0, ay = 0, az = 0;
            if (sensors_read_accel(&ax, &ay, &az) == ESP_OK) {
                s_latest_ax = ax;
                s_latest_ay = ay;
                s_latest_az = az;
                pedometer_process(ax, ay, az);
                gesture_process(ax, ay, az);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40)); // 25 Hz sampling (40ms)
    }
}

void pedometer_start_task(void) {
    xTaskCreate(pedometer_sampling_task, "pedometer_task", 2048, NULL, 3, NULL);
}

void pedometer_get_latest_accel(int16_t *x, int16_t *y, int16_t *z) {
    if (x) *x = s_latest_ax;
    if (y) *y = s_latest_ay;
    if (z) *z = s_latest_az;
}

void pedometer_init(void) {
    step_count = 0;
    last_step_time = 0;
    avg_mag = 256.0f;
}

void pedometer_process(int16_t ax, int16_t ay, int16_t az) {
    // Magnitude squared (cast to float to prevent int32_t overflow)
    float mag_sq = (float)ax * ax + (float)ay * ay + (float)az * az;
    float mag = sqrtf(mag_sq);

    // Low-pass filter for gravity estimation (slowly track baseline)
    avg_mag = avg_mag * 0.95f + mag * 0.05f;

    // Step detection via squared comparison (avoids one add+compare on mag domain)
    float thr = avg_mag + STEP_THRESHOLD;
    if (mag_sq > thr * thr) {
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

float pedometer_get_distance_m(void) {
    return (float)step_count * PEDO_STRIDE_M;
}

float pedometer_get_calories_kcal(void) {
    return (float)step_count * PEDO_KCAL_PER_STEP;
}

int pedometer_get_history(int days_ago) {
    if (days_ago < 0 || days_ago >= PEDO_HIST_DAYS) return -1;
    if (days_ago == 0) return step_count;
    if (hist_day[days_ago] < 0) return -1;
    return hist_steps[days_ago];
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

    // Load 7-day history (best-effort; missing keys = unknown)
    for (int i = 0; i < PEDO_HIST_DAYS; i++) {
        char key[10];
        snprintf(key, sizeof(key), "hist%d", i);
        int32_t v = 0;
        if (nvs_get_i32(my_handle, key, &v) == ESP_OK) {
            hist_steps[i] = v;
        }
        snprintf(key, sizeof(key), "hday%d", i);
        if (nvs_get_i32(my_handle, key, &v) == ESP_OK) {
            hist_day[i] = v;
        }
    }
    hist_steps[0] = step_count;
    hist_day[0] = last_day_saved;

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

    hist_steps[0] = step_count;
    hist_day[0] = last_day_saved;
    for (int i = 0; i < PEDO_HIST_DAYS; i++) {
        char key[10];
        snprintf(key, sizeof(key), "hist%d", i);
        nvs_set_i32(my_handle, key, hist_steps[i]);
        snprintf(key, sizeof(key), "hday%d", i);
        nvs_set_i32(my_handle, key, hist_day[i]);
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
        /* Shift history: yesterday becomes day-1, etc. */
        for (int i = PEDO_HIST_DAYS - 1; i > 0; i--) {
            hist_steps[i] = hist_steps[i - 1];
            hist_day[i] = hist_day[i - 1];
        }
        hist_steps[0] = 0;
        step_count = 0;
        last_day_saved = timeinfo->tm_mday;
        hist_day[0] = last_day_saved;
        pedometer_save(); // Save the reset state and new day immediately
    }
}
