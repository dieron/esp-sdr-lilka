#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_A,
    BTN_B,
    BTN_C,
    BTN_D,
    BTN_START,
    BTN_SELECT,
    BTN_COUNT
} button_t;

typedef struct {
    bool down[BTN_COUNT];    /* currently held */
    bool pressed[BTN_COUNT]; /* went down since the previous poll */
    bool repeat[BTN_COUNT];  /* pressed, or an auto-repeat tick while held */
    uint32_t held_ms[BTN_COUNT];
    unsigned repeats[BTN_COUNT]; /* auto-repeat ticks in the current hold */
} buttons_t;

void buttons_init(void);
void buttons_poll(buttons_t *b);
