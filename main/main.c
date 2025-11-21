// main.c

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "driver/gpio.h"

#include "display.h"
#include "watchfaces.h"
#include "settings.h"
#include "power.h"
#include "input.h"

/* WiFi credentials - configure these for your network */
#define WIFI_SSID      "SUPERONLINE_WiFi_C8AF"
#define WIFI_PASS      "4UFRYY9EAXMN"

static const char *TAG = "SmartWatchUI";

/* ---------- I2C / pins ---------- */
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SDA_IO           8   // ESP32-C3 SuperMini SDA
#define I2C_MASTER_SCL_IO           9   // ESP32-C3 SuperMini SCL
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_TIMEOUT_MS              1000
#define ACK_CHECK_EN                0x1

/* ---------- Device addresses ---------- */
#define AHT10_ADDR                  0x38
#define ADXL345_ADDR                0x53
#define SH1106_ADDR                 0x3C

/* ADXL345 registers */
#define ADXL345_REG_POWER_CTL       0x2D
#define ADXL345_REG_DATA_FORMAT     0x31
#define ADXL345_REG_DATAX0          0x32

/* AHT10 commands */
#define AHT10_CMD_SOFTRESET         0xBA
#define AHT10_CMD_TRIGGER           0xAC

/* ---------- Button GPIO pins ---------- */
#define BUTTON_UP_GPIO              3   // GPIO 3 - UP
#define BUTTON_DOWN_GPIO            4   // GPIO 4 - DOWN
#define BUTTON_OK_GPIO              5   // GPIO 5 - OK

/* ---------- Menu system ---------- */
typedef enum {
    MENU_WATCH,           // Main watch display
    MENU_ROOT,            // Top-level category menu
    MENU_SETTINGS,        // Settings menu
    MENU_SENSOR_DATA,     // Detailed sensor readings
    MENU_WATCHFACE,       // Watchface selection
    MENU_COUNT
} menu_mode_t;

typedef enum {
    WATCHFACE_DIGITAL = 0,    // HH:MM big display
    WATCHFACE_ANALOG_STYLE,   // Analog-inspired text
    WATCHFACE_MINIMAL,        // Minimal with date only
    WATCHFACE_COMPACT,        // Compact with all info
    WATCHFACE_COUNT
} watchface_t;

typedef enum {
    SETTINGS_MOTION_THRESHOLD = 0,
    SETTINGS_SCREEN_TIMEOUT,
    SETTINGS_COUNT
} settings_item_t;

typedef enum {
    SENSOR_TEMP = 0,
    SENSOR_HUMIDITY,
    SENSOR_ACCEL_X,
    SENSOR_ACCEL_Y,
    SENSOR_ACCEL_Z,
    SENSOR_COUNT
} sensor_item_t;

static menu_mode_t current_menu = MENU_WATCH;
static int32_t menu_last_activity_s = 0;  // Track menu activity for auto-exit
static settings_item_t current_setting = SETTINGS_MOTION_THRESHOLD;  // Currently selected setting
static sensor_item_t current_sensor = SENSOR_TEMP;  // Currently selected sensor
static int root_selection = 0; // 0 = Sensors, 1 = Settings, 2 = Watchface
static watchface_t current_watchface = WATCHFACE_DIGITAL;  // Currently selected watchface
static int watchface_selection = 0;  // For menu navigation
static bool editing_mode = false;  // Are we editing a setting value?

/* Editable settings */
static int16_t motion_threshold_editable = 100;
static int16_t screen_timeout_editable = 5;

/* Motion-based screen control */
#define MOTION_THRESHOLD        100    // threshold for motion detection (mg units, ~256 = 1g)
#define SCREEN_TIMEOUT_S        5      // turn off screen after 5 seconds of inactivity
static bool screen_on = true;
static int32_t last_motion_time_s = 0;
static int16_t last_ax = 0, last_ay = 0, last_az = 0;  // for delta calculation

/* small 5x7 font (only ASCII 32..127). We'll include a minimal subset for digits, letters, punctuation.
    For brevity we include charset for 32..127. This is a standard 5x7 font array (each char 5 bytes). */
