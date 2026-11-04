#include "bus.h"

#include "board.h"
#include "hardware/i2c.h"

static bus_t current = BUS_NONE;

static i2c_inst_t *inst(void) {
    return current == BUS_DDR4 ? i2c1 : i2c0;
}

static void pin_highz(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
}

void bus_detach(void) {
    pin_highz(PIN_BUS_A_I2C0);
    pin_highz(PIN_BUS_A_I2C1);
    pin_highz(PIN_BUS_B);
    pin_highz(PIN_BUS_C);
    if (current == BUS_DDR4)
        i2c_deinit(i2c1);
    else if (current == BUS_DDR5)
        i2c_deinit(i2c0);
    current = BUS_NONE;
}

void bus_attach(bus_t which) {
    bus_detach();
    if (which == BUS_NONE)
        return;
    current = which;
    // External 4.7 k pull-ups to VIO (2.5 V); the internal ones go to 3.3 V.
    if (which == BUS_DDR4) {
        i2c_init(i2c1, I2C_BAUD);
        gpio_set_function(PIN_BUS_A_I2C1, GPIO_FUNC_I2C);  // SDA
        gpio_set_function(PIN_BUS_C, GPIO_FUNC_I2C);       // SCL
    } else {
        i2c_init(i2c0, I2C_BAUD);
        gpio_set_function(PIN_BUS_B, GPIO_FUNC_I2C);       // SDA
        gpio_set_function(PIN_BUS_A_I2C0, GPIO_FUNC_I2C);  // SCL
    }
}

bus_t bus_current(void) {
    return current;
}

// 10 bit times per byte at I2C_BAUD, times 4, plus margin for clock stretching.
static uint32_t timeout_us(size_t len) {
    return (uint32_t)((len + 2) * 40ull * 1000000 / I2C_BAUD) + 2000;
}

static err_t map(int ret, size_t len) {
    if (ret == (int)len)
        return E_OK;
    if (ret == PICO_ERROR_TIMEOUT) {
        // The SDK does not abort the transfer on a timeout; start over.
        bus_attach(current);
        return E_TIMEOUT;
    }
    return E_NOACK;
}

// Long operations (a full 1 KB write) must not run on into a rail fault.
static bool rail_fault(void) {
    return !gpio_get(PIN_FAULT_N);
}

err_t bus_write(uint8_t addr, const uint8_t *buf, size_t len, bool nostop) {
    if (current == BUS_NONE)
        return E_STATE;
    if (rail_fault())
        return E_FAULT;
    return map(i2c_write_timeout_us(inst(), addr, buf, len, nostop, timeout_us(len)), len);
}

err_t bus_read(uint8_t addr, uint8_t *buf, size_t len) {
    if (current == BUS_NONE)
        return E_STATE;
    if (rail_fault())
        return E_FAULT;
    return map(i2c_read_timeout_us(inst(), addr, buf, len, false, timeout_us(len)), len);
}

err_t bus_write_read(uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen) {
    err_t e = bus_write(addr, w, wlen, true);
    if (e != E_OK)
        return e;
    return bus_read(addr, r, rlen);
}

bool bus_probe(uint8_t addr) {
    uint8_t dummy;
    return bus_read(addr, &dummy, 1) == E_OK;
}
