// gesture.c - Wrist-tilt raise-to-wake and lower-to-sleep gesture engine

#include "gesture.h"
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "gesture";

typedef enum {
    GESTURE_STATE_IDLE = 0,
    GESTURE_STATE_ARMED,
    GESTURE_STATE_VIEWING,
} gesture_state_t;

static gesture_state_t s_state = GESTURE_STATE_IDLE;
static int16_t s_last_ax = 0;
static int16_t s_last_ay = 0;
static int16_t s_last_az = 0;
static int16_t s_sensitivity = 35; // Dynamic delta threshold (Manhattan LSB)

static int s_armed_timeout = 0;
static int s_stable_count = 0;
static int s_lowered_count = 0;
static int s_ignore_lower_samples = 0; // Grace period after turning on
static bool s_screen_on = true;
static bool s_is_currently_viewing = false;
static bool s_raise_to_wake_flag = false;
static bool s_lower_to_sleep_flag = false;
static int s_view_count = 0;      // Consecutive raw-viewing samples
static int s_nonview_count = 0;   // Consecutive raw non-viewing samples
static int s_wake_cooldown = 0;   // Blocks arm right after screen-off (typing false wakes)

static TaskHandle_t s_notify_task = NULL;

void gesture_register_notify_task(TaskHandle_t task_handle) {
    s_notify_task = task_handle;
}

/* Check if the current orientation matches the viewing position (screen facing user)
 * Note: ADXL345 is mounted on the underside of the PCB, so Z is negative (-250..-340 LSB)
 * when the display is facing upwards towards the user.
 */
static bool check_viewing_orientation(int16_t ax, int16_t ay, int16_t az) {
    // Invert Z so z_up > 0 means screen facing up
    int16_t z_up = -az;

    // 1. Z component must be positive (screen facing upwards towards eyes)
    // 1g is ~260..330 LSB on this ADXL345. z_up >= 115 allows comfortable viewing angles up to ~65 deg.
    if (z_up < 115) return false;

    // 2. Lateral roll: screen is not tilted sharply sideways (> 45 degrees)
    if (abs(ax) > 150) return false;

    // 3. Forearm pitch: forearm is not hanging vertically down (standing arm has |ay| > 185)
    if (abs(ay) > 165) return false;

    // 4. Magnitude check: near 1g gravity (nominal ~260-330 LSB)
    int32_t mag_sq = (int32_t)ax * ax + (int32_t)ay * ay + (int32_t)az * az;
    if (mag_sq < 25000 || mag_sq > 160000) return false;

    return true;
}

/* Check if arm is lowered to the side or watch tilted away */
static bool check_lowered_orientation(int16_t ax, int16_t ay, int16_t az) {
    int16_t z_up = -az;

    // Arm hanging vertically down at side (gravity dominant on Y axis, Z near 0)
    // Forearm pointing down (|ay| > 185) and screen not facing up (z_up < 90)
    if (abs(ay) > 185 && z_up < 90) return true;

    // Watch tilted away or inverted face-down
    if (z_up < 15) return true;

    return false;
}

#define VIEW_DEBOUNCE_SAMPLES   3   /* ~120 ms before accepting orientation change */
#define NONVIEW_ARM_SAMPLES     5   /* Must be stably non-viewing before arming */
#define RAISE_SETTLE_SAMPLES    5   /* ~200 ms stable in view before wake */
#define WAKE_COOLDOWN_SAMPLES   50  /* ~2 s ignore raise after screen-off */
#define LOWER_SLEEP_SAMPLES     12  /* ~480 ms sustained lowered */

void gesture_init(void) {
    s_state = GESTURE_STATE_IDLE;
    s_armed_timeout = 0;
    s_stable_count = 0;
    s_lowered_count = 0;
    s_ignore_lower_samples = 40; // 1.6s grace period
    s_screen_on = true;
    s_is_currently_viewing = false;
    s_raise_to_wake_flag = false;
    s_lower_to_sleep_flag = false;
    s_view_count = 0;
    s_nonview_count = 0;
    s_wake_cooldown = 0;
    ESP_LOGI(TAG, "Gesture engine initialized");
}

void gesture_set_sensitivity(int16_t threshold) {
    int16_t sens = threshold / 2;
    if (sens < 15) sens = 15;
    if (sens > 80) sens = 80;
    if (sens != s_sensitivity) {
        s_sensitivity = sens;
        ESP_LOGI(TAG, "Gesture sensitivity set to %d LSB", s_sensitivity);
    }
}

