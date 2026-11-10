/* ESP-SDR on Lilka: a standalone 1.84-2.79 GHz spectrum and waterfall viewer.
 *
 * Each frame takes one ESP-SDR snapshot (up to 16380 samples), averages
 * Hann-windowed FFTs over it (Welch), draws the spectrum line and pushes one
 * new row onto the waterfall. Snapshots leave gaps between captures, so short
 * bursts can be missed; that is fine for a waterfall. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsps_fft2r.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lilka_buttons.h"
#include "lilka_display.h"
#include "lilka_font.h"
#include "sdr_rx.h"

static const char *TAG = "lilka";

#define NFFT     1024
#define W        LCD_WIDTH
#define STATUS_H 20
/* The panel's corners are rounded (radius ~40 px): the top text line (y 2..9)
 * loses ~28 px at each end, the second line (y 11..18) ~14 px. */
#define STATUS_INSET_TOP 28
#define STATUS_INSET     14
#define SPEC_Y   20
#define SPEC_H   90
#define AXIS_Y   (SPEC_Y + SPEC_H)
#define AXIS_H   10
#define WF_Y     (AXIS_Y + AXIS_H)
#define WF_H     (LCD_USED_HEIGHT - WF_Y) /* rows below LCD_USED_HEIGHT stay black */

#define RANGE_DB      65.0f /* vertical span of the spectrum and the colour map */
#define WF_AVG        3     /* frames averaged into one waterfall row (~10 rows/s, ~8 s shown) */
#define FLOOR_BELOW   10.0f /* noise floor sits this far above the bottom */
#define SAMPLES       SDR_MAX_SAMPLES
#define TUNE_SETTLE_MS 150   /* retune after the frequency stops changing */
#define TUNE_EVERY_MS  400   /* ...or this often while a button is held */
#define EXIT_HOLD_MS   1000

static const struct {
    unsigned code, msps;
} rates[] = {{SDR_RATE_80M, 80}, {SDR_RATE_40M, 40}, {SDR_RATE_16M, 16}};
#define RATE_COUNT (sizeof(rates) / sizeof(rates[0]))

static const unsigned gain_steps[] = {10, 20, 30, 40, 50, 60, 70, 80};
#define GAIN_STEPS (sizeof(gain_steps) / sizeof(gain_steps[0]))
#define DEFAULT_GAIN_IDX 5 /* 60: signals well clear of the floor without clipping 2.4 GHz Wi-Fi nearby */

typedef struct {
    unsigned mhz;
    int rate_idx;     /* span: 0 = 80, 1 = 40, 2 = 16 MHz */
    const char *name; /* <= 11 characters: also shown in the status bar */
    const char *note; /* <= 15 characters, shown in the preset list */
} preset_t;
/* The first entry is where the app starts. Cellular bands: DL = towers (always
 * on), UL = phones transmitting nearby (bursty, strongest when you make a call
 * or upload). Everything here fits the S3's 1840-2790 MHz tuning range. */
static const preset_t presets[] = {
    {2442, 0, "ISM 2.4 GHZ", "WHOLE BAND"},
    {2412, 1, "WIFI CH 1", ""},
    {2437, 1, "WIFI CH 6", ""},
    {2462, 1, "WIFI CH 11", ""},
    {2472, 1, "WIFI CH 13", "EU, JAPAN"},
    {2484, 1, "WIFI CH 14", "JAPAN, 11B ONLY"},
    {2402, 2, "BLE ADV 37", "ADVERTISING"},
    {2426, 2, "BLE ADV 38", "ADVERTISING"},
    {2480, 2, "BLE ADV 39", "ADVERTISING"},
    {2425, 2, "ZIGBEE 15", "SMART HOME"},
    {2450, 2, "ZIGBEE 20", "SMART HOME"},
    {2475, 2, "ZIGBEE 25", "SMART HOME"},
    {2332, 1, "SIRIUSXM", "SAT RADIO, US"},
    {2492, 2, "GLOBALSTAR", "SAT PHONE DL"},
    {2405, 2, "QO-100 UP", "HAM SAT UPLINK"},
    {1843, 0, "LTE B3 DL", "1800, EU/ASIA"},
    {1890, 1, "DECT", "CORDLESS, EU"},
    {1925, 2, "DECT 6.0", "CORDLESS, US"},
    {1880, 0, "PCS UL", "PHONES TX, US"},
    {1960, 0, "PCS DL", "1900, US"},
    {1950, 0, "LTE B1 UL", "PHONES TX, EU"},
    {2140, 0, "LTE B1 DL", "2100, EU/ASIA"},
    {2160, 0, "AWS DL", "B4/B66, US"},
    {2350, 0, "LTE B40", "TDD, ASIA"},
    {2535, 0, "LTE B7 UL", "PHONES TX, EU"},
    {2600, 0, "5G N41", "TDD, US"},
    {2655, 0, "LTE B7 DL", "2600, EU"},
};
#define PRESET_COUNT (int)(sizeof(presets) / sizeof(presets[0]))

