// display.c - implements framebuffer helpers and SH1106 rendering

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "display.h"
#include "sensors.h"
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
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_TIMEOUT_MS              1000
#define ACK_CHECK_EN                0x1
#define SH1106_ADDR                 0x3C

static uint8_t fb[DISP_WIDTH * PAGE_COUNT];
static uint8_t last_fb[DISP_WIDTH * PAGE_COUNT]; // For dirty check

void fb_clear(void) { memset(fb, 0x00, sizeof(fb)); }

void fb_set_pixel(int x, int y, int color) {
    if (x < 0 || x >= DISP_WIDTH || y < 0 || y >= DISP_HEIGHT) return;
    int page = y >> 3;
    int idx = page * DISP_WIDTH + x;
    uint8_t bit = 1 << (y & 0x7);
    if (color) fb[idx] |= bit;
    else fb[idx] &= ~bit;
}

void fb_draw_line(int x0, int y0, int x1, int y1, int color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    for (;;) {
        fb_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fb_draw_rect(int x, int y, int w, int h, int color) {
    fb_draw_line(x, y, x + w - 1, y, color);
    fb_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    fb_draw_line(x, y, x, y + h - 1, color);
    fb_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void fb_fill_rect(int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) return;

    /* Clip to screen */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > DISP_WIDTH ? DISP_WIDTH : x + w;
    int y1 = y + h > DISP_HEIGHT ? DISP_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    /* Fast path: operate byte-wise on fully covered pages */
    int page_start = y0 >> 3;
    int page_end = (y1 - 1) >> 3;

    for (int page = page_start; page <= page_end; page++) {
        int bit_lo = (page == page_start) ? (y0 & 7) : 0;
        int bit_hi = (page == page_end) ? ((y1 - 1) & 7) : 7;
        uint8_t mask = (uint8_t)(((1u << (bit_hi - bit_lo + 1)) - 1) << bit_lo);
        uint8_t val = color ? mask : 0;
        int idx = page * DISP_WIDTH;
        for (int i = x0; i < x1; i++) {
            if (color) fb[idx + i] |= mask;
            else fb[idx + i] &= ~mask;
        }
        (void)val;
    }
}

/* Internal helper for drawing characters with specific colors and scale */
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x07,0x00,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x02,0x01,0x02,0x04,0x02}, {0x7F,0x7F,0x7F,0x7F,0x7F}
};

static void fb_draw_char_internal(int x, int y, char c, int color, int bg_color, int scale) {
    if (c < 32 || c > 127) c = '?';

    const uint8_t *glyph = font5x7[c - 32];

    // Fast path: scale == 1 and fully on screen
    if (scale == 1 && x >= 0 && x + 5 <= DISP_WIDTH && y >= 0 && y + 7 <= DISP_HEIGHT) {
        int page = y >> 3;
        int shift = y & 7;

        for (int col = 0; col < 5; ++col) {
            uint8_t colbits = glyph[col];
            int idx = page * DISP_WIDTH + x + col;

            if (shift == 0) {
                if (color == 1 && bg_color == 0) {
                    fb[idx] = (fb[idx] & ~0x7F) | colbits;
                } else if (color == 1 && bg_color == -1) {
                    fb[idx] |= colbits;
                } else if (color == 0 && bg_color == 1) {
                    fb[idx] = (fb[idx] & ~0x7F) | (~colbits & 0x7F);
                } else if (color == 0 && bg_color == -1) {
                    fb[idx] &= ~colbits;
                } else {
                    for (int row = 0; row < 7; ++row) {
                        int pixel_on = (colbits >> row) & 0x01;
                        fb_set_pixel(x + col, y + row, pixel_on ? color : bg_color);
                    }
                }
            } else {
                uint8_t low_bits = (colbits << shift) & 0xFF;
                uint8_t high_bits = (colbits >> (8 - shift));
                uint8_t mask_low = (0x7F << shift) & 0xFF;
                uint8_t mask_high = (0x7F >> (8 - shift)) & 0xFF;

                if (color == 1 && bg_color == 0) {
                    fb[idx] = (fb[idx] & ~mask_low) | low_bits;
                    if (page + 1 < PAGE_COUNT) fb[idx + DISP_WIDTH] = (fb[idx + DISP_WIDTH] & ~mask_high) | high_bits;
                } else if (color == 1 && bg_color == -1) {
                    fb[idx] |= low_bits;
                    if (page + 1 < PAGE_COUNT) fb[idx + DISP_WIDTH] |= high_bits;
                } else if (color == 0 && bg_color == 1) {
                    fb[idx] = (fb[idx] & ~mask_low) | (~low_bits & mask_low);
                    if (page + 1 < PAGE_COUNT) fb[idx + DISP_WIDTH] = (fb[idx + DISP_WIDTH] & ~mask_high) | (~high_bits & mask_high);
                } else if (color == 0 && bg_color == -1) {
                    fb[idx] &= ~low_bits;
                    if (page + 1 < PAGE_COUNT) fb[idx + DISP_WIDTH] &= ~high_bits;
                } else {
                    for (int row = 0; row < 7; ++row) {
                        int pixel_on = (colbits >> row) & 0x01;
                        fb_set_pixel(x + col, y + row, pixel_on ? color : bg_color);
                    }
                }
            }
        }
        return;
    }

    // Generic fallback for scaled text or partially clipped coordinates
    for (int col = 0; col < 5; ++col) {
        uint8_t colbits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            int pixel_on = (colbits >> row) & 0x01;
            int draw_color = pixel_on ? color : bg_color;
            
            if (scale == 1) {
                if (draw_color != -1) fb_set_pixel(x + col, y + row, draw_color);
            } else {
                if (draw_color != -1) fb_fill_rect(x + col * scale, y + row * scale, scale, scale, draw_color);
            }
        }
    }
}