const uint8_t font5x7[][5] = {
    /* space (32) */ {0x00,0x00,0x00,0x00,0x00},
    /* ! (33) */     {0x00,0x00,0x5F,0x00,0x00},
    /* " (34) */     {0x00,0x07,0x00,0x07,0x00},
    /* # (35) */     {0x14,0x7F,0x14,0x7F,0x14},
    /* $ (36) */     {0x24,0x2A,0x7F,0x2A,0x12},
    /* % (37) */     {0x23,0x13,0x08,0x64,0x62},
    /* & (38) */     {0x36,0x49,0x55,0x22,0x50},
    /* ' (39) */     {0x00,0x05,0x03,0x00,0x00},
    /* ( (40) */     {0x00,0x1C,0x22,0x41,0x00},
    /* ) (41) */     {0x00,0x41,0x22,0x1C,0x00},
    /* * (42) */     {0x14,0x08,0x3E,0x08,0x14},
    /* + (43) */     {0x08,0x08,0x3E,0x08,0x08},
    /* , (44) */     {0x00,0x50,0x30,0x00,0x00},
    /* - (45) */     {0x08,0x08,0x08,0x08,0x08},
    /* . (46) */     {0x00,0x60,0x60,0x00,0x00},
    /* / (47) */     {0x20,0x10,0x08,0x04,0x02},
    /* 0 (48) */     {0x3E,0x51,0x49,0x45,0x3E},
    /* 1 (49) */     {0x00,0x42,0x7F,0x40,0x00},
    /* 2 (50) */     {0x42,0x61,0x51,0x49,0x46},
    /* 3 (51) */     {0x21,0x41,0x45,0x4B,0x31},
    /* 4 (52) */     {0x18,0x14,0x12,0x7F,0x10},
    /* 5 (53) */     {0x27,0x45,0x45,0x45,0x39},
    /* 6 (54) */     {0x3C,0x4A,0x49,0x49,0x30},
    /* 7 (55) */     {0x01,0x71,0x09,0x05,0x03},
    /* 8 (56) */     {0x36,0x49,0x49,0x49,0x36},
    /* 9 (57) */     {0x06,0x49,0x49,0x29,0x1E},
    /* : (58) */     {0x00,0x36,0x36,0x00,0x00},
    /* ; (59) */     {0x00,0x56,0x36,0x00,0x00},
    /* < (60) */     {0x08,0x14,0x22,0x41,0x00},
    /* = (61) */     {0x14,0x14,0x14,0x14,0x14},
    /* > (62) */     {0x00,0x41,0x22,0x14,0x08},
    /* ? (63) */     {0x02,0x01,0x51,0x09,0x06},
    /* @ (64) */     {0x32,0x49,0x79,0x41,0x3E},
    /* A (65) */     {0x7E,0x11,0x11,0x11,0x7E},
    /* B (66) */     {0x7F,0x49,0x49,0x49,0x36},
    /* C (67) */     {0x3E,0x41,0x41,0x41,0x22},
    /* D (68) */     {0x7F,0x41,0x41,0x22,0x1C},
    /* E (69) */     {0x7F,0x49,0x49,0x49,0x41},
    /* F (70) */     {0x7F,0x09,0x09,0x09,0x01},
    /* G (71) */     {0x3E,0x41,0x49,0x49,0x7A},
    /* H (72) */     {0x7F,0x08,0x08,0x08,0x7F},
    /* I (73) */     {0x00,0x41,0x7F,0x41,0x00},
    /* J (74) */     {0x20,0x40,0x41,0x3F,0x01},
    /* K (75) */     {0x7F,0x08,0x14,0x22,0x41},
    /* L (76) */     {0x7F,0x40,0x40,0x40,0x40},
    /* M (77) */     {0x7F,0x02,0x04,0x02,0x7F},
    /* N (78) */     {0x7F,0x04,0x08,0x10,0x7F},
    /* O (79) */     {0x3E,0x41,0x41,0x41,0x3E},
    /* P (80) */     {0x7F,0x09,0x09,0x09,0x06},
    /* Q (81) */     {0x3E,0x41,0x51,0x21,0x5E},
    /* R (82) */     {0x7F,0x09,0x19,0x29,0x46},
    /* S (83) */     {0x46,0x49,0x49,0x49,0x31},
    /* T (84) */     {0x01,0x01,0x7F,0x01,0x01},
    /* U (85) */     {0x3F,0x40,0x40,0x40,0x3F},
    /* V (86) */     {0x1F,0x20,0x40,0x20,0x1F},
    /* W (87) */     {0x3F,0x40,0x38,0x40,0x3F},
    /* X (88) */     {0x63,0x14,0x08,0x14,0x63},
    /* Y (89) */     {0x07,0x08,0x70,0x08,0x07},
    /* Z (90) */     {0x61,0x51,0x49,0x45,0x43},
    /* [ (91) */     {0x00,0x7F,0x41,0x41,0x00},
    /* \ (92) */     {0x02,0x04,0x08,0x10,0x20},
    /* ] (93) */     {0x00,0x41,0x41,0x7F,0x00},
    /* ^ (94) */     {0x04,0x02,0x01,0x02,0x04},
    /* _ (95) */     {0x40,0x40,0x40,0x40,0x40},
    /* ` (96) */     {0x00,0x03,0x07,0x00,0x00},
    /* a (97) */     {0x20,0x54,0x54,0x54,0x78},
    /* b (98) */     {0x7F,0x48,0x44,0x44,0x38},
    /* c (99) */     {0x38,0x44,0x44,0x44,0x20},
    /* d (100) */    {0x38,0x44,0x44,0x48,0x7F},
    /* e (101) */    {0x38,0x54,0x54,0x54,0x18},
    /* f (102) */    {0x08,0x7E,0x09,0x01,0x02},
    /* g (103) */    {0x0C,0x52,0x52,0x52,0x3E},
    /* h (104) */    {0x7F,0x08,0x04,0x04,0x78},
    /* i (105) */    {0x00,0x44,0x7D,0x40,0x00},
    /* j (106) */    {0x20,0x40,0x44,0x3D,0x00},
    /* k (107) */    {0x7F,0x10,0x28,0x44,0x00},
    /* l (108) */    {0x00,0x41,0x7F,0x40,0x00},
    /* m (109) */    {0x7C,0x04,0x18,0x04,0x78},
    /* n (110) */    {0x7C,0x08,0x04,0x04,0x78},
    /* o (111) */    {0x38,0x44,0x44,0x44,0x38},
    /* p (112) */    {0x7C,0x14,0x14,0x14,0x08},
    /* q (113) */    {0x08,0x14,0x14,0x18,0x7C},
    /* r (114) */    {0x7C,0x08,0x04,0x04,0x08},
    /* s (115) */    {0x48,0x54,0x54,0x54,0x20},
    /* t (116) */    {0x04,0x3F,0x44,0x40,0x20},
    /* u (117) */    {0x3C,0x40,0x40,0x20,0x7C},
    /* v (118) */    {0x1C,0x20,0x40,0x20,0x1C},
    /* w (119) */    {0x3C,0x40,0x30,0x40,0x3C},
    /* x (120) */    {0x44,0x28,0x10,0x28,0x44},
    /* y (121) */    {0x0C,0x50,0x50,0x50,0x3C},
    /* z (122) */    {0x44,0x64,0x54,0x4C,0x44},
    /* { (123) */    {0x00,0x08,0x36,0x41,0x00},
    /* | (124) */    {0x00,0x00,0x7F,0x00,0x00},
    /* } (125) */    {0x00,0x41,0x36,0x08,0x00},
    /* ~ (126) */    {0x02,0x01,0x02,0x04,0x02},
    /* DEL (127) */   {0x7F,0x7F,0x7F,0x7F,0x7F}
};