/* Preset list overlay (D opens it). */
#define MENU_X0    16
#define MENU_X1    (W - 16)
#define MENU_Y0    (STATUS_H + 2)
#define MENU_LIST_Y (MENU_Y0 + 16)
#define MENU_LINE  11
#define MENU_LINES ((LCD_USED_HEIGHT - 4 - MENU_LIST_Y) / MENU_LINE)
#define MENU_Y1    (MENU_LIST_Y + MENU_LINES * MENU_LINE + 4)

typedef struct {
    int16_t x, y;
    uint8_t scale;
    uint16_t color;
    char s[40];
} text_t;

typedef struct {
    /* radio */
    unsigned tuned_mhz, target_mhz;
    int rate_idx;
    bool agc;
    unsigned gain_idx;
    int64_t last_freq_change_ms, last_tune_ms;
    /* view */
    int ref_offset_db;
    bool peak_hold, paused;
    int preset_idx;
    bool menu_open;
    int menu_sel, menu_top;
    float floor_db, bottom_db, top_db;
    float col_db[W], peak_db[W];
    int16_t fill_h[W], line_lo[W], line_hi[W], peak_px[W];
    /* Waterfall: WF_H rows of W absolute levels, ring buffer. A level code c is
     * c/2 - 128 dBFS, so the colours follow REF and the floor for the whole
     * history instead of freezing at the scale each row was drawn with. */
    uint8_t *wf;
    int wf_head; /* row index of the newest line */
    float wf_acc[W]; /* linear power summed over the frames of the next row */
    int wf_nacc;
    uint16_t wf_lut[256]; /* level code -> colour at the current scale */
    float snap_gain; /* receiver gain index, mean over the last snapshot (for the log) */
    bool grid_row[SPEC_H];
    uint8_t marker[W]; /* 0 none, 1 minor (Wi-Fi channel), 2 major (1/6/11), 3 centre */
    uint8_t tick[W];   /* axis ticks: 1 minor, 2 labelled */
    uint16_t palette[256];
    /* stats */
    float fps;
    uint32_t capture_us;
    unsigned capture_errors;
    char msg[24];
    int64_t msg_until_ms;
    /* text overlay for the current frame */
    text_t text[56];
    int ntext;
} app_t;

static app_t *app;
static float *fft_buf, *window, *power;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* ---------- text ---------- */

static void text_add(int x, int y, int scale, uint16_t color, const char *s) {
    if (app->ntext >= (int)(sizeof(app->text) / sizeof(app->text[0]))) return;
    text_t *t = &app->text[app->ntext++];
    t->x = x;
    t->y = y;
    t->scale = scale;
    t->color = color;
    snprintf(t->s, sizeof(t->s), "%.*s", (int)sizeof(t->s) - 1, s);
}

static int text_width(const char *s, int scale) {
    return (int)strlen(s) * FONT_ADVANCE * scale - scale;
}

static void text_render_row(int y, uint16_t *row) {
    for (int i = 0; i < app->ntext; i++) {
        const text_t *t = &app->text[i];
        int gy = (y - t->y);
        if (gy < 0 || gy >= FONT_H * t->scale) continue;
        gy /= t->scale;
        int x = t->x;
        for (const char *p = t->s; *p; p++, x += FONT_ADVANCE * t->scale) {
            const char *g = font_glyph(*p);
            if (!g) continue;
            for (int c = 0; c < FONT_W; c++) {
                if (g[gy * FONT_W + c] != '#') continue;
                for (int s = 0; s < t->scale; s++) {
                    int px = x + c * t->scale + s;
                    if (px >= 0 && px < W) row[px] = t->color;
                }
            }
        }
    }
}

/* ---------- colours ---------- */

static void build_palette(void) {
    /* black -> blue -> cyan -> green -> yellow -> red -> white */
    static const uint8_t stops[][3] = {{0, 0, 0},     {0, 0, 140},   {0, 150, 220}, {0, 220, 80},
                                       {250, 230, 0}, {255, 60, 0},  {255, 255, 255}};
    const int nstops = sizeof(stops) / sizeof(stops[0]);
    for (int i = 0; i < 256; i++) {
        float f = i / 255.0f * (nstops - 1);
        int k = (int)f;
        if (k >= nstops - 1) k = nstops - 2;
        float t = f - k;
        uint8_t r = stops[k][0] + (stops[k + 1][0] - stops[k][0]) * t;
        uint8_t g = stops[k][1] + (stops[k + 1][1] - stops[k][1]) * t;
        uint8_t b = stops[k][2] + (stops[k + 1][2] - stops[k][2]) * t;
        app->palette[i] = lcd_rgb(r, g, b);
    }
}

