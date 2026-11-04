#include "power.h"

#include "board.h"
#include "bus.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include "spd.h"

static pwr_state_t state = PWR_OFF;
static dimm_mode_t powered_mode = MODE_INVALID;
static bool hv_on = false;
static absolute_time_t off_since;
static absolute_time_t next_debug_seat_check;

dimm_mode_t mode_read(void) {
    bool ddr4 = !gpio_get(PIN_SEL_DDR4), ddr5 = !gpio_get(PIN_SEL_DDR5);
    bool udimm = !gpio_get(PIN_SEL_UDIMM), rdimm = !gpio_get(PIN_SEL_RDIMM);
    if (ddr4 == ddr5 || udimm == rdimm)
        return MODE_INVALID;  // between positions (or broken switch)
    if (ddr4)
        return udimm ? MODE_DDR4U : MODE_DDR4R;
    return udimm ? MODE_DDR5U : MODE_DDR5R;
}

const char *mode_name(dimm_mode_t m) {
    switch (m) {
    case MODE_DDR4U: return "ddr4-udimm";
    case MODE_DDR4R: return "ddr4-rdimm";
    case MODE_DDR5U: return "ddr5-udimm";
    case MODE_DDR5R: return "ddr5-rdimm";
    default: return "invalid";
    }
}

uint32_t seat_measure_mv(void) {
    gpio_pull_up(PIN_SEAT_SENSE);
    sleep_us(SEAT_PULSE_US);
    uint32_t mv = adc_read_mv(ADC_CH_SEAT);
    gpio_disable_pulls(PIN_SEAT_SENSE);
    return mv;
}

static void sleep_ms_wdt(uint32_t ms) {
    while (ms > 0) {
        uint32_t step = ms > 100 ? 100 : ms;
        sleep_ms(step);
        watchdog_update();
        ms -= step;
    }
}

static void wait_discharged(void) {
    int64_t left_us = absolute_time_diff_us(get_absolute_time(), delayed_by_ms(off_since, T_DISCHARGE_MS));
    if (left_us > 0)
        sleep_ms_wdt((uint32_t)((left_us + 999) / 1000));
}

// Rail sequence. DDR4: contact 148 released (it becomes SCL), VDDSPD on and
// checked, then the I2C pull-ups. DDR5: HSA grounded before the supply, so
// the hub starts in offline mode (write protection can be cleared), then
// VIN_BULK (UDIMM) or VIN_MGMT (RDIMM), then the pull-ups. Leaves everything
// off on error.
static err_t rails_on(dimm_mode_t m) {
    wait_discharged();
    led_set(PIN_LED_BUSY, true);
    powered_mode = m;
    state = PWR_ON;  // so that power_off() undoes a partial sequence

    if (mode_is_ddr4(m)) {
        gpio_put(PIN_HSA_GND, false);  // contact 148 becomes SCL
        gpio_put(PIN_EN_VDDSPD, true);
        sleep_ms(T_VDDSPD_SETTLE_MS);
        if (adc_read_mv(ADC_CH_VDDSPD) < VDDSPD_MIN_MV)
            return E_VDDSPD;
        gpio_put(PIN_EN_VIO, true);
        bus_attach(BUS_DDR4);
    } else {
        gpio_put(PIN_HSA_GND, true);  // HSA grounded before VIN: offline mode
        gpio_put(m == MODE_DDR5U ? PIN_EN_VIN_BULK : PIN_EN_VIN_MGMT, true);
        sleep_ms(T_DDR5_POR_MS);
        if (!gpio_get(PIN_FAULT_N))
            return E_FAULT;
        gpio_put(PIN_EN_VIO, true);
        bus_attach(BUS_DDR5);
    }
    return E_OK;
}

err_t power_on(bool force) {
    if (state != PWR_OFF)
        return E_STATE;
    dimm_mode_t m = mode_read();
    if (m == MODE_INVALID)
        return E_SWITCH;
    if (seat_measure_mv() > SEAT_SEATED_MAX_MV)
        return E_SEAT;

    spd_forget_ident();
    err_t e = rails_on(m);
    if (e == E_OK && !force)
        e = spd_identify(m);
    if (e != E_OK)
        power_off();
    return e;
}

err_t power_debug(bool hv) {
    if (state != PWR_OFF)
        return E_STATE;
    dimm_mode_t m = mode_read();
    if (m == MODE_INVALID)
        return E_SWITCH;
    if (hv && !mode_is_ddr4(m))
        return E_STATE;
    if (seat_measure_mv() < SEAT_EMPTY_MIN_MV)
        return E_SEAT;  // debug mode is for measuring an empty socket only

    err_t e = rails_on(m);
    if (e == E_OK) {
        state = PWR_DEBUG;
        next_debug_seat_check = make_timeout_time_ms(250);
        if (hv) {
            gpio_put(PIN_HV_SHDN, false);
            hv_on = true;
        }
    } else {
        power_off();
    }
    return e;
}

void power_off(void) {
    if (state == PWR_OFF)
        return;
    gpio_put(PIN_HV_SHDN, true);
    hv_on = false;
    gpio_put(PIN_EN_VIO, false);
    bus_detach();
    gpio_put(PIN_EN_VIN_BULK, false);
    gpio_put(PIN_EN_VIN_MGMT, false);
    gpio_put(PIN_EN_VDDSPD, false);
    gpio_put(PIN_HSA_GND, true);  // also bleeds contact 5 and VIO
    state = PWR_OFF;
    powered_mode = MODE_INVALID;
    off_since = get_absolute_time();
    wait_discharged();
    led_set(PIN_LED_BUSY, false);
}

pwr_state_t power_state(void) {
    return state;
}

dimm_mode_t power_mode(void) {
    return powered_mode;
}

bool power_hv(void) {
    return hv_on;
}

err_t hv_enable(void) {
    if (state != PWR_ON || !mode_is_ddr4(powered_mode))
        return E_STATE;
    gpio_put(PIN_HV_SHDN, false);
    hv_on = true;
    sleep_ms(T_HV_SETTLE_MS);
    return E_OK;
}

void hv_disable(void) {
    if (!hv_on)
        return;
    gpio_put(PIN_HV_SHDN, true);
    hv_on = false;
    sleep_ms(T_HV_OFF_MS);
}

const char *power_poll(void) {
    if (state == PWR_OFF)
        return NULL;
    const char *reason = NULL;
    if (!gpio_get(PIN_FAULT_N))
        reason = "over-current fault";
    else if (mode_read() != powered_mode)
        reason = "mode switch changed";
    else if (state == PWR_DEBUG && time_reached(next_debug_seat_check)) {
        next_debug_seat_check = make_timeout_time_ms(250);
        if (seat_measure_mv() < SEAT_EMPTY_MIN_MV)
            reason = "module inserted during debug mode";
    }
    if (reason) {
        power_off();
        led_set(PIN_LED_FAULT, true);
    }
    return reason;
}

void power_init_time(void) {
    // The rails may have been on just before a reset (watchdog, picotool
    // reboot): treat boot as a power-down.
    off_since = get_absolute_time();
}
