#pragma once
#include <stdint.h>
#include "lilka_board.h"

/* Rows per strip handed to the renderer; the screen is pushed strip by strip. */
#define LCD_STRIP_ROWS 24

/* Pixels are RGB565 already byte-swapped for the wire (see lcd_rgb). */
static inline uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

void lcd_init(void);
/* Fill `rows` rows starting at y0 (all LCD_WIDTH wide) with `render`, which
 * writes one row of LCD_WIDTH pixels at a time, and send them to the panel. */
typedef void (*lcd_row_renderer_t)(int y, uint16_t *row, void *ctx);
void lcd_draw_rows(int y0, int rows, lcd_row_renderer_t render, void *ctx);
void lcd_fill(uint16_t color);