/* ---------- frequency layout ---------- */

static unsigned span_mhz(void) {
    return rates[app->rate_idx].msps;
}

/* Screen column of an RF frequency, or -1 if off screen. */
static int freq_to_x(float mhz) {
    float x = (mhz - (float)app->tuned_mhz) / span_mhz() * W + W / 2.0f;
    return x < 0 || x >= W ? -1 : (int)x;
}

static void build_markers(void) {
    memset(app->marker, 0, sizeof(app->marker));
    memset(app->tick, 0, sizeof(app->tick));
    for (int ch = 1; ch <= 13; ch++) {
        int x = freq_to_x(2407.0f + 5.0f * ch);
        if (x >= 0) app->marker[x] = (ch == 1 || ch == 6 || ch == 11) ? 2 : 1;
    }
    app->marker[W / 2] = 3;
    unsigned span = span_mhz();
    unsigned step = span / 4; /* 20, 10 or 4 MHz between labels */
    unsigned minor = step / 2;
    float lo = app->tuned_mhz - span / 2.0f;
    for (unsigned f = ((unsigned)lo / minor + 1) * minor; f < lo + span; f += minor) {
        int x = freq_to_x(f);
        if (x >= 0) app->tick[x] = f % step == 0 ? 2 : 1;
    }
}

/* ---------- DSP ---------- */

static bool dsp_init(void) {
    fft_buf = heap_caps_aligned_alloc(16, NFFT * 2 * sizeof(float), MALLOC_CAP_INTERNAL);
    window = heap_caps_malloc(NFFT * sizeof(float), MALLOC_CAP_INTERNAL);
    power = heap_caps_malloc(NFFT * sizeof(float), MALLOC_CAP_INTERNAL);
    if (!fft_buf || !window || !power) return false;
    if (dsps_fft2r_init_fc32(NULL, NFFT) != ESP_OK) return false;
    for (int i = 0; i < NFFT; i++) window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / NFFT);
    return true;
}

