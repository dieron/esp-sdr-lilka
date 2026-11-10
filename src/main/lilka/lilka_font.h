#pragma once
/* 5x7 glyphs, 6-pixel advance, 8-pixel line at scale 1. */
#define FONT_W       5
#define FONT_H       7
#define FONT_ADVANCE 6

/* 35 characters ('#' = ink), row-major, or NULL for blank. */
const char *font_glyph(char c);
