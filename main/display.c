// display.c - implements framebuffer helpers and SH1106 rendering

#include <string.h>
#include <stdint.h>

#include <string.h>
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_err.h"

/* prototype for helper defined later in this file */
static esp_err_t sh1106_write_page(uint8_t page, const uint8_t *data128);

/* I2C / SH1106 config (dup of main.c - kept local to display module) */
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SDA_IO           8
#define I2C_MASTER_SCL_IO           9
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_TIMEOUT_MS              1000
#define ACK_CHECK_EN                0x1
#define SH1106_ADDR                 0x3C

static uint8_t fb[DISP_WIDTH * PAGE_COUNT];

void fb_clear(void) { memset(fb, 0x00, sizeof(fb)); }

void fb_set_pixel(int x, int y, int color) {
    if (x < 0 || x >= DISP_WIDTH || y < 0 || y >= DISP_HEIGHT) return;
    int page = y >> 3;
    int idx = page * DISP_WIDTH + x;
    uint8_t bit = 1 << (y & 0x7);
    if (color) fb[idx] |= bit;
    else fb[idx] &= ~bit;
}

void fb_draw_char(int x, int y, char c) {
    if (c < 32 || c > 127) c = '?';
    extern const uint8_t font5x7[][5]; // font defined in main.c
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t colbits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            int px = x + col;
            int py = y + row;
            fb_set_pixel(px, py, (colbits >> row) & 0x01);
        }
    }
}

void fb_draw_text(int x, int y, const char *s) {
    while (*s) {
        fb_draw_char(x, y, *s++);
        x += 6;
    }
}

esp_err_t sh1106_render(void) {
    for (int p = 0; p < PAGE_COUNT; ++p) {
        esp_err_t r = sh1106_write_page(p, &fb[p*DISP_WIDTH]);
        if (r != ESP_OK) return r;
    }
    return ESP_OK;
}

/* ---------- SH1106 comms & control (moved from main.c) ---------- */
static esp_err_t sh1106_write_cmd(const uint8_t *cmds, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SH1106_ADDR<<1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    uint8_t control = 0x00;
    i2c_master_write(cmd, &control, 1, ACK_CHECK_EN);
    i2c_master_write(cmd, (uint8_t*)cmds, len, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* send one page (128 bytes) to SH1106. We send a 129-byte buffer: [0x40, data... ] */
static esp_err_t sh1106_write_page(uint8_t page, const uint8_t *data128) {
    uint8_t page_cmds[] = { (uint8_t)(0xB0 + page), 0x02, 0x10 };
    if (sh1106_write_cmd(page_cmds, sizeof(page_cmds)) != ESP_OK) return ESP_FAIL;

    uint8_t buf[129];
    buf[0] = 0x40; // data control byte
    memcpy(&buf[1], data128, 128);
    esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, SH1106_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    return err;
}

esp_err_t sh1106_init(void) {
    const uint8_t init_seq[] = {
        0xAE, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xA1, 0xC8,
        0xDA, 0x12, 0x81, 0x7F, 0xA4, 0xA6, 0xD5, 0x80,
        0x8D, 0x14, 0xAF
    };
    esp_err_t err = sh1106_write_cmd(init_seq, sizeof(init_seq));
    if (err != ESP_OK) return err;
    fb_clear();
    if (sh1106_render() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (sh1106_render() != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sh1106_display_on(void) {
    const uint8_t cmd[] = {0xAF};
    return sh1106_write_cmd(cmd, sizeof(cmd));
}

esp_err_t sh1106_display_off(void) {
    const uint8_t cmd[] = {0xAE};
    return sh1106_write_cmd(cmd, sizeof(cmd));
}