static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;


/* ---------- low-level I2C helpers ---------- */
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = I2C_MASTER_FREQ_HZ }
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

/* probe address (start + write address only) */
static esp_err_t i2c_probe_addr(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* ---------- ADXL345 ---------- */
static esp_err_t adxl345_init(void) {
    esp_err_t err;
    // set data format = 0x08 (full resolution, +-2g)
    uint8_t df[2] = {ADXL345_REG_DATA_FORMAT, 0x08};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write(cmd, df, sizeof(df), ACK_CHECK_EN);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) return err;

    // power ctl measure
    uint8_t pc[2] = {ADXL345_REG_POWER_CTL, 0x08};
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write(cmd, pc, sizeof(pc), ACK_CHECK_EN);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t adxl345_read(int16_t *x, int16_t *y, int16_t *z) {
    uint8_t reg = ADXL345_REG_DATAX0;
    uint8_t data[6];
    esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM, ADXL345_ADDR, &reg, 1, data, 6, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) return err;
    *x = (int16_t)((data[1]<<8) | data[0]);
    *y = (int16_t)((data[3]<<8) | data[2]);
    *z = (int16_t)((data[5]<<8) | data[4]);
    return ESP_OK;
}

/* ---------- AHT10/AHT20 reading (trigger + read) ---------- */
static esp_err_t aht_read(float *temperature, float *humidity) {
    // trigger: AC 33 00
    uint8_t cmdt[3] = {AHT10_CMD_TRIGGER, 0x33, 0x00};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AHT10_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write(cmd, cmdt, sizeof(cmdt), ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t data[6];
    err = i2c_master_read_from_device(I2C_MASTER_NUM, AHT10_ADDR, data, 6, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) return err;
    if (data[0] & 0x80) return ESP_ERR_INVALID_STATE; // busy

    uint32_t hum_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humidity = (float)hum_raw * 100.0f / 1048576.0f;
    *temperature = ((float)temp_raw * 200.0f / 1048576.0f) - 50.0f;

    return ESP_OK;
}

/* SH1106 functions moved to display.c (use display.h prototypes) */

/* Detect motion from accelerometer readings */
static bool detect_motion(int16_t ax, int16_t ay, int16_t az) {
    // Calculate delta (change) in acceleration from last reading
    int16_t dx = ax - last_ax;
    int16_t dy = ay - last_ay;
    int16_t dz = az - last_az;
    
    // Store current values for next comparison
    last_ax = ax;
    last_ay = ay;
    last_az = az;
    
    // Calculate magnitude of acceleration delta
    int32_t delta_mag_sq = (int32_t)dx*dx + (int32_t)dy*dy + (int32_t)dz*dz;
    int32_t threshold_sq = (int32_t)motion_threshold_editable * motion_threshold_editable;
    
    // Log for debugging (remove later if working)
    static int log_counter = 0;
    if (log_counter++ % 5 == 0) {  // Log every 5 samples to avoid spam
        ESP_LOGI(TAG, "ACC: ax=%d ay=%d az=%d | delta: dx=%d dy=%d dz=%d | mag_sq=%ld thr_sq=%ld | motion=%d", 
                 ax, ay, az, dx, dy, dz, delta_mag_sq, threshold_sq, delta_mag_sq > threshold_sq);
    }
    
    return delta_mag_sq > threshold_sq;
}

/* Update screen state based on motion and timeout */
static void update_screen_state(bool motion_detected, int32_t current_time_s) {
    // Don't auto-timeout screen if user is navigating menus
    bool in_menu = (current_menu != MENU_WATCH);
    
    if (motion_detected) {
        last_motion_time_s = current_time_s;
        if (!screen_on) {
            ESP_LOGI(TAG, "Motion detected - turning screen ON");
            sh1106_display_on();
            screen_on = true;
        }
    } else if (screen_on && !in_menu && (current_time_s - last_motion_time_s) >= screen_timeout_editable) {
        // Only auto-off in watch mode after timeout, not in menu
        ESP_LOGI(TAG, "No motion for %d seconds - turning screen OFF", screen_timeout_editable);
        sh1106_display_off();
        screen_on = false;
    }
}

/* framebuffer helpers are provided by display.c (include display.h at top) */

/* ---------- Menu rendering ---------- */
/* Forward declarations */
static void render_sensor_menu_list(float temp, float hum, int16_t ax, int16_t ay, int16_t az);

static void render_watch_display(float temp, float hum, int16_t ax, int16_t ay, int16_t az, struct tm *timeinfo) {
    // Delegate to appropriate watchface renderer
    switch (current_watchface) {
        case WATCHFACE_DIGITAL:
            render_watchface_digital(temp, hum, ax, ay, az, timeinfo);
            break;
        case WATCHFACE_ANALOG_STYLE:
            render_watchface_analog(temp, hum, ax, ay, az, timeinfo);
            break;
        case WATCHFACE_MINIMAL:
            render_watchface_minimal(temp, hum, ax, ay, az, timeinfo);
            break;
        case WATCHFACE_COMPACT:
            render_watchface_compact(temp, hum, ax, ay, az, timeinfo);
            break;
        default:
            render_watchface_digital(temp, hum, ax, ay, az, timeinfo);
            break;
    }
}

/* watchface implementations live in watchfaces.c; prototypes are in watchfaces.h (included at top) */

static void render_sensor_menu(float temp, float hum, int16_t ax, int16_t ay, int16_t az) {
    render_sensor_menu_list(temp, hum, ax, ay, az);
}

static void render_settings_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===SETTINGS===");
    
    // Motion Threshold
    if (current_setting == SETTINGS_MOTION_THRESHOLD) {
        fb_draw_text(0, 12, "> Motion Thresh");
    } else {
        fb_draw_text(0, 12, "  Motion Thresh");
    }
    
    if (current_setting == SETTINGS_MOTION_THRESHOLD && editing_mode) {
        snprintf(buf, sizeof(buf), "  [%d]  <OK to exit>", motion_threshold_editable);
    } else {
        snprintf(buf, sizeof(buf), "  Value: %d", motion_threshold_editable);
    }
    fb_draw_text(0, 22, buf);
    
    // Screen Timeout
    if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
        fb_draw_text(0, 37, "> Screen Timeout");
    } else {
        fb_draw_text(0, 37, "  Screen Timeout");
    }
    
    if (current_setting == SETTINGS_SCREEN_TIMEOUT && editing_mode) {
        snprintf(buf, sizeof(buf), "  [%d]s  <OK to exit>", screen_timeout_editable);
    } else {
        snprintf(buf, sizeof(buf), "  Value: %d s", screen_timeout_editable);
    }
    fb_draw_text(0, 47, buf);
}

