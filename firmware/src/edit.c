#include "edit.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "hardware/watchdog.h"
#include "spd.h"
#include "spd5118.h"
#include "store.h"
#include "timing.h"

#define MARKER          0x5D
#define REC_VERSION     1
#define REC_HDR         21
#define REC_MAX         ((STORE_REC_MAX - REC_HDR - 2) / 3)   // entries in a flash record
#define REC_MAX_MODULE  ((SPD_RECORD_LEN - REC_HDR - 2) / 3)  // entries in the record area
#define PRESET_VERSION  1

enum { OP_SET = 1, OP_MAP = 2, OP_SPEED = 3, OP_IF = 4, OP_TIME = 5 };
enum { TIME_SET = 0, TIME_SCALE = 1, TIME_ADD = 2, TIME_CLOCKS = 3 };
enum { SPEED_RATE = 0, SPEED_LOWEST = 1, SPEED_STEP = 2 };

typedef enum { WHERE_NONE, WHERE_MODULE, WHERE_FLASH } where_t;

static uint8_t cur[SPD5118_SIZE], tgt[SPD5118_SIZE];
static uint8_t rec[STORE_REC_MAX];

static uint16_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = v >> (8 * i);
}

// ---------------------------------------------------------------------------
// Marked serial

// Bytes that neither presets nor the content hash cover: serial, CRCs and
// the record area.
static bool excluded(unsigned i) {
    unsigned so = spd_serial_off();
    if ((i >= so && i < so + SPD_SERIAL_LEN) || i >= spd_record_off())
        return true;
    unsigned n;
    const crc_section_t *sec = spd_crc_sections(&n);
    for (unsigned k = 0; k < n; k++)
        if (i == sec[k].at || i == sec[k].at + 1u)
            return true;
    return false;
}

static uint32_t crc32_byte(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    return crc;
}

// CRC-32 as in zlib.
static uint32_t crc32(const uint8_t *p, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--)
        crc = crc32_byte(crc, *p++);
    return ~crc;
}

static uint32_t content_hash(const uint8_t *img) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned i = 0; i < spd_size(); i++)
        crc = crc32_byte(crc, excluded(i) ? 0 : img[i]);
    return ~crc;
}

static void marked_serial(const uint8_t *img, uint8_t out[SPD_SERIAL_LEN]) {
    uint32_t h = content_hash(img);
    out[0] = MARKER;
    out[1] = h;
    out[2] = h >> 8;
    out[3] = h >> 16;
}

// True if the serial is the marked serial of the current contents.
static bool hash_ok(const uint8_t *img) {
    uint8_t s[SPD_SERIAL_LEN];
    marked_serial(img, s);
    return memcmp(img + spd_serial_off(), s, sizeof s) == 0;
}

// ---------------------------------------------------------------------------
// Backup record

static size_t rec_len(const uint8_t *r) {
    return REC_HDR + 3u * r[4] + 2;
}

static bool rec_valid(const uint8_t *r, size_t space) {
    if (space < REC_HDR + 2 || r[0] != 'S' || r[1] != 'R' || r[2] != REC_VERSION)
        return false;
    size_t len = rec_len(r);
    return len <= space && spd_crc16(r, len - 2) == le16(r + len - 2);
}

// Looks for the record whose new serial is the module's serial: in the record
// area first, then in flash. *slot is the flash slot (or -1) either way.
static where_t find_record(const uint8_t *img, const uint8_t **r, int *slot) {
    const uint8_t *serial = img + spd_serial_off();
    const uint8_t *area = img + spd_record_off();
    where_t w = WHERE_NONE;
    if (rec_valid(area, SPD_RECORD_LEN) && memcmp(area + 13, serial, SPD_SERIAL_LEN) == 0) {
        *r = area;
        w = WHERE_MODULE;
    }
    uint8_t id[8];
    const uint8_t *f;
    size_t len;
    spd_module_id_of(img, id);
    *slot = store_find(id, 13, serial, SPD_SERIAL_LEN, &f, &len);
    if (*slot >= 0 && !rec_valid(f, len))
        *slot = -1;
    if (*slot >= 0 && w == WHERE_NONE) {
        *r = f;
        w = WHERE_FLASH;
    }
    return w;
}

