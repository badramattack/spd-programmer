#include "ui.h"

#include <stdio.h>

#include "board.h"
#include "edit.h"
#include "power.h"
#include "presets.h"

#define LONG_PRESS_MS 1000
#define DEBOUNCE_MS   30
// Fault code: the LED stays on and goes dark `code` times per round, so a
// fault is visible at any moment and the count is there when wanted.
#define BLIP_OFF_MS   200
#define BLIP_ON_MS    300
#define HOLD_ON_MS    2000  // steady on between rounds

typedef struct {
    uint pin;
    unsigned hold_ms;
    bool down, fired;
    absolute_time_t since;
} button_t;

static button_t program_btn = {.pin = PIN_BTN_PROGRAM, .hold_ms = LONG_PRESS_MS};
static button_t restore_btn = {.pin = PIN_BTN_RESTORE, .hold_ms = DEBOUNCE_MS};

static unsigned blink_code;  // 0: no blinking
static unsigned blink_step;
static absolute_time_t blink_next;

static const char *last_action = "none";
static err_t last_err = E_OK;
static char last_msg[160];

static unsigned code_of(err_t e) {
    switch (e) {
    case E_OK: return 0;
    case E_SWITCH:
    case E_SEAT: return 1;
    case E_VDDSPD:
    case E_FAULT:
    case E_NOACK:
    case E_TIMEOUT:
    case E_OVERRIDE: return 2;
    case E_IDENT: return 3;
    case E_WP:
    case E_VERIFY:
    case E_FLASH: return 5;
    default: return 4;  // E_REFUSED, E_ARGS (bad preset), E_STATE
    }
}

void ui_clear(void) {
    if (blink_code)
        led_set(PIN_LED_FAULT, false);
    blink_code = 0;
}

static void blink_start(unsigned code) {
    blink_code = code;
    blink_step = 0;
    led_set(PIN_LED_FAULT, true);
    blink_next = make_timeout_time_ms(HOLD_ON_MS);
}

// Phases 0 .. 2*code-1: 0 = steady on, odd = dark blip, even = on between blips.
static void blink_poll(void) {
    if (!blink_code || !time_reached(blink_next))
        return;
    blink_step = (blink_step + 1) % (2 * blink_code);
    bool on = blink_step % 2 == 0;
    led_set(PIN_LED_FAULT, on);
    blink_next = make_timeout_time_ms(blink_step == 0 ? HOLD_ON_MS : on ? BLIP_ON_MS : BLIP_OFF_MS);
}

static void run(bool program) {
    ui_clear();
    led_set(PIN_LED_DONE, false);
    last_action = program ? "program" : "restore";
    last_msg[0] = 0;
    err_t e = power_on(false);
    if (e == E_OK) {
        if (!program) {
            e = edit_restore(false, last_msg, sizeof last_msg);
        } else if (presets_active() < 0) {
            snprintf(last_msg, sizeof last_msg, "no active preset");
            e = E_REFUSED;
        } else {
            size_t len;
            const uint8_t *p = presets_get(presets_active(), &len);
            e = edit_program(p, len, last_msg, sizeof last_msg);
        }
    }
    power_off();
    last_err = e;
    if (e == E_OK)
        led_set(PIN_LED_DONE, true);
    else
        blink_start(code_of(e));
    printf("# button %s: %s %s\n", last_action, e == E_OK ? "OK" : err_name(e), last_msg);
}

// True once per press, when the button has been held for hold_ms.
static bool pressed(button_t *b) {
    bool now = !gpio_get(b->pin);
    if (!now) {
        b->down = false;
        return false;
    }
    if (!b->down) {
        b->down = true;
        b->fired = false;
        b->since = get_absolute_time();
    }
    if (b->fired || absolute_time_diff_us(b->since, get_absolute_time()) < b->hold_ms * 1000ll)
        return false;
    b->fired = true;
    return true;
}

void ui_poll(void) {
    blink_poll();
    if (power_state() != PWR_OFF) {
        // A session or debug mode owns the module: ignore presses, including
        // one still held when it ends.
        program_btn.down = restore_btn.down = true;
        program_btn.fired = restore_btn.fired = true;
        return;
    }
    if (pressed(&program_btn))
        run(true);
    else if (pressed(&restore_btn))
        run(false);
}

void ui_report(void) {
    printf("OK action=%s result=%s code=%u msg=%s\n", last_action, last_err == E_OK ? "OK" : err_name(last_err),
           code_of(last_err), last_msg);
}
