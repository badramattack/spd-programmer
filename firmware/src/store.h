// Backup records in the top 256 KB of the Pico flash, kept across firmware
// updates (picotool and UF2 only write the blocks the firmware covers).
//
// One record per 256-byte flash page ("slot"):
//   [0]     state: 0xFF free, 0xA5 valid, 0x00 deleted
//   [1..8]  module ID (spd_module_id: key bytes 0-3, module manufacturer, date)
//   [9]     record length
//   [10..]  record (edit.h)
// The first 4 KB sector of the area is reserved for presets.
#pragma once

#include "err.h"
#include "pico/stdlib.h"

#define STORE_REC_MAX 246  // FLASH_PAGE_SIZE - 10

// Adds a record; when no slot is free, erases the sector with the fewest
// valid records first.
err_t store_add(const uint8_t id[8], const uint8_t *rec, size_t len);
// Finds the newest valid record of the module with this ID whose bytes at
// `key_off` equal `key` (the record's new serial number). Returns the slot
// number or -1; *rec points into flash.
int store_find(const uint8_t id[8], size_t key_off, const uint8_t *key, size_t key_len, const uint8_t **rec,
               size_t *len);
err_t store_delete(int slot);
// Number of valid records.
unsigned store_count(void);

// The preset sector (presets.c): read in place, written as a whole.
#define STORE_PRESET_SIZE 4096
const uint8_t *store_preset_sector(void);
err_t store_preset_write(const uint8_t *data);
