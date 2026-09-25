#ifndef PEDOMETER_H
#define PEDOMETER_H

#include <stdint.h>

#include <time.h>

void pedometer_init(void);
void pedometer_start_task(void);
void pedometer_process(int16_t ax, int16_t ay, int16_t az);
int pedometer_get_steps(void);
/* Estimated distance in meters (stride ~0.75 m/step). */
float pedometer_get_distance_m(void);
/* Estimated calories in kcal (~0.04 kcal/step for 70 kg). */
float pedometer_get_calories_kcal(void);
/* Steps for N days ago (0=today). Returns -1 if unknown. */
int pedometer_get_history(int days_ago);
void pedometer_get_latest_accel(int16_t *x, int16_t *y, int16_t *z);
void pedometer_reset(void);

// NVS Persistence
void pedometer_load(void);
void pedometer_save(void);
void pedometer_check_midnight(struct tm *timeinfo);

#endif // PEDOMETER_H