static void render_sensor_menu_list(float temp, float hum, int16_t ax, int16_t ay, int16_t az) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===SENSORS===");
    
    // Display 3 items at a time with scrolling
    // Calculate which items to show (keep selected item visible)
    int start_idx = current_sensor - 1;  // Try to show 1 above
    if (start_idx < 0) start_idx = 0;
    if (start_idx > SENSOR_COUNT - 3) start_idx = SENSOR_COUNT - 3;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 3 && i < SENSOR_COUNT; i++) {
        bool is_selected = (i == current_sensor);
        char prefix = is_selected ? '>' : ' ';
        
        switch (i) {
            case SENSOR_TEMP:
                snprintf(buf, sizeof(buf), "%c Temp: %.2f C", prefix, temp);
                break;
            case SENSOR_HUMIDITY:
                snprintf(buf, sizeof(buf), "%c Humidity: %.1f%%", prefix, hum);
                break;
            case SENSOR_ACCEL_X:
                snprintf(buf, sizeof(buf), "%c AccelX: %d", prefix, ax);
                break;
            case SENSOR_ACCEL_Y:
                snprintf(buf, sizeof(buf), "%c AccelY: %d", prefix, ay);
                break;
            case SENSOR_ACCEL_Z:
                snprintf(buf, sizeof(buf), "%c AccelZ: %d", prefix, az);
                break;
        }
        
        fb_draw_text(0, y_pos, buf);
        y_pos += 10;
    }
    
    // Show scroll indicator at bottom
    if (start_idx > 0) {
        fb_draw_text(0, 52, "  [up]");
    }
    if (start_idx + 3 < SENSOR_COUNT) {
        fb_draw_text(70, 52, "[dn]");
    }
}

