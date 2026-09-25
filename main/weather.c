#include "weather.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "Weather";
static weather_data_t current_weather = { .temp_c = 0.0f, .weather_code = 0, .valid = false };
void weather_set_data(float temp_c, int weather_code) {
    current_weather.temp_c = temp_c;
    current_weather.weather_code = weather_code;
    current_weather.valid = true;
    ESP_LOGI(TAG, "Weather updated via BLE: %.1fC, Code %d", temp_c, weather_code);

    /* Persist so weather survives reboot/deep sleep (best-effort). */
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "wx_temp", (int32_t)(temp_c * 10.0f));
        nvs_set_i32(h, "wx_code", (int32_t)weather_code);
        nvs_set_u8(h, "wx_valid", 1);
        if (nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "Weather NVS commit failed");
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "Weather NVS open failed");
    }
}

void weather_load(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t valid = 0;
    int32_t t10 = 0, code = 0;
    if (nvs_get_u8(h, "wx_valid", &valid) == ESP_OK && valid &&
        nvs_get_i32(h, "wx_temp", &t10) == ESP_OK &&
        nvs_get_i32(h, "wx_code", &code) == ESP_OK) {
        current_weather.temp_c = (float)t10 / 10.0f;
        current_weather.weather_code = (int)code;
        current_weather.valid = true;
        ESP_LOGI(TAG, "Weather restored: %.1fC code %d", current_weather.temp_c, (int)code);
    }
    nvs_close(h);
}

/* -------------------------------------------------------------------------- */
/* Accessors for the fetched data                                              */
/* -------------------------------------------------------------------------- */
weather_data_t weather_get_current(void)
{
    return current_weather;
}

const char *weather_get_desc(int code)
{
    switch (code) {
        case 0:  return "Clear";
        case 1:  return "Mainly Clear";
        case 2:  return "Partly Cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 61: case 63: case 65: return "Rain";
        case 71: case 73: case 75: return "Snow";
        case 80: case 81: case 82: return "Rain Showers";
        case 95: case 96: case 99: return "Thunderstorm";
        default: return "Unknown";
    }
}
