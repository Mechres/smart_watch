// settings.c - NVS persistence implementation

#include <stdint.h>
#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NAMESPACE = "smartwatch";

esp_err_t settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

esp_err_t settings_load(int16_t *motion_threshold, int16_t *screen_timeout, int *watchface, int16_t *brightness) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "NVS namespace not found; using defaults");
            *motion_threshold = 100;
            *screen_timeout = 5;
            *watchface = 0; // WATCHFACE_DIGITAL
            *brightness = 128; // Default brightness
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Try to read each setting; use defaults if not found
    err = nvs_get_i16(handle, "motion_thr", motion_threshold);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading motion_threshold: %s", esp_err_to_name(err));
        *motion_threshold = 100;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *motion_threshold = 100;
    }

    err = nvs_get_i16(handle, "screen_to", screen_timeout);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading screen_timeout: %s", esp_err_to_name(err));
        *screen_timeout = 5;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *screen_timeout = 5;
    }

    err = nvs_get_i32(handle, "watchface", (int32_t*)watchface);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading watchface: %s", esp_err_to_name(err));
        *watchface = 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *watchface = 0;
    }

    err = nvs_get_i16(handle, "brightness", brightness);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Error reading brightness: %s", esp_err_to_name(err));
        *brightness = 128;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *brightness = 128;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded settings: motion_thr=%d, screen_to=%d, watchface=%d, brightness=%d", 
             *motion_threshold, *screen_timeout, *watchface, *brightness);
    return ESP_OK;
}

esp_err_t settings_save(int16_t motion_threshold, int16_t screen_timeout, int watchface, int16_t brightness) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i16(handle, "motion_thr", motion_threshold);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving motion_threshold: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_i16(handle, "screen_to", screen_timeout);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving screen_timeout: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_i32(handle, "watchface", (int32_t)watchface);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving watchface: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_i16(handle, "brightness", brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving brightness: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Saved settings: motion_thr=%d, screen_to=%d, watchface=%d, brightness=%d", 
             motion_threshold, screen_timeout, watchface, brightness);
    return ESP_OK;
}
