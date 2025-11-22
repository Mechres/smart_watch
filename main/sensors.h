#ifndef SENSORS_H
#define SENSORS_H

#include "esp_err.h"

/* Device addresses */
#define AHT10_ADDR      0x38
#define ADXL345_ADDR    0x53

/* Initialize sensors (AHT10/20 and ADXL345) */
void sensors_init(void);

/* Read temperature and humidity from AHT10/20 */
esp_err_t sensors_read_temp_hum(float *temperature, float *humidity);

/* Read accelerometer data from ADXL345 */
esp_err_t sensors_read_accel(int16_t *x, int16_t *y, int16_t *z);

/* Configure ADXL345 for single tap detection on INT1 */
esp_err_t sensors_config_tap_wakeup(void);

#endif // SENSORS_H
