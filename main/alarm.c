// alarm.c - Daily alarm implementation

#include "alarm.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "alarm";
static const char *NAMESPACE = "smartwatch";

static int s_hour = 7;
static int s_min = 0;
static bool s_enabled = false;
/* Last minute we fired in (monotonic guard so we trigger once per day). */
static int s_last_fired_yday = -1;
static int s_last_fired_min = -1;

void alarm_init(void) {
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved alarm; default disabled 07:00");
        return;
    }
    int32_t hr = 7, mn = 0, en = 0;
    if (nvs_get_i32(h, "alm_hr", &hr) != ESP_OK) hr = 7;
    if (nvs_get_i32(h, "alm_min", &mn) != ESP_OK) mn = 0;
    if (nvs_get_i32(h, "alm_en", &en) != ESP_OK) en = 0;
    nvs_close(h);
    if (hr < 0 || hr > 23) hr = 7;
    if (mn < 0 || mn > 59) mn = 0;
    s_hour = (int)hr;
    s_min = (int)mn;
    s_enabled = (en != 0);
    ESP_LOGI(TAG, "Alarm loaded: %s %02d:%02d", s_enabled ? "ON" : "OFF", s_hour, s_min);
}

esp_err_t alarm_set(int hour, int min, bool enabled) {
    if (hour < 0 || hour > 23 || min < 0 || min > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hour = hour;
    s_min = min;
    s_enabled = enabled;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(h, "alm_hr", (int32_t)hour);
    if (err == ESP_OK) err = nvs_set_i32(h, "alm_min", (int32_t)min);
    if (err == ESP_OK) err = nvs_set_i32(h, "alm_en", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Alarm save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Alarm saved: %s %02d:%02d", enabled ? "ON" : "OFF", hour, min);
    }
    return err;
}

void alarm_get(int *hour, int *min, bool *enabled) {
    if (hour) *hour = s_hour;
    if (min) *min = s_min;
    if (enabled) *enabled = s_enabled;
}

bool alarm_check(const struct tm *timeinfo) {
    if (!s_enabled || !timeinfo) return false;
    /* Reject invalid time (year < 2020 means never synced). */
    if (timeinfo->tm_year < (2020 - 1900)) return false;
    if (timeinfo->tm_hour != s_hour || timeinfo->tm_min != s_min) return false;
    /* Fire once per day. */
    if (s_last_fired_yday == timeinfo->tm_yday && s_last_fired_min == s_min) {
        return false;
    }
    s_last_fired_yday = timeinfo->tm_yday;
    s_last_fired_min = s_min;
    ESP_LOGI(TAG, "Alarm triggered at %02d:%02d", s_hour, s_min);
    return true;
}
