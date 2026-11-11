#pragma once
/* Lilka v2 pin map (from lilka-dev/sdk lib/Lilka/src/lilka/config.h). */

/* Buttons: active low, internal pull-up. */
#define LILKA_GPIO_UP     38
#define LILKA_GPIO_DOWN   41
#define LILKA_GPIO_LEFT   39
#define LILKA_GPIO_RIGHT  40
#define LILKA_GPIO_SELECT 0
#define LILKA_GPIO_START  4
#define LILKA_GPIO_A      5
#define LILKA_GPIO_B      6
#define LILKA_GPIO_C      10
#define LILKA_GPIO_D      9

/* ST7789 IPS 240x280 on the shared SPI bus (the SD card, CS 16, is unused). */
#define LILKA_SPI_SCK     18
#define LILKA_SPI_MOSI    17
#define LILKA_SPI_MISO    8  /* SD card only; the display is write-only */
#define LILKA_DISPLAY_DC  15
#define LILKA_DISPLAY_CS  7
#define LILKA_SDCARD_CS   16

/* Peripheral power: Keira drives it high at boot and low in power-saving
 * mode. Low leaves the screen black. */
#define LILKA_GPIO_SLEEP  46

/* Landscape, as Keira draws it: Arduino_GFX rotation 3 = MADCTL MY|MV, and
 * the 280-pixel axis of the panel starts 20 pixels into the controller RAM. */
#define LCD_WIDTH    280
#define LCD_HEIGHT   240
#define LCD_MADCTL   0xA0
#define LCD_X_OFFSET 20
#define LCD_Y_OFFSET 0

/* Rows the UI draws on, from the top; the rest stays black. Set at build time
 * with -DLILKA_LCD_USED_HEIGHT=<rows> (default 240, the whole screen). Use 196
 * for a panel that drops lines in its bottom strip, like the development
 * Lilka's (landscape rows ~198..239; Keira's demo apps show it too). */
#ifndef LCD_USED_HEIGHT
#define LCD_USED_HEIGHT 240
#endif
#if LCD_USED_HEIGHT < 160 || LCD_USED_HEIGHT > 240
#error "LCD_USED_HEIGHT must be 160..240"
#endif
