#include "board.h"

#include "hardware/adc.h"
#include "hardware/pwm.h"

static void out_init(uint pin, bool value) {
    gpio_init(pin);
    gpio_put(pin, value);
    gpio_set_dir(pin, GPIO_OUT);
}

static void in_pullup_init(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

void board_init(void) {
    // Safe state first, the same as the board's pull-up/pull-down defaults:
    // all rails off, contact 148 (DDR5 HSA) grounded, VHV off.
    out_init(PIN_EN_VIN_BULK, false);
    out_init(PIN_EN_VIN_MGMT, false);
    out_init(PIN_EN_VDDSPD, false);
    out_init(PIN_EN_VIO, false);
    out_init(PIN_HSA_GND, true);
    out_init(PIN_HV_SHDN, true);

    gpio_init(PIN_FAULT_N);
    gpio_set_dir(PIN_FAULT_N, GPIO_IN);
    gpio_disable_pulls(PIN_FAULT_N);  // external 10 k pull-up

    in_pullup_init(PIN_SEL_DDR4);
    in_pullup_init(PIN_SEL_DDR5);
    in_pullup_init(PIN_SEL_UDIMM);
    in_pullup_init(PIN_SEL_RDIMM);
    in_pullup_init(PIN_BTN_PROGRAM);
    in_pullup_init(PIN_BTN_RESTORE);

    // LEDs run from PWM (GP12-15 = slices 6 and 7) so their brightness can be
    // matched; 125 MHz / LED_LEVEL_MAX = 125 kHz, no visible flicker.
    for (uint pin = PIN_LED_DONE; pin <= PIN_LED_FAULT; pin++) {
        gpio_set_function(pin, GPIO_FUNC_PWM);
        pwm_set_wrap(pwm_gpio_to_slice_num(pin), LED_LEVEL_MAX - 1);
        pwm_set_gpio_level(pin, 0);
        pwm_set_enabled(pwm_gpio_to_slice_num(pin), true);
    }

    // I2C pins stay high-Z until a bus is attached (bus.c).
    const uint bus_pins[] = {PIN_BUS_A_I2C0, PIN_BUS_A_I2C1, PIN_BUS_B, PIN_BUS_C};
    for (unsigned i = 0; i < count_of(bus_pins); i++) {
        gpio_init(bus_pins[i]);
        gpio_set_dir(bus_pins[i], GPIO_IN);
        gpio_disable_pulls(bus_pins[i]);
    }

    adc_init();
    adc_gpio_init(PIN_SEAT_SENSE);
    adc_gpio_init(PIN_VDDSPD_MON);
}

static uint16_t led_levels[] = {LED_DONE_LEVEL, LED_MOD_LEVEL, LED_BUSY_LEVEL, LED_FAULT_LEVEL};
static bool led_on[count_of(led_levels)];

void led_set(uint pin, bool on) {
    led_on[pin - PIN_LED_DONE] = on;
    pwm_set_gpio_level(pin, on ? led_levels[pin - PIN_LED_DONE] : 0);
}

bool led_get(uint pin) {
    return led_on[pin - PIN_LED_DONE];
}

void led_set_level(uint pin, uint level) {
    led_levels[pin - PIN_LED_DONE] = level > LED_LEVEL_MAX ? LED_LEVEL_MAX : level;
    led_set(pin, led_get(pin));
}

uint led_level(uint pin) {
    return led_levels[pin - PIN_LED_DONE];
}

uint32_t adc_read_mv(uint channel) {
    adc_select_input(channel);
    adc_read();  // discard the first conversion after switching channels
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum += adc_read();
    return (sum / 8) * 3300 / 4096;
}
