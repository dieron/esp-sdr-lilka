/* SD card screenshots. The card sits on the display's SPI bus (CS 16); the
 * FAT volume is mounted read/write at /sd on first use and never formatted.
 * Pictures are written as uncompressed PNG: deflate "stored" blocks, so no
 * compressor is needed, and any viewer opens them (~200 KB each). */
#include "lilka_sd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"
#include "lilka_board.h"
#include "sdmmc_cmd.h"

static const char *TAG = "lilka";
#define MOUNT_POINT "/sd"
#define SHOT_DIR    MOUNT_POINT "/screenshots"

static sdmmc_card_t *card;
static int next_index = 1;

static bool sd_mount(void) {
    if (card) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = LILKA_SDCARD_CS;
    slot.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, /* it's Keira's card: never format it */
        .max_files = 2,
        .allocation_unit_size = 0,
    };
    esp_err_t e = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount, &card);
    if (e != ESP_OK) {
        card = NULL;
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted (%s)", card->cid.name);
    return true;
}

/* ---------- minimal streaming PNG writer (RGB, 8 bit, stored deflate) ---------- */

typedef struct {
    FILE *f;
    uint32_t crc;       /* CRC of the current chunk, type included */
    uint32_t adler_a, adler_b;
    uint32_t raw_left;  /* uncompressed bytes still to come */
    uint32_t block_left;
    bool ok;
} png_t;

static void put(png_t *p, const void *data, size_t n) {
    if (p->ok && fwrite(data, 1, n, p->f) != n) p->ok = false;
    p->crc = esp_rom_crc32_le(p->crc, data, n);
}

static void put_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    fwrite(b, 1, 4, f);
}

static void chunk_begin(png_t *p, const char *type, uint32_t len) {
    put_be32(p->f, len);
    p->crc = 0;
    put(p, type, 4);
}

static void chunk_end(png_t *p) {
    put_be32(p->f, p->crc);
}

/* Image bytes (filter bytes included): split into stored deflate blocks of at
 * most 65535 bytes, each with its header, and counted into the Adler-32. */
static void put_raw(png_t *p, const uint8_t *data, size_t n) {
    while (n) {
        if (!p->block_left) {
            uint32_t len = p->raw_left < 65535 ? p->raw_left : 65535;
            uint8_t hdr[5] = {len == p->raw_left ? 1 : 0, len & 255, len >> 8, ~len & 255, (~len >> 8) & 255};
            put(p, hdr, 5);
            p->block_left = len;
        }
        size_t k = n < p->block_left ? n : p->block_left;
        put(p, data, k);
        for (size_t i = 0; i < k; i++) {
            p->adler_a = (p->adler_a + data[i]) % 65521;
            p->adler_b = (p->adler_b + p->adler_a) % 65521;
        }
        p->block_left -= k;
        p->raw_left -= k;
        data += k;
        n -= k;
    }
}

static bool write_png(FILE *f, int w, int h, sd_row_source_t source) {
    uint16_t *row = heap_caps_malloc(w * 2, MALLOC_CAP_INTERNAL);
    uint8_t *rgb = heap_caps_malloc(1 + w * 3, MALLOC_CAP_INTERNAL);
    if (!row || !rgb) {
        free(row);
        free(rgb);
        return false;
    }
    png_t p = {.f = f, .adler_a = 1, .ok = true};
    uint32_t raw = (uint32_t)h * (1 + w * 3);
    uint32_t blocks = (raw + 65534) / 65535;
    uint32_t idat = 2 + blocks * 5 + raw + 4;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    chunk_begin(&p, "IHDR", 13);
    put(&p, ihdr, 13);
    chunk_end(&p);

    chunk_begin(&p, "IDAT", idat);
    static const uint8_t zhdr[2] = {0x78, 0x01};
    put(&p, zhdr, 2);
    p.raw_left = raw;
    for (int y = 0; y < h; y++) {
        source(y, row);
        rgb[0] = 0; /* filter: none */
        const uint8_t *px = (const uint8_t *)row;
        for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(px[2 * x] << 8 | px[2 * x + 1]); /* wire order = big-endian RGB565 */
            uint8_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            rgb[1 + 3 * x] = (r * 255 + 15) / 31;
            rgb[2 + 3 * x] = (g * 255 + 31) / 63;
            rgb[3 + 3 * x] = (b * 255 + 15) / 31;
        }
        put_raw(&p, rgb, 1 + w * 3);
    }
    uint8_t adler[4] = {p.adler_b >> 8, p.adler_b, p.adler_a >> 8, p.adler_a};
    put(&p, adler, 4);
    chunk_end(&p);

    chunk_begin(&p, "IEND", 0);
    chunk_end(&p);
    free(row);
    free(rgb);
    return p.ok && !ferror(f);
}

bool sd_save_screenshot(int width, int height, sd_row_source_t source, char *name_out, size_t name_len) {
    if (!sd_mount()) return false;
    if (mkdir(SHOT_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %d", SHOT_DIR, errno);
        return false;
    }
    char path[64];
    struct stat st;
    for (; next_index < 10000; next_index++) {
        snprintf(path, sizeof(path), SHOT_DIR "/esp-sdr_%03d.png", next_index);
        if (stat(path, &st) != 0) break; /* first free number */
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s: %d", path, errno);
        return false;
    }
    static char buf[4096];
    setvbuf(f, buf, _IOFBF, sizeof(buf));
    bool ok = write_png(f, width, height, source);
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(path);
        ESP_LOGW(TAG, "write %s failed", path);
        return false;
    }
    snprintf(name_out, name_len, "esp-sdr_%03d.png", next_index++);
    ESP_LOGI(TAG, "screenshot saved: %s", path);
    return true;
}