void fb_draw_char(int x, int y, char c) {
    fb_draw_char_internal(x, y, c, 1, 0, 1);
}

void fb_draw_char_ex(int x, int y, char c, int color, int bg_color) {
    fb_draw_char_internal(x, y, c, color, bg_color, 1);
}

void fb_draw_text(int x, int y, const char *s) {
    while (*s) {
        fb_draw_char(x, y, *s++);
        x += 6;
    }
}

void fb_draw_text_ex(int x, int y, const char *s, int color, int bg_color) {
    while (*s) {
        fb_draw_char_ex(x, y, *s++, color, bg_color);
        x += 6;
    }
}

void fb_draw_text_scaled(int x, int y, const char *s, int scale) {
    while (*s) {
        fb_draw_char_internal(x, y, *s++, 1, 0, scale);
        x += 6 * scale;
    }
}

esp_err_t sh1106_render(void) {
    /* Dirty-page check: skip pages whose framebuffer slice is unchanged */
    bool any_dirty = false;
    for (int p = 0; p < PAGE_COUNT; ++p) {
        if (memcmp(&fb[p * DISP_WIDTH], &last_fb[p * DISP_WIDTH], DISP_WIDTH) != 0) {
            any_dirty = true;
            break;
        }
    }
    if (!any_dirty) {
        return ESP_OK;
    }

    if (!sensors_i2c_take(100)) {
        return ESP_ERR_TIMEOUT;
    }

    for (int p = 0; p < PAGE_COUNT; ++p) {
        if (memcmp(&fb[p * DISP_WIDTH], &last_fb[p * DISP_WIDTH], DISP_WIDTH) == 0) {
            continue; /* page unchanged - don't push */
        }
        esp_err_t r = sh1106_write_page(p, &fb[p*DISP_WIDTH]);
        if (r != ESP_OK) {
            sensors_i2c_give();
            return r;
        }
    }

    // Update last_fb
    memcpy(last_fb, fb, sizeof(fb));
    sensors_i2c_give();

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

/* send one page (128 bytes) to SH1106 in a SINGLE I2C transaction.
 * Control bytes: Co=1,D/C=0 (0x80) prefixes each single command;
 * Co=0,D/C=1 (0x40) starts the data stream. */
static esp_err_t sh1106_write_page(uint8_t page, const uint8_t *data128) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SH1106_ADDR << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);

    /* Single commands: page address + column start (0x02 = col 2, SH1106 offset) */
    uint8_t co = 0x80; /* Co=1, D/C=0: next byte is one command */
    uint8_t page_addr = (uint8_t)(0xB0 + page);
    uint8_t col_lo = 0x02;
    uint8_t col_hi = 0x10;
    i2c_master_write(cmd, &co, 1, ACK_CHECK_EN);
    i2c_master_write(cmd, &page_addr, 1, ACK_CHECK_EN);
    i2c_master_write(cmd, &co, 1, ACK_CHECK_EN);
    i2c_master_write(cmd, &col_lo, 1, ACK_CHECK_EN);
    i2c_master_write(cmd, &co, 1, ACK_CHECK_EN);
    i2c_master_write(cmd, &col_hi, 1, ACK_CHECK_EN);

    /* Data stream: Co=0, D/C=1 + 128 data bytes */
    uint8_t buf[129];
    buf[0] = 0x40;
    memcpy(&buf[1], data128, 128);
    i2c_master_write(cmd, buf, sizeof(buf), ACK_CHECK_EN);

    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
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
    memset(last_fb, 0xFF, sizeof(last_fb)); // Force update on first render
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

esp_err_t sh1106_set_contrast(uint8_t contrast) {
    const uint8_t cmd[] = {0x81, contrast};
    return sh1106_write_cmd(cmd, sizeof(cmd));
}
