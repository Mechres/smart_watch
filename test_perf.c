#include <stdio.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "pedometer.h"

int app_main() {
    // initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    // measure save
    int64_t start_save = esp_timer_get_time();
    pedometer_save();
    int64_t end_save = esp_timer_get_time();

    // measure load
    int64_t start_load = esp_timer_get_time();
    pedometer_load();
    int64_t end_load = esp_timer_get_time();

    printf("Baseline Save: %lld us\n", end_save - start_save);
    printf("Baseline Load: %lld us\n", end_load - start_load);
    return 0;
}
