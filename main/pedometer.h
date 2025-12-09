#ifndef PEDOMETER_H
#define PEDOMETER_H

#include <stdint.h>

#include <time.h>

void pedometer_init(void);
void pedometer_process(int16_t ax, int16_t ay, int16_t az);
int pedometer_get_steps(void);
void pedometer_reset(void);

// NVS Persistence
void pedometer_load(void);
void pedometer_save(void);
void pedometer_check_midnight(struct tm *timeinfo);

#endif // PEDOMETER_H
