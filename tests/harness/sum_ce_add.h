/* CEdev truncating-add model. Test-only, checked against 798 owned-ROM samples.
 * Shifted-out bits are discarded. Only a carry rounds its single lost bit. */
#include <stdint.h>
#include <string.h>
#include <math.h>
static uint32_t sum_bits(float x) { uint32_t b; memcpy(&b, &x, 4); return b; }
static float sum_float(uint32_t b) { float x; memcpy(&x, &b, 4); return x; }
static float sum_ce_add(float a, float b)
{
    uint32_t ab = sum_bits(a), bb = sum_bits(b), ma, mb, m, tmp;
    unsigned sa = ab >> 31, sb = bb >> 31;
    int ea = (ab >> 23) & 255, eb = (bb >> 23) & 255, d;
    if (!(ab & 0x7fffffff)) return sum_float(bb & 0x7fffffff ? bb : ab & bb);
    if (!(bb & 0x7fffffff)) return a;
    if (ea == 255 || eb == 255) return a + b;
    ma = (ab & 0x7fffff) | (ea ? 0x800000 : 0);
    mb = (bb & 0x7fffff) | (eb ? 0x800000 : 0);
    if (!ea) ea = 1;
    if (!eb) eb = 1;
    if (ea < eb || (ea == eb && ma < mb)) {
        tmp = ma; ma = mb; mb = tmp;
        d = ea; ea = eb; eb = d;
        tmp = sa; sa = sb; sb = tmp;
    }
    d = ea - eb;
    mb = d < 32 ? mb >> d : 0;
    if (sa == sb) {
        m = ma + mb;
        if (m >> 24) {
            m = (m + 1) >> 1; ea++;
            if (m >> 24) { m >>= 1; ea++; }
        }
    } else {
        m = ma - mb;
        if (!m) return 0;
        while (m < 0x800000 && ea > 1) { m <<= 1; ea--; }
        if (m < 0x800000) ea = 0;
    }
    return sum_float((sa << 31) | (ea >= 255 ? 0x7f800000 : (uint32_t)ea << 23 | (m & 0x7fffff)));
}
/* The CE_SAFE_FLOAT add also preserves exact small integer differences. */
static float sum_checked_add(float a, float b)
{
    unsigned ea = (sum_bits(a) >> 23) & 255, eb = (sum_bits(b) >> 23) & 255;
    if ((ea == 151 || eb == 151) && ea >= 127 && ea <= 151 && eb >= 127 && eb <= 151) {
        long ia = (long)a, ib = (long)b, s = ia + ib;
        if ((float)ia == a && (float)ib == b && s >= -16777216L && s <= 16777216L && s) return (float)s;
    }
    return sum_ce_add(a, b);
}
