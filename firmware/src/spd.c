#include "spd.h"

#include <string.h>

#include "ee1004.h"
#include "spd5118.h"

static uint8_t ident[4];

static const crc_section_t crc_ddr4[] = {
    {"base", 0, 125, 126},    // Annex L: bytes 0-125 -> 126/127
    {"module", 128, 253, 254},  // bytes 128-253 -> 254/255
};
static const crc_section_t crc_ddr5[] = {
    {"base", 0, 509, 510},  // JESD400-5B: bytes 0-509 -> 510/511
};

static bool ddr4(void) {
    return mode_is_ddr4(power_mode());
}

err_t spd_identify(dimm_mode_t m) {
    memset(ident, 0, sizeof ident);
    err_t e = spd_read(0, ident, sizeof ident);
    if (e != E_OK)
        return e;
    uint8_t type = ident[2], base = ident[3] & 0x0F;
    bool udimm, rdimm;
    if (mode_is_ddr4(m)) {
        if (type != 0x0C && type != 0x0E)  // DDR4, DDR4E
            return E_IDENT;
        udimm = base == 0x2;
        rdimm = base == 0x1 || base == 0x4;  // RDIMM, LRDIMM
    } else {
        if (type != 0x12)  // DDR5 SDRAM
            return E_IDENT;
        udimm = base == 0x2 || base == 0x5;  // UDIMM, CUDIMM
        rdimm = base == 0x1 || base == 0x4 || base == 0x7;  // RDIMM, LRDIMM, MRDIMM
    }
    if (mode_is_rdimm(m) ? !rdimm : !udimm)
        return E_IDENT;
    return E_OK;
}

const uint8_t *spd_ident_bytes(void) {
    return ident;
}

void spd_forget_ident(void) {
    memset(ident, 0, sizeof ident);
}

#define MODULE_MFR_OFF (ddr4() ? 320 : 512)
#define MFG_DATE_OFF   (ddr4() ? 323 : 515)

err_t spd_module_id(uint8_t id[8]) {
    err_t e = spd_read(0, id, 4);
    if (e == E_OK)
        e = spd_read(MODULE_MFR_OFF, id + 4, 2);
    if (e == E_OK)
        e = spd_read(MFG_DATE_OFF, id + 6, 2);
    return e;
}

void spd_module_id_of(const uint8_t *img, uint8_t id[8]) {
    memcpy(id, img, 4);
    memcpy(id + 4, img + MODULE_MFR_OFF, 2);
    memcpy(id + 6, img + MFG_DATE_OFF, 2);
}

uint16_t spd_size(void) {
    return ddr4() ? EE1004_SIZE : SPD5118_SIZE;
}

uint16_t spd_serial_off(void) {
    return ddr4() ? 325 : 517;
}

uint16_t spd_record_off(void) {
    return spd_size() - SPD_RECORD_LEN;
}

unsigned spd_blocks(void) {
    return ddr4() ? EE1004_BLOCKS : SPD5118_BLOCKS;
}

err_t spd_read(uint16_t addr, uint8_t *buf, size_t len) {
    if (power_state() != PWR_ON)
        return E_STATE;
    return ddr4() ? ee1004_read(addr, buf, len) : spd5118_read(addr, buf, len);
}

err_t spd_write(uint16_t addr, const uint8_t *buf, size_t len) {
    if (power_state() != PWR_ON)
        return E_STATE;
    err_t e = ddr4() ? ee1004_write(addr, buf, len) : spd5118_write(addr, buf, len);
    if (e != E_OK)
        return e;

    static uint8_t check[SPD5118_SIZE];
    e = spd_read(addr, check, len);
    if (e != E_OK)
        return e;
    if (memcmp(check, buf, len) == 0)
        return E_OK;
    // Find out whether write protection is the reason.
    uint16_t mask;
    bool ovr;
    unsigned bsize = spd_size() / spd_blocks();
    if (spd_wp_status(&mask, &ovr) == E_OK)
        for (size_t i = 0; i < len; i++)
            if (check[i] != buf[i] && (mask >> ((addr + i) / bsize)) & 1)
                return E_WP;
    return E_VERIFY;
}

err_t spd_wp_status(uint16_t *mask, bool *override) {
    if (power_state() != PWR_ON)
        return E_STATE;
    if (ddr4()) {
        *override = true;  // DDR4 can always clear with VHV
        return ee1004_wp_status(mask);
    }
    return spd5118_wp_status(mask, override);
}

err_t spd_wp_clear(void) {
    if (power_state() != PWR_ON)
        return E_STATE;
    return ddr4() ? ee1004_wp_clear() : spd5118_wp_clear();
}

err_t spd_wp_set(uint16_t mask) {
    if (power_state() != PWR_ON)
        return E_STATE;
    if (mask >> spd_blocks())
        return E_ARGS;
    return ddr4() ? ee1004_wp_set(mask) : spd5118_wp_set(mask);
}

err_t spd_temp(int32_t *milli_c) {
    if (power_state() != PWR_ON)
        return E_STATE;
    return ddr4() ? ee1004_temp(milli_c) : spd5118_temp(milli_c);
}

uint16_t spd_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (int i = 0; i < 8; i++)
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

const crc_section_t *spd_crc_sections(unsigned *count) {
    if (ddr4()) {
        *count = count_of(crc_ddr4);
        return crc_ddr4;
    }
    *count = count_of(crc_ddr5);
    return crc_ddr5;
}
