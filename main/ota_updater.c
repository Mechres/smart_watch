// ota_updater.c - HTTPS OTA via esp_https_ota, triggered from BLE "ota=<url>"

#include "ota_updater.h"

#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "wifi_manager.h"

static const char *TAG = "OTA";

#define OTA_TASK_STACK   8192
#define OTA_TASK_PRIO    5
#define OTA_URL_MAX      256

static volatile bool s_ota_in_progress = false;
static char s_ota_url[OTA_URL_MAX];

static void ota_task(void *arg) {
    (void)arg;
    esp_err_t err;

    ESP_LOGI(TAG, "Starting OTA from %s", s_ota_url);
    ble_manager_send_command("ota_status=start");

    /* Ensure WiFi is up for the download */
    wifi_start();
    if (!wifi_ensure_connection(15000)) {
        ESP_LOGE(TAG, "WiFi connect failed - aborting OTA");
        ble_manager_send_command("ota_status=wifi_fail");
        wifi_stop();
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t http_config = {
        .url = s_ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        ble_manager_send_command("ota_status=fail");
        wifi_stop();
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    /* Optional: log new image descriptor */
    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(https_ota_handle, &new_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "New app: %s %s", new_app_info.project_name, new_app_info.version);
    }

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    int last_pct = -1;

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int read = esp_https_ota_get_image_len_read(https_ota_handle);
        if (image_size > 0 && read > 0) {
            int pct = (int)((int64_t)read * 100 / image_size);
            if (pct != last_pct && (pct % 10) == 0) {
                last_pct = pct;
                ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d)", pct, read, image_size);
                char msg[32];
                snprintf(msg, sizeof(msg), "ota_progress=%d", pct);
                ble_manager_send_command(msg);
            }
        }
    }

    if (err == ESP_OK) {
        if (!esp_https_ota_is_complete_data_received(https_ota_handle)) {
            ESP_LOGE(TAG, "Incomplete OTA image");
            err = ESP_FAIL;
        } else {
            err = esp_https_ota_finish(https_ota_handle);
        }
    } else {
        esp_https_ota_abort(https_ota_handle);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success - rebooting into new image");
        ble_manager_send_command("ota_status=done");
        wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    ble_manager_send_command("ota_status=fail");
    wifi_stop();
    s_ota_in_progress = false;
    vTaskDelete(NULL);
}

esp_err_t ota_updater_request(const char *https_url) {
    if (!https_url || https_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(https_url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "OTA URL must start with https://");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(https_url) >= OTA_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_ota_url, https_url, sizeof(s_ota_url));
    s_ota_in_progress = true;

    BaseType_t ok = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        s_ota_in_progress = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ota_updater_in_progress(void) {
    return s_ota_in_progress;
}
