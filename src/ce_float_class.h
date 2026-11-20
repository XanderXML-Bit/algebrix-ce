/*
 * ce_float_class.h - CE-safe NaN / infinity / finite tests.
 *
 * Use ce_isnan(), ce_isinf() and ce_isfinite() in src/. Do not call the raw
 * <math.h> isnan()/isinf()/isfinite() macros: tests/cemu/
 * test_ce_float_class_guard.py fails the build if they appear.
 *
 * Why: CEdev v11.2's libc _isinff (lib/libc/isinff.src) is wrong for every
 * finite float whose mantissa is non-zero and whose mantissa bits 7..14 are
 * all ones. For those values it tests the wrong byte and returns 1. Examples
 * are 65791, 131071, 1048575, 16777215 and 0.99999237. That is 65,280 of
 * the integers 0..2^24 and about 0.39% of all finite floats. On the real
 * OS 5.8 ROM, entering "16777215" displayed "= inf".
 * _isnanf and _isfinitef read the exponent correctly, but every caller goes
 * through the helpers below so there is one rule to follow.
 *
 * The CE helpers test the IEEE-754 single-precision bit pattern byte by
 * byte. On the eZ80 that needs no float library calls (a float compare is a
 * __fcmp call there), and the result does not depend on how __fcmp orders
 * NaN. CE `double` is the same 32-bit format as `float`, so double
 * arguments pass through unchanged.
 *
 * Desktop builds keep the host <math.h> macros, because desktop code mixes
 * in 64-bit double values that a float test would truncate. Defining
 * CE_FLOAT_CLASS_REDIRECT routes desktop builds through the CE helpers too.
 * That test-only mode converts a desktop double to float first. C99 leaves
 * an out-of-range conversion undefined, so it relies on IEEE (Annex F)
 * hosts, where GCC and Clang give +/-inf.
 * tests/test_ce_float_class.c compares the CE helpers with the host macros
 * over all 2^32 float bit patterns.
 */
#ifndef CE_FLOAT_CLASS_H
#define CE_FLOAT_CLASS_H

#include <math.h>

/* Little-endian byte view of an IEEE-754 binary32 value: b[3] holds the sign
 * and exponent bits 7..1, and bit 7 of b[2] is exponent bit 0. */
typedef union {
    float f;
    unsigned char b[4];
} ce_float_bytes_t;

/* Exponent field is all ones (inf or NaN). */
static inline int ce_float_exp_max_(const ce_float_bytes_t *u)
{
    return (u->b[3] & 0x7Fu) == 0x7Fu && (u->b[2] & 0x80u) != 0u;
}

/* Mantissa field is non-zero. */
static inline int ce_float_mant_nonzero_(const ce_float_bytes_t *u)
{
    return ((u->b[2] & 0x7Fu) | u->b[1] | u->b[0]) != 0u;
}

static inline int ce_isnan_f(float x)
{
    ce_float_bytes_t u;
    u.f = x;
    return ce_float_exp_max_(&u) && ce_float_mant_nonzero_(&u);
}

static inline int ce_isinf_f(float x)
{
    ce_float_bytes_t u;
    u.f = x;
    return ce_float_exp_max_(&u) && !ce_float_mant_nonzero_(&u);
}

static inline int ce_isfinite_f(float x)
{
    ce_float_bytes_t u;
    u.f = x;
    return !ce_float_exp_max_(&u);
}

#if defined(__ez80__) || defined(_EZ80) || defined(CE_FLOAT_CLASS_REDIRECT)
#define ce_isnan(x)    ce_isnan_f((float)(x))
#define ce_isinf(x)    ce_isinf_f((float)(x))
#define ce_isfinite(x) ce_isfinite_f((float)(x))
#else
#define ce_isnan(x)    isnan(x)
#define ce_isinf(x)    isinf(x)
#define ce_isfinite(x) isfinite(x)
#endif

#endif /* CE_FLOAT_CLASS_H */
