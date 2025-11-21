// display.h - framebuffer and SH1106 render helpers
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
void fb_draw_text(int x, int y, const char *s);
esp_err_t sh1106_render(void);

/* SH1106 control API */
esp_err_t sh1106_init(void);
esp_err_t sh1106_display_on(void);
esp_err_t sh1106_display_off(void);

#endif // DISPLAY_H
