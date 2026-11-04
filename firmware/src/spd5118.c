#include "spd5118.h"

#include <string.h>

#include "bus.h"
#include "hardware/watchdog.h"

#define ADDR_HUB 0x50  // HID = 000 with HSA tied to GND

#define MR11 0x0B  // I2C legacy mode: [3] 2-byte addressing, [2:0] NVM page
#define MR12 0x0C  // write protection blocks 7..0
#define MR13 0x0D  // write protection blocks 15..8
#define MR5 0x05  // device capability: [1] temperature sensor supported
#define MR14 0x0E  // host/local interface configuration (NVM-backed)
#define MR26 0x1A  // TS configuration: [0] TS disabled
#define MR48 0x30  // device status: [3] write in progress, [2] WP override
#define MR49 0x31  // temperature, low byte

#define MEMREG 0x80
#define NVM_PAGE 128
#define NVM_WRITE 16  // writes stop at a 16-byte boundary (JESD300-5B 2.8.1)
#define T_WRITE_MAX_MS 50

static err_t wait_write_done(void);

// The hub may NACK the repeated start of a read while it is busy
// (JESD300-5B Tables 8/9 note 2); the host may simply retry.
static err_t write_read_retry(const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen) {
    err_t e = E_NOACK;
    for (int attempt = 0; attempt < 3 && e == E_NOACK; attempt++) {
        if (attempt)
            sleep_ms(2);
        e = bus_write_read(ADDR_HUB, w, wlen, r, rlen);
    }
    return e;
}

// With an NVM page other than 0 selected in MR11, the hub on the first test
// module (Renesas/IDT, SK hynix HMCG78MEBRA115N) reads every register except
// MR11 as 0: the write-protect mask, and the write-in-progress bit too.
// Select page 0 before any other register access.
static err_t page0(uint8_t reg) {
    if (reg == MR11)
        return E_OK;
    const uint8_t pkt[2] = {MR11, 0};
    err_t e = E_NOACK;
    for (int attempt = 0; attempt < 3 && e == E_NOACK; attempt++) {
        if (attempt)
            sleep_ms(2);
        e = bus_write(ADDR_HUB, pkt, 2, false);
    }
    return e;
}

err_t spd5118_reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    if (reg + len > SPD5118_NREGS)
        return E_ARGS;
    err_t e = page0(reg);
    return e == E_OK ? write_read_retry(&reg, 1, buf, len) : e;
}

err_t spd5118_reg_write(uint8_t reg, uint8_t val) {
    if (reg >= SPD5118_NREGS)
        return E_ARGS;
    const uint8_t pkt[2] = {reg, val};
    err_t e = page0(reg);
    if (e == E_OK)
        e = bus_write(ADDR_HUB, pkt, 2, false);
    // MR12-MR14 are NVM-backed (attribute RWE): a write starts a write cycle.
    if (e == E_OK && reg >= MR12 && reg <= MR14)
        e = wait_write_done();
    return e;
}

// Poll MR48[3] until the internal NVM write cycle has finished. Register
// access is allowed while the hub is busy.
static err_t wait_write_done(void) {
    absolute_time_t deadline = make_timeout_time_ms(T_WRITE_MAX_MS);
    while (!time_reached(deadline)) {
        uint8_t st;
        err_t e = spd5118_reg_read(MR48, &st, 1);
        if (e == E_OK && !(st & 0x08))
            return E_OK;
        sleep_us(500);
    }
    return E_TIMEOUT;
}

static err_t set_page(unsigned page) {
    return spd5118_reg_write(MR11, page & 7);  // also keeps 1-byte addressing
}

err_t spd5118_read(uint16_t addr, uint8_t *buf, size_t len) {
    if (addr + len > SPD5118_SIZE)
        return E_ARGS;
    while (len > 0) {
        size_t n = NVM_PAGE - (addr % NVM_PAGE);
        if (n > len)
            n = len;
        err_t e = set_page(addr / NVM_PAGE);
        uint8_t cmd = MEMREG | (addr % NVM_PAGE);
        if (e == E_OK)
            e = write_read_retry(&cmd, 1, buf, n);
        if (e != E_OK)
            return e;
        addr += n;
        buf += n;
        len -= n;
        watchdog_update();
    }
    return E_OK;
}

err_t spd5118_write(uint16_t addr, const uint8_t *buf, size_t len) {
    if (addr + len > SPD5118_SIZE)
        return E_ARGS;
    while (len > 0) {
        size_t n = NVM_WRITE - (addr % NVM_WRITE);
        if (n > len)
            n = len;
        err_t e = set_page(addr / NVM_PAGE);
        if (e != E_OK)
            return e;
        uint8_t pkt[1 + NVM_WRITE];
        pkt[0] = MEMREG | (addr % NVM_PAGE);
        memcpy(pkt + 1, buf, n);
        e = bus_write(ADDR_HUB, pkt, 1 + n, false);
        if (e == E_OK)
            e = wait_write_done();
        if (e != E_OK)
            return e;
        // Writes to a protected block are silently ignored (MR52[6] is set);
        // the caller detects this by verifying.
        addr += n;
        buf += n;
        len -= n;
        watchdog_update();
    }
    return E_OK;
}

err_t spd5118_wp_status(uint16_t *mask, bool *override) {
    uint8_t r[2], st;
    err_t e = spd5118_reg_read(MR12, r, 2);
    if (e == E_OK)
        e = spd5118_reg_read(MR48, &st, 1);
    if (e != E_OK)
        return e;
    *mask = r[0] | (r[1] << 8);
    *override = st & 0x04;
    return E_OK;
}

static err_t write_wp(uint16_t mask) {
    err_t e = spd5118_reg_write(MR12, mask & 0xFF);
    if (e == E_OK)
        e = spd5118_reg_write(MR13, mask >> 8);
    if (e != E_OK)
        return e;
    uint16_t now;
    bool ovr;
    e = spd5118_wp_status(&now, &ovr);
    if (e == E_OK && now != mask)
        e = E_WP;
    return e;
}

err_t spd5118_wp_clear(void) {
    uint16_t mask;
    bool ovr;
    err_t e = spd5118_wp_status(&mask, &ovr);
    if (e != E_OK)
        return e;
    if (mask == 0)
        return E_OK;
    if (!ovr)
        return E_OVERRIDE;
    return write_wp(0);
}

err_t spd5118_wp_set(uint16_t mask) {
    uint16_t cur;
    bool ovr;
    err_t e = spd5118_wp_status(&cur, &ovr);
    if (e != E_OK)
        return e;
    if ((cur | mask) == cur)
        return E_OK;
    return write_wp(cur | mask);  // setting bits is allowed without override
}

err_t spd5118_temp(int32_t *milli_c) {
    uint8_t cap, cfg, raw[2];
    err_t e = spd5118_reg_read(MR5, &cap, 1);
    if (e == E_OK)
        e = spd5118_reg_read(MR26, &cfg, 1);
    if (e != E_OK)
        return e;
    if (!(cap & 0x02) || (cfg & 0x01))
        return E_STATE;  // no sensor, or sensor disabled
    e = spd5118_reg_read(MR49, raw, 2);  // low byte first
    if (e != E_OK)
        return e;
    int32_t t = ((raw[1] << 8) | raw[0]) & 0x1FFF;  // 0.25 C in bits [12:2]
    if (t & 0x1000)
        t -= 0x2000;
    *milli_c = t * 1000 / 16;
    return E_OK;
}