static inline int sext10(uint32_t v) {
    return (int)((v & 0x3ffu) ^ 0x200u) - 0x200;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Welch-average the snapshot into power[] (natural FFT order, DC at 0). */
static void spectrum_from_samples(const uint32_t *s, unsigned n) {
    float mi = 0, mq = 0;
    unsigned gsum = 0;
    for (unsigned j = 0; j < n; j++) {
        mi += sext10(s[j]);
        mq -= sext10(s[j] >> 10);       /* the dump stores -Q */
        gsum += (s[j] >> 20) & 0x7f;    /* receiver gain index for this sample */
    }
    app->snap_gain = gsum / (float)n;
    mi /= n;
    mq /= n;
    memset(power, 0, NFFT * sizeof(float));
    unsigned segs = n / NFFT;
    for (unsigned g = 0; g < segs; g++) {
        const uint32_t *p = s + g * NFFT;
        for (int k = 0; k < NFFT; k++) {
            fft_buf[2 * k] = (sext10(p[k]) - mi) * window[k];
            fft_buf[2 * k + 1] = (-sext10(p[k] >> 10) - mq) * window[k];
        }
        dsps_fft2r_fc32(fft_buf, NFFT);
        dsps_bit_rev_fc32(fft_buf, NFFT);
        for (int k = 0; k < NFFT; k++) power[k] += fft_buf[2 * k] * fft_buf[2 * k] + fft_buf[2 * k + 1] * fft_buf[2 * k + 1];
    }
    /* The residual DC/LO-leakage spike would dominate the scale: notch it. */
    float edge = 0.5f * (power[3] + power[NFFT - 3]);
    power[0] = power[1] = power[2] = power[NFFT - 1] = power[NFFT - 2] = edge;

    /* dBFS: 10-bit full scale is 512, window coherent gain sum(w) = NFFT/2. */
    float norm = (float)segs * 512.0f * 512.0f * (NFFT / 2.0f) * (NFFT / 2.0f);
    for (int c = 0; c < W; c++) {
        int b0 = c * NFFT / W, b1 = (c + 1) * NFFT / W;
        float m = 0;
        for (int b = b0; b < b1; b++) {
            float v = power[(b + NFFT / 2) % NFFT]; /* fftshift */
            if (v > m) m = v;
        }
        app->col_db[c] = 10.0f * log10f(m / norm + 1e-12f);
    }
}

static void update_scale(void) {
    float sorted[W];
    memcpy(sorted, app->col_db, sizeof(sorted));
    qsort(sorted, W, sizeof(float), cmp_float);
    float floor_now = sorted[W / 4];
    if (app->floor_db < -500.0f)
        app->floor_db = floor_now;
    else
        app->floor_db += 0.08f * (floor_now - app->floor_db);
    app->bottom_db = app->floor_db - FLOOR_BELOW + app->ref_offset_db;
    app->top_db = app->bottom_db + RANGE_DB;
    memset(app->grid_row, 0, sizeof(app->grid_row));
    for (float db = ceilf(app->bottom_db / 10.0f) * 10.0f; db < app->top_db; db += 10.0f) {
        int r = SPEC_H - 1 - (int)((db - app->bottom_db) / RANGE_DB * SPEC_H);
        if (r >= 0 && r < SPEC_H) app->grid_row[r] = true;
    }
}

static int db_to_px(float db) {
    int h = (int)((db - app->bottom_db) / RANGE_DB * SPEC_H);
    return h < 0 ? 0 : h > SPEC_H ? SPEC_H : h;
}

static void update_traces(void) {
    for (int x = 0; x < W; x++) {
        if (app->peak_hold && app->col_db[x] > app->peak_db[x]) app->peak_db[x] = app->col_db[x];
        app->fill_h[x] = db_to_px(app->col_db[x]);
        app->peak_px[x] = app->peak_hold ? db_to_px(app->peak_db[x]) : -1;
    }
    for (int x = 0; x < W; x++) {
        int a = app->fill_h[x], b = x ? app->fill_h[x - 1] : a;
        app->line_lo[x] = (a < b ? a : b) - 1;
        app->line_hi[x] = a > b ? a : b;
    }
    /* Waterfall: one snapshot is only 0.2-1 ms of air time, so a single
     * frame per row flickers with every packet. Average a few. */
    for (int x = 0; x < W; x++) app->wf_acc[x] += powf(10.0f, app->col_db[x] * 0.1f);
    if (++app->wf_nacc < WF_AVG) return;
    app->wf_head = (app->wf_head + 1) % WF_H;
    uint8_t *row = app->wf + app->wf_head * W;
    for (int x = 0; x < W; x++) {
        float db = 10.0f * log10f(app->wf_acc[x] / app->wf_nacc + 1e-13f);
        float c = (db + 128.0f) * 2.0f;
        row[x] = c < 1 ? 1 : c > 255 ? 255 : (uint8_t)c; /* 0 is reserved for "no data" */
        app->wf_acc[x] = 0;
    }
    app->wf_nacc = 0;
}

/* Forget the waterfall. After a retune or span change the old rows no longer
 * line up with the frequency axis; after a gain change their levels are on a
 * different footing (dBFS moves with the receiver gain). */
static void wf_clear(void) {
    memset(app->wf, 0, WF_H * W);
    memset(app->wf_acc, 0, sizeof(app->wf_acc));
    app->wf_nacc = 0;
}

/* ---------- rendering ---------- */

static uint16_t c_bg, c_status, c_grid, c_marker, c_major, c_centre, c_fill, c_line, c_peak, c_text, c_dim, c_warn,
    c_accent, c_chan, c_menu, c_menu_sel;

static void colours_init(void) {
    c_bg = lcd_rgb(0, 0, 0);
    c_status = lcd_rgb(16, 20, 32);
    c_grid = lcd_rgb(28, 36, 44);
    c_marker = lcd_rgb(40, 40, 70);
    c_major = lcd_rgb(70, 70, 130);
    c_centre = lcd_rgb(90, 50, 50);
    c_fill = lcd_rgb(0, 70, 50);
    c_line = lcd_rgb(90, 255, 140);
    c_peak = lcd_rgb(255, 190, 0);
    c_text = lcd_rgb(230, 230, 230);
    c_dim = lcd_rgb(130, 140, 150);
    c_warn = lcd_rgb(255, 90, 60);
    c_accent = lcd_rgb(120, 200, 255);
    c_chan = lcd_rgb(140, 140, 220);
    c_menu = lcd_rgb(12, 16, 26);
    c_menu_sel = lcd_rgb(30, 60, 110);
}

static void build_wf_lut(void) {
    app->wf_lut[0] = c_bg;
    for (int c = 1; c < 256; c++) {
        float db = c * 0.5f - 128.0f;
        float f = (db - app->bottom_db) / RANGE_DB * 255.0f;
        app->wf_lut[c] = app->palette[f < 0 ? 0 : f > 255 ? 255 : (int)f];
    }
}

static void render_row(int y, uint16_t *row, void *ctx) {
    (void)ctx;
    if (y < STATUS_H) {
        uint16_t bg = y == STATUS_H - 1 ? c_grid : c_status;
        for (int x = 0; x < W; x++) row[x] = bg;
    } else if (y < AXIS_Y) {
        int r = y - SPEC_Y;            /* 0 = top */
        int h = SPEC_H - r;            /* pixel height above the bottom, 1..SPEC_H */
        bool grid = app->grid_row[r];
        for (int x = 0; x < W; x++) {
            uint16_t c = c_bg;
            uint8_t m = app->marker[x];
            if (m == 3 && (r & 2))
                c = c_centre;
            else if (m == 2)
                c = c_major;
            else if (m == 1 && (r & 1))
                c = c_marker;
            else if (grid)
                c = c_grid;
            if (h <= app->fill_h[x]) c = c_fill;
            if (h > app->line_lo[x] && h <= app->line_hi[x]) c = c_line;
            if (h == app->peak_px[x] || h == app->peak_px[x] + 1) c = c_peak;
            row[x] = c;
        }
    } else if (y < WF_Y) {
        int r = y - AXIS_Y;
        for (int x = 0; x < W; x++) {
            uint8_t t = app->tick[x];
            row[x] = (t == 2 && r < 3) || (t == 1 && r < 2) ? c_dim : c_bg;
        }
    } else if (y >= LCD_USED_HEIGHT) {
        for (int x = 0; x < W; x++) row[x] = c_bg;
    } else {
        int r = y - WF_Y; /* 0 = newest */
        const uint8_t *src = app->wf + ((app->wf_head - r + WF_H) % WF_H) * W;
        for (int x = 0; x < W; x++) row[x] = app->wf_lut[src[x]];
    }
    if (app->menu_open && y >= MENU_Y0 && y < MENU_Y1) {
        uint16_t bg = c_menu;
        int line = (y - MENU_LIST_Y + 2) / MENU_LINE;
        if (y >= MENU_LIST_Y - 2 && line >= 0 && line < MENU_LINES && app->menu_top + line == app->menu_sel)
            bg = c_menu_sel;
        else if (y == MENU_Y0 || y == MENU_Y1 - 1)
            bg = c_grid;
        for (int x = MENU_X0; x < MENU_X1; x++) row[x] = bg;
        row[MENU_X0] = row[MENU_X1 - 1] = c_grid;
    }
    text_render_row(y, row);
}

static void menu_text(void) {
    char b[48];
    snprintf(b, sizeof(b), "PRESETS %d/%d", app->menu_sel + 1, PRESET_COUNT);
    text_add(MENU_X0 + 6, MENU_Y0 + 4, 1, c_accent, b);
    text_add(MENU_X1 - 6 - text_width("A/D GO  B BACK", 1), MENU_Y0 + 4, 1, c_dim, "A/D GO  B BACK");
    for (int i = 0; i < MENU_LINES && app->menu_top + i < PRESET_COUNT; i++) {
        const preset_t *p = &presets[app->menu_top + i];
        snprintf(b, sizeof(b), "%-11s %4u %2uM %s", p->name, p->mhz, rates[p->rate_idx].msps, p->note);
        bool sel = app->menu_top + i == app->menu_sel;
        text_add(MENU_X0 + 6, MENU_LIST_Y + i * MENU_LINE, 1, sel ? c_text : c_dim, b);
    }
    /* scroll hints */
    if (app->menu_top > 0) text_add(MENU_X1 - 12, MENU_LIST_Y, 1, c_accent, "+");
    if (app->menu_top + MENU_LINES < PRESET_COUNT)
        text_add(MENU_X1 - 12, MENU_LIST_Y + (MENU_LINES - 1) * MENU_LINE, 1, c_accent, "+");
}

static void build_text(void) {
    char b[24];
    app->ntext = 0;
    bool tuning = app->target_mhz != app->tuned_mhz;
    snprintf(b, sizeof(b), "%u", app->target_mhz);
    text_add(STATUS_INSET_TOP - 2, 3, 2, tuning ? c_peak : c_text, b);
    text_add(STATUS_INSET_TOP - 2 + text_width(b, 2) + 4, 10, 1, c_dim, "MHZ");

    snprintf(b, sizeof(b), "SPAN %uM", span_mhz());
    text_add(104, 2, 1, c_text, b);
    if (app->agc)
        snprintf(b, sizeof(b), "GAIN AGC");
    else
        snprintf(b, sizeof(b), "GAIN %u", gain_steps[app->gain_idx]);
    text_add(104, 11, 1, c_accent, b);

    snprintf(b, sizeof(b), "REF %d", (int)lroundf(app->top_db));
    text_add(164, 2, 1, c_text, b);
    if (app->paused)
        text_add(164, 11, 1, c_warn, "PAUSED");
    else if (app->peak_hold)
        text_add(164, 11, 1, c_peak, "PEAK");

    if (app->msg_until_ms > now_ms())
        text_add(W - STATUS_INSET - text_width(app->msg, 1), 11, 1, c_peak, app->msg);
    snprintf(b, sizeof(b), "%dFPS", (int)lroundf(app->fps));
    text_add(W - STATUS_INSET_TOP - text_width(b, 1), 2, 1, c_dim, b);

    if (app->menu_open) {
        menu_text(); /* covers the spectrum labels below */
        return;
    }
    /* Wi-Fi channel numbers along the top of the spectrum */
    static const int major[] = {1, 6, 11};
    for (int i = 0; i < 3; i++) {
        int x = freq_to_x(2407.0f + 5.0f * major[i]);
        if (x < 0) continue;
        snprintf(b, sizeof(b), "%d", major[i]);
        text_add(x + 2, SPEC_Y + 2, 1, c_chan, b);
    }
    /* Frequency labels */
    unsigned span = span_mhz(), step = span / 4;
    float lo = app->tuned_mhz - span / 2.0f;
    for (unsigned f = ((unsigned)lo / step + 1) * step; f < lo + span; f += step) {
        int x = freq_to_x(f);
        snprintf(b, sizeof(b), "%u", f);
        int w = text_width(b, 1);
        if (x - w / 2 < 0 || x + w / 2 >= W) continue;
        text_add(x - w / 2, AXIS_Y + 3, 1, c_dim, b);
    }
    if (app->capture_errors) {
        snprintf(b, sizeof(b), "CAPTURE ERR %u", app->capture_errors);
        text_add(4, SPEC_Y + 12, 1, c_warn, b);
    }
}

static void draw_frame(void) {
    build_text();
    build_wf_lut();
    lcd_draw_rows(0, LCD_HEIGHT, render_row, NULL);
}

static void show_message(const char *m) {
    snprintf(app->msg, sizeof(app->msg), "%s", m);
    app->msg_until_ms = now_ms() + 1500;
}

/* ---------- splash ---------- */

static int64_t splash_ms;

static void splash_row(int y, uint16_t *row, void *ctx) {
    (void)ctx;
    for (int x = 0; x < W; x++) row[x] = c_status;
    text_render_row(y, row);
}

void lilka_early_init(void) {
    app = heap_caps_calloc(1, sizeof(app_t), MALLOC_CAP_INTERNAL);
    assert(app);
    app->wf = heap_caps_calloc(WF_H, W, MALLOC_CAP_INTERNAL);
    assert(app->wf);
    colours_init();
    buttons_init();
    lcd_init();
    static const char *help[] = {
        "LEFT/RIGHT  TUNE 1 MHZ (HOLD: FAST)",
        "UP/DOWN     REFERENCE LEVEL",
        "A  SPAN 80/40/16 MHZ",
        "B  GAIN 10-80, THEN AGC",
        "C  PEAK HOLD",
        "D  PRESET LIST (WIFI, BLE, CELL...)",
        "START  PAUSE",
        "SELECT  HOLD: KEIRA, TAP: SCREENSHOT",
    };
    app->ntext = 0;
    text_add(20, 14, 3, c_accent, "ESP-SDR");
    text_add(20, 42, 1, c_dim, "2.4 GHZ SPECTRUM ON THE WI-FI RADIO");
    for (int i = 0; i < 8; i++) text_add(20, 70 + i * 13, 1, c_text, help[i]);
    text_add(20, 70 + 8 * 13 + 10, 1, c_dim, "STARTING RADIO...");
    lcd_draw_rows(0, LCD_HEIGHT, splash_row, NULL);
    splash_ms = now_ms();
}

/* ---------- control ---------- */

static void apply_gain(void) {
    unsigned code = gain_steps[app->gain_idx];
    if (!app->agc && code > sdr_gain_max()) code = sdr_gain_max();
    if (!sdr_gain_set(!app->agc, code)) show_message("GAIN ERR");
}

static void set_target(int mhz) {
    if (mhz < 1840) mhz = 1840;
    if (mhz > 2790) mhz = 2790;
    if ((unsigned)mhz != app->target_mhz) {
        app->target_mhz = (unsigned)mhz;
        app->last_freq_change_ms = now_ms();
    }
}

static void retune_if_due(bool holding) {
    if (app->target_mhz == app->tuned_mhz) return;
    int64_t t = now_ms();
    if (t - app->last_freq_change_ms < TUNE_SETTLE_MS && !(holding && t - app->last_tune_ms >= TUNE_EVERY_MS)) return;
    int64_t t0 = esp_timer_get_time();
    if (!sdr_tune(app->target_mhz)) {
        show_message("OUT OF RANGE");
        app->target_mhz = app->tuned_mhz;
        return;
    }
    ESP_LOGI(TAG, "tuned %u MHz in %lld us", app->target_mhz, (long long)(esp_timer_get_time() - t0));
    app->tuned_mhz = app->target_mhz;
    app->last_tune_ms = now_ms();
    for (int x = 0; x < W; x++) app->peak_db[x] = -200.0f;
    build_markers();
    wf_clear();
}

static void exit_to_keira(void) {
    app->ntext = 0;
    text_add(40, 110, 2, c_text, "BACK TO KEIRA");
    lcd_draw_rows(0, LCD_HEIGHT, splash_row, NULL);
    /* Keira's multiboot runs this image from the other OTA slot. Point the
     * bootloader back at the slot we were not started from, then reset. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
        running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        const esp_partition_t *other = esp_ota_get_next_update_partition(running);
        if (other) {
            esp_err_t e = esp_ota_set_boot_partition(other);
            ESP_LOGI(TAG, "boot partition -> %s: %s", other->label, esp_err_to_name(e));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void apply_preset(int idx) {
    const preset_t *p = &presets[idx];
    app->preset_idx = idx;
    if (p->rate_idx != app->rate_idx) {
        app->rate_idx = p->rate_idx;
        for (int x = 0; x < W; x++) app->peak_db[x] = -200.0f;
        build_markers();
        wf_clear();
    }
    set_target((int)p->mhz);
    app->last_freq_change_ms = 0; /* tune now */
    show_message(p->name);
}

static void menu_move(int delta) {
    int s = app->menu_sel + delta;
    if (s < 0) s = delta < -1 ? 0 : PRESET_COUNT - 1; /* single steps wrap, pages stop */
    if (s >= PRESET_COUNT) s = delta > 1 ? PRESET_COUNT - 1 : 0;
    app->menu_sel = s;
    if (s < app->menu_top) app->menu_top = s;
    if (s >= app->menu_top + MENU_LINES) app->menu_top = s - MENU_LINES + 1;
}

/* The preset list owns the buttons while it is open: UP/DOWN move,
 * LEFT/RIGHT page, A or D tunes, B closes. */
static void handle_menu(const buttons_t *b) {
    if (b->repeat[BTN_UP]) menu_move(-1);
    if (b->repeat[BTN_DOWN]) menu_move(1);
    if (b->repeat[BTN_LEFT]) menu_move(-MENU_LINES);
    if (b->repeat[BTN_RIGHT]) menu_move(MENU_LINES);
    if (b->pressed[BTN_A] || b->pressed[BTN_D]) {
        app->menu_open = false;
        apply_preset(app->menu_sel);
    } else if (b->pressed[BTN_B]) {
        app->menu_open = false;
    }
}

/* Screenshot: the current frame as text on the USB console, one hex line per
 * row of byte-swapped RGB565 (the wire format, i.e. big-endian RGB565).
 * tools/lilka_screenshot.py turns it into a PNG. */
static void send_screenshot(void) {
    static uint16_t row[W];
    static char hex[W * 4 + 1];
    static const char digits[] = "0123456789abcdef";
    build_text();
    build_wf_lut();
    printf("\nLILKA-SHOT BEGIN %d %d\n", W, LCD_HEIGHT);
    for (int y = 0; y < LCD_HEIGHT; y++) {
        render_row(y, row, NULL);
        const uint8_t *p = (const uint8_t *)row;
        for (int i = 0; i < W * 2; i++) {
            hex[2 * i] = digits[p[i] >> 4];
            hex[2 * i + 1] = digits[p[i] & 15];
        }
        hex[W * 4] = 0;
        printf("LILKA-SHOT %d %s\n", y, hex);
    }
    printf("LILKA-SHOT END\n");
    fflush(stdout);
    show_message("SCREENSHOT");
}

static void handle_buttons(const buttons_t *b) {
    if (b->down[BTN_SELECT] && b->held_ms[BTN_SELECT] >= EXIT_HOLD_MS) exit_to_keira();
    /* SELECT tap (released before the exit hold): screenshot. */
    static bool select_was_down;
    static uint32_t select_held;
    if (b->down[BTN_SELECT]) {
        select_held = b->held_ms[BTN_SELECT];
    } else if (select_was_down && select_held < EXIT_HOLD_MS) {
        send_screenshot();
    }
    select_was_down = b->down[BTN_SELECT];
    if (app->menu_open) {
        handle_menu(b);
        return;
    }
    if (b->repeat[BTN_LEFT] || b->repeat[BTN_RIGHT]) {
        int dir = b->repeat[BTN_RIGHT] ? 1 : -1;
        unsigned reps = b->repeat[BTN_RIGHT] ? b->repeats[BTN_RIGHT] : b->repeats[BTN_LEFT];
        int step = reps > 12 ? 5 : 1;
        set_target((int)app->target_mhz + dir * step);
    }
    /* Reference level, as on a bench analyzer: UP raises the top of the scale. */
    if (b->repeat[BTN_UP]) app->ref_offset_db += 5;
    if (b->repeat[BTN_DOWN]) app->ref_offset_db -= 5;
    if (b->pressed[BTN_A]) {
        app->rate_idx = (app->rate_idx + 1) % RATE_COUNT;
        for (int x = 0; x < W; x++) app->peak_db[x] = -200.0f;
        build_markers();
        wf_clear();
    }
    if (b->pressed[BTN_B]) {
        if (app->agc) {
            app->agc = false;
            app->gain_idx = 0;
        } else if (app->gain_idx + 1 < GAIN_STEPS && gain_steps[app->gain_idx + 1] <= sdr_gain_max()) {
            app->gain_idx++;
        } else {
            app->agc = true;
        }
        apply_gain();
        wf_clear();
    }
    if (b->pressed[BTN_C]) {
        app->peak_hold = !app->peak_hold;
        for (int x = 0; x < W; x++) app->peak_db[x] = -200.0f;
    }
    if (b->pressed[BTN_D]) {
        app->menu_open = true;
        app->menu_sel = app->preset_idx;
        app->menu_top = 0;
        menu_move(0);
    }
    if (b->pressed[BTN_START]) app->paused = !app->paused;
}

void lilka_app_run(void) {
    if (!dsp_init()) {
        ESP_LOGE(TAG, "out of memory for DSP buffers");
        abort();
    }
    build_palette();
    app->rate_idx = 0;
    /* Fixed gain by default. The receiver's AGC is the Wi-Fi packet AGC: it
     * re-gains on almost every snapshot (index 49..79 seen at 2442 MHz), which
     * moves whole rows by up to ~20 dB and makes the waterfall streaky. */
    app->agc = false;
    app->gain_idx = DEFAULT_GAIN_IDX;
    app->floor_db = -1000.0f;
    app->tuned_mhz = app->target_mhz = presets[0].mhz;
    app->preset_idx = 0;
    sdr_bandwidth(0); /* widest analog filter: the full span is useful */
    sdr_tune(app->tuned_mhz);
    apply_gain();
    build_markers();
    for (int x = 0; x < W; x++) app->peak_db[x] = -200.0f;
    ESP_LOGI(TAG, "heap free %u, largest %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    while (now_ms() - splash_ms < 2500) vTaskDelay(pdMS_TO_TICKS(20));

    buttons_t b = {0};
    int64_t last = esp_timer_get_time();
    for (;;) {
        buttons_poll(&b);
        handle_buttons(&b);
        retune_if_due(b.down[BTN_LEFT] || b.down[BTN_RIGHT]);
        if (!app->paused) {
            const uint32_t *s = NULL;
            uint32_t us = 0;
            sdr_status_t st = sdr_capture(SAMPLES, rates[app->rate_idx].code, &s, &us);
            if (st == SDR_OK) {
                app->capture_us = us;
                spectrum_from_samples(s, SAMPLES);
                update_scale();
                update_traces();
            } else {
                app->capture_errors++;
                ESP_LOGW(TAG, "capture failed: %d", (int)st);
            }
        } else {
            update_scale();
            for (int x = 0; x < W; x++) {
                app->fill_h[x] = db_to_px(app->col_db[x]);
                app->peak_px[x] = app->peak_hold ? db_to_px(app->peak_db[x]) : -1;
            }
        }
        draw_frame();
        int64_t t = esp_timer_get_time();
        float inst = 1e6f / (float)(t - last);
        app->fps = app->fps ? app->fps + 0.1f * (inst - app->fps) : inst;
        last = t;
        static int64_t last_stats;
        if (t - last_stats >= 5000000) {
            last_stats = t;
            float top = -1000.0f;
            for (int x = 0; x < W; x++) top = app->col_db[x] > top ? app->col_db[x] : top;
            ESP_LOGI(TAG, "%u MHz span %u, gain %s %.0f, %.1f fps, capture %u us, floor %.1f dB, peak %.1f dB, errors %u",
                     app->tuned_mhz, rates[app->rate_idx].msps, app->agc ? "agc" : "fixed", app->snap_gain, app->fps,
                     (unsigned)app->capture_us, app->floor_db, top, (unsigned)app->capture_errors);
        }
        vTaskDelay(1);
    }
}
