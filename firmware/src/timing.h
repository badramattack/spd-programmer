// SPD timing fields in picoseconds, for the TIME preset op (edit.h). Pure C
// with no Pico dependencies, so host/tests can compile it natively and check
// it against spdtool's own encoders (host/spdtool/fields.py).
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    TIMING_PS16 = 1,      // DDR5: 16-bit LE picoseconds at off
    TIMING_NS16 = 2,      // DDR5: 16-bit LE nanoseconds at off (tRFC*)
    TIMING_MTB_FINE = 3,  // DDR4: 125 ps units at off (1..255), signed fine ps at off2
    TIMING_MTB12 = 4,     // DDR4: 12-bit units, high nibble of off at `shift`, low byte
                          // at off2, optional signed fine ps at off3 (0xFFFF: none)
    TIMING_MTB16 = 5,     // DDR4: 16-bit LE 125 ps units at off
};

#define TIMING_NO_FINE 0xFFFF

typedef struct {
    uint8_t enc, shift;
    uint16_t off, off2, off3;
} timing_t;

// False for an unknown encoding.
bool timing_get(const uint8_t *img, const timing_t *t, int32_t *ps);
// Encodes like spdtool: medium units rounded up with a negative fine part;
// without a fine byte, rounded up. False (img unchanged) if out of range.
bool timing_set(uint8_t *img, const timing_t *t, int32_t ps);