// Deletes every flash record of this module with this new serial (left over
// from an interrupted Program, or the one just restored).
static err_t drop_flash_records(const uint8_t id[8], const uint8_t *serial) {
    const uint8_t *f;
    size_t len;
    int slot;
    while ((slot = store_find(id, 13, serial, SPD_SERIAL_LEN, &f, &len)) >= 0) {
        err_t e = store_delete(slot);
        if (e != E_OK)
            return e;
    }
    return E_OK;
}

// Overclocking profiles in the end-user area (host/spdtool/decode.py
// extra_profiles). XMP 3.0 has slots for user profiles, which the BIOS may
// write, up to the end of the SPD: with any profile present, the record area
// is not used.
static bool has_profiles(const uint8_t *img) {
    if (mode_is_ddr4(power_mode()))
        return img[384] == 0x0C && img[385] == 0x4A;  // XMP 2.0
    return (img[640] == 0x0C && img[641] == 0x4A) ||  // XMP 3.0
           memcmp(img + 832, "EXPO", 4) == 0;
}

static bool area_filled(const uint8_t *img, uint8_t fill) {
    const uint8_t *a = img + spd_record_off();
    for (unsigned i = 0; i < SPD_RECORD_LEN; i++)
        if (a[i] != fill)
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Presets

typedef struct {
    const uint8_t *p, *end;
    bool bad;
} cursor_t;

static uint8_t get8(cursor_t *c) {
    if (c->p >= c->end) {
        c->bad = true;
        return 0;
    }
    return *c->p++;
}

static uint16_t get16(cursor_t *c) {
    uint16_t lo = get8(c);
    return lo | get8(c) << 8;
}

static const uint8_t *take(cursor_t *c, size_t n) {
    const uint8_t *start = c->p;
    if ((size_t)(c->end - c->p) < n) {
        c->bad = true;
        c->p = c->end;
    } else {
        c->p += n;
    }
    return start;
}

static err_t check_off(unsigned off, char *msg, size_t msg_len) {
    if (off < spd_size() && !excluded(off))
        return E_OK;
    snprintf(msg, msg_len, "preset op at byte %u: outside the SPD or on the serial/CRC/record area", off);
    return E_ARGS;
}

// Standard speed grades: tCKAVGmin in ps (host/spdtool/speed.py).
static const uint16_t ddr4_rate[] = {1600, 1866, 2133, 2400, 2666, 2933, 3200};
static const uint16_t ddr4_tck[] = {1250, 1071, 938, 833, 750, 682, 625};
#define DDR5_RATE_MIN  3200
#define DDR5_RATE_STEP 400
#define DDR5_GRADES    15  // 3200 .. 8800

static unsigned grades(bool ddr4) {
    return ddr4 ? count_of(ddr4_rate) : DDR5_GRADES;
}

static unsigned grade_rate(bool ddr4, unsigned i) {
    return ddr4 ? ddr4_rate[i] : DDR5_RATE_MIN + DDR5_RATE_STEP * i;
}

static uint32_t grade_tck(bool ddr4, unsigned i) {
    return ddr4 ? ddr4_tck[i] : 2000000u / grade_rate(false, i);  // JESD79-5: truncated
}

// DDR4 times: medium units (125 ps) plus a signed fine correction (Annex L).
static int32_t ddr4_time(const uint8_t *img, unsigned mtb, unsigned fine) {
    return img[mtb] * 125 + (int8_t)img[fine];
}

static int32_t get_tck(const uint8_t *img, bool ddr4, bool max) {
    if (ddr4)
        return max ? ddr4_time(img, 19, 124) : ddr4_time(img, 18, 125);
    return le16(img + (max ? 22 : 20));
}

static int32_t get_taa(const uint8_t *img, bool ddr4) {
    return ddr4 ? ddr4_time(img, 24, 123) : le16(img + 30);
}

static void set_tck(uint8_t *img, bool ddr4, uint32_t ps) {
    if (ddr4) {
        uint32_t units = (ps + 124) / 125;  // rounded up, negative fine part
        img[18] = units;
        img[125] = (uint8_t)(int8_t)((int32_t)ps - (int32_t)units * 125);
    } else {
        img[20] = ps & 0xFF;
        img[21] = ps >> 8;
    }
}

// Clock count a controller derives from a time (host/spdtool/speed.py nck).
static unsigned nck(bool ddr4, int32_t t, uint32_t tck) {
    if (t <= 0)
        return 0;
    if (!ddr4)
        return ((uint64_t)t * 997 / tck + 1000) / 1000;  // JESD400-5B, 0.3% guard band
    int64_t num = 100 * (int64_t)t - tck;  // Annex L: ceil(t / tck - 0.01)
    return num <= 0 ? 0 : (unsigned)((num + 100 * (int64_t)tck - 1) / (100 * (int64_t)tck));
}

// Index of the standard grade within 1% of the module's tCKmin, or -1.
static int current_grade(const uint8_t *img, bool ddr4) {
    int32_t tck = get_tck(img, ddr4, false);
    for (unsigned i = 0; i < grades(ddr4); i++) {
        int32_t t = grade_tck(ddr4, i);
        if (tck >= t - t / 100 && tck <= t + t / 100)
            return (int)i;
    }
    return -1;
}

// Sets the maximum speed to a standard grade; the timings stay in ns, and the
// CL a controller needs at the new clock is added to the supported list.
static err_t apply_speed(uint8_t *img, uint8_t kind, int16_t value, char *msg, size_t msg_len) {
    bool ddr4 = mode_is_ddr4(power_mode());
    int cur = current_grade(img, ddr4), g;
    if (kind == SPEED_RATE) {
        for (g = 0; g < (int)grades(ddr4) && grade_rate(ddr4, g) != (uint16_t)value; g++)
            ;
        if (g == (int)grades(ddr4)) {
            snprintf(msg, msg_len, "%u MT/s is not a standard %s speed", (uint16_t)value, ddr4 ? "DDR4" : "DDR5");
            return E_ARGS;
        }
    } else if (kind == SPEED_LOWEST) {
        g = 0;
    } else if (kind == SPEED_STEP) {
        if (cur < 0) {
            snprintf(msg, msg_len, "the module's speed (tCKmin %ld ps) is not a standard grade",
                     (long)get_tck(img, ddr4, false));
            return E_REFUSED;
        }
        g = cur + value;
        if (g < 0 || g >= (int)grades(ddr4)) {
            snprintf(msg, msg_len, "already at the %s standard speed (%u MT/s)", value > 0 ? "fastest" : "slowest",
                     grade_rate(ddr4, cur));
            return E_REFUSED;
        }
    } else {
        snprintf(msg, msg_len, "malformed preset (speed kind %u)", kind);
        return E_ARGS;
    }
    uint32_t tck = grade_tck(ddr4, g);
    int32_t tck_max = get_tck(img, ddr4, true);
    if (tck_max > 0 && (int32_t)tck > tck_max) {
        snprintf(msg, msg_len, "%u MT/s is below the module's slowest clock (tCKmax %ld ps)", grade_rate(ddr4, g),
                 (long)tck_max);
        return E_REFUSED;
    }
    set_tck(img, ddr4, tck);

    unsigned cl = nck(ddr4, get_taa(img, ddr4), tck);
    if (!ddr4 && cl % 2)
        cl++;
    int bit;
    unsigned first;
    if (ddr4) {
        bit = (int)cl - (img[23] & 0x80 ? 23 : 7);
        first = 20;
        if (bit < 0 || bit >= 30)
            bit = -1;
    } else {
        bit = cl >= 20 && cl <= 98 ? (int)(cl - 20) / 2 : -1;
        first = 24;
    }
    if (bit < 0) {
        snprintf(msg, msg_len, "CL%u at %u MT/s cannot be encoded in this module's CL list", cl,
                 grade_rate(ddr4, g));
        return E_REFUSED;
    }
    img[first + bit / 8] |= 1u << (bit % 8);
    return E_OK;
}

static uint32_t get32(cursor_t *c) {
    uint32_t lo = get16(c);
    return lo | (uint32_t)get16(c) << 16;
}

// TIME: set, scale (a/b, rounded up), add or set to a clocks at tCKmin.
static err_t apply_time(uint8_t *img, const timing_t *t, uint8_t action, int32_t a, int32_t b, char *msg,
                        size_t msg_len) {
    // The bytes this encoding uses must be ordinary SPD bytes.
    unsigned used[3], n = 0;
    used[n++] = t->off;
    if (t->enc == TIMING_PS16 || t->enc == TIMING_NS16 || t->enc == TIMING_MTB16)
        used[n++] = t->off + 1;
    if (t->enc == TIMING_MTB_FINE || t->enc == TIMING_MTB12)
        used[n++] = t->off2;
    if (t->enc == TIMING_MTB12 && t->off3 != TIMING_NO_FINE)
        used[n++] = t->off3;
    for (unsigned k = 0; k < n; k++) {
        err_t e = check_off(used[k], msg, msg_len);
        if (e != E_OK)
            return e;
    }
    int32_t ps;
    if (!timing_get(img, t, &ps)) {
        snprintf(msg, msg_len, "malformed preset (timing encoding %u)", t->enc);
        return E_ARGS;
    }
    int64_t v;
    if (action == TIME_SET)
        v = a;
    else if (action == TIME_SCALE && b > 0)
        v = ((int64_t)ps * a + b - 1) / b;
    else if (action == TIME_ADD)
        v = (int64_t)ps + a;
    else if (action == TIME_CLOCKS)
        v = (int64_t)a * get_tck(img, mode_is_ddr4(power_mode()), false);
    else {
        snprintf(msg, msg_len, "malformed preset (time action %u)", action);
        return E_ARGS;
    }
    if (v < 0 || v > INT32_MAX || !timing_set(img, t, (int32_t)v)) {
        snprintf(msg, msg_len, "timing at byte %u: %lld ps does not fit the field", t->off, (long long)v);
        return E_REFUSED;
    }
    return E_OK;
}

// Walks `count` ops; changes `img` only when `apply`.
static err_t run_ops(cursor_t *c, unsigned count, uint8_t *img, bool apply, char *msg, size_t msg_len) {
    for (unsigned k = 0; k < count && !c->bad; k++) {
        uint8_t type = get8(c);
        if (type == OP_SET) {
            uint16_t off = get16(c);
            uint8_t mask = get8(c), val = get8(c);
            if (!apply || c->bad)
                continue;
            err_t e = check_off(off, msg, msg_len);
            if (e != E_OK)
                return e;
            img[off] = (img[off] & ~mask) | (val & mask);
        } else if (type == OP_MAP) {
            uint16_t off = get16(c);
            uint8_t mask = get8(c), n = get8(c);
            const uint8_t *pairs = take(c, 2u * n);
            if (!apply || c->bad)
                continue;
            err_t e = check_off(off, msg, msg_len);
            if (e != E_OK)
                return e;
            uint8_t v = img[off] & mask;
            unsigned j = 0;
            while (j < n && pairs[2 * j] != v)
                j++;
            if (j == n) {
                snprintf(msg, msg_len, "byte %u: value 0x%02x has no mapping in this preset (at the limit?)", off, v);
                return E_REFUSED;
            }
            img[off] = (img[off] & ~mask) | (pairs[2 * j + 1] & mask);
        } else if (type == OP_SPEED) {
            uint8_t kind = get8(c);
            int16_t value = (int16_t)get16(c);
            if (!apply || c->bad)
                continue;
            err_t e = apply_speed(img, kind, value, msg, msg_len);
            if (e != E_OK)
                return e;
        } else if (type == OP_TIME) {
            timing_t t;
            t.enc = get8(c);
            t.off = get16(c);
            t.off2 = get16(c);
            t.off3 = get16(c);
            t.shift = get8(c);
            uint8_t action = get8(c);
            int32_t a = (int32_t)get32(c), b = (int32_t)get32(c);
            if (!apply || c->bad)
                continue;
            err_t e = apply_time(img, &t, action, a, b, msg, msg_len);
            if (e != E_OK)
                return e;
        } else if (type == OP_IF) {
            // The next `n` ops run only if byte & mask == value.
            uint16_t off = get16(c);
            uint8_t mask = get8(c), value = get8(c), n = get8(c);
            bool cond = false;
            if (apply && !c->bad) {
                err_t e = off < spd_size() ? E_OK : check_off(off, msg, msg_len);
                if (e != E_OK)
                    return e;
                cond = (img[off] & mask) == (value & mask);
            }
            err_t e = run_ops(c, n, img, apply && cond, msg, msg_len);
            if (e != E_OK)
                return e;
        } else {
            c->bad = true;
        }
    }
    if (c->bad) {
        snprintf(msg, msg_len, "malformed preset");
        return E_ARGS;
    }
    return E_OK;
}

// Applies the ops for the powered module's generation to `img`; with img ==
// NULL only checks the format.
static err_t apply_preset(const uint8_t *p, size_t len, uint8_t *img, char *name, size_t name_size, char *msg,
                          size_t msg_len) {
    cursor_t c = {p, p + len, false};
    if (get8(&c) != 'S' || get8(&c) != 'P' || get8(&c) != PRESET_VERSION) {
        snprintf(msg, msg_len, "not a version-%d preset", PRESET_VERSION);
        return E_ARGS;
    }
    uint8_t nl = get8(&c);
    const uint8_t *n = take(&c, nl);
    take(&c, get8(&c));  // description
    if (c.bad) {
        snprintf(msg, msg_len, "malformed preset");
        return E_ARGS;
    }
    size_t copy = nl < name_size - 1 ? nl : name_size - 1;
    memcpy(name, n, copy);
    name[copy] = 0;

    bool ddr4 = img && mode_is_ddr4(power_mode());
    unsigned n4 = get8(&c);
    err_t e = run_ops(&c, n4, img, img && ddr4, msg, msg_len);
    if (e != E_OK)
        return e;
    unsigned n5 = get8(&c);
    e = run_ops(&c, n5, img, img && !ddr4, msg, msg_len);
    if (e != E_OK)
        return e;
    if (c.bad || c.p != c.end) {
        snprintf(msg, msg_len, "malformed preset");
        return E_ARGS;
    }
    if (img && (ddr4 ? n4 : n5) == 0) {
        snprintf(msg, msg_len, "preset '%s' does nothing on %s modules", name, ddr4 ? "DDR4" : "DDR5");
        return E_IDENT;
    }
    return E_OK;
}

err_t edit_preset_check(const uint8_t *p, size_t len, char *name, size_t name_size, char *msg, size_t msg_len) {
    return apply_preset(p, len, NULL, name, name_size, msg, msg_len);
}

// ---------------------------------------------------------------------------
// Writing

static uint16_t blocks_touched(void) {
    unsigned bsize = spd_size() / spd_blocks();
    uint16_t mask = 0;
    for (unsigned i = 0; i < spd_size(); i++)
        if (cur[i] != tgt[i])
            mask |= 1u << (i / bsize);
    return mask;
}

// Writes the bytes of [lo, hi) where tgt differs from cur, in runs.
static err_t write_runs(unsigned lo, unsigned hi) {
    for (unsigned i = lo; i < hi;) {
        if (cur[i] == tgt[i]) {
            i++;
            continue;
        }
        unsigned j = i;
        while (j < hi && cur[j] != tgt[j])
            j++;
        err_t e = spd_write(i, tgt + i, j - i);
        if (e != E_OK)
            return e;
        watchdog_update();
        i = j;
    }
    return E_OK;
}

// Writes tgt over cur with write protection lifted where needed, restores the
// protection and verifies the whole SPD.
static err_t commit(bool program) {
    uint16_t mask;
    bool ovr;
    err_t e = spd_wp_status(&mask, &ovr);
    if (e != E_OK)
        return e;
    bool unlock = (mask & blocks_touched()) != 0;
    if (unlock && (e = spd_wp_clear()) != E_OK)
        return e;

    unsigned so = spd_serial_off(), ro = spd_record_off(), n = spd_size();
    if (program) {
        // Record and marked serial first: an interrupted Program leaves a
        // module that "restore force" can still undo.
        e = write_runs(ro, n);
        if (e == E_OK)
            e = write_runs(so, so + SPD_SERIAL_LEN);
        if (e == E_OK)
            e = write_runs(0, so);
        if (e == E_OK)
            e = write_runs(so + SPD_SERIAL_LEN, ro);
    } else {
        // The original serial last: until then the module still shows as
        // modified.
        e = write_runs(0, so);
        if (e == E_OK)
            e = write_runs(so + SPD_SERIAL_LEN, n);
        if (e == E_OK)
            e = write_runs(so, so + SPD_SERIAL_LEN);
    }
    if (unlock) {
        err_t e2 = spd_wp_set(mask);
        if (e == E_OK)
            e = e2;
    }
    if (e != E_OK)
        return e;
    e = spd_read(0, cur, n);
    if (e == E_OK && memcmp(cur, tgt, n) != 0)
        e = E_VERIFY;
    return e;
}

static void set_crcs(uint8_t *img) {
    unsigned n;
    const crc_section_t *sec = spd_crc_sections(&n);
    for (unsigned k = 0; k < n; k++) {
        uint16_t crc = spd_crc16(img + sec[k].start, sec[k].end - sec[k].start + 1);
        img[sec[k].at] = crc & 0xFF;
        img[sec[k].at + 1] = crc >> 8;
    }
}

// ---------------------------------------------------------------------------
// Program / Restore

err_t edit_program(const uint8_t *preset, size_t plen, char *msg, size_t msg_len) {
    unsigned n = spd_size(), so = spd_serial_off(), ro = spd_record_off();
    msg[0] = 0;
    err_t e = spd_read(0, cur, n);
    if (e != E_OK)
        return e;
    uint32_t pid = crc32(preset, plen);

    if (cur[so] == MARKER) {
        const uint8_t *r;
        int slot;
        where_t w = find_record(cur, &r, &slot);
        bool same = hash_ok(cur);
        if (w != WHERE_NONE && le32(r + 5) == pid && same) {
            snprintf(msg, msg_len, "already applied");
            led_set(PIN_LED_MOD, true);
            return E_OK;
        }
        if (w != WHERE_NONE) {
            snprintf(msg, msg_len, "already modified (%s): restore first",
                     le32(r + 5) != pid ? "by another preset" : "and changed since");
            return E_REFUSED;
        }
        if (same) {
            snprintf(msg, msg_len, "already modified, and no backup record found: restore first");
            return E_REFUSED;
        }
        // Otherwise a genuine serial that happens to start with the marker.
    }

    char name[24];
    memcpy(tgt, cur, n);
    e = apply_preset(preset, plen, tgt, name, sizeof name, msg, msg_len);
    if (e != E_OK)
        return e;

    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (excluded(i) || tgt[i] == cur[i])
            continue;
        if (k == REC_MAX) {
            snprintf(msg, msg_len, "preset '%s' changes more than %d bytes", name, REC_MAX);
            return E_REFUSED;
        }
        uint8_t *ent = rec + REC_HDR + 3 * k++;
        ent[0] = i & 0xFF;
        ent[1] = i >> 8;
        ent[2] = cur[i];
    }
    if (k == 0) {
        snprintf(msg, msg_len, "preset '%s' changes nothing on this module", name);
        return E_REFUSED;
    }

    bool fill_ff = area_filled(cur, 0xFF);
    bool on_module = (fill_ff || area_filled(cur, 0x00)) && k <= REC_MAX_MODULE && !has_profiles(cur);
    rec[0] = 'S';
    rec[1] = 'R';
    rec[2] = REC_VERSION;
    rec[3] = fill_ff;
    rec[4] = k;
    put_le32(rec + 5, pid);
    memcpy(rec + 9, cur + so, SPD_SERIAL_LEN);
    marked_serial(tgt, tgt + so);
    memcpy(rec + 13, tgt + so, SPD_SERIAL_LEN);
    unsigned nsec;
    const crc_section_t *sec = spd_crc_sections(&nsec);
    memset(rec + 17, 0, 4);
    for (unsigned s = 0; s < nsec && s < 2; s++)
        memcpy(rec + 17 + 2 * s, cur + sec[s].at, 2);
    size_t len = REC_HDR + 3 * k;
    uint16_t crc = spd_crc16(rec, len);
    rec[len++] = crc & 0xFF;
    rec[len++] = crc >> 8;

    set_crcs(tgt);
    if (on_module)
        memcpy(tgt + ro, rec, len);

    uint8_t id[8];
    spd_module_id_of(tgt, id);
    e = drop_flash_records(id, tgt + so);
    if (e == E_OK)
        e = store_add(id, rec, len);
    if (e != E_OK) {
        snprintf(msg, msg_len, "could not store the backup record in flash");
        return e;
    }
    e = commit(true);
    if (e != E_OK)
        return e;
    led_set(PIN_LED_MOD, true);
    snprintf(msg, msg_len, "preset=%s id=%08lx changed=%u record=%s serial=%02x%02x%02x%02x", name,
             (unsigned long)pid, k, on_module ? "module+flash" : "flash", tgt[so], tgt[so + 1], tgt[so + 2],
             tgt[so + 3]);
    return E_OK;
}

