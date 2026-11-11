#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Row source for a screenshot: fill `row` (width pixels) for line y with
 * byte-swapped RGB565, as sent to the display. */
typedef void (*sd_row_source_t)(int y, uint16_t *row);

/* Save the screen as /screenshots/esp-sdr_NNN.png on the SD card (the folder
 * Keira uses for its own screenshots). Mounts the card on first use, on the
 * display's SPI bus; never formats it. On success writes the file name (e.g.
 * "esp-sdr_007.png") to name_out and returns true. */
bool sd_save_screenshot(int width, int height, sd_row_source_t source, char *name_out, size_t name_len);
