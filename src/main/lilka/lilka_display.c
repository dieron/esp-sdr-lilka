/* Minimal synchronous ST7789 driver for the Lilka v2 panel. Every transfer is
 * a polling SPI transaction, so drawing never depends on SPI interrupts. Bus
 * settings and panel init follow Keira (Arduino_GFX): 80 MHz, SPI mode 3. */
#include "lilka_display.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lilka";
static spi_device_handle_t lcd;
static uint16_t *strip;

static void lcd_send(bool data, const void *buf, size_t len) {
    if (!len) return;
    gpio_set_level(LILKA_DISPLAY_DC, data);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = buf};
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &t));
}

static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    lcd_send(false, &cmd, 1);
    lcd_send(true, data, len);
}

static void lcd_window(int x0, int y0, int x1, int y1) {
    x0 += LCD_X_OFFSET;
    x1 += LCD_X_OFFSET;
    y0 += LCD_Y_OFFSET;
    y1 += LCD_Y_OFFSET;
    uint8_t col[4] = {x0 >> 8, x0 & 255, x1 >> 8, x1 & 255};
    uint8_t row[4] = {y0 >> 8, y0 & 255, y1 >> 8, y1 & 255};
    lcd_cmd(0x2a, col, 4); /* CASET */
    lcd_cmd(0x2b, row, 4); /* RASET */
    lcd_cmd(0x2c, NULL, 0); /* RAMWR */
}

void lcd_init(void) {
    /* Power up the display (Keira's Board::begin drives LILKA_SLEEP high; low
     * cuts the panel supply and the screen stays black) and keep the SD card
     * on the shared bus deselected. */
    gpio_config_t out = {
        .pin_bit_mask = (1ull << LILKA_DISPLAY_DC) | (1ull << LILKA_SDCARD_CS) | (1ull << LILKA_GPIO_SLEEP),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(LILKA_SDCARD_CS, 1);
    gpio_set_level(LILKA_GPIO_SLEEP, 1);
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the panel supply settle before SWRESET */

    spi_bus_config_t bus = {
        .mosi_io_num = LILKA_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LILKA_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_STRIP_ROWS * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 80 * 1000 * 1000,
        .mode = 3, /* Arduino_GFX forces mode 3 for the ST7789 on ESP32 */
        .spics_io_num = LILKA_DISPLAY_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd));

    strip = heap_caps_malloc(LCD_WIDTH * LCD_STRIP_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(strip);

    lcd_cmd(0x01, NULL, 0); /* SWRESET: the panel may still hold Keira's state */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11, NULL, 0); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    /* Arduino_GFX's ST7789 init, as Keira runs it: porch, gate, VCOM, power
     * and gamma for this panel instead of the reset defaults. */
    lcd_cmd(0x3a, (const uint8_t[]){0x55}, 1); /* COLMOD: 16 bpp */
    lcd_cmd(0xb0, (const uint8_t[]){0x00, 0xf0}, 2);
    lcd_cmd(0xb2, (const uint8_t[]){0x0c, 0x0c, 0x00, 0x33, 0x33}, 5);
    lcd_cmd(0xb7, (const uint8_t[]){0x35}, 1);
    lcd_cmd(0xbb, (const uint8_t[]){0x19}, 1);
    lcd_cmd(0xc0, (const uint8_t[]){0x2c}, 1);
    lcd_cmd(0xc2, (const uint8_t[]){0x01}, 1);
    lcd_cmd(0xc3, (const uint8_t[]){0x12}, 1);
    lcd_cmd(0xc4, (const uint8_t[]){0x20}, 1);
    lcd_cmd(0xc6, (const uint8_t[]){0x0f}, 1);
    lcd_cmd(0xd0, (const uint8_t[]){0xa4, 0xa1}, 2);
    lcd_cmd(0xe0, (const uint8_t[]){0xf0, 0x09, 0x13, 0x12, 0x12, 0x2b, 0x3c, 0x44, 0x4b, 0x1b, 0x18, 0x17, 0x1d, 0x21},
            14);
    lcd_cmd(0xe1, (const uint8_t[]){0xf0, 0x09, 0x13, 0x0c, 0x0d, 0x27, 0x3b, 0x44, 0x4d, 0x0b, 0x17, 0x17, 0x1d, 0x21},
            14);
    lcd_cmd(0x36, (const uint8_t[]){LCD_MADCTL}, 1); /* MADCTL: landscape */
    lcd_cmd(0x21, NULL, 0);                          /* INVON: IPS panel */
    lcd_cmd(0x13, NULL, 0);                          /* NORON */
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_fill(0);
    lcd_cmd(0x29, NULL, 0); /* DISPON */
    ESP_LOGI(TAG, "LCD SPI at 80 MHz, mode 3");
}

void lcd_draw_rows(int y0, int rows, lcd_row_renderer_t render, void *ctx) {
    while (rows > 0) {
        int n = rows < LCD_STRIP_ROWS ? rows : LCD_STRIP_ROWS;
        for (int r = 0; r < n; r++) render(y0 + r, strip + r * LCD_WIDTH, ctx);
        lcd_window(0, y0, LCD_WIDTH - 1, y0 + n - 1);
        lcd_send(true, strip, (size_t)n * LCD_WIDTH * 2);
        y0 += n;
        rows -= n;
    }
}

static void fill_row(int y, uint16_t *row, void *ctx) {
    (void)y;
    uint16_t c = *(const uint16_t *)ctx;
    for (int x = 0; x < LCD_WIDTH; x++) row[x] = c;
}

void lcd_fill(uint16_t color) {
    lcd_draw_rows(0, LCD_HEIGHT, fill_row, &color);
}
