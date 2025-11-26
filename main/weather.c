#include "weather.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "Weather";

// Open‑Meteo endpoint (Istanbul as default)
#define WEATHER_URL "https://api.open-meteo.com/v1/forecast?latitude=41.01&longitude=28.97&current_weather=true"

static weather_data_t current_weather = { .temp_c = 0.0f, .weather_code = 0, .valid = false };

void weather_init(void) {
    // No special initialisation required for now
}

/* -------------------------------------------------------------------------- */
/* HTTP event handler – copies the response into a user supplied buffer          */
/* -------------------------------------------------------------------------- */
static size_t resp_offset = 0;   // tracks write position inside the buffer
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->user_data) {
            char *buf = (char *)evt->user_data;
            memcpy(buf + resp_offset, evt->data, evt->data_len);
            resp_offset += evt->data_len;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        // Reset offset for the next request
        resp_offset = 0;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Fetch the current weather from Open‑Meteo                                    */
/* -------------------------------------------------------------------------- */
static bool is_fetching = false;

/* -------------------------------------------------------------------------- */
/* Fetch the current weather from Open‑Meteo                                    */
/* -------------------------------------------------------------------------- */
esp_err_t weather_fetch(void)
{
    if (is_fetching) return ESP_ERR_INVALID_STATE; // Already fetching
    is_fetching = true;

    char response[1024] = {0};   // buffer large enough for the JSON payload
    resp_offset = 0;             // ensure offset starts at zero

    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .event_handler = _http_event_handler,
        .user_data = response,
        .disable_auto_redirect = true,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP client");
        is_fetching = false;
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        // Parse the JSON response stored in `response`
        cJSON *root = cJSON_Parse(response);
        if (root) {
            cJSON *cw = cJSON_GetObjectItem(root, "current_weather");
            if (cw) {
                cJSON *temp = cJSON_GetObjectItem(cw, "temperature");
                cJSON *code = cJSON_GetObjectItem(cw, "weathercode");
                if (temp && code) {
                    current_weather.temp_c = (float)temp->valuedouble;
                    current_weather.weather_code = code->valueint;
                    current_weather.valid = true;
                    ESP_LOGI(TAG, "Weather updated: %.1fC, Code %d", current_weather.temp_c, current_weather.weather_code);
                } else {
                    ESP_LOGW(TAG, "Temperature or weather code missing in JSON");
                }
            } else {
                ESP_LOGW(TAG, "`current_weather` object not found in JSON");
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Failed to parse weather JSON");
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    is_fetching = false;
    return err;
}

static void weather_fetch_task(void *arg) {
    weather_fetch();
    vTaskDelete(NULL);
}

void weather_fetch_async(void) {
    if (!is_fetching) {
        xTaskCreate(weather_fetch_task, "weather_fetch", 4096, NULL, 5, NULL);
    }
}

bool weather_is_fetching(void) {
    return is_fetching;
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
