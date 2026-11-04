#include "store.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"

#define AREA_SIZE    (256 * 1024)
#define AREA_OFFSET  (PICO_FLASH_SIZE_BYTES - AREA_SIZE)
#define REC_OFFSET   (AREA_OFFSET + FLASH_SECTOR_SIZE)  // after the preset sector
#define SLOTS        ((AREA_SIZE - FLASH_SECTOR_SIZE) / FLASH_PAGE_SIZE)
#define SLOTS_PER_SECTOR (FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE)

#define SLOT_FREE    0xFF
#define SLOT_VALID   0xA5
#define SLOT_DELETED 0x00

static const uint8_t *slot(unsigned i) {
    return (const uint8_t *)(XIP_BASE + REC_OFFSET + i * FLASH_PAGE_SIZE);
}

typedef struct {
    uint32_t offset;
    const uint8_t *page;  // NULL: erase the sector at `offset`
} flash_op_t;

static void run_op(void *param) {
    const flash_op_t *op = param;
    if (op->page)
        flash_range_program(op->offset, op->page, FLASH_PAGE_SIZE);
    else
        flash_range_erase(op->offset, FLASH_SECTOR_SIZE);
}

static err_t flash_op(uint32_t offset, const uint8_t *page) {
    flash_op_t op = {offset, page};
    return flash_safe_execute(run_op, &op, 100) == PICO_OK ? E_OK : E_FLASH;
}

static int free_slot(void) {
    for (unsigned i = 0; i < SLOTS; i++)
        if (slot(i)[0] == SLOT_FREE)
            return (int)i;
    // Reclaim the sector with the fewest valid records (ideally none).
    unsigned best = 0, best_valid = SLOTS_PER_SECTOR + 1;
    for (unsigned s = 0; s < SLOTS / SLOTS_PER_SECTOR; s++) {
        unsigned valid = 0;
        for (unsigned j = 0; j < SLOTS_PER_SECTOR; j++)
            valid += slot(s * SLOTS_PER_SECTOR + j)[0] == SLOT_VALID;
        if (valid < best_valid) {
            best = s;
            best_valid = valid;
        }
    }
    if (flash_op(REC_OFFSET + best * FLASH_SECTOR_SIZE, NULL) != E_OK)
        return -1;
    return (int)(best * SLOTS_PER_SECTOR);
}

err_t store_add(const uint8_t id[8], const uint8_t *rec, size_t len) {
    if (len > STORE_REC_MAX)
        return E_ARGS;
    int i = free_slot();
    if (i < 0)
        return E_FLASH;
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    page[0] = SLOT_VALID;
    memcpy(page + 1, id, 8);
    page[9] = (uint8_t)len;
    memcpy(page + 10, rec, len);
    err_t e = flash_op(REC_OFFSET + (uint32_t)i * FLASH_PAGE_SIZE, page);
    if (e == E_OK && memcmp(slot(i), page, FLASH_PAGE_SIZE) != 0)
        e = E_FLASH;
    return e;
}

int store_find(const uint8_t id[8], size_t key_off, const uint8_t *key, size_t key_len, const uint8_t **rec,
               size_t *len) {
    for (int i = SLOTS - 1; i >= 0; i--) {
        const uint8_t *p = slot(i);
        if (p[0] != SLOT_VALID || memcmp(p + 1, id, 8) != 0 || p[9] > STORE_REC_MAX ||
            key_off + key_len > p[9] || memcmp(p + 10 + key_off, key, key_len) != 0)
            continue;
        *rec = p + 10;
        *len = p[9];
        return i;
    }
    return -1;
}

err_t store_delete(int i) {
    if (i < 0 || i >= (int)SLOTS)
        return E_ARGS;
    // Programming can only clear bits: 0xFF everywhere leaves the rest as is.
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    page[0] = SLOT_DELETED;
    err_t e = flash_op(REC_OFFSET + (uint32_t)i * FLASH_PAGE_SIZE, page);
    return e == E_OK && slot(i)[0] != SLOT_DELETED ? E_FLASH : e;
}

unsigned store_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < SLOTS; i++)
        n += slot(i)[0] == SLOT_VALID;
    return n;
}

const uint8_t *store_preset_sector(void) {
    return (const uint8_t *)(XIP_BASE + AREA_OFFSET);
}

err_t store_preset_write(const uint8_t *data) {
    err_t e = flash_op(AREA_OFFSET, NULL);
    for (uint32_t off = 0; off < FLASH_SECTOR_SIZE && e == E_OK; off += FLASH_PAGE_SIZE)
        e = flash_op(AREA_OFFSET + off, data + off);
    if (e == E_OK && memcmp(store_preset_sector(), data, FLASH_SECTOR_SIZE) != 0)
        e = E_FLASH;
    return e;
}