/* render top-level menu (categories) */
static void render_root_menu(void) {
    fb_clear();
    fb_draw_text(0, 0, "===MENU===");
    if (root_selection == 0) fb_draw_text(0, 12, "> Sensors"); else fb_draw_text(0, 12, "  Sensors");
    if (root_selection == 1) fb_draw_text(0, 24, "> Settings"); else fb_draw_text(0, 24, "  Settings");
    if (root_selection == 2) fb_draw_text(0, 36, "> Watchface"); else fb_draw_text(0, 36, "  Watchface");
}

/* Watchface selection menu */
static void render_watchface_menu(void) {
    fb_clear();
    char buf[64];
    
    fb_draw_text(0, 0, "===WATCHFACE===");
    
    const char *names[WATCHFACE_COUNT] = {
        "Digital",
        "Analog Style",
        "Minimal",
        "Compact"
    };
    
    // Show 3 watchfaces at a time
    int start_idx = watchface_selection - 1;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > WATCHFACE_COUNT - 3) start_idx = WATCHFACE_COUNT - 3;
    
    int y_pos = 12;
    for (int i = start_idx; i < start_idx + 3 && i < WATCHFACE_COUNT; i++) {
        bool is_selected = (i == watchface_selection);
        char prefix = is_selected ? '>' : ' ';
        snprintf(buf, sizeof(buf), "%c %s", prefix, names[i]);
        fb_draw_text(0, y_pos, buf);
        y_pos += 10;
    }
    
    // Show scroll indicator
    if (start_idx > 0) {
        fb_draw_text(0, 52, "  [up]");
    }
    if (start_idx + 3 < WATCHFACE_COUNT) {
        fb_draw_text(70, 52, "[dn]");
    }
}

