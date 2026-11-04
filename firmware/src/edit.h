// Program / Restore: apply a preset to the module and undo it again, with a
// backup record on the module and in the Pico flash.
//
// Marked serial. Program gives the module the serial 5D h0 h1 h2, where h is
// the low 24 bits (LSB first) of the CRC-32 (zlib) of the whole SPD with the
// serial, the CRC bytes and the record area set to zero. The serial thus
// shows that the module was modified, and Restore can tell whether the
// contents changed since.
//
// Record (little endian), at the start of the record area when that area is
// all 0x00 or all 0xFF and the record fits, and always in the Pico flash:
//   0   'S' 'R'
//   2   version (1)
//   3   flags: bit 0 = record area was 0xFF (else 0x00)
//   4   n, the number of entries
//   5   preset ID (CRC-32 of the preset)
//   9   original serial (4)
//   13  new serial (4)
//   17  original CRC bytes (DDR4: base, module; DDR5: base, 0)
//   21  n entries: offset (2), original byte
//   21+3n  CRC-16 (spd_crc16) of bytes 0 .. 20+3n
//
// Preset (from the host, see host/spdtool/preset.py):
//   0   'S' 'P', version (1)
//   3   name length, name, description length, description
//   ..  DDR4 op count, ops; DDR5 op count, ops
// Ops:
//   1 SET   offset (2), mask, value          byte = byte & ~mask | value & mask
//   2 MAP   offset (2), mask, n, n x (from, to)  of byte & mask; else refused
//   3 SPEED kind, value (2)                  max speed to a standard grade: kind 0 =
//                                            value MT/s, 1 = lowest, 2 = value grades up
//                                            (signed); timings stay in ns, the CL needed
//                                            at the new clock is added to the CL list
//   4 IF    offset (2), mask, value, n       the next n ops only if byte & mask == value
//   5 TIME  encoding, offset, offset2, offset3 (2 each), shift, action, a (4), b (4)
//                                            a timing field (timing.h): action 0 = a ps,
//                                            1 = scale by a/b (rounded up), 2 = add a ps,
//                                            3 = a clocks at the current tCKmin
#pragma once

#include "err.h"
#include "pico/stdlib.h"

// All three need an open session (PWR_ON) and fill `msg` with the details
// of a refusal.
err_t edit_program(const uint8_t *preset, size_t len, char *msg, size_t msg_len);
err_t edit_restore(bool force, char *msg, size_t msg_len);
// One "OK ..." status line about the marked serial and the backup record.
err_t edit_report(void);
// Checks a preset's format (no module needed) and returns its name.
err_t edit_preset_check(const uint8_t *p, size_t len, char *name, size_t name_size, char *msg, size_t msg_len);
