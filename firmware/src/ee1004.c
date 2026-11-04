#include "ee1004.h"
#include <string.h>

#include "board.h"
#include "bus.h"
#include "hardware/watchdog.h"
#include "power.h"

// Device select codes (EE1004-v Table 2), as 7-bit addresses. SA2..SA0 = 0.
#define ADDR_EE   0x50  // 1010 000
#define ADDR_TS   0x18  // 0011 000
#define ADDR_CWP  0x33  // 0110 011
#define ADDR_SPA0 0x36  // 0110 110, also RPA when read
#define ADDR_SPA1 0x37  // 0110 111
// SWPn (write) and RPSn (read) share an address per block. The block bits
// are deliberately not binary (EE1004-v Table 2 note 4).
static const uint8_t addr_wp[EE1004_BLOCKS] = {0x31, 0x34, 0x35, 0x30};

#define PAGE_WRITE 16
#define T_WRITE_MAX_MS 20  // tW is 5 ms max; generous margin

static err_t set_page(unsigned page) {
    // SPAn takes a don't-care data byte. Some devices switch pages but do not
    // ACK, so check the result with RPA (ACK = page 0, NACK = page 1), as the
    // Linux ee1004 driver does.
    const uint8_t dummy = 0;
    bus_write(page ? ADDR_SPA1 : ADDR_SPA0, &dummy, 1, false);
    bool is_page0 = bus_probe(ADDR_SPA0);
    return is_page0 == (page == 0) ? E_OK : E_NOACK;
}

err_t ee1004_read(uint16_t addr, uint8_t *buf, size_t len) {
    if (addr + len > EE1004_SIZE)
        return E_ARGS;
    while (len > 0) {
        unsigned page = addr >> 8;
        uint8_t offset = addr & 0xFF;
        size_t n = 256 - offset;
        if (n > len)
            n = len;
        err_t e = set_page(page);
        if (e == E_OK)
            e = bus_write_read(ADDR_EE, &offset, 1, buf, n);
        if (e != E_OK)
            return e;
        addr += n;
        buf += n;
        len -= n;
        watchdog_update();
    }
    return E_OK;
}

// ACK polling: the device NACKs its address during the internal write cycle.
static err_t wait_write_done(void) {
    absolute_time_t deadline = make_timeout_time_ms(T_WRITE_MAX_MS);
    while (!time_reached(deadline)) {
        if (bus_probe(ADDR_EE))  // current-address read: no side effects
            return E_OK;
        sleep_us(500);
    }
    return E_TIMEOUT;
}

static bool block_protected(unsigned block) {
    uint16_t mask;
    return ee1004_wp_status(&mask) == E_OK && (mask >> block) & 1;
}

err_t ee1004_write(uint16_t addr, const uint8_t *buf, size_t len) {
    if (addr + len > EE1004_SIZE)
        return E_ARGS;
    while (len > 0) {
        unsigned page = addr >> 8;
        uint8_t offset = addr & 0xFF;
        size_t n = PAGE_WRITE - (offset % PAGE_WRITE);
        if (n > len)
            n = len;
        err_t e = set_page(page);
        if (e != E_OK)
            return e;
        uint8_t pkt[1 + PAGE_WRITE];
        pkt[0] = offset;
        memcpy(pkt + 1, buf, n);
        e = bus_write(ADDR_EE, pkt, 1 + n, false);
        if (e == E_NOACK && block_protected(addr / 128))
            return E_WP;  // protected blocks NACK the data byte
        if (e != E_OK)
            return e;
        e = wait_write_done();
        if (e != E_OK)
            return e;
        addr += n;
        buf += n;
        len -= n;
        watchdog_update();
    }
    return E_OK;
}

err_t ee1004_wp_status(uint16_t *mask) {
    // RPSn: the device ACKs the select code if the block is NOT protected.
    *mask = 0;
    for (unsigned b = 0; b < EE1004_BLOCKS; b++)
        if (!bus_probe(addr_wp[b]))
            *mask |= 1u << b;
    // If every block "is protected", the device may simply be absent.
    if (*mask == 0xF && !bus_probe(ADDR_SPA0) && !bus_probe(ADDR_EE))
        return E_NOACK;
    return E_OK;
}

static err_t wp_command(uint8_t addr) {
    const uint8_t dummy[2] = {0, 0};
    err_t e = bus_write(addr, dummy, 2, false);
    sleep_ms(T_WRITE_MAX_MS);  // the WP bits are non-volatile: one tW
    return e;
}

err_t ee1004_wp_clear(void) {
    err_t e = hv_enable();
    if (e != E_OK)
        return e;
    e = wp_command(ADDR_CWP);
    hv_disable();
    if (e != E_OK)
        return e;  // a NACK here usually means VHV did not reach SA0
    uint16_t mask;
    e = ee1004_wp_status(&mask);
    if (e == E_OK && mask)
        e = E_WP;
    return e;
}

err_t ee1004_wp_set(uint16_t mask) {
    if (mask & ~0xFu)
        return E_ARGS;
    err_t e = hv_enable();
    if (e != E_OK)
        return e;
    for (unsigned b = 0; b < EE1004_BLOCKS; b++)
        if ((mask >> b) & 1)
            wp_command(addr_wp[b]);  // NACK if the block was already protected
    hv_disable();
    uint16_t now;
    e = ee1004_wp_status(&now);
    if (e == E_OK && (now & mask) != mask)
        e = E_WP;
    return e;
}

err_t ee1004_temp(int32_t *milli_c) {
    const uint8_t reg = 0x05;  // ambient temperature, MSB first
    uint8_t raw[2];
    err_t e = bus_write_read(ADDR_TS, &reg, 1, raw, 2);
    if (e != E_OK)
        return e;
    int32_t t = ((raw[0] << 8) | raw[1]) & 0x1FFF;  // 13-bit two's complement, 1/16 C
    if (t & 0x1000)
        t -= 0x2000;
    *milli_c = t * 1000 / 16;
    return E_OK;
}
