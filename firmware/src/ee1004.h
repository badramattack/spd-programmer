// DDR4 SPD: JEDEC EE1004-v / TSE2004av (512 bytes in two 256-byte pages,
// four 128-byte write-protect blocks).
#pragma once

#include "err.h"
#include "pico/stdlib.h"

#define EE1004_SIZE 512
#define EE1004_BLOCKS 4

err_t ee1004_read(uint16_t addr, uint8_t *buf, size_t len);
err_t ee1004_write(uint16_t addr, const uint8_t *buf, size_t len);
// Bit n set = block n write protected (RPSn).
err_t ee1004_wp_status(uint16_t *mask);
// CWP: clears all blocks. Needs VHV on SA0.
err_t ee1004_wp_clear(void);
// SWPn for every block in `mask`. Needs VHV on SA0.
err_t ee1004_wp_set(uint16_t mask);
// TSE2004av temperature sensor, if fitted.
err_t ee1004_temp(int32_t *milli_c);
