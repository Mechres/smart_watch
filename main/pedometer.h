#ifndef PEDOMETER_H
#define PEDOMETER_H

#include <stdint.h>

void pedometer_init(void);
void pedometer_process(int16_t ax, int16_t ay, int16_t az);
int pedometer_get_steps(void);
void pedometer_reset(void);

#endif // PEDOMETER_H
