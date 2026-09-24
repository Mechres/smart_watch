#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Device addresses */
#define AHT10_ADDR      0x38
#define ADXL345_ADDR    0x53

/* Initialize sensors (AHT10/20 and ADXL345) */
void sensors_init(void);

/* Non-blocking temperature/humidity read:
 * start_temp_hum() kicks off a measurement; poll_temp_hum() returns
 * ESP_ERR_INVALID_STATE until ~80ms have elapsed, then ESP_OK with results. */
esp_err_t sensors_start_temp_hum(void);
esp_err_t sensors_poll_temp_hum(float *temperature, float *humidity);

/* Blocking convenience wrapper (starts + waits 80ms + reads) */
esp_err_t sensors_read_temp_hum(float *temperature, float *humidity);

/* Read accelerometer data from ADXL345 */
esp_err_t sensors_read_accel(int16_t *x, int16_t *y, int16_t *z);

/* Configure ADXL345 for single tap detection on INT1 */
esp_err_t sensors_config_tap_wakeup(void);

/* Clear ADXL345 interrupt flag by reading INT_SOURCE */
esp_err_t sensors_clear_tap_interrupt(void);

/* I2C bus concurrency locks */
bool sensors_i2c_take(uint32_t timeout_ms);
void sensors_i2c_give(void);

#endif // SENSORS_H
