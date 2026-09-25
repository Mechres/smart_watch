#include "sensors.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Sensors";

/* I2C configuration (externally defined or we can redefine if we want to keep it self-contained, 
   but main.c initializes I2C master. We'll assume I2C is already initialized by main.c 
   and we just use the port number.) */
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_TIMEOUT_MS 1000
#define ACK_CHECK_EN   0x1

/* ADXL345 registers */
#define ADXL345_REG_THRESH_TAP      0x1D
#define ADXL345_REG_DUR             0x21
#define ADXL345_REG_LATENT          0x22
#define ADXL345_REG_WINDOW          0x23
#define ADXL345_REG_TAP_AXES        0x2A
#define ADXL345_REG_BW_RATE         0x2C
#define ADXL345_REG_POWER_CTL       0x2D
#define ADXL345_REG_INT_ENABLE      0x2E
#define ADXL345_REG_INT_MAP         0x2F
#define ADXL345_REG_INT_SOURCE      0x30
#define ADXL345_REG_DATA_FORMAT     0x31
#define ADXL345_REG_DATAX0          0x32

/* AHT10 commands */
#define AHT10_CMD_SOFTRESET         0xBA
#define AHT10_CMD_TRIGGER           0xAC

static esp_err_t adxl345_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, reg, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, val, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t adxl345_init(void) {
    esp_err_t err;
    // set data format = 0x28 (full resolution, +-2g, INT_INVERT active low)
    uint8_t df[2] = {ADXL345_REG_DATA_FORMAT, 0x28};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
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
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write(cmd, pc, sizeof(pc), ACK_CHECK_EN);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

#include "freertos/semphr.h"

static SemaphoreHandle_t s_i2c_mutex = NULL;

bool sensors_i2c_take(uint32_t timeout_ms) {
    if (!s_i2c_mutex) return true;
    return xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void sensors_i2c_give(void) {
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

void sensors_init(void) {
    ESP_LOGI(TAG, "Initializing sensors...");

    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
    }
    
    // ADXL345 init
    if (adxl345_init() != ESP_OK) {
        ESP_LOGW(TAG, "ADXL345 init failed");
    }

    // AHT10 soft reset
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        ESP_LOGW(TAG, "AHT10 soft reset skipped (no mem)");
        return;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AHT10_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, AHT10_CMD_SOFTRESET, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(TAG, "AHT10 soft reset OK");
    } else {
        ESP_LOGW(TAG, "AHT10 soft reset failed");
    }
}

esp_err_t sensors_start_temp_hum(void) {
    if (!sensors_i2c_take(150)) {
        return ESP_ERR_TIMEOUT;
    }

    // trigger: AC 33 00
    uint8_t cmdt[3] = {AHT10_CMD_TRIGGER, 0x33, 0x00};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        sensors_i2c_give();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AHT10_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write(cmd, cmdt, sizeof(cmdt), ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    sensors_i2c_give();
    return err;
}

esp_err_t sensors_poll_temp_hum(float *temperature, float *humidity) {
    if (!sensors_i2c_take(150)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t data[6];
    esp_err_t err = i2c_master_read_from_device(I2C_MASTER_NUM, AHT10_ADDR, data, 6, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    sensors_i2c_give();
    if (err != ESP_OK) return err;
    if (data[0] & 0x80) return ESP_ERR_INVALID_STATE; // still busy

    uint32_t hum_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humidity = (float)hum_raw * 100.0f / 1048576.0f;
    *temperature = ((float)temp_raw * 200.0f / 1048576.0f) - 50.0f;

    return ESP_OK;
}

esp_err_t sensors_read_temp_hum(float *temperature, float *humidity) {
    esp_err_t err = sensors_start_temp_hum();
    if (err != ESP_OK) return err;

    // Release CPU while waiting for measurement conversion (80ms)
    vTaskDelay(pdMS_TO_TICKS(80));

    return sensors_poll_temp_hum(temperature, humidity);
}

esp_err_t sensors_read_accel(int16_t *x, int16_t *y, int16_t *z) {
    if (!sensors_i2c_take(50)) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t reg = ADXL345_REG_DATAX0;
    uint8_t data[6];
    esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM, ADXL345_ADDR, &reg, 1, data, 6, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    sensors_i2c_give();
    if (err != ESP_OK) return err;
    *x = (int16_t)((data[1]<<8) | data[0]);
    *y = (int16_t)((data[3]<<8) | data[2]);
    *z = (int16_t)((data[5]<<8) | data[4]);
    return ESP_OK;
}

esp_err_t sensors_config_tap_wakeup(void) {
    ESP_LOGI(TAG, "Configuring ADXL345 tap detection...");
    
    // 1. THRESH_TAP (0x1D): Threshold for tap. 62.5mg/LSB. 
    // Value 48 = 3g (48 * 0.0625 = 3). Adjust as needed.
    if (adxl345_write_reg(ADXL345_REG_THRESH_TAP, 48) != ESP_OK) return ESP_FAIL;
    
    // 2. DUR (0x21): Duration. 625us/LSB.
    // Value 32 = 20ms.
    if (adxl345_write_reg(ADXL345_REG_DUR, 32) != ESP_OK) return ESP_FAIL;
    
    // 3. LATENT (0x22): Latency. 1.25ms/LSB.
    // Value 80 = 100ms.
    if (adxl345_write_reg(ADXL345_REG_LATENT, 80) != ESP_OK) return ESP_FAIL;
    
    // 4. WINDOW (0x23): Window. 1.25ms/LSB.
    // Value 240 = 300ms.
    if (adxl345_write_reg(ADXL345_REG_WINDOW, 240) != ESP_OK) return ESP_FAIL;
    
    // 5. TAP_AXES (0x2A): Enable all axes (X, Y, Z) for tap.
    // Bit 0=Z, 1=Y, 2=X. 0x07 = all axes.
    if (adxl345_write_reg(ADXL345_REG_TAP_AXES, 0x07) != ESP_OK) return ESP_FAIL;
    
    // 6. INT_MAP (0x2F): Map SINGLE_TAP to INT1.
    // SINGLE_TAP is bit 6. 0 = INT1, 1 = INT2.
    // We want INT1, so bit 6 should be 0.
    // Default is 0, but let's ensure.
    if (adxl345_write_reg(ADXL345_REG_INT_MAP, 0x00) != ESP_OK) return ESP_FAIL;
    
    // 7. INT_ENABLE (0x2E): Enable SINGLE_TAP interrupt.
    // Bit 6 = SINGLE_TAP.
    if (adxl345_write_reg(ADXL345_REG_INT_ENABLE, 0x40) != ESP_OK) return ESP_FAIL;
    
    // Clear any pending interrupt
    sensors_clear_tap_interrupt();

    ESP_LOGI(TAG, "ADXL345 tap config done");
    return ESP_OK;
}

esp_err_t sensors_clear_tap_interrupt(void) {
    uint8_t reg = ADXL345_REG_INT_SOURCE;
    uint8_t val = 0;
    return i2c_master_write_read_device(I2C_MASTER_NUM, ADXL345_ADDR, &reg, 1, &val, 1, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t sensors_set_low_power_mode(bool enable) {
    /* BW_RATE: bit4=LOW_POWER, bits[3:0]=rate.
     * Normal: 0x0A = 100 Hz normal. Low-power: 0x17 = LOW_POWER + 12.5 Hz.
     * 12.5 Hz is plenty for tap/wrist-tilt while saving current. */
    uint8_t rate = enable ? 0x17 : 0x0A;
    esp_err_t err = adxl345_write_reg(ADXL345_REG_BW_RATE, rate);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BW_RATE config failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ADXL345 %s-power mode (rate=0x%02X)", enable ? "low" : "normal", rate);
    }
    return err;
}
