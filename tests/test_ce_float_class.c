/*
 * test_ce_float_class.c - exhaustive check of src/ce_float_class.h.
 *
 * Compares ce_isnan_f/ce_isinf_f/ce_isfinite_f (the helpers every CE build
 * uses) with the host <math.h> macros for all 2^32 binary32 bit patterns,
 * so every NaN payload, both infinities, denormals and signed zeros are
 * covered.
 *
 * It also models CEdev v11.2's libc _isinff (lib/libc/isinff.src) bit for
 * bit, to pin down the bug the helpers replace. With a
 * non-zero mantissa that routine tests mantissa bits 7..14 instead of the
 * exponent, so it reports finite values such as 16777215 as infinite.
 */
/* Exercise the CE macro path (ce_isinf(x) -> ce_isinf_f((float)(x))). */
#define CE_FLOAT_CLASS_REDIRECT 1
#include "ce_float_class.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static uint64_t g_run = 0;  /* ~1.3e10: long is 32-bit on Windows */

static void check(int cond, const char *what, uint32_t bits)
{
    g_run++;
    if (!cond) {
        if (g_fail < 20) printf("FAIL: %s (0x%08lX)\n", what, (unsigned long)bits);
        g_fail++;
    }
}

static float from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* CEdev v11.2 __isinff, instruction by instruction:
 *   ld hl,(ix+6) ; low 24 bits      adc hl,hl (carry is 0 after frameset0)
 *   jr nz,.nxt   ; mantissa != 0 -> test h, i.e. original bits 7..14
 *   ld hl,(ix+8) ; add hl,hl        -> h = exponent
 *   .nxt: inc h; result = (h == 0xFF) */
static int cedev_isinff_model(uint32_t u)
{
    uint32_t hl = (u << 1) & 0xFFFFFFUL;
    if (hl == 0UL) hl = ((u >> 16) << 1) & 0xFFFFFFUL;
    return ((hl >> 8) & 0xFFUL) == 0xFFUL;
}

int main(void)
{
    uint32_t u = 0;
    long model_bad_ints = 0;
    long n;

    {
        const uint32_t one = 1;
        unsigned char b;
        memcpy(&b, &one, 1);
        if (sizeof(float) != 4 || b != 1) {
            printf("test_ce_float_class: needs a little-endian host with 32-bit float\n");
            return 1;
        }
    }

    do {
        float f = from_bits(u);
        int nan_ok = (ce_isnan_f(f) != 0) == (isnan(f) != 0);
        int inf_ok = (ce_isinf_f(f) != 0) == (isinf(f) != 0);
        int fin_ok = (ce_isfinite_f(f) != 0) == (isfinite(f) != 0);
        if (!(nan_ok && inf_ok && fin_ok)) {
            check(nan_ok, "ce_isnan_f matches isnan", u);
            check(inf_ok, "ce_isinf_f matches isinf", u);
            check(fin_ok, "ce_isfinite_f matches isfinite", u);
        } else {
            g_run += 3;
        }
        u++;
    } while (u != 0);

    /* The macros as src/ uses them, through the redirect. */
    check(ce_isinf(16777215.0f) == 0, "ce_isinf(16777215) is 0", 0x4B7FFFFFUL);
    check(ce_isinf(INFINITY) != 0, "ce_isinf(inf)", 0x7F800000UL);
    check(ce_isinf(-INFINITY) != 0, "ce_isinf(-inf)", 0xFF800000UL);
    check(ce_isnan(NAN) != 0, "ce_isnan(nan)", 0x7FC00000UL);
    check(ce_isnan(INFINITY) == 0, "ce_isnan(inf) is 0", 0x7F800000UL);
    check(ce_isfinite(3.4028235e38f) != 0, "ce_isfinite(FLT_MAX)", 0x7F7FFFFFUL);
    check(ce_isfinite(INFINITY) == 0, "ce_isfinite(inf) is 0", 0x7F800000UL);

    /* The CEdev bug the helpers avoid: the cases seen on the real ROM. */
    check(cedev_isinff_model(0x4B7FFFFFUL), "model: CEdev isinf(16777215) misfires", 0x4B7FFFFFUL);
    check(cedev_isinff_model(0x497FFFF0UL), "model: CEdev isinf(1048575) misfires", 0x497FFFF0UL);
    check(cedev_isinff_model(0x47807F80UL), "model: CEdev isinf(65791) misfires", 0x47807F80UL);
    check(!cedev_isinff_model(0x47807F00UL), "model: CEdev isinf(65790) is right", 0x47807F00UL);
    check(!cedev_isinff_model(0x49742400UL), "model: CEdev isinf(1e6) is right", 0x49742400UL);
    check(cedev_isinff_model(0x7F800000UL), "model: CEdev isinf(inf) is right", 0x7F800000UL);
    for (n = 0; n <= 16777216L; n++) {
        float f = (float)n;
        uint32_t bits;
        memcpy(&bits, &f, sizeof bits);
        if (cedev_isinff_model(bits)) model_bad_ints++;
        if (ce_isinf_f(f)) check(0, "ce_isinf_f on an integer", bits);
    }
    check(model_bad_ints == 65280L, "model: CEdev isinf misfires on 65280 integers in 0..2^24", 0);

    printf("test_ce_float_class: %" PRIu64 " checks, %d failed (CEdev isinf model: %ld of 0..2^24 misfire)\n",
           g_run, g_fail, model_bad_ints);
    return g_fail == 0 ? 0 : 1;
}
