#include "wifi_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"

static const char *TAG = "WiFiManager";

static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static bool s_sntp_initialized = false;
static bool s_wifi_inited = false;
static volatile bool s_wifi_enabled = false;
static void sntp_initialize(void);

/* Sync Status: 0=Idle, 1=Syncing, 2=Success, 3=Failed */
static volatile int s_sync_status = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_enabled) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "auto-connect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // Only reconnect when WiFi was intentionally enabled (avoids racing wifi_stop)
        if (s_wifi_enabled) {
            ESP_LOGI(TAG, "wifi disconnected, reconnecting...");
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (!s_sntp_initialized) {
            sntp_initialize();
            s_sntp_initialized = true;
        }
    }
}

esp_err_t wifi_init_sta(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        ESP_LOGE(TAG, "create default wifi sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_EVENT handler register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP_EVENT handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password)-1);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // Enable Modem Sleep
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ps failed: %s (continuing)", esp_err_to_name(err));
    }

    s_wifi_inited = true;
    ESP_LOGI(TAG, "WiFi driver initialized (SSID:%s)", WIFI_SSID);
    return ESP_OK;
}

static void sntp_initialize(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

esp_err_t wifi_stop(void) {
    ESP_LOGI(TAG, "Stopping WiFi...");
    s_wifi_enabled = false;
    if (s_wifi_inited) {
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "disconnect failed: %s", esp_err_to_name(err));
        }
        err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "wifi stop failed: %s", esp_err_to_name(err));
        }
    }
    if (s_sntp_initialized) {
        esp_sntp_stop();
    }
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    return ESP_OK;
}

esp_err_t wifi_start(void) {
    ESP_LOGI(TAG, "Starting WiFi...");
    esp_err_t err = wifi_init_sta(); /* lazy init on first use */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed, cannot start: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_enabled = true;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s (will retry via event handler)", esp_err_to_name(err));
    }
    return ESP_OK;
}

bool wifi_is_connected(void) {
    if (s_wifi_event_group == NULL) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_ensure_connection(int timeout_ms) {
    if (s_wifi_event_group == NULL) return false;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* Async Time Sync Task */
static void wifi_sync_time_task(void *arg) {
    ESP_LOGI(TAG, "Starting Time Sync Task");
    s_sync_status = 1; // Syncing

    wifi_start();

    // 1. Wait for WiFi Connection
    if (!wifi_ensure_connection(10000)) {
        ESP_LOGE(TAG, "Failed to connect to WiFi for time sync");
        s_sync_status = 3; // Failed
        wifi_stop();
        vTaskDelete(NULL);
        return;
    }

    // 2. Wait for SNTP Sync
    // We check if year > 2020 as a simple validity check
    int retry = 0;
    const int max_retries = 20; // 10 seconds (500ms * 20)
    bool success = false;
    while (retry < max_retries) {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2020 - 1900)) {
            success = true;
            break;
        }
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry + 1, max_retries);
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }

    if (success) {
        ESP_LOGI(TAG, "Time synced successfully!");
        s_sync_status = 2; // Success
    } else {
        ESP_LOGE(TAG, "Time sync timed out");
        s_sync_status = 3; // Failed
    }

    wifi_stop();
    vTaskDelete(NULL);
}

void wifi_sync_time_async(void) {
    if (s_sync_status == 1) {
        ESP_LOGW(TAG, "Sync already in progress");
        return;
    }
    s_sync_status = 0; // Reset status
    xTaskCreate(wifi_sync_time_task, "wifi_sync_task", 4096, NULL, 5, NULL);
}

int wifi_get_sync_status(void) {
    return s_sync_status;
}