static void sntp_initialize(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

/* wait for NTP sync (with timeout seconds) */
static bool wait_for_sntp_sync(int timeout_s)
{
    int retry = 0;
    const int retry_count = timeout_s * 2;

    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }

    return (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
}

/* event handler for wifi */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "wifi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password)-1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init finished. Connecting to SSID:%s", WIFI_SSID);

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
    } else {
        ESP_LOGW(TAG, "Failed to connect to AP within timeout");
    }
}

/* ---------- Main task: sensors + display + time ---------- */
static void main_task(void *arg) {
    ESP_LOGI(TAG, "main_task starting: init sensors & display");

    // Initialize settings from NVS
    settings_init();
    settings_load(&motion_threshold_editable, &screen_timeout_editable, (int*)&current_watchface);

    adxl345_init(); // ignore error but good to try
    // try soft reset aht (not required always)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (AHT10_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, AHT10_CMD_SOFTRESET, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (sh1106_init() != ESP_OK) {
        ESP_LOGE(TAG, "SH1106 init failed - aborting");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Display ready");
    
    // Initialize event-driven button input
    if (input_init() != ESP_OK) {
        ESP_LOGE(TAG, "Input module init failed - continuing without buttons");
    }

    // setup sntp
    sntp_initialize();
    ESP_LOGI(TAG, "SNTP started, waiting sync...");
    bool synced = wait_for_sntp_sync(10); // 10 seconds timeout
    if (!synced) {
        ESP_LOGW(TAG, "SNTP not synced within timeout; will still show UTC+3 local time when possible");
    } else {
        ESP_LOGI(TAG, "SNTP sync OK");
    }

    // Initialize time tracking (must get time first, then set last_motion_time)
    time_t now;
    time(&now);
    now += 3*3600; // apply UTC+3 offset
    last_motion_time_s = now; // Initialize so inactivity starts from zero on boot
    
    // main loop: adaptive polling based on power mode
    while (1) {
        // get current time (UTC+3)
        time(&now);
        now += 3*3600; // apply UTC+3 offset
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);

        // Calculate inactivity time and update power mode
        uint32_t inactivity_secs = (now - last_motion_time_s);
        power_update_mode(inactivity_secs);
        uint32_t poll_interval_ms = power_get_poll_interval_ms();

        // Only read sensors in active/idle modes (skip in deep sleep to save power)
        power_mode_t mode = power_get_mode();
        float temp=0.0f, hum=0.0f;
        int16_t ax=0, ay=0, az=0;
        
        if (mode != POWER_DEEP_SLEEP) {
            // Read sensors normally in active/idle/light-sleep modes
            if (aht_read(&temp,&hum) != ESP_OK) {
                // do nothing, leave zeros
            }
            if (adxl345_read(&ax,&ay,&az) != ESP_OK) {
                ax=ay=az=0;
            }
        } else {
            // In deep sleep, only read motion if motion was detected (via ISR in future)
            // For now, just skip sensor reads to conserve power
            ax=ay=az=0;
            temp=hum=0.0f;
        }

        // Detect motion and update screen state (only in active/idle modes)
        bool motion_detected = false;
        if (mode != POWER_DEEP_SLEEP) {
            motion_detected = detect_motion(ax, ay, az);
        }
        update_screen_state(motion_detected, now);

        // Handle event-driven button inputs from queue
        button_event_t btn_event;
        bool button_pressed = false;
        
        // Check if any button event is available (non-blocking)
        if (input_get_event(&btn_event)) {
            button_pressed = true;
            
            // Wake screen if it's off
            if (!screen_on) {
                ESP_LOGI(TAG, "Button pressed - waking screen");
                sh1106_display_on();
                screen_on = true;
            }

            // Update both motion and menu activity times to keep screen on
            last_motion_time_s = now;
            menu_last_activity_s = now;
        }
        
        // Handle button inputs with context-aware navigation
        if (button_pressed && btn_event == BTN_UP_PRESS) {
            ESP_LOGI(TAG, "UP button pressed");
            
            if (current_menu == MENU_ROOT) {
                // navigate root categories
                if (root_selection > 0) {
                    root_selection--;
                    ESP_LOGI(TAG, "Root selection: %d", root_selection);
                }
            } else if (current_menu == MENU_WATCHFACE) {
                // Navigate watchface menu up
                if (watchface_selection > 0) {
                    watchface_selection--;
                    ESP_LOGI(TAG, "Watchface: moved up to %d", watchface_selection);
                }
            } else if (current_menu == MENU_SETTINGS && editing_mode) {
                // In edit mode: increase value
                if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                    motion_threshold_editable += 10;
                    if (motion_threshold_editable > 500) motion_threshold_editable = 500;
                    ESP_LOGI(TAG, "Motion threshold: %d", motion_threshold_editable);
                    settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                    screen_timeout_editable += 1;
                    if (screen_timeout_editable > 60) screen_timeout_editable = 60;
                    ESP_LOGI(TAG, "Screen timeout: %d", screen_timeout_editable);
                    settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                }
            } else if (current_menu == MENU_SETTINGS) {
                // Menu navigation up
                if (current_setting > 0) {
                    current_setting--;
                    ESP_LOGI(TAG, "Settings: moved up to %d", current_setting);
                }
            } else if (current_menu == MENU_SENSOR_DATA) {
                // Sensor menu navigation up
                if (current_sensor > 0) {
                    current_sensor--;
                    ESP_LOGI(TAG, "Sensor: moved up to %d", current_sensor);
                }
            }
        }
        
        if (button_pressed && btn_event == BTN_DOWN_PRESS) {
            ESP_LOGI(TAG, "DOWN button pressed");
            
            if (current_menu == MENU_ROOT) {
                // navigate root categories
                if (root_selection < 2) {
                    root_selection++;
                    ESP_LOGI(TAG, "Root selection: %d", root_selection);
                }
            } else if (current_menu == MENU_WATCHFACE) {
                // Navigate watchface menu down
                if (watchface_selection < WATCHFACE_COUNT - 1) {
                    watchface_selection++;
                    ESP_LOGI(TAG, "Watchface: moved down to %d", watchface_selection);
                }
            } else if (current_menu == MENU_SETTINGS && editing_mode) {
                // In edit mode: decrease value
                if (current_setting == SETTINGS_MOTION_THRESHOLD) {
                    motion_threshold_editable -= 10;
                    if (motion_threshold_editable < 10) motion_threshold_editable = 10;
                    ESP_LOGI(TAG, "Motion threshold: %d", motion_threshold_editable);
                    settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                } else if (current_setting == SETTINGS_SCREEN_TIMEOUT) {
                    screen_timeout_editable -= 1;
                    if (screen_timeout_editable < 1) screen_timeout_editable = 1;
                    ESP_LOGI(TAG, "Screen timeout: %d", screen_timeout_editable);
                    settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                }
            } else if (current_menu == MENU_SETTINGS) {
                // Menu navigation down
                if (current_setting < SETTINGS_COUNT - 1) {
                    current_setting++;
                    ESP_LOGI(TAG, "Settings: moved down to %d", current_setting);
                }
            } else if (current_menu == MENU_SENSOR_DATA) {
                // Sensor menu navigation down
                if (current_sensor < SENSOR_COUNT - 1) {
                    current_sensor++;
                    ESP_LOGI(TAG, "Sensor: moved down to %d", current_sensor);
                }
            }
        }
        
        if (button_pressed && btn_event == BTN_OK_PRESS) {
            ESP_LOGI(TAG, "OK button pressed");
            
            if (current_menu == MENU_WATCH) {
                // OK from watch -> enter root menu
                current_menu = MENU_ROOT;
                root_selection = 0;
                ESP_LOGI(TAG, "Entered top-level menu");
            } else if (current_menu == MENU_ROOT) {
                // OK on root selects category
                if (root_selection == 0) {
                    current_menu = MENU_SENSOR_DATA;
                    ESP_LOGI(TAG, "Entered Sensors submenu");
                } else if (root_selection == 1) {
                    current_menu = MENU_SETTINGS;
                    ESP_LOGI(TAG, "Entered Settings submenu");
                } else if (root_selection == 2) {
                    current_menu = MENU_WATCHFACE;
                    watchface_selection = current_watchface;
                    ESP_LOGI(TAG, "Entered Watchface menu");
                }
            } else if (current_menu == MENU_WATCHFACE) {
                // OK selects watchface
                current_watchface = watchface_selection;
                ESP_LOGI(TAG, "Selected watchface: %d", watchface_selection);
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                current_menu = MENU_WATCH;
                editing_mode = false;
            } else if (current_menu == MENU_SETTINGS && !editing_mode) {
                // Enter edit mode for selected setting
                editing_mode = true;
                ESP_LOGI(TAG, "Entering edit mode for setting %d", current_setting);
            } else if (current_menu == MENU_SETTINGS && editing_mode) {
                // Exit edit mode and save changes
                editing_mode = false;
                settings_save(motion_threshold_editable, screen_timeout_editable, current_watchface);
                ESP_LOGI(TAG, "Exiting edit mode - changes saved to NVS");
            } else {
                // In other submenus (e.g., sensor list), OK returns to watch
                current_menu = MENU_WATCH;
                editing_mode = false;
                ESP_LOGI(TAG, "Returning to watch display");
            }
        }

        // Auto-exit menu after 10 seconds of inactivity
        if (current_menu != MENU_WATCH && (now - menu_last_activity_s) > 10) {
            ESP_LOGI(TAG, "Menu timeout - returning to watch");
            current_menu = MENU_WATCH;
            editing_mode = false;
        }

        // Only render if screen is on
        if (screen_on) {
            // Render appropriate menu
            switch (current_menu) {
                case MENU_WATCH:
                    render_watch_display(temp, hum, ax, ay, az, &timeinfo);
                    break;
                case MENU_ROOT:
                    render_root_menu();
                    break;
                case MENU_SENSOR_DATA:
                    render_sensor_menu(temp, hum, ax, ay, az);
                    break;
                case MENU_SETTINGS:
                    render_settings_menu();
                    break;
                case MENU_WATCHFACE:
                    render_watchface_menu();
                    break;
                default:
                    render_watch_display(temp, hum, ax, ay, az, &timeinfo);
                    break;
            }

            // render
            if (sh1106_render() != ESP_OK) {
                ESP_LOGW(TAG, "render failed");
            }
        }

        // Sleep for adaptive interval based on power mode (saves battery)
        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
    }
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "Init I2C");
    if (i2c_master_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return;
    }

    // quick scan
    int found = 0;
    for (uint8_t a=1; a<127; ++a) {
        if (i2c_probe_addr(a) == ESP_OK) {
            ESP_LOGI(TAG, "Device at 0x%02X", a);
            ++found;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    ESP_LOGI(TAG, "I2C devices found: %d", found);

    // WiFi init
    ESP_LOGI(TAG, "Init WiFi");
    wifi_init_sta();

    // create main task
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}