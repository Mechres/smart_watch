#ifndef DISPLAY_H
#define DISPLAY_H

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
void fb_draw_rect(int x, int y, int w, int h, int color);
void fb_fill_rect(int x, int y, int w, int h, int color);

esp_err_t sh1106_render(void);

/* SH1106 control API */
esp_err_t sh1106_init(void);
esp_err_t sh1106_display_on(void);
esp_err_t sh1106_display_off(void);

/* Set display contrast (brightness) 0-255 */
esp_err_t sh1106_set_contrast(uint8_t contrast);

#endif // DISPLAY_H
