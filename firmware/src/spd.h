// Generation-independent SPD access, dispatching to ee1004 / spd5118 on the
// mode the module was powered in.
#pragma once

#include "err.h"
#include "power.h"

typedef struct {
    const char *name;
    uint16_t start, end;  // covered bytes, inclusive
    uint16_t at;          // CRC location, LSB first
} crc_section_t;

err_t spd_identify(dimm_mode_t m);
// Key bytes 0-3 read by the last identity check (or zeros).
const uint8_t *spd_ident_bytes(void);
void spd_forget_ident(void);
// Identifies a module across edits: key bytes 0-3, module manufacturer ID and
// manufacturing date (unlike the serial number, edits do not change these).
err_t spd_module_id(uint8_t id[8]);
// The same ID taken from a full SPD image.
void spd_module_id_of(const uint8_t *img, uint8_t id[8]);

uint16_t spd_size(void);
unsigned spd_blocks(void);
// Module serial number: 4 bytes, not covered by any CRC.
#define SPD_SERIAL_LEN 4
uint16_t spd_serial_off(void);
// Backup record area: the last SPD_RECORD_LEN bytes of the end-user area
// (DDR4 448-511, DDR5 960-1023).
#define SPD_RECORD_LEN 64
uint16_t spd_record_off(void);
err_t spd_read(uint16_t addr, uint8_t *buf, size_t len);
// Writes, then reads back. E_VERIFY or E_WP if the data did not stick.
err_t spd_write(uint16_t addr, const uint8_t *buf, size_t len);

err_t spd_wp_status(uint16_t *mask, bool *override);
err_t spd_wp_clear(void);
err_t spd_wp_set(uint16_t mask);
err_t spd_temp(int32_t *milli_c);

// CRC-16, polynomial 0x1021, init 0, MSB first (Annex L / JESD400-5B).
uint16_t spd_crc16(const uint8_t *data, size_t len);
// The CRC sections of the current generation.
const crc_section_t *spd_crc_sections(unsigned *count);
