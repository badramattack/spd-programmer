// DDR5 SPD: JEDEC SPD5118 hub (1024 bytes NVM in sixteen 64-byte blocks,
// volatile registers MR0-MR127). I2C legacy mode, 1-byte addressing with the
// MR11 page pointer.
#pragma once

#include "err.h"
#include "pico/stdlib.h"

#define SPD5118_SIZE 1024
#define SPD5118_BLOCKS 16
#define SPD5118_NREGS 128

err_t spd5118_reg_read(uint8_t reg, uint8_t *buf, size_t len);
err_t spd5118_reg_write(uint8_t reg, uint8_t val);
err_t spd5118_read(uint16_t addr, uint8_t *buf, size_t len);
err_t spd5118_write(uint16_t addr, const uint8_t *buf, size_t len);
// MR12/MR13: bit n set = block n write protected. `override` = MR48[2],
// i.e. the hub saw HSA tied to GND at power-up and allows clearing bits.
err_t spd5118_wp_status(uint16_t *mask, bool *override);
err_t spd5118_wp_clear(void);
err_t spd5118_wp_set(uint16_t mask);
err_t spd5118_temp(int32_t *milli_c);