err_t edit_restore(bool force, char *msg, size_t msg_len) {
    unsigned n = spd_size(), so = spd_serial_off(), ro = spd_record_off();
    msg[0] = 0;
    err_t e = spd_read(0, cur, n);
    if (e != E_OK)
        return e;
    if (cur[so] != MARKER) {
        snprintf(msg, msg_len, "not modified: nothing to restore");
        return E_REFUSED;
    }
    const uint8_t *r;
    int slot;
    where_t w = find_record(cur, &r, &slot);
    if (w == WHERE_NONE) {
        snprintf(msg, msg_len, "modified, but no backup record on the module or on this board");
        return E_REFUSED;
    }
    if (!force && !hash_ok(cur)) {
        snprintf(msg, msg_len, "contents changed since Program ('restore force' overrides)");
        return E_REFUSED;
    }
    size_t len = rec_len(r);
    memcpy(rec, r, len);  // r may point into cur

    memcpy(tgt, cur, n);
    unsigned k = rec[4];
    for (unsigned j = 0; j < k; j++) {
        const uint8_t *ent = rec + REC_HDR + 3 * j;
        unsigned off = le16(ent);
        if (off >= n || excluded(off)) {
            snprintf(msg, msg_len, "corrupt backup record (entry at byte %u)", off);
            return E_REFUSED;
        }
        tgt[off] = ent[2];
    }
    memcpy(tgt + so, rec + 9, SPD_SERIAL_LEN);
    unsigned nsec;
    const crc_section_t *sec = spd_crc_sections(&nsec);
    for (unsigned s = 0; s < nsec && s < 2; s++)
        memcpy(tgt + sec[s].at, rec + 17 + 2 * s, 2);
    if (w == WHERE_MODULE)
        memset(tgt + ro, rec[3] & 1 ? 0xFF : 0x00, SPD_RECORD_LEN);

    uint8_t id[8];
    spd_module_id_of(cur, id);  // as stored by Program: from the programmed image
    e = commit(false);
    if (e != E_OK)
        return e;
    if (slot >= 0 && drop_flash_records(id, rec + 13) != E_OK) {
        snprintf(msg, msg_len, "restored, but the flash record could not be deleted");
        return E_FLASH;
    }
    led_set(PIN_LED_MOD, false);
    snprintf(msg, msg_len, "restored=%u record=%s serial=%02x%02x%02x%02x", k,
             w == WHERE_MODULE ? (slot >= 0 ? "module+flash" : "module") : "flash", tgt[so], tgt[so + 1],
             tgt[so + 2], tgt[so + 3]);
    return E_OK;
}

err_t edit_report(void) {
    unsigned so = spd_serial_off();
    err_t e = spd_read(0, cur, spd_size());
    if (e != E_OK)
        return e;
    const uint8_t *r = NULL;
    int slot;
    where_t w = find_record(cur, &r, &slot);
    bool marked = cur[so] == MARKER, same = marked && hash_ok(cur);
    bool modified = marked && (same || w != WHERE_NONE);
    const char *area = w == WHERE_MODULE                                   ? "record"
                       : has_profiles(cur)                                  ? "profiles"
                       : area_filled(cur, 0x00) || area_filled(cur, 0xFF) ? "free"
                                                                            : "used";
    led_set(PIN_LED_MOD, modified);
    printf("OK modified=%d hash=%s record=%s area=%s flash_records=%u", modified,
           !marked ? "-" : same ? "ok" : "changed",
           w == WHERE_NONE ? "none" : w == WHERE_MODULE ? (slot >= 0 ? "module+flash" : "module") : "flash", area,
           store_count());
    if (w != WHERE_NONE) {
        printf(" preset=%08lx rec=", (unsigned long)le32(r + 5));
        for (size_t i = 0; i < rec_len(r); i++)
            printf("%02x", r[i]);
    }
    printf("\n");
    return E_OK;
}