void gesture_notify_screen_state(bool is_screen_on) {
    s_screen_on = is_screen_on;
    s_lowered_count = 0;
    s_lower_to_sleep_flag = false;
    s_raise_to_wake_flag = false;
    if (is_screen_on) {
        // Give 1.5 seconds grace period where lower-to-sleep cannot trigger
        s_ignore_lower_samples = 38; // 38 samples @ 25Hz ~= 1.5s
        s_wake_cooldown = 0;
    } else {
        s_state = GESTURE_STATE_IDLE;
        s_armed_timeout = 0;
        s_stable_count = 0;
        // Cooldown prevents immediate re-wake from residual typing/arm motion
        s_wake_cooldown = WAKE_COOLDOWN_SAMPLES;
    }
}

bool gesture_is_in_viewing_position(void) {
    return s_is_currently_viewing;
}

bool gesture_has_raised_to_wake(void) {
    if (s_raise_to_wake_flag) {
        s_raise_to_wake_flag = false; // Clear on read
        return true;
    }
    return false;
}

bool gesture_should_lower_to_sleep(void) {
    if (s_lower_to_sleep_flag) {
        s_lower_to_sleep_flag = false; // Clear on read
        return true;
    }
    return false;
}

void gesture_process(int16_t ax, int16_t ay, int16_t az) {
    // Calculate motion delta (Manhattan distance for speed)
    int16_t dx = ax - s_last_ax;
    int16_t dy = ay - s_last_ay;
    int16_t dz = az - s_last_az;
    int16_t delta = abs(dx) + abs(dy) + abs(dz);

    s_last_ax = ax;
    s_last_ay = ay;
    s_last_az = az;

    // Debounce viewing orientation so desk vibration / typing jitter does not
    // flicker the flag (flicker was re-arming timeout resets and false wakes).
    bool raw_viewing = check_viewing_orientation(ax, ay, az);
    if (raw_viewing) {
        if (s_view_count < 1000) s_view_count++;
        s_nonview_count = 0;
        if (s_view_count >= VIEW_DEBOUNCE_SAMPLES) {
            s_is_currently_viewing = true;
        }
    } else {
        if (s_nonview_count < 1000) s_nonview_count++;
        s_view_count = 0;
        if (s_nonview_count >= VIEW_DEBOUNCE_SAMPLES) {
            s_is_currently_viewing = false;
        }
    }
    bool is_viewing = s_is_currently_viewing;

    // Lower-to-sleep detection (only while screen is ON)
    if (s_screen_on) {
        if (s_ignore_lower_samples > 0) {
            s_ignore_lower_samples--;
            s_lowered_count = 0;
        } else if (check_lowered_orientation(ax, ay, az)) {
            s_lowered_count++;
            if (s_lowered_count >= LOWER_SLEEP_SAMPLES) {
                s_lower_to_sleep_flag = true;
                s_lowered_count = 0;
                ESP_LOGI(TAG, "Lower-to-sleep triggered (arm lowered)");
                if (s_notify_task) {
                    xTaskNotifyGive(s_notify_task);
                }
            }
        } else {
            if (s_lowered_count > 0) s_lowered_count--;
        }
    }

    // Raise-to-wake detection (only while screen is OFF)
    if (!s_screen_on) {
        if (s_wake_cooldown > 0) {
            s_wake_cooldown--;
            s_state = GESTURE_STATE_IDLE;
            s_armed_timeout = 0;
            s_stable_count = 0;
        } else {
        switch (s_state) {
            case GESTURE_STATE_IDLE:
                // Arm only from a stably non-viewing pose with a sharp motion burst
                // (filters out repetitive typing micro-movements).
                if (!is_viewing &&
                    s_nonview_count >= NONVIEW_ARM_SAMPLES &&
                    delta >= s_sensitivity) {
                    s_state = GESTURE_STATE_ARMED;
                    s_armed_timeout = 25; // ~1 s window to complete gesture
                    s_stable_count = 0;
                }
                break;

            case GESTURE_STATE_ARMED:
                if (s_armed_timeout > 0) {
                    s_armed_timeout--;
                }

                if (is_viewing) {
                    s_stable_count++;
                    if (s_stable_count >= RAISE_SETTLE_SAMPLES) {
                        s_state = GESTURE_STATE_VIEWING;
                        s_raise_to_wake_flag = true;
                        ESP_LOGI(TAG, "Raise-to-wake triggered (wrist tilted to face)");
                        if (s_notify_task) {
                            xTaskNotifyGive(s_notify_task);
                        }
                    }
                } else {
                    s_stable_count = 0;
                    if (s_armed_timeout == 0) {
                        s_state = GESTURE_STATE_IDLE;
                    }
                }
                break;

            case GESTURE_STATE_VIEWING:
                if (!is_viewing) {
                    s_state = GESTURE_STATE_IDLE;
                }
                break;
        }
        }
    }
}
