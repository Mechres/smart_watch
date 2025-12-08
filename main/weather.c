#include "weather.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "Weather";
static weather_data_t current_weather = { .temp_c = 0.0f, .weather_code = 0, .valid = false };
void weather_set_data(float temp_c, int weather_code) {
    current_weather.temp_c = temp_c;
    current_weather.weather_code = weather_code;
    current_weather.valid = true;
    ESP_LOGI(TAG, "Weather updated via BLE: %.1fC, Code %d", temp_c, weather_code);
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
