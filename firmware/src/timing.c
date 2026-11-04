#include "timing.h"

#define MTB 125

static int32_t ceil_div(int32_t a, int32_t b) {
    return (a + b - 1) / b;  // a >= 0
}

bool timing_get(const uint8_t *img, const timing_t *t, int32_t *ps) {
    switch (t->enc) {
    case TIMING_PS16:
        *ps = img[t->off] | img[t->off + 1] << 8;
        return true;
    case TIMING_NS16:
        *ps = (img[t->off] | img[t->off + 1] << 8) * 1000;
        return true;
    case TIMING_MTB_FINE:
        *ps = img[t->off] * MTB + (int8_t)img[t->off2];
        return true;
    case TIMING_MTB12: {
        int32_t units = ((img[t->off] >> t->shift) & 0xF) << 8 | img[t->off2];
        *ps = units * MTB + (t->off3 == TIMING_NO_FINE ? 0 : (int8_t)img[t->off3]);
        return true;
    }
    case TIMING_MTB16:
        *ps = (img[t->off] | img[t->off + 1] << 8) * MTB;
        return true;
    default:
        return false;
    }
}

bool timing_set(uint8_t *img, const timing_t *t, int32_t ps) {
    if (ps < 0)
        return false;
    int32_t units = ceil_div(ps, MTB), fine = ps - units * MTB;  // fine in (-125, 0]
    switch (t->enc) {
    case TIMING_PS16:
        if (ps > 0xFFFF)
            return false;
        img[t->off] = ps & 0xFF;
        img[t->off + 1] = ps >> 8;
        return true;
    case TIMING_NS16: {
        int32_t ns = ceil_div(ps, 1000);
        if (ns > 0xFFFF)
            return false;
        img[t->off] = ns & 0xFF;
        img[t->off + 1] = ns >> 8;
        return true;
    }
    case TIMING_MTB_FINE:
        if (units < 1 || units > 255)
            return false;
        img[t->off] = units;
        img[t->off2] = (uint8_t)(int8_t)fine;
        return true;
    case TIMING_MTB12:
        if (units > 0xFFF)
            return false;
        if (t->off3 != TIMING_NO_FINE)
            img[t->off3] = (uint8_t)(int8_t)fine;
        img[t->off] = (img[t->off] & ~(0xF << t->shift)) | (units >> 8) << t->shift;
        img[t->off2] = units & 0xFF;
        return true;
    case TIMING_MTB16:
        if (units > 0xFFFF)
            return false;
        img[t->off] = units & 0xFF;
        img[t->off + 1] = units >> 8;
        return true;
    default:
        return false;
    }
}
