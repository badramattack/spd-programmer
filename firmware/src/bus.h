// I2C access to the module. Contact 4 is DDR4 SDA and DDR5 HSCL, so it is
// wired to both GP22 (I2C1 SDA) and GP21 (I2C0 SCL); only the peripheral of
// the powered generation is attached at a time, the other pin stays high-Z.
#pragma once

#include "err.h"
#include "pico/stdlib.h"

typedef enum { BUS_NONE, BUS_DDR4, BUS_DDR5 } bus_t;

void bus_attach(bus_t which);
void bus_detach(void);
bus_t bus_current(void);

// Plain I2C transfers. `nostop` keeps the bus for a repeated start.
// Return E_OK, E_NOACK or E_TIMEOUT.
err_t bus_write(uint8_t addr, const uint8_t *buf, size_t len, bool nostop);
err_t bus_read(uint8_t addr, uint8_t *buf, size_t len);
// Write `wlen` bytes, repeated start, read `rlen` bytes.
err_t bus_write_read(uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen);
// True if `addr` acknowledges a one-byte read.
bool bus_probe(uint8_t addr);
