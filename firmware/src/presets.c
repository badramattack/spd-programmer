#include "presets.h"

#include <string.h>

#include "default_presets.h"
#include "store.h"

#define HDR      5
#define NO_ACTIVE 0xFF

static uint8_t sector[STORE_PRESET_SIZE];
static bool loaded, builtin;

// Offset of entry i (its length field), or of the end after the last one.
static size_t entry(const uint8_t *s, unsigned i) {
    size_t off = HDR;
    for (unsigned k = 0; k < i; k++)
        off += 2 + (s[off] | s[off + 1] << 8);
    return off;
}

static bool valid(const uint8_t *s) {
    if (s[0] != 'P' || s[1] != 'S' || s[2] != 1)
        return false;
    size_t off = HDR;
    for (unsigned k = 0; k < s[4]; k++) {
        if (off + 2 > STORE_PRESET_SIZE)
            return false;
        off += 2 + (s[off] | s[off + 1] << 8);
    }
    return off <= STORE_PRESET_SIZE && (s[3] == NO_ACTIVE || s[3] < s[4]);
}

static void load(void) {
    if (loaded)
        return;
    const uint8_t *f = store_preset_sector();
    builtin = !valid(f);
    memset(sector, 0xFF, sizeof sector);
    if (builtin)
        memcpy(sector, default_presets, sizeof default_presets);
    else
        memcpy(sector, f, sizeof sector);
    loaded = true;
}

static err_t save(void) {
    err_t e = store_preset_write(sector);
    if (e == E_OK)
        builtin = false;
    return e;
}

unsigned presets_count(void) {
    load();
    return sector[4];
}

const uint8_t *presets_get(unsigned i, size_t *len) {
    load();
    if (i >= sector[4])
        return NULL;
    size_t off = entry(sector, i);
    *len = sector[off] | sector[off + 1] << 8;
    return sector + off + 2;
}

void presets_name(unsigned i, char *name, size_t size) {
    size_t len;
    const uint8_t *p = presets_get(i, &len);
    size_t n = p && len > 4 ? p[3] : 0;
    if (!p || n > len - 4 || n >= size)
        n = 0;
    if (n)
        memcpy(name, p + 4, n);
    name[n] = 0;
}

int presets_find(const char *name) {
    char buf[32];
    for (unsigned i = 0; i < presets_count(); i++) {
        presets_name(i, buf, sizeof buf);
        if (strcmp(buf, name) == 0)
            return (int)i;
    }
    return -1;
}

int presets_active(void) {
    load();
    return sector[3] == NO_ACTIVE ? -1 : sector[3];
}

bool presets_builtin(void) {
    load();
    return builtin;
}

err_t presets_use(unsigned i) {
    if (i >= presets_count())
        return E_ARGS;
    sector[3] = i;
    return save();
}

err_t presets_delete(unsigned i) {
    unsigned n = presets_count();
    if (i >= n)
        return E_ARGS;
    size_t from = entry(sector, i), to = entry(sector, i + 1), end = entry(sector, n);
    memmove(sector + from, sector + to, end - to);
    memset(sector + end - (to - from), 0xFF, to - from);
    sector[4] = n - 1;
    if (sector[3] != NO_ACTIVE)
        sector[3] = sector[3] == i ? NO_ACTIVE : sector[3] > i ? sector[3] - 1 : sector[3];
    return save();
}

err_t presets_add(const uint8_t *p, size_t len) {
    char name[32];
    size_t n = len > 4 && p[3] < sizeof name && 4u + p[3] <= len ? p[3] : 0;
    memcpy(name, p + 4, n);
    name[n] = 0;
    int old = presets_find(name);
    int active = presets_active();
    // Remove the old version in RAM only; one save at the end.
    if (old >= 0) {
        unsigned cnt = sector[4];
        size_t from = entry(sector, old), to = entry(sector, old + 1), end = entry(sector, cnt);
        memmove(sector + from, sector + to, end - to);
        memset(sector + end - (to - from), 0xFF, to - from);
        sector[4] = cnt - 1;
        if (active > old)
            sector[3] = active - 1;
    }
    size_t end = entry(sector, sector[4]);
    if (sector[4] == 0xFE || end + 2 + len > STORE_PRESET_SIZE) {
        loaded = false;  // drop the RAM change
        return E_FLASH;
    }
    sector[end] = len & 0xFF;
    sector[end + 1] = len >> 8;
    memcpy(sector + end + 2, p, len);
    sector[4]++;
    if (old >= 0 && active == old)
        sector[3] = sector[4] - 1;  // a replaced active preset stays active
    return save();
}

err_t presets_reset(void) {
    memset(sector, 0xFF, sizeof sector);
    memcpy(sector, default_presets, sizeof default_presets);
    err_t e = save();
    builtin = true;
    return e;
}
