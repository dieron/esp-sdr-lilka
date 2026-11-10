#include "lilka_buttons.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "lilka_board.h"

#define REPEAT_DELAY_MS  350
#define REPEAT_PERIOD_MS 70
#define DEBOUNCE_MS      15

static const int pins[BTN_COUNT] = {
    [BTN_UP] = LILKA_GPIO_UP,       [BTN_DOWN] = LILKA_GPIO_DOWN,   [BTN_LEFT] = LILKA_GPIO_LEFT,
    [BTN_RIGHT] = LILKA_GPIO_RIGHT, [BTN_A] = LILKA_GPIO_A,         [BTN_B] = LILKA_GPIO_B,
    [BTN_C] = LILKA_GPIO_C,         [BTN_D] = LILKA_GPIO_D,         [BTN_START] = LILKA_GPIO_START,
    [BTN_SELECT] = LILKA_GPIO_SELECT,
};

static int64_t since[BTN_COUNT];  /* when the debounced state last changed */
static int64_t last_tick[BTN_COUNT];
static bool state[BTN_COUNT];
/* A button that already reads pressed at start-up (stuck, or held through the
 * launch from Keira) is ignored until it has been released once. */
static bool locked[BTN_COUNT];

void buttons_init(void) {
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) mask |= 1ull << pins[i];
    gpio_config_t in = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    esp_rom_delay_us(50); /* let the pull-ups charge the lines */
    for (int i = 0; i < BTN_COUNT; i++) locked[i] = gpio_get_level(pins[i]) == 0;
}

void buttons_poll(buttons_t *b) {
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < BTN_COUNT; i++) {
        bool raw = gpio_get_level(pins[i]) == 0;
        if (locked[i]) {
            if (raw) raw = false;
            else locked[i] = false;
        }
        b->pressed[i] = false;
        b->repeat[i] = false;
        if (raw != state[i] && now - since[i] >= DEBOUNCE_MS) {
            state[i] = raw;
            since[i] = now;
            if (raw) {
                b->pressed[i] = true;
                b->repeat[i] = true;
                b->repeats[i] = 0;
                last_tick[i] = now;
            }
        }
        b->down[i] = state[i];
        b->held_ms[i] = state[i] ? (uint32_t)(now - since[i]) : 0;
        if (state[i] && !b->pressed[i] && b->held_ms[i] >= REPEAT_DELAY_MS && now - last_tick[i] >= REPEAT_PERIOD_MS) {
            b->repeat[i] = true;
            b->repeats[i]++;
            last_tick[i] = now;
        }
    }
}
