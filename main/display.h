#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "esp_err.h"

/* Display dimensions */
#define DISP_WIDTH 128
#define DISP_HEIGHT 64
#define PAGE_COUNT (DISP_HEIGHT / 8)

void fb_clear(void);
void fb_set_pixel(int x, int y, int color);
void fb_draw_char(int x, int y, char c);
void fb_draw_char_ex(int x, int y, char c, int color, int bg_color);
void fb_draw_text(int x, int y, const char *s);
void fb_draw_text_ex(int x, int y, const char *s, int color, int bg_color);
void fb_draw_text_scaled(int x, int y, const char *s, int scale);

void fb_draw_line(int x0, int y0, int x1, int y1, int color);
void fb_draw_line_thick(int x0, int y0, int x1, int y1, int thickness, int color);
void fb_draw_rect(int x, int y, int w, int h, int color);
void fb_fill_rect(int x, int y, int w, int h, int color);
void fb_draw_circle(int cx, int cy, int r, int color);
void fb_fill_circle(int cx, int cy, int r, int color);
void fb_draw_bitmap(int x, int y, int w, int h, const uint8_t *bitmap, int color);
void fb_draw_progress_bar(int x, int y, int w, int h, int pct);
void fb_draw_battery_icon(int x, int y, int pct, int color, int bg);
int fb_text_width(const char *s, int scale);
void fb_draw_text_centered(int y, const char *s);
void fb_draw_text_centered_ex(int y, const char *s, int color, int bg_color);
void fb_draw_text_centered_scaled(int y, const char *s, int scale);
void fb_draw_header(const char *title);

/* 12x10 weather glyph for WMO codes (sun/cloud/fog/rain/snow/storm). */
void fb_draw_wx_icon(int x, int y, int wx_code);

/* 7-segment style big digits (14x24 cell). Supports '0'-'9', ':', '.', ' '. */
void fb_draw_big_digit(int x, int y, int digit, int color);
void fb_draw_big_text(int x, int y, const char *s, int color);
int fb_big_text_width(const char *s);

esp_err_t sh1106_render(void);

/* SH1106 control API */
esp_err_t sh1106_init(void);
esp_err_t sh1106_display_on(void);
esp_err_t sh1106_display_off(void);

/* Set display contrast (brightness) 0-255 */
esp_err_t sh1106_set_contrast(uint8_t contrast);

#endif // DISPLAY_H
