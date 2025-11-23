#include "pedometer.h"
#include <math.h>
#include "esp_timer.h"

// 1g is approx 256 LSBs (3.9mg/LSB)
// Threshold of 80 is approx 0.3g
#define STEP_THRESHOLD 80 
#define MIN_STEP_INTERVAL_MS 300

static int step_count = 0;
static int64_t last_step_time = 0;
static float avg_mag = 256.0f; // Initial guess for 1g

void pedometer_init(void) {
    step_count = 0;
    last_step_time = 0;
    avg_mag = 256.0f;
}

void pedometer_process(int16_t ax, int16_t ay, int16_t az) {
    // Calculate magnitude
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    
    // Low-pass filter for gravity estimation (slowly track baseline)
    avg_mag = avg_mag * 0.95f + mag * 0.05f;
    
    // Step detection
    // We look for a significant deviation from the average (impact)
    // This is a very simple peak detection.
    if (mag > avg_mag + STEP_THRESHOLD) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_step_time > MIN_STEP_INTERVAL_MS) {
            step_count++;
            last_step_time = now;
        }
    }
}

int pedometer_get_steps(void) {
    return step_count;
}

void pedometer_reset(void) {
    step_count = 0;
}
