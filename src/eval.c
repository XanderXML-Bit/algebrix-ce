/*
 * eval.c — Numerical expression evaluator
 *
 * Recursive tree-walk over an AST.
 * Supports variable bindings, all built-in functions,
 * and numeric evaluation of derivatives via central difference.
 */
#include "eval.h"
#include "steps.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Overflow limits for float32 (FLT_MAX = 3.4028235e38).
 * CE_EVAL_EXP_MAX is the float nearest ln(FLT_MAX) = 88.7228390..., which
 * is just above it, so exp(a) overflows exactly when a >= it.
 * CEdev's coshf/sinhf compute expf(|a|) first (lib/libc/cosh.c.src), so on
 * target they break from the same limit even though cosh(a) itself fits up
 * to 89.416. Hyperbolics therefore refuse |a| >= CE_EVAL_EXP_MAX too.
 * Powers use t = r*ln|l|, which carries rounding (one ulp of logf, plus the
 * product), so they refuse from CE_EVAL_POW_MAX: results within about 1e-4
 * (relative) of FLT_MAX are reported as overflow rather than risk an
 * unreliable CEdev powf. CE_EVAL_EXP_MIN is below ln(smallest denormal),
 * -103.28, so results under it are 0. */
#define CE_EVAL_EXP_MAX 88.72283935546875f
#define CE_EVAL_POW_MAX (CE_EVAL_EXP_MAX - 1e-4f)
#define CE_EVAL_EXP_MIN (-104.0f)
/* CE_EVAL_ENABLE_NUMERIC_INT=0 drops numeric definite integration (the
 * four-grid Simpson below, ~1.1-2 KB) from workers that never answer a
 * definite integral and are at the size limit (CECISM, CEIPFR, CEITRG,
 * CECALC). CECINT routes every int(f,x,a,b) to CECIDF. */
#ifndef CE_EVAL_ENABLE_NUMERIC_INT
#define CE_EVAL_ENABLE_NUMERIC_INT 1
#endif
/* CE_EVAL_ENABLE_NUMERIC_LIM_TAN=0 drops eval's numeric lim() and
 * tangent() (~1.5 KB) from CECIDF, which answers definite integrals only:
 * an integrand containing lim() or tangent() is refused there. */
#ifndef CE_EVAL_ENABLE_NUMERIC_LIM_TAN
#define CE_EVAL_ENABLE_NUMERIC_LIM_TAN 1
#endif

/* CE_EVAL_RECORD_STEPS=0 drops eval's own step text (sum/product terms,
 * Newton/bisection iterations, ...). Workers that call eval only as a
 * numeric helper (the int/lim/solve workers) set it to 0: their steps come
 * from integrate.c/limits.c/algebra.c, and the eval strings would cost
 * them ~1.5-3 KB of the 51,200 B module budget. */
#ifndef CE_EVAL_SUM_COMPENSATED
#ifdef __ez80__
#define CE_EVAL_SUM_COMPENSATED 0
#else
#define CE_EVAL_SUM_COMPENSATED 1
#endif
#endif

#ifndef CE_EVAL_SUM_DIRECT_DISPATCH
#define CE_EVAL_SUM_DIRECT_DISPATCH 0
#endif

#ifndef CE_EVAL_RECORD_STEPS
#define CE_EVAL_RECORD_STEPS 1
#endif
#if !CE_EVAL_RECORD_STEPS
#define steps_addf(...) ((void)0)
#endif

#ifndef CE_EVAL_ENABLE_NODE_STAT
#define CE_EVAL_ENABLE_NODE_STAT 1
#endif

#ifndef CE_EVAL_ENABLE_NODE_LIST
#define CE_EVAL_ENABLE_NODE_LIST 1
#endif

#ifndef CE_EVAL_ENABLE_ADV_POLAR
#define CE_EVAL_ENABLE_ADV_POLAR 1
#endif

#ifndef CE_EVAL_ENABLE_POLYDIV
#define CE_EVAL_ENABLE_POLYDIV CE_EVAL_ENABLE_ADV_POLAR
#endif

#ifndef CE_EVAL_ENABLE_PROB
#define CE_EVAL_ENABLE_PROB CE_EVAL_ENABLE_NODE_STAT
#endif

#ifndef CE_EVAL_ENABLE_FINANCE
#define CE_EVAL_ENABLE_FINANCE CE_EVAL_ENABLE_NODE_STAT
#endif

#ifndef CE_EVAL_ENABLE_BASIC_STATS
#define CE_EVAL_ENABLE_BASIC_STATS CE_EVAL_ENABLE_NODE_STAT
#endif

#if CE_EVAL_ENABLE_BASIC_STATS
#include "stats.h"
#endif

#if CE_EVAL_ENABLE_PROB
#include "prob.h"
#endif

#if CE_EVAL_ENABLE_FINANCE
#include "finance.h"
#endif

#if CE_EVAL_ENABLE_NODE_LIST
#include "list.h"
#endif

#if CE_EVAL_ENABLE_ADV_POLAR
#include "deriv.h"
#include "simplify.h"
#include "integrate.h"
#endif

#if CE_EVAL_ENABLE_POLYDIV
#include "algebra.h"
#endif

/* ── CE-safe float arithmetic (ce_float_ops.h) ─────────────
 *
 * Built when CE_SAFE_FLOAT is 1 (ce_float_ops.h). These live in eval.c
 * because every module that links simplify.c also links eval.c, so no
 * build list changes. See ce_float_ops.h for the measured CE behaviour
 * they work around.
 *
 * They are written for eZ80 code size and speed (CECALC has about 2.6 KB
 * left, and eval runs inside integration and graphing loops): every float
 * operation or compare is a library call there, so the common path reads
 * only the exponent bytes, and the float tests run only near the limits.
 *
 * CE_FOPS_NATIVE(expr) marks a native float operation whose result has
 * been shown to be in range, and CE_FOPS_NATIVE_IF(checked, expr) one that
 * is when `checked` holds. tests/test_ce_float_ops.c redefines them to
 * count calls whose IEEE result is infinite. */

/* Zero and sign tests that see -0 correctly on the CE (TINY builds); the
 * native compares otherwise, as before. FOPS_RECIP_ is 1/x, refused when
 * it falls below FLT_MIN (acsc and acot of |x| > 2^126 return about 1/x). */
#if CE_SAFE_FLOAT_TINY
#define FOPS_ZERO_(x)  (!ce_fops_class(x))
#define FOPS_NEG_(x)   ((fops_sign_(x) & 0x80u) && ce_fops_class(x))
#define FOPS_RECIP_(x) ce_fdiv(1.0f, (x))
/* Arithmetic outside the binop path (arithsum, geomsum, distance, slope,
 * midpoint): native __fmul/__fdiv flush a result below FLT_MIN to +0 on
 * the CE, which would pass as a supported 0. */
#define FT_ADD_(a, b)  ce_fadd((a), (b))
#define FT_SUB_(a, b)  ce_fsub((a), (b))
#define FT_MUL_(a, b)  ce_fmul((a), (b))
#define FT_DIV_(a, b)  ce_fdiv((a), (b))
/* An argument's refusal that reached no error yet (a FUNC root sets only
 * the flag) is reported before the arguments are used. */
#define FT_ARGS_REFUSED_(ctx) eval_tiny_refused((ctx), 0, 0)
#else
#define FOPS_ZERO_(x)  ((x) == 0.0f)
#define FOPS_NEG_(x)   ((x) < 0.0f)
#define FOPS_RECIP_(x) (1.0f / (x))
#define FT_ADD_(a, b)  ((a) + (b))
#define FT_SUB_(a, b)  ((a) - (b))
#define FT_MUL_(a, b)  ((a) * (b))
#define FT_DIV_(a, b)  ((a) / (b))
#define FT_ARGS_REFUSED_(ctx) 0
#endif

#if CE_SAFE_FLOAT
#ifndef CE_FOPS_NATIVE
#define CE_FOPS_NATIVE(expr) (expr)
#endif
#ifndef CE_FOPS_NATIVE_IF
#define CE_FOPS_NATIVE_IF(checked, expr) (expr)
#endif

#define FOPS_SAFE_MAX 3.40282326e38f        /* the float below FLT_MAX */

/* Biased exponent field, 0..255 (255: inf or NaN). The bytes are read
 * through a volatile pointer: otherwise clang merges them into 32-bit
 * shifts, which are library calls on the eZ80. */
static unsigned fops_exp_(ce_float_t x)
{
    volatile unsigned char *p = (volatile unsigned char *)&x;
    return (unsigned char)((p[3] << 1) | (p[2] >> 7));
}

#if !CE_SAFE_FLOAT_TINY
/* Mantissa field, 0..2^23-1, of the float at p. */
static unsigned long fops_mant_(volatile unsigned char *p)
{
    return ((unsigned long)(p[2] & 0x7Fu) << 16) | ((unsigned)p[1] << 8) | p[0];
}
#endif

/* Sign byte: bit 7 is the sign (also for -0). */
static unsigned fops_sign_(ce_float_t x)
{
    volatile unsigned char *p = (volatile unsigned char *)&x;
    return p[3];
}

#if CE_SAFE_FLOAT_TINY
unsigned char g_ce_fops_tiny;

unsigned ce_fops_class(ce_float_t x)
{
    volatile unsigned char *p = (volatile unsigned char *)&x;
    if ((p[3] & 0x7Fu) || (p[2] & 0x80u)) return 2u;   /* exponent field != 0 */
    return (p[2] | p[1] | p[0]) ? 1u : 0u;
}

/* Refuse a tiny operand or result (ce_float_ops.h). */
static ce_float_t fops_tiny_refuse_(void)
{
    g_ce_fops_tiny = 1;
    return 0.0f;
}

/* Just below ln(FLT_MIN) = -87.336545, so FLT_MIN itself (2^-126) is
 * reached. A result between this bound and FLT_MIN is caught after the
 * call by its exponent field (powf and expf below FLT_MIN give a
 * subnormal or 0). With a correctly rounded logf, t's error near 87 is
 * about 1e-5; TI-OS logf is less exact, so on the CE a supported result
 * within about 1e-4 relative of FLT_MIN may be refused (never accepted
 * wrong: an accepted result is checked by its exponent field). */
#define FOPS_LN_MIN (-87.3366f)
#define FOPS_2P64   0x1p64f
#define FOPS_2M64   0x1p-64f
#endif

ce_float_t ce_farith(ce_float_t a, ce_float_t b, int op)
{
    unsigned ea, eb;
    bool finite;
    int s;
    if (op == OP_SUB) {
        b = -b;
        op = OP_ADD;
    }
    ea = fops_exp_(a);
    eb = fops_exp_(b);
    finite = ea != 255u && eb != 255u;
#if CE_SAFE_FLOAT_TINY
    if (finite) {
        unsigned ca = ce_fops_class(a), cb = ce_fops_class(b);
        /* The CE discards (+ -) or flushes (* /) a tiny operand. */
        if (ca == 1u || cb == 1u) return fops_tiny_refuse_();
        /* A +-0 operand, told from the bytes: the CE's own compare treats
         * -0 as a negative nonzero number. A zero result is +0, which is
         * what the CE's native + - * / give (its (-1)*0 is +0), so -0
         * never comes out of here and never prints as "-0". */
        if (op == OP_ADD) {
            if (!ca) return cb ? b : 0.0f;
            if (!cb) return a;
        } else if (!ca || (op == OP_MUL && !cb)) {
            return 0.0f;                       /* 0*x, x*0, 0/x */
        }
        /* x/+-0 falls through to the native division below, as before;
         * eval_node() reports it as a division by zero first. */
    }
#endif
    if (!finite) {
        /* inf or NaN operand: the CE routines treat inf as a number
         * (inf*2 gave 2.35e-38), so apply the IEEE rules here. */
        s = (int)(fops_sign_(a) ^ fops_sign_(b));   /* bit 7: signs differ */
        if (ce_isnan(a) || ce_isnan(b)) return NAN;
        if (op == OP_ADD) {
            if (ea == eb && (s & 0x80)) return NAN;   /* inf - inf */
            return ea == 255u ? a : b;
        }
        if (op == OP_MUL ? (a == 0.0f || b == 0.0f) : ea == eb)
            return NAN;                               /* inf*0, inf/inf */
        if (op == OP_DIV && eb == 255u)               /* x / inf */
            return (s & 0x80) ? -0.0f : 0.0f;
        goto over;                                    /* +-inf */
    } else {
        if (op == OP_ADD) {
            if (ea == 254u || eb == 254u) {
                /* A finite sum overflows only if an operand is >= 2^127.
                 * The half sum (exact scaling) is >= 2^127 exactly when the
                 * full sum overflows, with either rounding (IEEE) or
                 * truncation (CE); with opposite signs it is smaller. */
                if (fops_exp_(CE_FOPS_NATIVE(a * 0.5f + b * 0.5f)) == 254u) goto over;
            } else if ((ea == 151u || eb == 151u) && ea >= 127u && eb >= 127u &&
                       ea <= 151u && eb <= 151u) {
                /* An integer in [2^24, 2^25) plus an integer of at least 1:
                 * add exactly in 32-bit integers. CE __fadd drops the bits
                 * of the smaller operand that it shifts out, so 16777216-1
                 * gave 16777216. Below 2^24 nothing is shifted out. */
                long ia = (long)a, ib = (long)b, sum = ia + ib;
                if ((ce_float_t)ia == a && (ce_float_t)ib == b &&
                    sum >= -16777216L && sum <= 16777216L && sum != 0)
                    return (ce_float_t)sum;
            }
#if CE_SAFE_FLOAT_TINY
            else if ((ea < 24u || eb < 24u) && ea <= 190u && eb <= 190u) {
                /* Both operands are nonzero normals here. A nonzero exact
                 * sum is a multiple of the smaller operand's ulp,
                 * 2^(min(ea,eb)-150), so it reaches FLT_MIN when both
                 * exponents are >= 24 and the native add is safe. Below
                 * that, CE __fadd turns a difference under FLT_MIN into
                 * 0 or a non-IEEE value (2.9843E-38-2.0235E-38 gave
                 * 00513E68). Add the operands scaled by 2^64 instead
                 * (exact, and the sum is >= 2^-85 unless it is 0): +-0 is
                 * an exact cancellation (+0, as IEEE), a result below
                 * 2^-62 means below FLT_MIN unscaled, and anything else
                 * scales back exactly. From 2^64 up the larger operand
                 * swamps the sum, which stays normal. */
                ce_float_t r = CE_FOPS_NATIVE(a * FOPS_2P64 + b * FOPS_2P64);
                if (!ce_fops_class(r)) return 0.0f;
                if (fops_exp_(r) < 65u) return fops_tiny_refuse_();
                r = CE_FOPS_NATIVE(r * FOPS_2M64);
                return ce_fops_class(r) == 2u ? r : fops_tiny_refuse_();
            }
#endif
        } else if (ea && eb) {
            /* |a*b| = ma*mb * 2^(Ea+Eb) with ma*mb in [1, 4), and
             * |a/b| = ma/mb * 2^(Ea-Eb) with ma/mb in (1/2, 2), so s >= 128
             * always overflows and s <= 126 never does. At s = 127 compare
             * against FOPS_SAFE_MAX/|b| (|b| >= 1 there) or FOPS_SAFE_MAX*|b|
             * (|b| < 1); results within about 2 ulp of FLT_MAX count as
             * overflow. A zero or subnormal operand cannot overflow, except
             * a subnormal divisor, handled below. */
            s = (op == OP_MUL) ? (int)(ea + eb) - 254 : (int)ea - (int)eb - 1;
            if (s > 127) goto over;
            if (s == 127) {
                ce_float_t aa = fabsf(a), ab = fabsf(b);
                if (aa > CE_FOPS_NATIVE(op == OP_MUL ? FOPS_SAFE_MAX / ab : FOPS_SAFE_MAX * ab))
                    goto over;
            }
#if CE_SAFE_FLOAT_TINY
            /* The same bounds at the bottom: s <= -128 is always below
             * FLT_MIN (the CE flushes it: 00800000*0.5 gave 0) and
             * s >= -126 never is. At s = -127 the result is in
             * [2^-127, 2^-125): keep it only if the native operation
             * returns a normal with exponent field 1 or 2. */
            if (s < -127) return fops_tiny_refuse_();
            if (s == -127) {
                ce_float_t r = CE_FOPS_NATIVE(op == OP_MUL ? a * b : a / b);
                return fops_exp_(r) - 1u <= 1u ? r : fops_tiny_refuse_();
            }
        }
#else
        } else if (op == OP_DIV && !eb) {
            /* Subnormal divisor b = +-m * 2^-149, 0 < m < 2^23. CE __fadd
             * can produce one (it truncates instead of flushing), and CE
             * __fdiv returns NaN for any x/b (1E-30/b too). Divide by m
             * instead: a/b = (a*2^46 / m) * 2^103. The scalings are exact,
             * a*2^46/m is a normal float, and each step is checked, so this
             * is the IEEE result. A subnormal a = +-ma * 2^-149 would be
             * flushed to 0 by CE __fmul, so then a/b = ma/m, one rounding of
             * two exact integers. m = 0 is +-0: the caller's division by
             * zero. */
            volatile unsigned char *pb = (volatile unsigned char *)&b;
            unsigned long m = fops_mant_(pb);
            if (m) {
                volatile unsigned char *pa = (volatile unsigned char *)&a;
                unsigned long ma = ea ? 0ul : fops_mant_(pa);
                if (ma) {
                    bool neg = ((pa[3] ^ pb[3]) & 0x80u) != 0;
                    a = CE_FOPS_NATIVE((ce_float_t)ma / (ce_float_t)m);
                    return neg ? -a : a;
                }
                a = ce_fmul(ce_fdiv(ce_fmul(a, 70368744177664.0f), (ce_float_t)m),
                            1.01412048e31f);
                return (pb[3] & 0x80u) ? -a : a;
            }
        }
#endif /* CE_SAFE_FLOAT_TINY */
    }
    switch (op) {
    case OP_ADD: return CE_FOPS_NATIVE(a + b);
    case OP_MUL: return CE_FOPS_NATIVE(a * b);
    default:     return CE_FOPS_NATIVE_IF(eb != 0u, a / b);   /* b = +-0: caller's */
    }
over:
    /* A sum's operands agree in sign (or a is the inf); otherwise
     * sign(a) xor sign(b). */
    s = (int)fops_sign_(a);
    if (op != OP_ADD) s ^= (int)fops_sign_(b);
    return (s & 0x80) ? -CE_INF : CE_INF;
}

ce_float_t ce_fpow(ce_float_t base, ce_float_t expo)
{
    ce_float_t ab = fabsf(base), res = CE_INF, t;
    unsigned ee = fops_exp_(expo), eb = fops_exp_(base);
    int n = 0;
    bool integral = true, odd = false, neg;
#if CE_SAFE_FLOAT_TINY
    if (ce_fops_class(base) == 1u || ce_fops_class(expo) == 1u)
        return fops_tiny_refuse_();
#endif
    /* inf/NaN follow C99 Annex F here: TI-OS powf does not handle them. */
    if (FOPS_ZERO_(expo) || base == 1.0f) return 1.0f;    /* also NaN^0, 1^NaN */
    if (ce_isnan(base) || ce_isnan(expo)) return NAN;
    if (ee < 150u) {                       /* |expo| < 2^23: fits an int */
        n = (int)expo;
        integral = ((ce_float_t)n == expo);
        odd = integral && (n & 1);
    } else if (ee == 150u) {               /* integer in [2^23, 2^24) */
        odd = (((volatile unsigned char *)&expo)[0] & 1u) != 0;
    }                                      /* above, and +-inf: even */
    if (FOPS_NEG_(base) && !integral && eb != 255u)
        return NAN;                        /* (-8)^(1/3): no real value */
    neg = (fops_sign_(base) & 0x80u) && odd;   /* odd power of base < 0 or -0 */
    if (ee == 255u) {                      /* expo = +-inf */
        if (ab == 1.0f) return 1.0f;       /* (-1)^inf */
        if ((ab > 1.0f) == !(fops_sign_(expo) & 0x80u)) return CE_INF;
        return 0.0f;
    }
    if (eb == 255u || FOPS_ZERO_(base)) {  /* +-inf or +-0 base */
        if ((eb == 255u) == !(fops_sign_(expo) & 0x80u)) res = CE_INF;
        else res = 0.0f;
    } else {
        bool done = false;
        if (ee < 150u && integral && n >= -32 && n <= 32 && (n > 0 || ab >= 1.0f)) {
            /* Integer exponent up to 32: repeated squaring through ce_farith
             * (inf, not a wrap, on overflow). Exact for an integer base
             * while the result fits in 24 bits (TI-OS powf gave 2^30 =
             * 1073743360 and 3^15 = 14348918) and at most 21 ulp off (mean
             * 2) for random bases, where TI-OS powf was 1..96 ulp off. A
             * negative n divides once at the end, which needs |base| >= 1
             * so 1/p cannot overflow; if p overflows, the result may still
             * be a subnormal, so powf below decides. */
            unsigned k = (unsigned)(n < 0 ? -n : n);
            ce_float_t sq = ab;
            res = 1.0f;
            for (;;) {
                if (k & 1u) res = ce_fmul(res, sq);
                k >>= 1;
                if (!k) break;
                sq = ce_fmul(sq, sq);
            }
            if (n > 0) done = true;
            else if (fops_exp_(res) != 255u) {
#if CE_SAFE_FLOAT_TINY
                res = ce_fdiv(1.0f, res);      /* refuses below FLT_MIN */
#else
                res = CE_FOPS_NATIVE(1.0f / res);
#endif
                done = true;
            }
        }
        if (!done) {
            /* Decide the range from expo*ln|base| (ce_fmul, so a huge
             * exponent gives +-inf), then call powf. Below -104 even a
             * subnormal rounds to 0. Near the top t is only good to its
             * rounding error (about 1e-5 at 88.7), so powf may be called
             * on a result just past FLT_MAX, and a result within about
             * 1e-5 relative below FLT_MAX may count as overflow. TI-OS powf
             * returns NaN or 4.9e-39 past FLT_MAX, which the check against
             * t turns into inf; below FLT_MIN it returns 0 (ROM: 10^(-40),
             * 10^(-44)). This powf call is outside the CE_FOPS_NATIVE
             * audit for that reason. */
#if CE_SAFE_FLOAT_TINY
            /* The operands are normal here (checked above), so a refusal
             * by this product means 0 < |expo*ln|base|| < FLT_MIN: the
             * power is 1 (2^1.2E-38), never tiny, and expo is no odd
             * integer. Only that refusal is dropped (ce_float_ops.h). */
            {
                unsigned char held = g_ce_fops_tiny;
                g_ce_fops_tiny = 0;
                t = ce_fmul(expo, logf(ab));
                if (g_ce_fops_tiny) {
                    g_ce_fops_tiny = held;
                    return 1.0f;
                }
                g_ce_fops_tiny = held;
            }
#else
            t = ce_fmul(expo, logf(ab));
#endif
            res = CE_INF;
#if CE_SAFE_FLOAT_TINY
            /* A nonzero base to a finite power is never 0: a result below
             * FLT_MIN is refused, before powf by t (TI-OS powf flushes
             * it: 10^(-40), 2^(-127)) and after by its exponent field. */
            if (t < FOPS_LN_MIN) return fops_tiny_refuse_();
            if (t <= CE_FOPS_EXP_MAX) {
                ce_float_t p = powf(ab, expo);
                if (!fops_exp_(p)) return fops_tiny_refuse_();
                if (fops_exp_(p) != 255u && !(t > 1.0f && p < 1.0f)) res = p;
            }
#else
            if (t < -104.0f) res = 0.0f;
            else if (t <= CE_FOPS_EXP_MAX) {
                ce_float_t p = powf(ab, expo);
                if (fops_exp_(p) != 255u && !(t > 1.0f && p < 1.0f)) res = p;
            }
#endif
        }
    }
    return neg ? -res : res;
}

ce_float_t ce_fexpfn(ce_float_t x, int fn)
{
    ce_float_t ax = fabsf(x), r = CE_INF;
#if CE_SAFE_FLOAT_TINY
    /* exp(x) < FLT_MIN is refused, but exp(-inf) is exactly 0. */
    if (fn == FN_EXP && x < FOPS_LN_MIN)
        return fops_exp_(x) == 255u ? 0.0f : fops_tiny_refuse_();
#else
    if (fn == FN_EXP && x < -104.0f) return 0.0f;      /* also -inf */
#endif
    /* !(x > max) lets NaN through: it stays NaN on desktop. */
    if (fn == FN_EXP ? !(x > CE_FOPS_EXP_MAX) : !(ax > CE_FOPS_HYP_MAX)) {
        r = fn == FN_EXP ? expf(x) : fn == FN_COSH ? coshf(x) : sinhf(x);
        /* TI-OS expf/sinhf/coshf return NaN, 4.9e-39 or 0 instead of inf on
         * overflow. The bounds avoid that; this catches a miss, since for
         * |x| >= 1 (exp: x >= 1) the true |result| is >= 1. */
#if CE_SAFE_FLOAT_TINY
        /* exp and sinh of a nonzero x are nonzero: a 0 or tiny result
         * is a flush (cosh is >= 1). */
        if (fn != FN_COSH && ce_fops_class(x) == 2u && ce_fops_class(r) != 2u)
            return fops_tiny_refuse_();
#endif
        if (fops_exp_(x) == 255u ||
            (fops_exp_(r) != 255u && !((fn == FN_EXP ? x : ax) >= 1.0f && fabsf(r) < 1.0f)))
            return r;
        r = CE_INF;
    }
    return (fn == FN_SINH && (fops_sign_(x) & 0x80u)) ? -r : r;
}
#endif /* CE_SAFE_FLOAT */

/* Convert angle to radians if degree mode is active */
static ce_float_t to_rad(ce_float_t a)
{
    if (g_settings.angle_mode == ANGLE_DEGREES)
#if CE_SAFE_FLOAT_TINY
        return ce_fmul(a, CE_PI / 180.0f);    /* refuses below FLT_MIN */
#else
        return a * (CE_PI / 180.0f);
#endif
    return a;
}

#if CE_SAFE_FLOAT_TINY
/* cos and sec of an angle that is below FLT_MIN in radians are exactly 1,
 * so for them the conversion's refusal is dropped (degree cos(1E-37));
 * sin, tan, csc and cot keep it. A tiny a never gets here: eval_node
 * refuses a tiny argument first. */
static __attribute__((noinline)) ce_float_t to_rad_even(ce_float_t a)
{
    unsigned char held = g_ce_fops_tiny;
    ce_float_t r;
    g_ce_fops_tiny = 0;
    r = to_rad(a);
    if (g_ce_fops_tiny) r = 0.0f;        /* only this refusal is dropped */
    g_ce_fops_tiny = held;
    return r;
}
#else
#define to_rad_even to_rad
#endif

/* Convert radians to current angle unit (for inverse trig output) */
static ce_float_t from_rad(ce_float_t a)
{
    if (g_settings.angle_mode == ANGLE_DEGREES)
        return a * (180.0f / CE_PI);
    return a;
}

/* ── Helpers ─────────────────────────────────────────────── */

static void set_err(eval_ctx_t *ctx, ce_error_t code, const char *msg)
{
    ctx->error = code;
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
}

/* ── Variable lookup ─────────────────────────────────────── */

#if CE_EVAL_USER_VARS
ce_user_vars_t g_user_vars;

/* Canonical letter name for both local bindings and stored values. */
static char user_var_name(char name)
{
    if (name >= 'a' && name <= 'z') name = (char)(name - 'a' + 'A');
    return name;
}

static int user_var_slot(char name)
{
    name = user_var_name(name);
    if (name < 'A' || name > 'Z') return -1;
    return name - 'A';
}
#define EVAL_VAR_SAME(a, b) (user_var_name(a) == user_var_name(b))
/* Numeric dependency guards must recognize the same letter aliases. */
static bool eval_has_var(const ast_node_t *expr, char name)
{
    name = user_var_name(name);
    if (ast_contains_var(expr, name)) return true;
    if (name >= 'A' && name <= 'Z')
        return ast_contains_var(expr, (char)(name - 'A' + 'a'));
    return false;
}
#else
#define EVAL_VAR_SAME(a, b) ((a) == (b))
#define eval_has_var ast_contains_var
#endif

static ce_float_t lookup_var(eval_ctx_t *ctx, char name)
{
    int i;
#if CE_EVAL_USER_VARS
    name = user_var_name(name);
#endif
    for (i = 0; i < MAX_VARS; i++) {
        if (ctx->vars[i].bound && ctx->vars[i].name == name)
            return ctx->vars[i].value;
    }
#if CE_EVAL_USER_VARS
    /* No local binding: use the value stored by `expr->X` (internal review). */
    i = user_var_slot(name);
    if (i >= 0 && (g_user_vars.mask[i >> 3] & (uint8_t)(1u << (i & 7)))) {
#if CE_SAFE_FLOAT_TINY
        /* A stored tiny value (below FLT_MIN, from CE_STATE) is refused
         * like a tiny result (#140), never used. */
        if (ce_fops_class(g_user_vars.value[i]) == 1u) {
            g_ce_fops_tiny = 1;
            set_err(ctx, CE_ERR_PRECISION, CE_FOPS_TINY_MSG);
            return 0;
        }
#endif
        return g_user_vars.value[i];
    }
#endif
    set_err(ctx, CE_ERR_UNDEFINED, "Undefined var");
    return 0;
}

#if CE_EVAL_ENABLE_ADV_POLAR
static bool var_is_bound(const eval_ctx_t *ctx, char name, ce_float_t *value_out)
{
    int i;
    if (!ctx) return false;
#if CE_EVAL_USER_VARS
    name = user_var_name(name);
#endif
    for (i = 0; i < MAX_VARS; i++) {
        if (ctx->vars[i].bound && ctx->vars[i].name == name) {
            if (value_out) *value_out = ctx->vars[i].value;
            return true;
        }
    }
    return false;
}
#endif

/* ── Factorial (integer, iterative) ──────────────────────── */

static ce_float_t factorial(ce_float_t n)
{
    ce_float_t r = 1;
    int i, k = (int)n;
    if (n < 0 || n != (float)k || k > 12) return 0; /* guard overflow */
    for (i = 2; i <= k; i++) r *= i;
    return r;
}

/* ── Core evaluator ──────────────────────────────────────── */

/*
 * Recursion cost (internal review). eval_expr() is the only frame repeated once
 * per AST level, so it evaluates the operands of the recursive shapes
 * (BINOP, UNOP, FUNC) itself with a small frame and hands them to the
 * noinline eval_node() switch, which runs after the operands return and
 * therefore never stacks per level. On the real OS 5.8 ROM the previous
 * eval_expr -> eval_expr_inner pair cost 131 B per level, so a 32-term sum
 * already ran past the 0xD19881 stack limit in CEBASE.
 */
static __attribute__((noinline))
ce_float_t eval_node(eval_ctx_t *ctx, const ast_node_t *node,
                     ce_float_t l, ce_float_t r);
#if CE_EVAL_ENABLE_NODE_LIST
/* List ops are dispatched by eval_expr() itself (internal review), so a nested
 * list level never holds an eval_node frame as well. */
static __attribute__((noinline))
ce_float_t eval_list_op(eval_ctx_t *ctx, const ast_node_t *node);
#endif

#if CE_EVAL_SUM_COMPENSATED && CE_EVAL_SUM_DIRECT_DISPATCH
static __attribute__((noinline))
ce_float_t eval_sum_compensated(eval_ctx_t *ctx, const ast_node_t *node);
#endif

/*
 * Native stack backstop for eZ80 builds. The OS stack is 4 KB and its
 * stack-limit NMI fires only on a write to the single byte at 0xD19881; a
 * frame that skips that byte silently overwrites RAM below it. eval_expr()
 * refuses to recurse once its frame is within CE_EVAL_STACK_RESERVE bytes
 * of that address, leaving room for the eval_node() and libm frames below
 * the last level (&ctx sits just above eval_expr's own ~18 B of return
 * address, IX and locals, so slightly less than the reserve remains below
 * it). The refusal is the depth cap's (see eval_expr()). A NUMBER leaf
 * needs no frame and is never refused. The address of the incoming
 * argument stands for SP (__builtin_frame_address produces
 * bitcode that CEdev's LTO reader rejects). Desktop builds keep the depth
 * cap only.
 */
#if defined(__ez80__) || defined(_EZ80)
#ifndef CE_EVAL_STACK_LIMIT_ADDR
#define CE_EVAL_STACK_LIMIT_ADDR 0xD19881u
#endif
#ifndef CE_EVAL_STACK_RESERVE
#define CE_EVAL_STACK_RESERVE 832u
#endif
#define EVAL_STACK_LOW(probe) \
    ((uintptr_t)(probe) < (uintptr_t)(CE_EVAL_STACK_LIMIT_ADDR + CE_EVAL_STACK_RESERVE))
#else
#define EVAL_STACK_LOW(probe) ((void)(probe), 0)
#endif

static bool ce_float_to_int_checked(ce_float_t v, int *out)
{
    /* Native int is 24-bit on CE, 32-bit on the host. Use the exclusive
     * upper endpoint: (float)INT_MAX rounds up to 2^31 on the host. */
    const ce_float_t int_min_f = sizeof(int) == 3 ? -8388608.0f : -2147483648.0f;
    ce_float_t rounded;
    if (!out || !ce_isfinite(v)) return false;
    if (v < int_min_f || v >= -int_min_f) return false;
    rounded = roundf(v);
    if (fabsf(v - rounded) > 1e-4f) return false;
    *out = (int)rounded;
    return true;
}

#if CE_SAFE_FLOAT_TINY
/* True (and the refusal reported) when a ce_float_ops.h helper has refused
 * a tiny operand or result, or when l or r is a tiny value. Out of line,
 * and called from eval_node(), not eval_expr(): each recursion level then
 * keeps eval_expr's 9-byte frame. */
static __attribute__((noinline)) bool eval_tiny_refused(eval_ctx_t *ctx,
                                                        ce_float_t l, ce_float_t r)
{
    if (!g_ce_fops_tiny && ce_fops_class(l) != 1u && ce_fops_class(r) != 1u)
        return false;
    g_ce_fops_tiny = 1;
    set_err(ctx, CE_ERR_PRECISION, CE_FOPS_TINY_MSG);
    return true;
}

/* Helpers for arithmetic outside the binop path, out of line so that
 * eval_node's frame does not grow. Each drops only a refusal it made and
 * can prove harmless (ce_float_ops.h). */

/* geomsum's r^n: with r normal, |r^n| below FLT_MIN makes 1 - r^n exactly
 * 1, so that refusal is dropped (0+geomsum(3,0.1,40) = 3.33333); a tiny r
 * keeps it. */
static __attribute__((noinline)) ce_float_t ft_pow_rn_(ce_float_t r, ce_float_t n)
{
    unsigned char held = g_ce_fops_tiny;
    ce_float_t rn;
    g_ce_fops_tiny = 0;
    rn = ce_fpow(r, n);
    if (g_ce_fops_tiny && ce_fops_class(r) == 2u) {
        g_ce_fops_tiny = 0;
        rn = 0.0f;
    }
    g_ce_fops_tiny |= held;
    return rn;
}

/* distance's dx^2 + dy^2. A square below FLT_MIN is dropped when the other
 * is at least 2^-100 (it is then below half an ulp of the sum:
 * distance(0,0,1E-20,1) = 1); two such squares stay refused. */
static __attribute__((noinline)) ce_float_t ft_hyp2_(ce_float_t dx, ce_float_t dy)
{
    unsigned char held = g_ce_fops_tiny, tx, ty;
    ce_float_t sx, sy;
    g_ce_fops_tiny = 0;
    sx = ce_fmul(dx, dx);
    tx = g_ce_fops_tiny;
    g_ce_fops_tiny = 0;
    sy = ce_fmul(dy, dy);
    ty = g_ce_fops_tiny;
    if (tx && !(sy < 0x1p-100f)) { tx = 0; sx = 0.0f; }
    if (ty && !(sx < 0x1p-100f)) { ty = 0; sy = 0.0f; }
    g_ce_fops_tiny = held | tx | ty;
    return ce_fadd(sx, sy);
}

/* d/dx and tangent: (fp - fm) / (2h) of two normals can fall below
 * FLT_MIN, which __fsub and __fdiv turn into 0 on the CE. */
static __attribute__((noinline)) ce_float_t ft_cdiff_(ce_float_t fp, ce_float_t fm, ce_float_t h2)
{
    return ce_fdiv(ce_fsub(fp, fm), h2);
}

static __attribute__((noinline)) ce_float_t ft_richardson_(ce_float_t d2, ce_float_t d1)
{
    return ce_fdiv(ce_fsub(ce_fmul(4.0f, d2), d1), 3.0f);
}

/* lim's extrapolation (10*f(h/10) - f(h))/9 and its two-sided mean: from
 * normal samples either can fall below FLT_MIN, which __fdiv flushes to 0
 * on the CE (0+lim((1E-30*x+2E-37/10)-1.5E-37/10,x,0) printed 0 for 5e-39,
 * review round 9). The guarded operations refuse it instead. */
static __attribute__((noinline)) ce_float_t ft_lim_rich_(ce_float_t v1, ce_float_t v2)
{
    return ce_fdiv(ce_fsub(ce_fmul(10.0f, v1), v2), 9.0f);
}

static __attribute__((noinline)) ce_float_t ft_mean2_(ce_float_t a, ce_float_t b)
{
    return ce_fdiv(ce_fadd(a, b), 2.0f);
}
/*
 * Samples of int, sum, d/dx, lim and tangent (review rounds 4 and 5). A
 * sample is an internal value, not a requested result. Inside a sample
 * (s_in_sample), a BINOP or exp that refuses its own result from normal
 * operands (ce_farith, ce_fpow and ce_fexpfn refuse only a true result
 * below FLT_MIN) does not set the flag: it becomes the pending mark
 * s_tiny_at, at eval depth s_tiny_depth, with |value| < s_tiny_bound *
 * 2^-125, and returns 0. At each later eval_node entry (eval_tiny_entry_):
 *  - the mark's parent passes it up through a product, a quotient by a
 *    nonzero normal o, or a negation, with the bound scaled by |o| or
 *    1/|o| (x*exp(-x^2)); in a sum or difference with a normal o whose
 *    quarter ulp the bound cannot reach, o +- mark rounds to o exactly,
 *    so the mark is discharged and the node computes o +- 0 as usual
 *    (1/(1+exp(-x)); later cancellation follows float rounding, as IEEE
 *    would: 1+0.5^k-1 = 0); otherwise the node takes the value +-o
 *    (s_tiny_val), counting the bound as its error (1E-37+exp(-x^2)), and
 *    passes it no further; anything else refuses;
 *  - a node at the mark's depth or deeper is in a later sibling subtree
 *    and evaluates normally (exp(-x^2)*cos(x));
 *  - a shallower node means the mark's parent did not pass it (a
 *    multi-argument function used the 0): it refuses.
 * A second mark while one is pending refuses. When the mark reaches the
 * sample's root, ft_sample_ takes the sample as +0 (or +-o) and adds its
 * weight in the sampler's result times the bound to s_tiny_mass; a mark
 * anywhere else refuses. The sampler keeps its result only when that
 * mass cannot move it by half an ulp (ft_drops_ok_); otherwise the result
 * could be tiny or wrong, and it is refused. A sample refused any other
 * way stays refused (1+sum(sqrt(1E-20*1E-20)+k,k,1,3)): it need not be
 * tiny. 2*int(exp(-x^2),x,-10,10) = 3.54491 and 1+sum(0.5^n,n,1,200) = 2,
 * as on main; 0+sum(3E-38*k/7,k,1,50) stays refused (its dropped terms
 * reach the sixth digit). Outside samples nothing changes: there is never
 * a pending mark, and these refusals set the flag and the error as before.
 */
static const ast_node_t *s_tiny_at;
static ce_float_t s_tiny_mass, s_tiny_bound, s_tiny_val;
static unsigned char s_in_sample, s_tiny_depth;

/* Report a refusal although the flag may be clear (a pending mark that
 * cannot be resolved). */
static __attribute__((noinline)) bool ft_refuse_(eval_ctx_t *ctx)
{
    g_ce_fops_tiny = 1;
    return eval_tiny_refused(ctx, 0, 0);
}

/* Inside a sample, node's own result was refused below FLT_MIN (the flag
 * is set): make it the pending mark, |value| < 2^-125, unless one is
 * pending already. True when the refusal is reported instead. */
static __attribute__((noinline)) bool ft_mark_(eval_ctx_t *ctx, const ast_node_t *node)
{
    if (!s_in_sample || s_tiny_at) return ft_refuse_(ctx);
    g_ce_fops_tiny = 0;
    s_tiny_at = node;
    s_tiny_depth = ctx->eval_depth;
    s_tiny_bound = 1.0f;
    s_tiny_val = 0.0f;
    return false;
}

/* The BINOP post-op check: its own result was refused. */
static __attribute__((noinline)) bool eval_tiny_own_(eval_ctx_t *ctx,
                                                     const ast_node_t *node)
{
    if (!g_ce_fops_tiny) return false;
    (void)ft_mark_(ctx, node);
    return true;                      /* refused, or 0 as the marked value */
}

/* eval_node's entry check: eval_tiny_refused, plus the pending mark's
 * rules (see above). */
static __attribute__((noinline)) bool eval_tiny_entry_(eval_ctx_t *ctx,
                                                       const ast_node_t *node,
                                                       ce_float_t l, ce_float_t r)
{
    const ast_node_t *m = s_tiny_at;
    bool binop, pass = false;
    if (g_ce_fops_tiny || ce_fops_class(l) == 1u || ce_fops_class(r) == 1u)
        return eval_tiny_refused(ctx, l, r);
    if (!m) return false;
    binop = node->type == NODE_BINOP &&
            (node->d.binop.left == m || node->d.binop.right == m);
    if (!binop && !(node->type == NODE_UNOP && node->d.unop.operand == m) &&
        !(node->type == NODE_FUNC && node->d.func.arg == m)) {
        /* Not the mark's parent: a later sibling subtree evaluates as
         * usual; a shallower node finds the mark orphaned. */
        if (ctx->eval_depth >= s_tiny_depth) return false;
        return ft_refuse_(ctx);
    }
    if (!ce_fops_class(s_tiny_val)) {          /* an absorbed mark stops */
        ce_float_t b = s_tiny_bound, v = 0.0f;
        if (binop) {
            binop_t op = node->d.binop.op;
            bool right = node->d.binop.right == m;
            ce_float_t o = right ? l : r;
            if (fops_exp_(o) == 255u) {
                /* inf or NaN: refused */
            } else if (op == OP_ADD || op == OP_SUB) {
                /* With E the true exponents (the biased fields differ by
                 * the same amount): |mark| < b * 2^-125 <= 2^(E_b - 124).
                 * When that is at most a quarter ulp of a normal o,
                 * 2^(E_o - 25), which E_o >= E_b - 99 ensures, o +- mark
                 * rounds to o exactly (also for a power of two o and an
                 * opposite sign), as IEEE gradual underflow would give.
                 * The mark is then discharged and the node computes o +- 0
                 * as usual (2*int(1/(1+e^(-x)),x,0,100)). Otherwise the
                 * node takes +-o with the bound as its error; o = +-0
                 * leaves a plain mark (v = 0). */
                if (ce_fops_class(o) == 2u &&
                    (int)fops_exp_(o) >= (int)fops_exp_(b) - 99) {
                    s_tiny_at = 0;
                    return false;
                }
                v = (right || op == OP_ADD) ? o : -o;
                pass = true;
            } else if (op == OP_MUL || (op == OP_DIV && !right && ce_fops_class(o) == 2u)) {
                b = op == OP_MUL ? ce_fmul(b, fabsf(o)) : ce_fdiv(b, fabsf(o));
                /* A product or quotient refused below FLT_MIN gives 0,
                 * which FLT_MIN still bounds (as it does a 0*mark); an
                 * overflow to inf bounds nothing and refuses. */
                g_ce_fops_tiny = 0;
                if (ce_fops_class(b) != 2u) b = 0x1p-126f;
                pass = fops_exp_(b) != 255u;
            }
        } else {
            pass = node->type == NODE_UNOP && node->d.unop.op == UOP_NEG;
        }
        if (pass) {
            s_tiny_at = node;
            s_tiny_depth = ctx->eval_depth;
            s_tiny_bound = b;
            s_tiny_val = v;
            return true;                  /* 0 as the marked value */
        }
    }
    return ft_refuse_(ctx);
}

static __attribute__((noinline)) ce_float_t ft_sample_(eval_ctx_t *ctx,
                                                       const ast_node_t *expr,
                                                       ce_float_t w)
{
    const ast_node_t *at = s_tiny_at;
    ce_float_t mass = s_tiny_mass, val = s_tiny_val, bound = s_tiny_bound, v;
    unsigned char held = g_ce_fops_tiny, depth = s_tiny_depth;
    if (ctx->error) return 0;
    s_tiny_at = 0;
    s_tiny_val = 0;
    s_tiny_mass = 0;      /* a sampler inside this sample starts empty */
    g_ce_fops_tiny = 0;
    s_in_sample++;
    v = eval_expr(ctx, expr);
    s_in_sample--;
    if (!ctx->error) {
        if (s_tiny_at == expr && !g_ce_fops_tiny) {
            /* The sample's share, w * bound, kept >= FLT_MIN: a weight the
             * CE flushed to 0, or a product it would, is below FLT_MIN,
             * so FLT_MIN still bounds it. An overflow gives inf, which
             * ft_drops_ok_ refuses. mass >= 0 and m >= FLT_MIN, so the
             * add cannot fall below FLT_MIN. */
            ce_float_t m;
            if (ce_fops_class(w) != 2u) w = 0x1p-126f;
            m = ce_fmul(w, s_tiny_bound);
            if (g_ce_fops_tiny) m = 0x1p-126f;
            g_ce_fops_tiny = 0;
            v = s_tiny_val;              /* 0, or the o that absorbed it */
            mass = ce_fadd(mass, m);
        } else if (s_tiny_at || g_ce_fops_tiny) {
            /* a mark that did not reach the root, or a refusal that set
             * only the flag: reported as before */
            (void)ft_refuse_(ctx);
        }
    }
    g_ce_fops_tiny |= held;
    s_tiny_at = at;
    s_tiny_depth = depth;
    s_tiny_bound = bound;
    s_tiny_val = val;
    s_tiny_mass = mass;
    return v;
}

/* True when the samples dropped since the last call cannot move a result
 * of size `scale` by half an ulp: mass * 2^-125 <= |scale| * 2^-25, which
 * holds when exp(scale) >= exp(mass) - 99 (biased fields). Otherwise the
 * refusal is set. The mass is cleared either way. */
static __attribute__((noinline)) bool ft_drops_ok_(ce_float_t scale)
{
    ce_float_t mass = s_tiny_mass;
    s_tiny_mass = 0;
    if (!ce_fops_class(mass)) return true;
    if (fops_exp_(mass) != 255u && ce_fops_class(scale) == 2u &&
        (int)fops_exp_(scale) >= (int)fops_exp_(mass) - 99) return true;
    g_ce_fops_tiny = 1;
    return false;
}
#define FT_POW_RN_(r, n)      ft_pow_rn_((r), (n))
#define FT_HYP2_(dx, dy)      ft_hyp2_((dx), (dy))
#define FT_CDIFF_(fp, fm, h2) ft_cdiff_((fp), (fm), (h2))
#define FT_RICH_(d2, d1)      ft_richardson_((d2), (d1))
#define FT_LIM_RICH_(v1, v2)  ft_lim_rich_((v1), (v2))
#define FT_MEAN2_(a, b)       ft_mean2_((a), (b))
#define FT_SAMPLE_(ctx, e, w) ft_sample_((ctx), (e), (w))
#define FT_MASS_RESET_()      (s_tiny_mass = 0)
#define FT_DROPS_OK_(scale)   ft_drops_ok_(scale)
#else
#define FT_POW_RN_(r, n)      ce_fpow((r), (n))
#define FT_HYP2_(dx, dy)      ((dx) * (dx) + (dy) * (dy))
#define FT_CDIFF_(fp, fm, h2) (((fp) - (fm)) / (h2))
#define FT_RICH_(d2, d1)      ((4.0f * (d2) - (d1)) / 3.0f)
#define FT_LIM_RICH_(v1, v2)  ((10.0f * (v1) - (v2)) / 9.0f)
#define FT_MEAN2_(a, b)       (((a) + (b)) / 2)
#define FT_SAMPLE_(ctx, e, w) eval_expr((ctx), (e))
#define FT_MASS_RESET_()      ((void)0)
#define FT_DROPS_OK_(scale)   1
#endif

/*
 * Both recursion refusals, the CE_EVAL_MAX_DEPTH cap (every build) and the
 * eZ80 stack backstop, are CE_ERR_DEPTH "Eval recursion limit". It is never
 * a numeric status: integrate.c reads an OVERFLOW at a bound as divergence.
 * Each also sets the sticky g_eval_refused (calcengine.h), so a caller that
 * maps the error to a value (a zero Fourier coefficient, a converged
 * Newton root, a nan volume) cannot report it as an answer.
 */
ce_float_t eval_expr(eval_ctx_t *ctx, const ast_node_t *node)
{
    ce_float_t ret;
    if (!node) { set_err(ctx, CE_ERR_INTERNAL, "NULL node"); return 0; }
    if (ctx->error != CE_OK) return 0;  /* propagate earlier error */
    if (ctx->eval_depth >= CE_EVAL_MAX_DEPTH) goto refuse;
    if (node->type == NODE_NUMBER) return node->d.number;  /* no recursion */
#if defined(__ez80__) || defined(_EZ80)
    if (EVAL_STACK_LOW(&ctx)) goto refuse;
#endif
#if CE_EVAL_SUM_COMPENSATED && CE_EVAL_SUM_DIRECT_DISPATCH
    /* Keep the recursive eval_node frame out of finite-sum evaluation. */
    if (node->type == NODE_SUM) {
        ctx->eval_depth++;
        ret = eval_sum_compensated(ctx, node);
        ctx->eval_depth--;
        return ret;
    }
#endif
#if CE_EVAL_ENABLE_NODE_LIST
    /* A list op evaluates its own elements. Calling eval_list_op() here,
     * not through eval_node(), keeps a nested list level at eval_expr +
     * eval_list_op (internal review). Every check above has run, and
     * ctx->error is clear. Written as an early return because folding it
     * into the operand switch below grew this frame from 9 to 12-15 B. */
    if (node->type == NODE_LIST_OP) {
        ctx->eval_depth++;
        ret = eval_list_op(ctx, node);
        ctx->eval_depth--;
        return ret;
    }
#endif
    ctx->eval_depth++;
    {
        ce_float_t l = 0, r = 0;
        switch (node->type) {
        case NODE_BINOP:
            l = eval_expr(ctx, node->d.binop.left);
            r = eval_expr(ctx, node->d.binop.right);
            break;
        case NODE_UNOP:
            l = eval_expr(ctx, node->d.unop.operand);
            break;
        case NODE_FUNC:
            l = eval_expr(ctx, node->d.func.arg);
            break;
#if CE_EVAL_USER_VARS
        case NODE_STORE:
            /* A variable store's value is evaluated here too, so eval_node's
             * frame (about 100 B) is not held under the whole value: on the
             * CE a store of a deep int( nest reached 147 B closer to the
             * stack limit than the same nest without the store
             * (internal review). */
            if (node->d.store.list_idx >= 128)
                l = eval_expr(ctx, node->d.store.expr);
            break;
#endif
        default:
            break;
        }
        ret = ctx->error ? 0 : eval_node(ctx, node, l, r);
    }
    ctx->eval_depth--;
    return ret;

refuse:
    if (!g_eval_refused) g_eval_refused_ans = g_last_answer;
    g_eval_refused = true;
    set_err(ctx, CE_ERR_DEPTH, "Eval recursion limit");
    return 0;
}

/*
 * Evaluate an expression and require an integer result in native int range.
 * Used for index/count/bound fields where silent truncation is a bug.
 */
static bool eval_expr_as_int(eval_ctx_t *ctx,
                             const ast_node_t *expr,
                             int *out,
                             const char *domain_msg)
{
    ce_float_t v;
    if (!expr || !out) {
        set_err(ctx, CE_ERR_INTERNAL, "NULL integer expression");
        return false;
    }
    v = eval_expr(ctx, expr);
    if (ctx->error) return false;
    if (!ce_float_to_int_checked(v, out)) {
        set_err(ctx, CE_ERR_DOMAIN, domain_msg);
        return false;
    }
    return true;
}

#if CE_EVAL_ENABLE_NUMERIC_INT
/* Numeric definite integral. Out of line, and never inlined: its locals
 * (four Simpson grids, tolerances) would otherwise sit in every recursive
 * eval_node frame and cost stack on deep non-integral expressions
 * (the lim worker's nested-sin margin fell from 600+ to 555 B). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static ce_float_t eval_numeric_integral(eval_ctx_t *ctx, const ast_node_t *node)
{
    if (!node->d.integral.lo || !node->d.integral.hi) {
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Need bounds for numeric int");
        return 0;
    }
    {
        ce_float_t a_val = eval_expr(ctx, node->d.integral.lo);
        ce_float_t b_val = eval_expr(ctx, node->d.integral.hi);
        ce_float_t saved;
        char var = node->d.integral.var;
        int i;
        bool was_bound = false;
        ce_float_t s1, s2, abs_tot = 0;
        /* The integrand runs up to 647 times; roll back any step lines it
         * records (nested int/sum) so only this integral's steps stay. */
        int steps_before = steps_save();

        if (ctx->error) return 0;
        /* Equal finite bounds typed alike: the integral is 0 whatever
         * the integrand (every Simpson weight is 0, so the grids could
         * not agree). Bounds typed differently that round to the same
         * float32 are refused below by the 2^-10 width check
         * (int(int(1e8,t,1,1+1/100000000),x,0,1) printed 0, round 21). */
        if (ce_isfinite(a_val) && a_val == b_val &&
            ast_same_bound(node->d.integral.lo, node->d.integral.hi)) return 0;
        for (i = 0; i < MAX_VARS; i++) {
            if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                was_bound = true; break;
            }
        }
        saved = lookup_var(ctx, var);
        if (ctx->error) { ctx->error = CE_OK; saved = 0; }
        /* Overflow guard. CE soft-float wraps instead of returning inf,
         * so an overflowed sum cannot be detected afterwards; on the
         * desktop an infinite sum of |f| made every tolerance pass
         * (int(4e36*(sin(x^2)-0.2),x,0,1) printed 0, round 18). With
         * |b-a| <= 1e4 and every |f| <= 1e33 (checked per sample), no
         * weighted sum (<= 1604 |f|) or sum*h can overflow. A 400-point
         * grid cannot resolve a non-trivial integrand over a wider
         * interval anyway. */
        if (!(fabsf(b_val - a_val) <= 1e4f)) goto integ_too_large;
        /* Bound representation: each bound is rounded to float32 (half
         * an ulp, 2^-24 of its size), so a width under 2^-10 of the
         * larger bound is off by more than 2^-13 of itself
         * (int(e^(x^2),x,1,1+1/1000000) printed 2.59236e-06 for
         * 2.71828e-06, round 19). The FTC path checks the same. */
        if (fabsf(b_val - a_val) * 1024.0f <
            (fabsf(a_val) > fabsf(b_val) ? fabsf(a_val) : fabsf(b_val))) {
            set_err(ctx, CE_ERR_PRECISION, "bounds too close");
            goto integ_restore;
        }

        /* Simpson's rule at n = 100, 146, 200 and 400. The 400 grid
         * reuses the 200 samples plus the 200 midpoints, which sit
         * h/2 from every n=200 point, endpoints included; 146 shares
         * only the centre point with the 100/200 grids. All four must
         * agree to within 2e-4 of the part of the integral that varies
         * (see tol below), or the integral is refused. A fixed grid is fooled by integrands that vary
         * between its points (int(e^(-x^2),x,-1000,1000) printed 6.2);
         * grids that step through a narrow feature in lockstep can
         * agree on a wrong value, which a baseline hides from any
         * share-of-mass trigger (review rounds 8-12). */
        {
            static const short ns[3] = { 100, 146, 200 };
            ce_float_t s[4];
            ce_float_t abs_int = 0, t200 = 0, var_int = 0, mean = 0, tol;
            int k;

            FT_MASS_RESET_();
            for (k = 0; k < 4; k++) {
                int n = k < 3 ? ns[k] : 200;
                ce_float_t h = (b_val - a_val) / n;
                ce_float_t sum = 0, asum = 0;
                for (i = 0; i <= n; i++) {
                    ce_float_t w, fx, xi;
                    if (k < 3) {
                        w = (i == 0 || i == n) ? 1.0f : (i % 2 == 1) ? 4.0f : 2.0f;
                        xi = a_val + i * h;
                    } else {
                        if (i == n) break;
                        w = 1.0f;          /* midpoints of the n=200 grid */
                        xi = a_val + ((ce_float_t)i + 0.5f) * h;
                    }
                    if (!eval_set_var(ctx, var, xi)) return 0;
                    fx = FT_SAMPLE_(ctx, node->d.integral.expr,
                                    fabsf(b_val - a_val) * 0.0078125f);
                    if (ctx->error) goto integ_restore;
                    if (!(fabsf(fx) <= 1e33f)) goto integ_too_large;
                    sum += w * fx;
                    asum += w * fabsf(fx);
                    if (k == 2)  /* n=200 trapezoid sum */
                        t200 += (i == 0 || i == n) ? 0.5f * fx : fx;
                    if (k == 3)  /* |f - mean| at the midpoints */
                        var_int += fabsf(fx - mean);
                }
                if (k < 3) {
                    s[k] = sum * h / 3.0f;
                    if (k == 2) {
                        abs_int = fabsf(asum * h / 3.0f);
                        t200 *= h;
                        mean = s[2] / (b_val - a_val);
                    }
                } else {
                    /* T400 = T200/2 + (h/2) sum(mid); S400 = (4 T400 - T200)/3 */
                    ce_float_t t400 = 0.5f * t200 + 0.5f * h * sum;
                    s[3] = (4.0f * t400 - t200) / 3.0f;
                    var_int = fabsf(h * var_int);
                }
            }
            s1 = s[2];   /* n = 200 */
            s2 = s[3];   /* n = 400 */
            abs_tot = abs_int;
            /* The grids must agree to 2e-4 of the integral of |f - mean|
             * (midpoint estimate) and of |result|, whichever is smaller,
             * plus a float-noise floor of 2e-6 of the integral of |f|.
             * Scaling by the integral of |f| alone let a constant
             * baseline hide an unresolved peak: int(1+e^(-1800000*
             * (x-0.0011)^2),x,0,1) printed 1.00019 for 1.0013 (review
             * round 13). S100 gets 3x: with an h^1.5 endpoint
             * (sqrt(1-x^2) at 1) |S100-S200| is 2.8x |S200-S400|.
             * A result under 2% of the integral of |f| is dominated by
             * cancellation, where the noise floor can hide an
             * unresolved peak (int(x-0.5+0.03*e^(-3000000*(x-0.0011)^2),
             * x,0,1) printed 9.1e-7 for 3.06e-5, round 14): it is
             * refused unless it is a noise-level 0 on agreeing grids.
             * Refuse when every sample is 0 (a feature missed by all
             * grids: int(e^(-x^2),x,-20,10000) printed 0), when every
             * midpoint equals the mean although the integrand depends on
             * the variable (a constant one integrates symbolically, so
             * flat samples mean a missed feature:
             * int(1+100*e^(-1e10*(x-0.123456)^2),x,0,1) printed 1,
             * round 16), or when the grids disagree. */
            /* Every sample dropped as tiny: the integral may be tiny. */
            if (!(abs_int > 0) && !FT_DROPS_OK_(0.0f)) goto integ_restore;
            tol = 2e-4f * (var_int < fabsf(s[2]) ? var_int : fabsf(s[2]))
                  + 2e-6f * abs_int;
            if (!(abs_int > 0) ||
                (!(var_int > 0) && eval_has_var(node->d.integral.expr, var)) ||
                !(fabsf(s[2] - s[0]) <= 3.0f * tol) ||
                !(fabsf(s[1] - s[2]) <= tol) ||
                !(fabsf(s[3] - s[2]) <= tol) ||
                (fabsf(s[2]) < 0.02f * abs_int &&
                 !(fabsf(s[2]) <= 1e-6f * abs_int &&
                   fabsf(s[3] - s[2]) <= 1e-6f * abs_int))) {
                set_err(ctx, CE_ERR_TOO_COMPLEX, "no convergence");
                goto integ_restore;
            }
        }

        if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
        else eval_clear_var(ctx, var);

        /* Richardson extrapolation for Simpson's: (16*S400 - S200)/15
         * (Simpson's error is O(h^4)). A value within float noise of 0
         * next to the integral of |f| is 0 (int(x*e^(-x^2),x,-1,1)
         * printed -1.2e-9). */
        {
            ce_float_t rv = (16.0f * s2 - s1) / 15.0f;
            /* Only a noise-level S200 gets here when it is under 2% of
             * the integral of |f| (see above); show it as 0. */
            if (fabsf(s1) <= 1e-6f * abs_tot) rv = 0;
            /* Dropped tiny samples: a noise-level 0 is measured against
             * the noise floor, any other value against itself. */
            if (!FT_DROPS_OK_(FOPS_ZERO_(rv) ? abs_tot * 0x1p-20f : rv)) return 0;
            steps_restore(steps_before);
            /* Plain words, no interval brackets: keeps the line short
             * and readable on the 26-column screen. */
            steps_addf("Numeric: Simpson's rule from %.6g to %.6g",
                       (double)a_val, (double)b_val);
            steps_addf("S200 = %.6g, S400 = %.6g", (double)s1, (double)s2);
            steps_addf("Richardson: (16*S400 - S200)/15 = %.6g", (double)rv);
            return rv;
        }

    integ_too_large:
        set_err(ctx, CE_ERR_OVERFLOW, "values or interval too large");
    integ_restore:
        /* Steps from the failed integrand stay: error responses go
         * through proc_failf, which clears show_steps, and the viewer
         * lists steps only when show_steps is set. */
        if (was_bound) eval_set_var(ctx, var, saved);
        else eval_clear_var(ctx, var);
        return 0;
    }
}
#endif /* CE_EVAL_ENABLE_NUMERIC_INT */

#if CE_EVAL_ENABLE_NODE_LIST
/* List operations that return scalars. Out of line, and never inlined
 * (internal review): the 201 B tmp_list would otherwise sit in every eval_node
 * frame. eval_node runs once per nested sum/int/lim/nderiv level and once
 * below the deepest eval_expr, so in CELSCX (the only module with lists) it
 * cost 277 B of locals, about 322 B per nesting level on the OS 5.8 ROM. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static ce_float_t eval_list_op(eval_ctx_t *ctx, const ast_node_t *node)
{
    uint8_t fn = node->d.list_op.fn;
    const list_t *lst = NULL;
    list_t tmp_list;

    /* Resolve argument to a list_t */
    if (node->d.list_op.arg) {
        if (node->d.list_op.arg->type == NODE_LIST_REF) {
            lst = list_get(node->d.list_op.arg->d.list_ref.idx);
        } else if (node->d.list_op.arg->type == NODE_LIST_LITERAL) {
            int cnt = 0;
            const ast_node_t *pair = node->d.list_op.arg->d.list_literal.elems;
            tmp_list.len = 0;
            while (pair && pair->type == NODE_PAIR && cnt < MAX_LIST_SIZE) {
                tmp_list.data[cnt] = eval_expr(ctx, pair->d.pair.head);
                if (ctx->error) return 0;
                cnt++;
                pair = pair->d.pair.tail;
            }
            tmp_list.len = (uint8_t)cnt;
            lst = &tmp_list;
        }
    }

    if (fn == LIST_DIM) {
        return lst ? (ce_float_t)lst->len : 0;
    }
    if (lst && lst->len > 0) {
        switch (fn) {
        case LIST_SUM:    return list_sum(lst);
        case LIST_PROD:   return list_prod(lst);
        case LIST_MIN:    return list_min(lst);
        case LIST_MAX:    return list_max(lst);
        case LIST_MEAN:   return list_mean(lst);
        case LIST_MEDIAN: return list_median(lst);
        case LIST_STDEV:
            if (lst->len < 2) {
                set_err(ctx, CE_ERR_DOMAIN, "Sample stdev needs 2+ values");
                return 0;
            }
            return list_stdev(lst);
        default: break;
        }
    }
    set_err(ctx, CE_ERR_TOO_COMPLEX, "List op (not scalar)");
    return 0;
}
#endif /* CE_EVAL_ENABLE_NODE_LIST */

#if CE_EVAL_ENABLE_POLYDIV
/* polydiv(p, q, var): polynomial long division with steps. Out of line for
 * the same reason as eval_list_op (internal review): its two 120 B text buffers
 * made eval_node 302 B in CEAPDV, the only module that enables it. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static ce_float_t eval_polydiv(eval_ctx_t *ctx, const ast_node_t *node)
{
    char var = node->d.polar.var;
    char rem_buf[120];
    ast_node_t *quot = algebra_poly_div(node->d.polar.expr1,
                                        node->d.polar.expr2,
                                        var, rem_buf, sizeof(rem_buf));
    if (!quot) {
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Cannot perform polynomial division");
        return 0;
    }
    {
        char qbuf[120];
        ce_float_t result;
        ast_to_str(quot, qbuf, sizeof(qbuf));
        steps_addf("Quotient: %s", qbuf);
        steps_addf("Remainder: %s", rem_buf);
        result = eval_expr(ctx, quot);
        ast_free(quot);
        return result;
    }
}
#endif /* CE_EVAL_ENABLE_POLYDIV */

#if CE_EVAL_SUM_COMPENSATED
#ifndef CE_SUM_FADD
#if CE_SAFE_FLOAT
#define CE_SUM_FADD(a, b) ce_fadd((a), (b))
#else
#define CE_SUM_FADD(a, b) ((a) + (b))
#endif
#endif

/* Own the sum locals outside every recursive eval_node frame. */
static __attribute__((noinline)) ce_float_t eval_sum_compensated(
    eval_ctx_t *ctx, const ast_node_t *node)
{
    if (!node->d.sum.lo || !node->d.sum.hi) {
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Need bounds for sum");
        return 0;
    }
    {
        int lo, hi;
        ce_float_t total = 0, saved, comp = 0;
        long integer = 0;
        bool exact = true;
        char var = node->d.sum.var;
        int i, n_terms;
        bool was_bound = false;
        bool show_steps = g_settings.show_steps;

        if (!eval_expr_as_int(ctx, node->d.sum.lo, &lo,
                              "sum bounds must be integers")) return 0;
        if (!eval_expr_as_int(ctx, node->d.sum.hi, &hi,
                              "sum bounds must be integers")) return 0;
        if ((int64_t)hi - (int64_t)lo > 10000) {
            set_err(ctx, CE_ERR_OVERFLOW, "Sum too many terms");
            return 0;
        }
        for (i = 0; i < MAX_VARS; i++) {
            if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                was_bound = true; break;
            }
        }
        saved = lookup_var(ctx, var);
        if (ctx->error) { ctx->error = CE_OK; saved = 0; }

        n_terms = hi < lo ? 0 : hi - lo + 1;
        if (show_steps && n_terms > 0) {
            steps_addf("Sum %c=%d to %d (%d terms)", var, lo, hi, n_terms);
        }

        FT_MASS_RESET_();
        for (i = lo; i <= hi; i++) {
            ce_float_t term;
            if ((i & 0xFF) == 0 && check_cancel()) {
                set_err(ctx, CE_ERR_CANCELLED, "Cancelled");
                if (was_bound) { eval_set_var(ctx, var, saved); }
                else eval_clear_var(ctx, var);
                return 0;
            }
            if ((i & 0xFF) == 0) progress_check();
            if (!eval_set_var(ctx, var, (ce_float_t)i)) return 0;
            term = FT_SAMPLE_(ctx, node->d.sum.expr, 1.0f);
            if (ctx->error) {
                if (was_bound) { eval_set_var(ctx, var, saved); }
                else eval_clear_var(ctx, var);
                return 0;
            }
            if (exact) {
                /* Check the float range before converting to long. */
                if (fabsf(term) < 16777216.0f) {
                    long ti = (long)fabsf(term);
                    if (term < 0) ti = -ti;
                    /* Strict |partial| < 2^31, without signed overflow. */
                    if ((ce_float_t)ti == term &&
                        (ti >= 0 ? integer <= 2147483647L - ti
                                 : integer >= -2147483647L - ti)) {
                        integer += ti;
#if CE_EVAL_RECORD_STEPS && !CE_DISABLE_STEP_RECORDING
                        total = (ce_float_t)integer;
#endif
                        goto sum_term_done;
                    }
                }
                exact = false;
                total = (ce_float_t)integer;
                /* Unsigned magnitude conversion also handles a 2^31 seed.
                 * The unsigned difference converts back to a small signed residual. */
                {
                    unsigned long rounded = (unsigned long)fabsf(total);
                    long residual = integer < 0 ?
                        (long)((unsigned long)integer + rounded) :
                        (long)((unsigned long)integer - rounded);
                    comp = -(ce_float_t)residual;
                }
            }
            {
                ce_float_t y, next;
#if CE_SAFE_FLOAT_TINY
                /* The term's refusal. After this the flag is clear, so
                 * the y and comp drops below clear only their own. */
                if (g_ce_fops_tiny) goto sum_tiny;
#endif
                y = CE_SUM_FADD(term, -comp);
#if CE_SAFE_FLOAT_TINY
                /* term - comp can cancel below FLT_MIN although both are
                 * normal. With |total| >= 2^-102 that y is below half an ulp
                 * of the total, so it is dropped like the correction; a
                 * smaller total keeps the refusal. */
                if (g_ce_fops_tiny && !(fabsf(total) < 0x1p-102f)) {
                    g_ce_fops_tiny = 0;
                    y = 0.0f;
                }
#endif
                next = CE_SUM_FADD(total, y);
#if CE_SAFE_FLOAT
                if (!ce_isfinite(term) || !ce_isfinite(y) || !ce_isfinite(next)) {
                    set_err(ctx, CE_ERR_OVERFLOW, "Sum overflow");
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    return 0;
                }
#endif
#if CE_SAFE_FLOAT_TINY
                /* A term or running total below FLT_MIN is refused here.
                 * The correction is an error estimate, not a value: one
                 * that falls below FLT_MIN (a sum of values near 1e-37) is
                 * dropped for this step, never refused. */
                if (g_ce_fops_tiny) {
                sum_tiny:
                    (void)eval_tiny_refused(ctx, 0, 0);
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    return 0;
                }
                comp = CE_SUM_FADD(CE_SUM_FADD(next, -total), -y);
                if (g_ce_fops_tiny) {
                    g_ce_fops_tiny = 0;
                    comp = 0;
                }
#else
                comp = CE_SUM_FADD(CE_SUM_FADD(next, -total), -y);
#endif
                total = next;
            }
        sum_term_done:

            /* Show individual terms for small sums (≤8 terms) */
            if (show_steps && n_terms <= 8) {
                steps_addf("  %c=%d: term = %.6g, running = %.6g",
                           var, i, (double)term, (double)total);
            }
            /* Do not increment past native INT_MAX on the last term. */
            if (i == hi) break;
        }
        if (exact) {
            /* Split high significand and low remainder, then round ties even. */
            volatile bool negative = integer < 0;
            unsigned long q = negative ? (unsigned long)-integer : (unsigned long)integer;
            unsigned char low = (unsigned char)q;
            unsigned char unit = 1;
            while (q > 16777215L) {
                q >>= 1;
                unit <<= 1;
            }
            if (unit > 1) {
                unsigned char rem = low & (unit - 1);
                if (rem + ((unsigned char)q & 1u) > (unit >> 1)) q++;
            }
            total = (ce_float_t)(negative ? -(long)q : (long)q) * (ce_float_t)unit;
        }
        if (!FT_DROPS_OK_(total)) {    /* terms dropped as tiny */
            if (was_bound) { eval_set_var(ctx, var, saved); }
            else eval_clear_var(ctx, var);
            return 0;
        }
        if (show_steps && n_terms > 8) {
            steps_addf("  (computed %d terms)", n_terms);
        }
        if (show_steps) {
            steps_addf("  Total = %.6g", (double)total);
        }
        if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
        else eval_clear_var(ctx, var);
        return total;
    }
}
#endif

/* l and r are the already evaluated operands of BINOP (l, r), UNOP (l) and
 * FUNC (l); eval_expr() computed them and has checked ctx->error. Every
 * other node type ignores them. */
static ce_float_t eval_node(eval_ctx_t *ctx, const ast_node_t *node,
                            ce_float_t l, ce_float_t r)
{
#if CE_SAFE_FLOAT_TINY
    /* A tiny operand of an operator or function (an Ans saved by an older
     * build, or a value a native routine produced) is refused before any
     * native routine reads it: they disagree on what its bytes mean.
     * Other node types pass l = r = 0. A refusal a helper made below this
     * node (g_ce_fops_tiny, sticky until eval_init) is reported here too;
     * one at the root is reported by the caller (processor.c). Inside a
     * sample a mark can pass upward (eval_tiny_entry_). */
    if (eval_tiny_entry_(ctx, node, l, r)) return 0;
#endif
    switch (node->type) {

    /* ── literals / constants ─────────────────────────── */
    case NODE_NUMBER:
        return node->d.number;

    case NODE_VARIABLE: {
        /* Only a real read records the name: the "save the current
         * binding" lookups in deriv/int/sum/... clear their error. The
         * latest failing read wins, because eval_expr() stops at the first
         * error it keeps: a read whose error was discarded (critical(),
         * Newton probes) is overwritten by the read that really failed. */
        ce_float_t v = lookup_var(ctx, node->d.variable);
        if (ctx->error == CE_ERR_UNDEFINED)
            ctx->undefined_var = node->d.variable;
        return v;
    }

    case NODE_CONSTANT:
        if (node->d.constant == CONST_PI) return CE_PI;
        if (node->d.constant == CONST_E)  return CE_E;
        if (node->d.constant == CONST_I)  {
            set_err(ctx, CE_ERR_DOMAIN, "Complex mode required for i");
            return 0;
        }
        return CE_INF;  /* CONST_INF */

    /* ── binary operators ─────────────────────────────── */
    case NODE_BINOP: {
#if CE_SAFE_FLOAT
        ce_float_t v;
        binop_t op = node->d.binop.op;
        /* FOPS_ZERO_/FOPS_NEG_ test zero from the bytes in TINY builds:
         * the CE's compare treats -0 as negative and nonzero, so 1/-0 and
         * (-0)^-1 are poles too. Otherwise the native compares, as before. */
        if ((op == OP_DIV && FOPS_ZERO_(r)) ||
            (op == OP_POW && FOPS_ZERO_(l) && FOPS_NEG_(r))) {
            set_err(ctx, CE_ERR_DIV_ZERO, "Division by zero");
            return 0;
        }
        /* ce_farith/ce_fpow decide overflow (TINY: and a result below
         * FLT_MIN) from the operands, so the result can be trusted on the
         * CE too, whose float routines wrap and flush (ce_float_ops.h). */
        v = (op == OP_POW) ? ce_fpow(l, r) : ce_farith(l, r, op);
#if CE_SAFE_FLOAT_TINY
        if (eval_tiny_own_(ctx, node)) return 0;
#endif
        if (!ce_isfinite(v) && ce_isfinite(l) && ce_isfinite(r)) {
            set_err(ctx, CE_ERR_OVERFLOW, op == OP_POW ? "Power overflow" : "Overflow");
            return 0;
        }
        return v;
#else
        switch (node->d.binop.op) {
        case OP_ADD: return l + r;
        case OP_SUB: return l - r;
        case OP_MUL: return l * r;
        case OP_DIV:
            if (r == 0) { set_err(ctx, CE_ERR_DIV_ZERO, "Division by zero"); return 0; }
            return l / r;
        case OP_POW: {
            ce_float_t result;
            /* 0^negative is a pole. glibc powf returns inf (caught below),
             * but CEdev powf returns a finite value, which let FTC span
             * the pole on target: int(1/x^2,x,0,1) showed "= -1". */
            if (l == 0 && r < 0) {
                set_err(ctx, CE_ERR_DIV_ZERO, "Division by zero");
                return 0;
            }
            /* Range-check before powf. CEdev powf/expf do not return inf
             * on overflow (real-ROM: e^100 printed "= 0" and 1/e^100
             * "= nan"), so the ce_isfinite() test below never fires on target. */
            if (r == 1.0f) return l;
            /* |l^r| >= 2^126 or <= 2^-126 needs |r|*|log2|l|| >= 126, so
             * with |r| <= 7 only |l| >= 2^18 or |l| <= 2^-18 can get there.
             * Everything else skips logf (pow is hot in graph sampling). */
            if (l != 0 && ce_isfinite(l) && ce_isfinite(r) &&
                (fabsf(r) > 7.0f || fabsf(l) >= 262144.0f || fabsf(l) <= 3.8146973e-6f)) {
                ce_float_t t = r * logf(fabsf(l));
                if (t >= CE_EVAL_POW_MAX) {
                    set_err(ctx, CE_ERR_OVERFLOW, "Power overflow");
                    return 0;
                }
                /* Deep underflow is 0, but a negative base with a
                 * non-integer exponent stays a domain error below. */
                if (t < CE_EVAL_EXP_MIN && (l > 0 || r == floorf(r))) return 0;
            }
            /* Handle negative base with integer exponent specially
             * since powf(-1, n) may use log internally and fail */
            if (r >= 2.0f && r <= 16.0f && r == floorf(r)) {
                /* Small integer powers by repeated squaring: a few float
                 * roundings, where CEdev powf (exp(r ln l)) is off by
                 * ~|r ln l| ulps (int(x^3,x,999,1001) showed 1.99965e9,
                 * not 2.000002e9). The range check above already ran. */
                int e = (int)r;
                ce_float_t b = l;
                result = 1.0f;
                while (e) {
                    if (e & 1) result *= b;
                    e >>= 1;
                    if (e) b *= b;
                }
            } else if (l < 0 && r == floorf(r) && fabsf(r) <= 100000) {
                ce_float_t mag = powf(-l, r);
                result = ((int)r % 2 == 0) ? mag : -mag;
            } else {
                result = (ce_float_t)powf(l, r);
            }
            /* Overflow / underflow detection */
            if (!ce_isfinite(result) && ce_isfinite(l) && ce_isfinite(r)) {
                set_err(ctx, CE_ERR_OVERFLOW, "Power overflow");
                return 0;
            }
            return result;
        }
        }
        break;
#endif
    }

    /* ── unary operators ──────────────────────────────── */
    case NODE_UNOP: {
        ce_float_t v = l;
        if (node->d.unop.op == UOP_NEG)  return -v;
        if (node->d.unop.op == UOP_FACT) {
            if (v < 0 || v != floorf(v)) {
                set_err(ctx, CE_ERR_DOMAIN, "Factorial domain");
                return 0;
            }
            if (v > 12) {
                set_err(ctx, CE_ERR_OVERFLOW, "Factorial overflow");
                return 0;
            }
            return factorial(v);
        }
        break;
    }

    /* ── function calls ───────────────────────────────── */
    case NODE_FUNC: {
        ce_float_t a = l;
        /* sinh/cosh/sech/csch/coth all go through expf(|a|) in CEdev, which
         * does not return inf on overflow; refuse them in one place. */
        if ((node->d.func.id == FN_SINH || node->d.func.id == FN_COSH ||
             (node->d.func.id >= FN_SECH && node->d.func.id <= FN_COTH)) &&
            fabsf(a) >= CE_EVAL_EXP_MAX) {
            int id = node->d.func.id;
            /* Same messages as the per-function checks below. */
            set_err(ctx, CE_ERR_OVERFLOW,
                    id == FN_SINH ? "sinh overflow" : id == FN_COSH ? "cosh overflow" :
                    id == FN_SECH ? "sech overflow" : id == FN_CSCH ? "csch overflow" :
                    "coth overflow");
            return 0;
        }
        switch (node->d.func.id) {
        case FN_SIN:  return sinf(to_rad(a));
        case FN_COS:  return cosf(to_rad_even(a));
        case FN_TAN: {
            ce_float_t c = cosf(to_rad(a));
            if (fabsf(c) < 1e-7f) { set_err(ctx, CE_ERR_DOMAIN, "tan domain"); return 0; }
            return sinf(to_rad(a)) / c;
        }
        case FN_SEC: {
            ce_float_t c = cosf(to_rad_even(a));
            if (fabsf(c) < 1e-7f) { set_err(ctx, CE_ERR_DOMAIN, "sec domain"); return 0; }
            return 1.0f / c;
        }
        case FN_CSC: {
            ce_float_t s = sinf(to_rad(a));
            if (fabsf(s) < 1e-7f) { set_err(ctx, CE_ERR_DOMAIN, "csc domain"); return 0; }
            return 1.0f / s;
        }
        case FN_COT: {
            ce_float_t s = sinf(to_rad(a));
            if (fabsf(s) < 1e-7f) { set_err(ctx, CE_ERR_DOMAIN, "cot domain"); return 0; }
            return cosf(to_rad(a)) / s;
        }
        case FN_ASIN:
            if (a < -1 || a > 1) { set_err(ctx, CE_ERR_DOMAIN, "asin domain"); return 0; }
            return from_rad(asinf(a));
        case FN_ACOS:
            if (a < -1 || a > 1) { set_err(ctx, CE_ERR_DOMAIN, "acos domain"); return 0; }
            return from_rad(acosf(a));
        case FN_ATAN: return from_rad(atanf(a));
        case FN_ASEC:
            if (a > -1 && a < 1) { set_err(ctx, CE_ERR_DOMAIN, "asec domain"); return 0; }
            return from_rad(acosf(1.0f / a));
        case FN_ACSC:
            if (a > -1 && a < 1) { set_err(ctx, CE_ERR_DOMAIN, "acsc domain"); return 0; }
            return from_rad(asinf(FOPS_RECIP_(a)));
        case FN_ACOT:
            return from_rad(atanf(FOPS_RECIP_(a)));
        case FN_LN:
            if (a <= 0) { set_err(ctx, CE_ERR_DOMAIN, "ln domain"); return 0; }
            return logf(a);
        case FN_LOG:
            if (a <= 0) { set_err(ctx, CE_ERR_DOMAIN, "log domain"); return 0; }
            return log10f(a);
        case FN_SQRT:
#if CE_SAFE_FLOAT_TINY
            if (!ce_fops_class(a)) return 0.0f;    /* sqrt(+-0): not a domain error */
#endif
            if (a < 0) { set_err(ctx, CE_ERR_DOMAIN, "sqrt domain"); return 0; }
            return sqrtf(a);
        case FN_ABS:  return fabsf(a);
        case FN_EXP: {
#if CE_SAFE_FLOAT
            ce_float_t r = ce_fexp(a);
#else
            ce_float_t r;
            /* See OP_POW: CEdev expf does not return inf on overflow. */
            if (a >= CE_EVAL_EXP_MAX) { set_err(ctx, CE_ERR_OVERFLOW, "exp overflow"); return 0; }
            if (a < CE_EVAL_EXP_MIN) return 0;
            r = expf(a);
#endif
            if (!ce_isfinite(r)) { set_err(ctx, CE_ERR_OVERFLOW, "exp overflow"); return 0; }
#if CE_SAFE_FLOAT_TINY
            /* its own refusal, in a sample: the pending mark */
            if (g_ce_fops_tiny && s_in_sample && ft_mark_(ctx, node)) return 0;
#endif
            return r;
        }
        case FN_SINH: {
            ce_float_t r = ce_fsinh(a);
            if (!ce_isfinite(r)) { set_err(ctx, CE_ERR_OVERFLOW, "sinh overflow"); return 0; }
            return r;
        }
        case FN_COSH: {
            ce_float_t r = ce_fcosh(a);
            if (!ce_isfinite(r)) { set_err(ctx, CE_ERR_OVERFLOW, "cosh overflow"); return 0; }
            return r;
        }
        case FN_TANH:
            /* tanh(9) already rounds to 1 in float32; avoid CEdev tanhf's
             * large-argument path, which builds on expf. */
            if (fabsf(a) > 9.0f) return a > 0 ? 1.0f : -1.0f;
            return tanhf(a);
        case FN_ASINH: return asinhf(a);
        case FN_ACOSH:
            if (a < 1) { set_err(ctx, CE_ERR_DOMAIN, "acosh domain"); return 0; }
            return acoshf(a);
        case FN_ATANH:
            if (a <= -1 || a >= 1) { set_err(ctx, CE_ERR_DOMAIN, "atanh domain"); return 0; }
            return atanhf(a);
        case FN_SECH: {
            ce_float_t c = ce_fcosh(a);
            if (!ce_isfinite(c)) { set_err(ctx, CE_ERR_OVERFLOW, "sech overflow"); return 0; }
            return FOPS_RECIP_(c);           /* below FLT_MIN near |a| = 88 */
        }
        case FN_CSCH: {
            ce_float_t s = ce_fsinh(a);
            if (!ce_isfinite(s)) { set_err(ctx, CE_ERR_OVERFLOW, "csch overflow"); return 0; }
            if (s == 0) { set_err(ctx, CE_ERR_DOMAIN, "csch domain"); return 0; }
            return FOPS_RECIP_(s);
        }
        case FN_COTH: {
            ce_float_t s = ce_fsinh(a);
            if (!ce_isfinite(s)) { set_err(ctx, CE_ERR_OVERFLOW, "coth overflow"); return 0; }
            if (s == 0) { set_err(ctx, CE_ERR_DOMAIN, "coth domain"); return 0; }
            return ce_fcosh(a) / s;
        }
        case FN_FLOOR: return floorf(a);
        case FN_CEIL:  return ceilf(a);
        case FN_ROUND: return roundf(a);
        case FN_IPART: return truncf(a);
        case FN_FPART: return a - truncf(a);
        case FN_DEC: case FN_HEX: case FN_BIN: case FN_OCT:
            return a;  /* numeric value is the same; formatting is at display layer */
        /* Sign and Heaviside */
        case FN_SIGN:
            return (a > 0) ? 1.0f : (a < 0) ? -1.0f : 0.0f;
        case FN_HEAVISIDE:
            return (a > 0) ? 1.0f : (a < 0) ? 0.0f : 0.5f;
        /* Complex funcs in real mode */
        case FN_REAL: return a;
        case FN_IMAG: return 0;
        case FN_CONJ: return a;
        case FN_ANGLE: return (a >= 0) ? 0 : CE_PI;
        }
        break;
    }

    /* ── numerical derivative via Richardson extrapolation ─ */
    case NODE_DERIV: {
        ce_float_t saved;
        char var = node->d.deriv.var;
        bool was_bound = false;
        int i;
        ce_float_t d1, d2, fp, fm, result;
        ce_float_t h1 = 1e-3f, h2 = 5e-4f;

        for (i = 0; i < MAX_VARS; i++) {
            if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                was_bound = true; break;
            }
        }
        saved = lookup_var(ctx, var);
        if (ctx->error) { ctx->error = CE_OK; saved = 0; }

        /* Central difference at h1. A sample dropped as tiny weighs
         * 1/(3*2*h1) < 256 in the result, one at h2 4/(3*2*h2) < 2048. */
        FT_MASS_RESET_();
        if (!eval_set_var(ctx, var, saved + h1)) return 0;
        fp = FT_SAMPLE_(ctx, node->d.deriv.expr, 256.0f);
        if (!eval_set_var(ctx, var, saved - h1)) return 0;
        fm = FT_SAMPLE_(ctx, node->d.deriv.expr, 256.0f);
        if (ctx->error) goto deriv_restore;
        d1 = FT_CDIFF_(fp, fm, 2 * h1);

        /* Central difference at h2 = h1/2 */
        if (!eval_set_var(ctx, var, saved + h2)) return 0;
        fp = FT_SAMPLE_(ctx, node->d.deriv.expr, 2048.0f);
        if (!eval_set_var(ctx, var, saved - h2)) return 0;
        fm = FT_SAMPLE_(ctx, node->d.deriv.expr, 2048.0f);
        if (ctx->error) goto deriv_restore;
        d2 = FT_CDIFF_(fp, fm, 2 * h2);

        /* Richardson extrapolation: (4*d2 - d1) / 3
         * Eliminates the O(h^2) error term, yielding O(h^4) accuracy */
        result = FT_RICH_(d2, d1);
        if (!FT_DROPS_OK_(result)) goto deriv_restore;

        if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
        else eval_clear_var(ctx, var);
        return result;

    deriv_restore:
        if (was_bound) eval_set_var(ctx, var, saved);
        else eval_clear_var(ctx, var);
        return 0;
    }

    /* ── numerical integral via Simpson's rule (out of line) ── */
    case NODE_INTEGRAL:
#if !CE_EVAL_ENABLE_NUMERIC_INT
        /* Size-limited indefinite workers: a nested definite integral is
         * refused rather than summed without the convergence checks. */
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Numeric int not in this module");
        return 0;
#else
        return eval_numeric_integral(ctx, node);
#endif

    /* ── numerical summation ──────────────────────────── */
    case NODE_SUM: {
#if CE_EVAL_SUM_COMPENSATED
        return eval_sum_compensated(ctx, node);
#else
        if (!node->d.sum.lo || !node->d.sum.hi) {
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Need bounds for sum");
            return 0;
        }
        {
            int lo, hi;
            ce_float_t total = 0, saved;
            char var = node->d.sum.var;
            int i, n_terms;
            bool was_bound = false;
            bool show_steps = g_settings.show_steps;

            if (!eval_expr_as_int(ctx, node->d.sum.lo, &lo,
                                  "sum bounds must be integers")) return 0;
            if (!eval_expr_as_int(ctx, node->d.sum.hi, &hi,
                                  "sum bounds must be integers")) return 0;
            if ((int64_t)hi - (int64_t)lo > 10000) {
                set_err(ctx, CE_ERR_OVERFLOW, "Sum too many terms");
                return 0;
            }
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; break;
                }
            }
            saved = lookup_var(ctx, var);
            if (ctx->error) { ctx->error = CE_OK; saved = 0; }

            n_terms = hi < lo ? 0 : hi - lo + 1;
            if (show_steps && n_terms > 0) {
                steps_addf("Sum %c=%d to %d (%d terms)", var, lo, hi, n_terms);
            }

            for (i = lo; i <= hi; i++) {
                ce_float_t term;
                if ((i & 0xFF) == 0 && check_cancel()) {
                    set_err(ctx, CE_ERR_CANCELLED, "Cancelled");
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    return 0;
                }
                if ((i & 0xFF) == 0) progress_check();
                if (!eval_set_var(ctx, var, (ce_float_t)i)) return 0;
                term = eval_expr(ctx, node->d.sum.expr);
                if (ctx->error) {
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    return 0;
                }
#if CE_SAFE_FLOAT
                {
                    /* ce_f* give inf on overflow (the CE's own ops wrap). */
                    ce_float_t next = ce_fadd(total, term);
                    if (!ce_isfinite(next) && ce_isfinite(total) && ce_isfinite(term)) {
                        set_err(ctx, CE_ERR_OVERFLOW, "Sum overflow");
                        if (was_bound) { eval_set_var(ctx, var, saved); }
                        else eval_clear_var(ctx, var);
                        return 0;
                    }
                    total = next;
                }
#else
                total += term;
#endif
                /* Show individual terms for small sums (≤8 terms) */
                if (show_steps && n_terms <= 8) {
                    steps_addf("  %c=%d: term = %.6g, running = %.6g",
                               var, i, (double)term, (double)total);
                }
                /* Do not increment past native INT_MAX on the last term. */
                if (i == hi) break;
            }
            if (show_steps && n_terms > 8) {
                steps_addf("  (computed %d terms)", n_terms);
            }
            if (show_steps) {
                steps_addf("  Total = %.6g", (double)total);
            }
            if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
            else eval_clear_var(ctx, var);
            return total;
        }
#endif
    }

    /* ── product notation (Π) ────────────────────────────── */
    case NODE_PRODUCT: {
        if (!node->d.product.lo || !node->d.product.hi) {
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Need bounds for prod");
            return 0;
        }
        {
            int lo, hi;
            ce_float_t total = 1.0f, saved;
            char var = node->d.product.var;
            int i, n_terms;
            bool was_bound = false;
            bool show_steps = g_settings.show_steps;

            if (!eval_expr_as_int(ctx, node->d.product.lo, &lo,
                                  "product bounds must be integers")) return 0;
            if (!eval_expr_as_int(ctx, node->d.product.hi, &hi,
                                  "product bounds must be integers")) return 0;
            if ((int64_t)hi - (int64_t)lo > 10000) {
                set_err(ctx, CE_ERR_OVERFLOW, "Product too many terms");
                return 0;
            }
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; break;
                }
            }
            saved = lookup_var(ctx, var);
            if (ctx->error) { ctx->error = CE_OK; saved = 0; }

            n_terms = hi - lo + 1;
            if (show_steps && n_terms > 0) {
                steps_addf("Product %c=%d to %d (%d terms)", var, lo, hi, n_terms);
            }

            for (i = lo; i <= hi; i++) {
                ce_float_t term;
                if ((i & 0xFF) == 0 && check_cancel()) {
                    set_err(ctx, CE_ERR_CANCELLED, "Cancelled");
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    return 0;
                }
                if ((i & 0xFF) == 0) progress_check();
                if (!eval_set_var(ctx, var, (ce_float_t)i)) return 0;
                term = eval_expr(ctx, node->d.product.expr);
                if (ctx->error) {
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    return 0;
                }
#if CE_SAFE_FLOAT
                {
                    /* ce_f* give inf on overflow (the CE's own ops wrap). */
                    ce_float_t next = ce_fmul(total, term);
                    if (!ce_isfinite(next) && ce_isfinite(total) && ce_isfinite(term)) {
                        set_err(ctx, CE_ERR_OVERFLOW, "Product overflow");
                        if (was_bound) { eval_set_var(ctx, var, saved); }
                        else eval_clear_var(ctx, var);
                        return 0;
                    }
                    total = next;
                }
#else
                total *= term;
#endif
                if (show_steps && n_terms <= 8) {
                    steps_addf("  %c=%d: factor = %.6g, running = %.6g",
                               var, i, (double)term, (double)total);
                }
            }
            if (show_steps && n_terms > 8) {
                steps_addf("  (computed %d factors)", n_terms);
            }
            if (show_steps) {
                steps_addf("  Product = %.6g", (double)total);
            }
            if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
            else eval_clear_var(ctx, var);
            return total;
        }
    }

    /* ── limit: multi-point convergence ──────────────────── */
    case NODE_LIMIT: {
#if !CE_EVAL_ENABLE_NUMERIC_LIM_TAN
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Numeric lim/tangent not in this module");
        return 0;
#else
        if (!node->d.limit.point) {
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Need point for lim");
            return 0;
        }
        {
            ce_float_t pt = eval_expr(ctx, node->d.limit.point);
            ce_float_t saved, fl = 0, fr = 0;
            char var = node->d.limit.var;
            int8_t dir = node->d.limit.direction;
            bool was_bound = false;
            int i;
            /* Multi-point approach: evaluate at decreasing h values
             * and use Richardson extrapolation for convergence */
            static const ce_float_t h_vals[] = { 1e-3f, 1e-4f, 1e-5f, 1e-6f };
            #define N_LIM_PTS 4

            if (ctx->error) return 0;
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; break;
                }
            }
            saved = lookup_var(ctx, var);
            if (ctx->error) { ctx->error = CE_OK; saved = 0; }

            FT_MASS_RESET_();
            if (dir <= 0) {
                /* Evaluate at multiple points approaching from left */
                ce_float_t vals[N_LIM_PTS];
                for (i = 0; i < N_LIM_PTS; i++) {
                    if (!eval_set_var(ctx, var, pt - h_vals[i])) goto lim_restore;
                    vals[i] = FT_SAMPLE_(ctx, node->d.limit.expr, 2.0f);
                    if (ctx->error) goto lim_restore;
                }
                /* Use closest point if values are converging,
                 * otherwise Richardson extrapolation of last two */
                if (ce_isfinite(vals[N_LIM_PTS-1]) && ce_isfinite(vals[N_LIM_PTS-2])) {
                    ce_float_t diff = fabsf(vals[N_LIM_PTS-1] - vals[N_LIM_PTS-2]);
                    if (diff < 1e-3f) {
                        /* Richardson: (10*f(h/10) - f(h)) / 9 for linear convergence */
                        fl = FT_LIM_RICH_(vals[N_LIM_PTS-1], vals[N_LIM_PTS-2]);
                    } else {
                        fl = vals[N_LIM_PTS-1]; /* use closest point */
                    }
                } else {
                    fl = vals[N_LIM_PTS-1];
                }
            }
            if (dir >= 0) {
                /* Evaluate at multiple points approaching from right */
                ce_float_t vals[N_LIM_PTS];
                for (i = 0; i < N_LIM_PTS; i++) {
                    if (!eval_set_var(ctx, var, pt + h_vals[i])) goto lim_restore;
                    vals[i] = FT_SAMPLE_(ctx, node->d.limit.expr, 2.0f);
                    if (ctx->error) goto lim_restore;
                }
                if (ce_isfinite(vals[N_LIM_PTS-1]) && ce_isfinite(vals[N_LIM_PTS-2])) {
                    ce_float_t diff = fabsf(vals[N_LIM_PTS-1] - vals[N_LIM_PTS-2]);
                    if (diff < 1e-3f) {
                        fr = FT_LIM_RICH_(vals[N_LIM_PTS-1], vals[N_LIM_PTS-2]);
                    } else {
                        fr = vals[N_LIM_PTS-1];
                    }
                } else {
                    fr = vals[N_LIM_PTS-1];
                }
            }

            if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
            else eval_clear_var(ctx, var);
            if (ctx->error) return 0;
            /* A sample dropped as tiny weighs at most 10/9 in fl or fr.
             * The two-sided mean is checked against |fl| + |fr|, the size
             * its own rounding works at (lim(x^7,x,0) is fl = -fr). */
            if (!FT_DROPS_OK_(fabsf(fl) + fabsf(fr))) return 0;

            if (dir < 0)  return fl;
            if (dir > 0)  return fr;
            return FT_MEAN2_(fl, fr);

        lim_restore:
            if (was_bound) eval_set_var(ctx, var, saved);
            else eval_clear_var(ctx, var);
            return 0;
            #undef N_LIM_PTS
        }
#endif /* CE_EVAL_ENABLE_NUMERIC_LIM_TAN */
    }

    /* ── statistics: collect values from PAIR chain ───── */
    case NODE_STAT: {
#if CE_EVAL_ENABLE_NODE_STAT
        static ce_float_t sbuf[MAX_STAT_DATA];
        int count = 0;
        const ast_node_t *pair = node->d.stat.args;

        /* Walk PAIR chain, evaluate each head */
        while (pair && pair->type == NODE_PAIR && count < MAX_STAT_DATA) {
            sbuf[count] = eval_expr(ctx, pair->d.pair.head);
            if (ctx->error) return 0;
            count++;
            pair = pair->d.pair.tail;
        }
        if (count == 0) {
            set_err(ctx, CE_ERR_DOMAIN, "No data for stat");
            return 0;
        }

        switch (node->d.stat.fn) {
        case STAT_MEAN:
#if CE_EVAL_ENABLE_BASIC_STATS
            return stat_mean(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_MEDIAN:
#if CE_EVAL_ENABLE_BASIC_STATS
            return stat_median(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_MODE:
#if CE_EVAL_ENABLE_BASIC_STATS
            return stat_mode(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_STDEV:
#if CE_EVAL_ENABLE_BASIC_STATS
            /* Sample statistics (n-1 denominator): undefined for one value.
             * stat_stdev() returns 0 for n<=1; refuse here instead, as the
             * TI-84 does (internal review). */
            if (count < 2) {
                set_err(ctx, CE_ERR_DOMAIN, "Sample stdev needs 2+ values");
                return 0;
            }
            return stat_stdev(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_VAR:
#if CE_EVAL_ENABLE_BASIC_STATS
            if (count < 2) {
                set_err(ctx, CE_ERR_DOMAIN, "Sample variance needs 2+ values");
                return 0;
            }
            return stat_variance(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_MIN:
#if CE_EVAL_ENABLE_BASIC_STATS
            return stat_min(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_MAX:
#if CE_EVAL_ENABLE_BASIC_STATS
            return stat_max(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_LINREG:
            /* linreg returns multi-value; handled in main.c */
            set_err(ctx, CE_ERR_TOO_COMPLEX,
                    "Use linreg() at top level");
            return 0;
        case STAT_NCR:
#if CE_EVAL_ENABLE_BASIC_STATS
            if (count < 2) {
                set_err(ctx, CE_ERR_DOMAIN, "nCr needs 2 args");
                return 0;
            }
            {
                int n_i, r_i;
                if (!ce_float_to_int_checked(sbuf[0], &n_i) ||
                    !ce_float_to_int_checked(sbuf[1], &r_i)) {
                    set_err(ctx, CE_ERR_DOMAIN, "nCr args must be integers");
                    return 0;
                }
                if (n_i < 0 || r_i < 0 || r_i > n_i) {
                    set_err(ctx, CE_ERR_DOMAIN, "nCr domain");
                    return 0;
                }
                return stat_ncr((ce_float_t)n_i, (ce_float_t)r_i);
            }
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_NPR:
#if CE_EVAL_ENABLE_BASIC_STATS
            if (count < 2) {
                set_err(ctx, CE_ERR_DOMAIN, "nPr needs 2 args");
                return 0;
            }
            {
                int n_i, r_i;
                if (!ce_float_to_int_checked(sbuf[0], &n_i) ||
                    !ce_float_to_int_checked(sbuf[1], &r_i)) {
                    set_err(ctx, CE_ERR_DOMAIN, "nPr args must be integers");
                    return 0;
                }
                if (n_i < 0 || r_i < 0 || r_i > n_i) {
                    set_err(ctx, CE_ERR_DOMAIN, "nPr domain");
                    return 0;
                }
                return stat_npr((ce_float_t)n_i, (ce_float_t)r_i);
            }
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_GCD:
#if CE_EVAL_ENABLE_BASIC_STATS
            if (count < 2) { set_err(ctx, CE_ERR_DOMAIN, "gcd needs 2 args"); return 0; }
            {
                int a_i, b_i;
                if (!ce_float_to_int_checked(sbuf[0], &a_i) ||
                    !ce_float_to_int_checked(sbuf[1], &b_i)) {
                    set_err(ctx, CE_ERR_DOMAIN, "gcd args must be integers");
                    return 0;
                }
                return stat_gcd((ce_float_t)a_i, (ce_float_t)b_i);
            }
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_LCM:
#if CE_EVAL_ENABLE_BASIC_STATS
            if (count < 2) { set_err(ctx, CE_ERR_DOMAIN, "lcm needs 2 args"); return 0; }
            {
                int a_i, b_i;
                if (!ce_float_to_int_checked(sbuf[0], &a_i) ||
                    !ce_float_to_int_checked(sbuf[1], &b_i)) {
                    set_err(ctx, CE_ERR_DOMAIN, "lcm args must be integers");
                    return 0;
                }
                return stat_lcm((ce_float_t)a_i, (ce_float_t)b_i);
            }
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_MOD:
            if (count < 2) { set_err(ctx, CE_ERR_DOMAIN, "mod needs 2 args"); return 0; }
            if (sbuf[1] == 0) { set_err(ctx, CE_ERR_DIV_ZERO, "mod by zero"); return 0; }
            return fmodf(sbuf[0], sbuf[1]);
        case STAT_RANDINT:
            if (count < 2) { set_err(ctx, CE_ERR_DOMAIN, "randint needs 2 args"); return 0; }
            {
                int lo_i, hi_i;
                long long span_signed, draw;
                unsigned long long span;
                if (!ce_float_to_int_checked(sbuf[0], &lo_i) ||
                    !ce_float_to_int_checked(sbuf[1], &hi_i)) {
                    set_err(ctx, CE_ERR_DOMAIN, "randint bounds must be integers");
                    return 0;
                }
                if (lo_i > hi_i) {
                    set_err(ctx, CE_ERR_DOMAIN, "randint lo > hi");
                    return 0;
                }
                span_signed = (long long)hi_i - (long long)lo_i;
                if (span_signed < 0) {
                    set_err(ctx, CE_ERR_DOMAIN, "randint range overflow");
                    return 0;
                }
                span = (unsigned long long)span_signed + 1ULL;
                if (span == 0ULL) {
                    set_err(ctx, CE_ERR_DOMAIN, "randint range overflow");
                    return 0;
                }
                draw = (long long)lo_i +
                       (long long)((unsigned long long)rand() % span);
                if (draw < -2147483648LL || draw > 2147483647LL) {
                    set_err(ctx, CE_ERR_DOMAIN, "randint result overflow");
                    return 0;
                }
                return (ce_float_t)draw;
            }
        /* Probability distributions — routed to prob module */
#if CE_EVAL_ENABLE_PROB
        case STAT_NORMALCDF:  return prob_normalcdf(sbuf, count, ctx);
        case STAT_INVNORM:    return prob_invnorm(sbuf, count, ctx);
        case STAT_BINOMPDF:   return prob_binompdf(sbuf, count, ctx);
        case STAT_BINOMCDF:   return prob_binomcdf(sbuf, count, ctx);
        case STAT_POISSONPDF: return prob_poissonpdf(sbuf, count, ctx);
        case STAT_POISSONCDF: return prob_poissoncdf(sbuf, count, ctx);
        case STAT_GEOMETPDF:  return prob_geometpdf(sbuf, count, ctx);
        case STAT_GEOMETCDF:  return prob_geometcdf(sbuf, count, ctx);
        case STAT_TCDF:       return prob_tcdf(sbuf, count, ctx);
        case STAT_CHI2CDF:    return prob_chi2cdf(sbuf, count, ctx);
        case STAT_NORMALPDF:  return prob_normalpdf(sbuf, count, ctx);
        case STAT_INVT:       return prob_invt(sbuf, count, ctx);
        case STAT_FPDF:       return prob_fpdf(sbuf, count, ctx);
        case STAT_FCDF:       return prob_fcdf(sbuf, count, ctx);
        case STAT_INVCHI2:    return prob_invchi2(sbuf, count, ctx);
        case STAT_INVF:       return prob_invf(sbuf, count, ctx);
#else
        case STAT_NORMALCDF: case STAT_INVNORM:
        case STAT_BINOMPDF: case STAT_BINOMCDF:
        case STAT_POISSONPDF: case STAT_POISSONCDF:
        case STAT_GEOMETPDF: case STAT_GEOMETCDF:
        case STAT_TCDF: case STAT_CHI2CDF:
        case STAT_NORMALPDF: case STAT_INVT:
        case STAT_FPDF: case STAT_FCDF:
        case STAT_INVCHI2: case STAT_INVF:
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Probability not enabled in this module");
            return 0;
#endif
        /* Additional regressions — multi-value returns handled at top level */
        case STAT_QUADREG: case STAT_EXPREG:
        case STAT_PWRREG: case STAT_LNREG:
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Use regression at top level");
            return 0;
        /* 2-Var Stats */
        case STAT_COVAR:
#if CE_EVAL_ENABLE_BASIC_STATS
            if (count < 4) { set_err(ctx, CE_ERR_DOMAIN, "covar needs paired data"); return 0; }
            return stat_covariance(sbuf, count);
#else
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Basic stats not enabled in this module");
            return 0;
#endif
        case STAT_2VARSTATS:
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Use 2varstats at top level");
            return 0;
        /* Statistical tests — multi-value returns handled at top level */
        case STAT_ZTEST: case STAT_TTEST:
        case STAT_2SAMPZTEST: case STAT_2SAMPTTEST:
        case STAT_1PROPZTEST: case STAT_2PROPZTEST:
        case STAT_CHI2TEST:
        case STAT_ZINTERVAL: case STAT_TINTERVAL:
        case STAT_1PROPZINT: case STAT_2PROPZINT:
        case STAT_2SAMPZINT: case STAT_2SAMPTINT:
        case STAT_LINREGTTEST: case STAT_ANOVA: case STAT_RESID:
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Use stat test at top level");
            return 0;
        /* Finance TVM — routed to finance module */
#if CE_EVAL_ENABLE_FINANCE
        case STAT_FIN_PV:   return finance_pv(sbuf, count, ctx);
        case STAT_FIN_FV:   return finance_fv(sbuf, count, ctx);
        case STAT_FIN_PMT:  return finance_pmt(sbuf, count, ctx);
        case STAT_FIN_NPER: return finance_nper(sbuf, count, ctx);
        case STAT_FIN_RATE: return finance_rate(sbuf, count, ctx);
        case STAT_FIN_NOM:  return finance_nom(sbuf, count, ctx);
        case STAT_FIN_EFF:  return finance_eff(sbuf, count, ctx);
        case STAT_FIN_BAL:    return finance_bal(sbuf, count, ctx);
        case STAT_FIN_SUMPRN: return finance_sum_prn(sbuf, count, ctx);
        case STAT_FIN_SUMINT: return finance_sum_int(sbuf, count, ctx);
#else
        case STAT_FIN_PV: case STAT_FIN_FV:
        case STAT_FIN_PMT: case STAT_FIN_NPER:
        case STAT_FIN_RATE: case STAT_FIN_NOM:
        case STAT_FIN_EFF: case STAT_FIN_BAL:
        case STAT_FIN_SUMPRN: case STAT_FIN_SUMINT:
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Finance not enabled in this module");
            return 0;
#endif
        }
#else
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Statistics not enabled in this module");
        return 0;
#endif
        break;
    }

    /* ── pair nodes should not be evaluated directly ── */
    case NODE_PAIR:
        set_err(ctx, CE_ERR_INTERNAL, "Cannot eval pair");
        break;

    /* ── matrix ops are handled in main.c ────────────── */
    case NODE_MATRIX:
    case NODE_MATRIX_OP:
        set_err(ctx, CE_ERR_TOO_COMPLEX,
                "Matrix (not numeric scalar)");
        break;

    /* ── tangent line: return slope f'(a) via Richardson ── */
    case NODE_TANGENT: {
#if !CE_EVAL_ENABLE_NUMERIC_LIM_TAN
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Numeric lim/tangent not in this module");
        return 0;
#else
        char var = node->d.tangent.var;
        ce_float_t pt, fp, fm, d1, d2, saved, result;
        ce_float_t h1 = 1e-3f, h2 = 5e-4f;
        bool was_bound = false;
        int i;

        if (!node->d.tangent.point) {
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Need point for tangent");
            return 0;
        }
        pt = eval_expr(ctx, node->d.tangent.point);
        if (ctx->error) return 0;

        for (i = 0; i < MAX_VARS; i++) {
            if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                was_bound = true; break;
            }
        }
        saved = lookup_var(ctx, var);
        if (ctx->error) { ctx->error = CE_OK; saved = 0; }

        /* Central difference at h1 (sample weights as in d/dx) */
        FT_MASS_RESET_();
        if (!eval_set_var(ctx, var, pt + h1)) return 0;
        fp = FT_SAMPLE_(ctx, node->d.tangent.expr, 256.0f);
        if (!eval_set_var(ctx, var, pt - h1)) return 0;
        fm = FT_SAMPLE_(ctx, node->d.tangent.expr, 256.0f);
        if (ctx->error) goto tan_restore;
        d1 = FT_CDIFF_(fp, fm, 2 * h1);

        /* Central difference at h2 = h1/2 */
        if (!eval_set_var(ctx, var, pt + h2)) return 0;
        fp = FT_SAMPLE_(ctx, node->d.tangent.expr, 2048.0f);
        if (!eval_set_var(ctx, var, pt - h2)) return 0;
        fm = FT_SAMPLE_(ctx, node->d.tangent.expr, 2048.0f);
        if (ctx->error) goto tan_restore;
        d2 = FT_CDIFF_(fp, fm, 2 * h2);

        /* Richardson extrapolation: O(h^4) accuracy */
        result = FT_RICH_(d2, d1);
        if (!FT_DROPS_OK_(result)) goto tan_restore;

        if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
        else eval_clear_var(ctx, var);
        return result;

    tan_restore:
        if (was_bound) eval_set_var(ctx, var, saved);
        else eval_clear_var(ctx, var);
        return 0;
#endif /* CE_EVAL_ENABLE_NUMERIC_LIM_TAN */
    }

    /* ── symbolic-only node types ─────────────────────── */
    case NODE_TAYLOR:
    case NODE_FOURIER:
    case NODE_EXPAND:
    case NODE_FACTOR:
    case NODE_SOLVE:
        set_err(ctx, CE_ERR_TOO_COMPLEX,
                "Symbolic op (use at top level)");
        break;

    /* ── list element access: L1(i) returns scalar ───── */
    case NODE_LIST_ELEM: {
#if CE_EVAL_ENABLE_NODE_LIST
        int idx = node->d.list_elem.list_idx;
        int elem;
        const list_t *lst;
        if (!eval_expr_as_int(ctx, node->d.list_elem.index, &elem,
                              "List index must be an integer")) return 0;
        lst = list_get(idx);
        if (!lst || elem < 1 || elem > lst->len) {
            set_err(ctx, CE_ERR_DOMAIN, "List index out of bounds");
            return 0;
        }
        return lst->data[elem - 1];
#else
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Lists not enabled in this module");
        return 0;
#endif
    }

    /* ── list operations that return scalars ──────────── */
    case NODE_LIST_OP: {
#if CE_EVAL_ENABLE_NODE_LIST
        return eval_list_op(ctx, node);   /* eval_expr() normally calls it */
#else
        set_err(ctx, CE_ERR_TOO_COMPLEX, "Lists not enabled in this module");
        return 0;
#endif
    }

    /* ── store / list literal / list ref ──────────────── */
    case NODE_STORE: {
        uint8_t idx = node->d.store.list_idx;
        if (idx >= 128) {
            /* Variable store: evaluate expr and bind to variable */
            char vname = (char)(idx - 128);
#if CE_EVAL_USER_VARS
            /* Evaluated by eval_expr() (l), which checked ctx->error; the
             * entry check above has refused a tiny value. */
            ce_float_t val = l;
#else
            ce_float_t val = eval_expr(ctx, node->d.store.expr);
            if (ctx->error) return 0;
#endif
#if CE_SAFE_FLOAT_TINY
            /* -0 is stored as +0 (the CE's compare would print it as
             * "-0"); a tiny value is refused by the caller (processor.c),
             * which then keeps the previous table (module_runtime.c). */
            if (!ce_fops_class(val)) val = 0.0f;
#endif
            if (!eval_set_var(ctx, vname, val)) {
                set_err(ctx, CE_ERR_OVERFLOW, "Too many variables");
                return 0;
            }
#if CE_EVAL_USER_VARS
            {
                /* Keep the value for later entries (internal review). */
                int slot = user_var_slot(vname);
                if (slot >= 0) {
                    g_user_vars.value[slot] = val;
                    g_user_vars.mask[slot >> 3] |= (uint8_t)(1u << (slot & 7));
                }
            }
#endif
            return val;
        }
        /* List store — not a scalar expression */
        set_err(ctx, CE_ERR_TOO_COMPLEX, "List (not numeric scalar)");
        break;
    }
    case NODE_LIST_LITERAL:
    case NODE_LIST_REF:
        set_err(ctx, CE_ERR_TOO_COMPLEX,
                "List (not numeric scalar)");
        break;

    /* ── piecewise function evaluation ────────────────── */
    case NODE_PIECEWISE: {
        const ast_node_t *pair = node->d.piecewise.pieces;
        /* Walk pairs: (expr, cond, expr, cond, ..., default)
         * Pairs come in groups of 2 (expr, cond).
         * If only one remains at the end, it's the default. */
        while (pair && pair->type == NODE_PAIR) {
            ast_node_t *expr_node = pair->d.pair.head;
            const ast_node_t *next = pair->d.pair.tail;
            if (!next || next->type != NODE_PAIR) {
                /* Last element — this is the default value */
                return eval_expr(ctx, expr_node);
            }
            /* next->head is the condition */
            {
                ce_float_t cond_val = eval_expr(ctx, next->d.pair.head);
                if (ctx->error) return 0;
                if (cond_val != 0) {
                    return eval_expr(ctx, expr_node);
                }
            }
            /* Skip to the pair after the condition */
            pair = next->d.pair.tail;
        }
        set_err(ctx, CE_ERR_DOMAIN, "Piecewise: no match");
        return 0;
    }

    /* ── polar/advanced operations (numeric subset) ───── */
    case NODE_POLAR: {
        polar_op_t pop = node->d.polar.op;

        /* arithsum(a1, d, n): S = n/2 * (2*a1 + (n-1)*d) */
        if (pop == POLOP_ARITHSUM) {
            ce_float_t a1 = eval_expr(ctx, node->d.polar.expr1);
            ce_float_t d  = eval_expr(ctx, node->d.polar.expr2);
            ce_float_t n  = eval_expr(ctx, node->d.polar.lo);
            if (ctx->error || FT_ARGS_REFUSED_(ctx)) return 0;
            if (n < 1 || n != floorf(n)) {
                set_err(ctx, CE_ERR_DOMAIN, "arithsum: n must be positive integer");
                return 0;
            }
            steps_addf("Arithmetic series: a1=%.6g, d=%.6g, n=%.6g",
                       (double)a1, (double)d, (double)n);
            steps_addf("S = n/2 * (2*a1 + (n-1)*d)");
            {
                ce_float_t result = FT_MUL_(n / 2.0f,
                                            FT_ADD_(FT_MUL_(2.0f, a1), FT_MUL_(n - 1.0f, d)));
                steps_addf("S = %.6g/2 * (%.6g + %.6g)",
                           (double)n, (double)(2*a1), (double)((n-1)*d));
                steps_addf("S = %.6g", (double)result);
                return result;
            }
        }

        /* geomsum(a1, r, n): S = a1*(1-r^n)/(1-r)  [r!=1] */
        if (pop == POLOP_GEOMSUM) {
            ce_float_t a1 = eval_expr(ctx, node->d.polar.expr1);
            ce_float_t r  = eval_expr(ctx, node->d.polar.expr2);
            ce_float_t n  = eval_expr(ctx, node->d.polar.lo);
            if (ctx->error || FT_ARGS_REFUSED_(ctx)) return 0;
            if (n < 1 || n != floorf(n)) {
                set_err(ctx, CE_ERR_DOMAIN, "geomsum: n must be positive integer");
                return 0;
            }
            steps_addf("Geometric series: a1=%.6g, r=%.6g, n=%.6g",
                       (double)a1, (double)r, (double)n);
            if (fabsf(r - 1.0f) < 1e-10f) {
                ce_float_t result = FT_MUL_(a1, n);
                steps_addf("r=1: S = a1*n = %.6g", (double)result);
                return result;
            }
            {
                ce_float_t rn = FT_POW_RN_(r, n);
                ce_float_t result = FT_DIV_(FT_MUL_(a1, FT_SUB_(1.0f, rn)), FT_SUB_(1.0f, r));
                steps_addf("S = a1*(1-r^n)/(1-r)");
                steps_addf("S = %.6g*(1-%.6g)/(1-%.6g)",
                           (double)a1, (double)rn, (double)r);
                steps_addf("S = %.6g", (double)result);
                return result;
            }
        }

        /* distance(x1,y1,x2,y2): sqrt((x2-x1)^2 + (y2-y1)^2) */
        if (pop == POLOP_DISTANCE) {
            ce_float_t x1 = eval_expr(ctx, node->d.polar.expr1);
            ce_float_t y1 = eval_expr(ctx, node->d.polar.expr2);
            ce_float_t x2 = eval_expr(ctx, node->d.polar.lo);
            ce_float_t y2 = eval_expr(ctx, node->d.polar.hi);
            if (ctx->error || FT_ARGS_REFUSED_(ctx)) return 0;
            {
                ce_float_t dx = FT_SUB_(x2, x1), dy = FT_SUB_(y2, y1);
                ce_float_t result = sqrtf(FT_HYP2_(dx, dy));
                steps_addf("Distance: sqrt((%.6g-%.6g)^2+(%.6g-%.6g)^2)",
                           (double)x2, (double)x1, (double)y2, (double)y1);
                steps_addf("= sqrt(%.6g + %.6g)", (double)(dx*dx), (double)(dy*dy));
                steps_addf("= %.6g", (double)result);
                return result;
            }
        }

        /* slope(x1,y1,x2,y2): (y2-y1)/(x2-x1) */
        if (pop == POLOP_SLOPE) {
            ce_float_t x1 = eval_expr(ctx, node->d.polar.expr1);
            ce_float_t y1 = eval_expr(ctx, node->d.polar.expr2);
            ce_float_t x2 = eval_expr(ctx, node->d.polar.lo);
            ce_float_t y2 = eval_expr(ctx, node->d.polar.hi);
            if (ctx->error || FT_ARGS_REFUSED_(ctx)) return 0;
            {
                ce_float_t dx = FT_SUB_(x2, x1), dy = FT_SUB_(y2, y1);
                if (fabsf(dx) < 1e-10f) {
                    set_err(ctx, CE_ERR_DIV_ZERO, "slope: vertical line (undefined)");
                    return 0;
                }
                steps_addf("Slope: (%.6g-%.6g)/(%.6g-%.6g)",
                           (double)y2, (double)y1, (double)x2, (double)x1);
                {
                    ce_float_t result = FT_DIV_(dy, dx);
                    steps_addf("= %.6g / %.6g = %.6g",
                               (double)dy, (double)dx, (double)result);
                    return result;
                }
            }
        }

        /* midpoint(x1,y1,x2,y2): returns midpoint x-coordinate
         * (full "(mx, my)" display handled by main.c) */
        if (pop == POLOP_MIDPOINT) {
            ce_float_t x1 = eval_expr(ctx, node->d.polar.expr1);
            ce_float_t y1 = eval_expr(ctx, node->d.polar.expr2);
            ce_float_t x2 = eval_expr(ctx, node->d.polar.lo);
            ce_float_t y2 = eval_expr(ctx, node->d.polar.hi);
            if (ctx->error || FT_ARGS_REFUSED_(ctx)) return 0;
            {
                ce_float_t mx = FT_DIV_(FT_ADD_(x1, x2), 2.0f);
                ce_float_t my = (y1 + y2) / 2.0f;       /* step text only */
                steps_addf("Midpoint: ((%.6g+%.6g)/2, (%.6g+%.6g)/2)",
                           (double)x1, (double)x2, (double)y1, (double)y2);
                steps_addf("= (%.6g, %.6g)", (double)mx, (double)my);
                (void)my; /* only read by the step text */
                /* Return x-coordinate; main.c displays full "(mx,my)" */
                return mx;
            }
        }

        /* newton(expr, var, x0) — Newton's method with step display */
#if CE_EVAL_ENABLE_ADV_POLAR
        if (pop == POLOP_NEWTON) {
            char var = node->d.polar.var;
            ce_float_t x0_val = eval_expr(ctx, node->d.polar.expr2);
            ce_float_t saved;
            bool was_bound = false;
            int iter, i;
            ce_float_t xn, fx, fpx, h = 1e-5f;
            const int max_iter = 20;
            const ce_float_t tol = 1e-7f;

            if (ctx->error) return 0;
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; break;
                }
            }
            saved = lookup_var(ctx, var);
            if (ctx->error) { ctx->error = CE_OK; saved = 0; }

            xn = x0_val;
            steps_addf("Newton's method: x0 = %.6g", (double)xn);
            steps_addf("x_{n+1} = x_n - f(x_n)/f'(x_n)");

            for (iter = 0; iter < max_iter; iter++) {
                ce_float_t fp_val, fm_val;
                /* f(xn) */
                if (!eval_set_var(ctx, var, xn)) goto newton_restore;
                fx = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto newton_restore;

                /* f'(xn) via central difference */
                if (!eval_set_var(ctx, var, xn + h)) goto newton_restore;
                fp_val = eval_expr(ctx, node->d.polar.expr1);
                if (!eval_set_var(ctx, var, xn - h)) goto newton_restore;
                fm_val = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto newton_restore;
                fpx = (fp_val - fm_val) / (2.0f * h);

                if (fabsf(fpx) < 1e-12f) {
                    steps_addf("  Iter %d: f'(%.6g) ~ 0, stuck", iter + 1, (double)xn);
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    set_err(ctx, CE_ERR_DOMAIN, "Newton: derivative zero");
                    return 0;
                }

                {
                    ce_float_t xnew = xn - fx / fpx;
                    steps_addf("  Iter %d: x=%.6g, f(x)=%.6g, f'(x)=%.6g -> %.6g",
                               iter + 1, (double)xn, (double)fx, (double)fpx, (double)xnew);
                    if (fabsf(xnew - xn) < tol) {
                        steps_addf("Converged: root = %.6g", (double)xnew);
                        if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
                        else eval_clear_var(ctx, var);
                        return xnew;
                    }
                    xn = xnew;
                }
            }
            steps_addf("Did not converge in %d iterations", max_iter);
            if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
            else eval_clear_var(ctx, var);
            return xn;

        newton_restore:
            if (was_bound) eval_set_var(ctx, var, saved);
            else eval_clear_var(ctx, var);
            return 0;
        }

        /* bisect(expr, var, a, b) — Bisection method with step display */
        if (pop == POLOP_BISECT) {
            char var = node->d.polar.var;
            ce_float_t a_val = eval_expr(ctx, node->d.polar.lo);
            ce_float_t b_val = eval_expr(ctx, node->d.polar.hi);
            ce_float_t saved, fa, fb, mid = 0, fmid = 0;
            bool was_bound = false;
            int iter, i;
            const int max_iter = 50;
            const ce_float_t tol = 1e-7f;

            if (ctx->error) return 0;
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; break;
                }
            }
            saved = lookup_var(ctx, var);
            if (ctx->error) { ctx->error = CE_OK; saved = 0; }

            /* Evaluate at endpoints */
            if (!eval_set_var(ctx, var, a_val)) goto bisect_restore;
            fa = eval_expr(ctx, node->d.polar.expr1);
            if (!eval_set_var(ctx, var, b_val)) goto bisect_restore;
            fb = eval_expr(ctx, node->d.polar.expr1);
            if (ctx->error) goto bisect_restore;

            if (fa * fb > 0) {
                if (was_bound) eval_set_var(ctx, var, saved);
                else eval_clear_var(ctx, var);
                set_err(ctx, CE_ERR_DOMAIN, "bisect: f(a) and f(b) must have opposite signs");
                return 0;
            }

            steps_addf("Bisection: [%.6g, %.6g]", (double)a_val, (double)b_val);
            steps_addf("f(a)=%.6g, f(b)=%.6g", (double)fa, (double)fb);

            for (iter = 0; iter < max_iter; iter++) {
                mid = (a_val + b_val) / 2.0f;
                if (!eval_set_var(ctx, var, mid)) goto bisect_restore;
                fmid = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto bisect_restore;

                if (iter < 8) {
                    steps_addf("  Iter %d: mid=%.6g, f(mid)=%.6g",
                               iter + 1, (double)mid, (double)fmid);
                }

                if (fabsf(fmid) < tol || (b_val - a_val) / 2.0f < tol) {
                    steps_addf("Converged: root = %.6g", (double)mid);
                    if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
                    else eval_clear_var(ctx, var);
                    return mid;
                }

                if (fa * fmid < 0) {
                    b_val = mid; fb = fmid;
                } else {
                    a_val = mid; fa = fmid;
                }
            }
            steps_addf("Converged after %d iterations: %.6g", max_iter, (double)mid);
            if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
            else eval_clear_var(ctx, var);
            return mid;

        bisect_restore:
            if (was_bound) eval_set_var(ctx, var, saved);
            else eval_clear_var(ctx, var);
            return 0;
        }

        /* secant(expr, var, x0, x1) — Secant method with step display */
        if (pop == POLOP_SECANT) {
            char var = node->d.polar.var;
            ce_float_t x0_val = eval_expr(ctx, node->d.polar.lo);
            ce_float_t x1_val = eval_expr(ctx, node->d.polar.hi);
            ce_float_t saved, f0, f1;
            bool was_bound = false;
            int iter, i;
            const int max_iter = 30;
            const ce_float_t tol = 1e-7f;

            if (ctx->error) return 0;
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; break;
                }
            }
            saved = lookup_var(ctx, var);
            if (ctx->error) { ctx->error = CE_OK; saved = 0; }

            /* Evaluate at initial points */
            if (!eval_set_var(ctx, var, x0_val)) goto secant_restore;
            f0 = eval_expr(ctx, node->d.polar.expr1);
            if (!eval_set_var(ctx, var, x1_val)) goto secant_restore;
            f1 = eval_expr(ctx, node->d.polar.expr1);
            if (ctx->error) goto secant_restore;

            steps_addf("Secant method: x0=%.6g, x1=%.6g", (double)x0_val, (double)x1_val);
            steps_addf("x_{n+1} = x_n - f(x_n)*(x_n-x_{n-1})/(f(x_n)-f(x_{n-1}))");

            for (iter = 0; iter < max_iter; iter++) {
                ce_float_t denom = f1 - f0;
                ce_float_t xnew;
                if (fabsf(denom) < 1e-12f) {
                    steps_addf("  Iter %d: f(x0)~f(x1), stuck", iter + 1);
                    if (was_bound) { eval_set_var(ctx, var, saved); }
                    else eval_clear_var(ctx, var);
                    set_err(ctx, CE_ERR_DOMAIN, "secant: f values too close");
                    return 0;
                }
                xnew = x1_val - f1 * (x1_val - x0_val) / denom;

                steps_addf("  Iter %d: x=%.6g, f=%.6g -> %.6g",
                           iter + 1, (double)x1_val, (double)f1, (double)xnew);

                if (fabsf(xnew - x1_val) < tol) {
                    steps_addf("Converged: root = %.6g", (double)xnew);
                    if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
                    else eval_clear_var(ctx, var);
                    return xnew;
                }

                x0_val = x1_val; f0 = f1;
                x1_val = xnew;
                if (!eval_set_var(ctx, var, x1_val)) goto secant_restore;
                f1 = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto secant_restore;
            }
            steps_addf("Result after %d iterations: %.6g", max_iter, (double)x1_val);
            if (was_bound) { if (!eval_set_var(ctx, var, saved)) return 0; }
            else eval_clear_var(ctx, var);
            return x1_val;

        secant_restore:
            if (was_bound) eval_set_var(ctx, var, saved);
            else eval_clear_var(ctx, var);
            return 0;
        }

        /* pdiff(expr, var1 [, var2]) — partial/mixed partial derivatives */
        if (pop == POLOP_PDIFF) {
            ast_node_t *df;
            char var1 = node->d.polar.var;
            /* First partial derivative with respect to var1 */
            df = deriv_diff_quiet(node->d.polar.expr1, var1);
            if (!df) {
                set_err(ctx, CE_ERR_TOO_COMPLEX, "Cannot compute partial derivative");
                return 0;
            }
            steps_addf("Partial derivative w.r.t. %c", var1);
            /* If var2 is given, take second partial w.r.t. var2 */
            if (node->d.polar.expr2 && node->d.polar.expr2->type == NODE_VARIABLE) {
                char var2 = node->d.polar.expr2->d.variable;
                ast_node_t *d2f = deriv_diff_quiet(df, var2);
                ast_free(df);
                if (!d2f) {
                    set_err(ctx, CE_ERR_TOO_COMPLEX, "Cannot compute mixed partial");
                    return 0;
                }
                steps_addf("Then partial w.r.t. %c", var2);
                df = d2f;
            }
            {
                char buf[120];
                ce_float_t result;
                ast_to_str(df, buf, sizeof(buf));
                steps_addf("Result: %s", buf);
                result = eval_expr(ctx, df);
                ast_free(df);
                return result;
            }
        }

        /* gradient(expr, var1, var2) — gradient vector (df/dv1, df/dv2) */
        if (pop == POLOP_GRADIENT) {
            char var1 = node->d.polar.var;
            char var2 = (node->d.polar.expr2 && node->d.polar.expr2->type == NODE_VARIABLE)
                        ? node->d.polar.expr2->d.variable : 'y';
            ast_node_t *df1 = deriv_diff_quiet(node->d.polar.expr1, var1);
            ast_node_t *df2 = deriv_diff_quiet(node->d.polar.expr1, var2);
            if (!df1 || !df2) {
                if (df1) ast_free(df1);
                if (df2) ast_free(df2);
                set_err(ctx, CE_ERR_TOO_COMPLEX, "Cannot compute gradient");
                return 0;
            }
            {
                char b1[80], b2[80];
                ce_float_t v1, v2;
                ast_to_str(df1, b1, sizeof(b1));
                ast_to_str(df2, b2, sizeof(b2));
                steps_addf("Gradient: (d/d%c, d/d%c)", var1, var2);
                steps_addf("d/d%c = %s", var1, b1);
                steps_addf("d/d%c = %s", var2, b2);
                v1 = eval_expr(ctx, df1);
                v2 = eval_expr(ctx, df2);
                ast_free(df1);
                ast_free(df2);
                if (ctx->error) return 0;
                steps_addf("= (%.6g, %.6g)", (double)v1, (double)v2);
                /* Return magnitude of gradient */
                return sqrtf(v1 * v1 + v2 * v2);
            }
        }

        /* dblint(expr, x, xlo, xhi, y, ylo, yhi) — numerical double integral */
        if (pop == POLOP_DBLINT) {
            char xvar = node->d.polar.var;
            char yvar = (node->d.polar.expr2 && node->d.polar.expr2->type == NODE_VARIABLE)
                        ? node->d.polar.expr2->d.variable : 'y';
            ce_float_t xlo, xhi, ylo, yhi;
            ce_float_t total = 0;
            int nx = 20, ny = 20, ix, iy;
            ce_float_t saved_x = 0, saved_y = 0;
            bool x_was_bound = false, y_was_bound = false;
            int i;

            /* Extract bounds from PAIR nodes */
            if (!node->d.polar.lo || !node->d.polar.hi ||
                node->d.polar.lo->type != NODE_PAIR ||
                node->d.polar.hi->type != NODE_PAIR) {
                set_err(ctx, CE_ERR_SYNTAX, "dblint: invalid bounds");
                return 0;
            }
            xlo = eval_expr(ctx, node->d.polar.lo->d.pair.head);
            xhi = eval_expr(ctx, node->d.polar.lo->d.pair.tail);
            ylo = eval_expr(ctx, node->d.polar.hi->d.pair.head);
            yhi = eval_expr(ctx, node->d.polar.hi->d.pair.tail);
            if (ctx->error) return 0;

            /* Save existing variable bindings */
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, xvar)) {
                    x_was_bound = true; saved_x = ctx->vars[i].value; break;
                }
            }
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, yvar)) {
                    y_was_bound = true; saved_y = ctx->vars[i].value; break;
                }
            }

            steps_addf("Double integral over [%.4g,%.4g]x[%.4g,%.4g]",
                       (double)xlo, (double)xhi, (double)ylo, (double)yhi);
            steps_addf("Using %dx%d Simpson's rule", nx, ny);

            /* 2D Simpson's rule */
            {
                ce_float_t hx = (xhi - xlo) / (ce_float_t)nx;
                ce_float_t hy = (yhi - ylo) / (ce_float_t)ny;
                for (ix = 0; ix <= nx; ix++) {
                    ce_float_t xi = xlo + ix * hx;
                    ce_float_t wx = (ix == 0 || ix == nx) ? 1.0f :
                                    (ix % 2 == 1) ? 4.0f : 2.0f;
                    if (!eval_set_var(ctx, xvar, xi)) goto dblint_restore;
                    for (iy = 0; iy <= ny; iy++) {
                        ce_float_t yi = ylo + iy * hy;
                        ce_float_t wy = (iy == 0 || iy == ny) ? 1.0f :
                                        (iy % 2 == 1) ? 4.0f : 2.0f;
                        ce_float_t fval;
                        if (!eval_set_var(ctx, yvar, yi)) goto dblint_restore;
                        fval = eval_expr(ctx, node->d.polar.expr1);
                        if (ctx->error) goto dblint_restore;
                        total += wx * wy * fval;
                    }
                }
                total *= hx * hy / 9.0f;
            }

            steps_addf("Result = %.6g", (double)total);

            /* Restore variables */
            if (x_was_bound) { if (!eval_set_var(ctx, xvar, saved_x)) return 0; }
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) { if (!eval_set_var(ctx, yvar, saved_y)) return 0; }
            else eval_clear_var(ctx, yvar);
            return total;

        dblint_restore:
            if (x_was_bound) eval_set_var(ctx, xvar, saved_x);
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) eval_set_var(ctx, yvar, saved_y);
            else eval_clear_var(ctx, yvar);
            return 0;
        }

        /* critical(expr, var) — find and classify critical points */
        if (pop == POLOP_CRITICAL) {
            char var = node->d.polar.var;
            ast_node_t *fp = deriv_diff_quiet(node->d.polar.expr1, var);
            ast_node_t *fpp = NULL;
            ce_float_t saved = 0;
            bool was_bound = false;
            int i, n_found = 0;
            /* Search grid for critical points */
            ce_float_t search_pts[] = {-10, -5, -3, -2, -1, -0.5f, 0, 0.5f, 1, 2, 3, 5, 10};
            int n_search = 13;
            ce_float_t crits[10];
            int n_crits = 0;

            if (!fp) {
                set_err(ctx, CE_ERR_TOO_COMPLEX, "Cannot differentiate expression");
                return 0;
            }
            fpp = deriv_diff_quiet(fp, var);

            /* Save variable */
            for (i = 0; i < MAX_VARS; i++) {
                if (ctx->vars[i].bound && EVAL_VAR_SAME(ctx->vars[i].name, var)) {
                    was_bound = true; saved = ctx->vars[i].value; break;
                }
            }

            steps_addf("Finding critical points: f'(%c) = 0", var);
            {
                char buf[80];
                ast_to_str(fp, buf, sizeof(buf));
                steps_addf("f'(%c) = %s", var, buf);
            }

            /* Find zeros of f' using Newton's method from each search point */
            for (i = 0; i < n_search && n_crits < 10; i++) {
                ce_float_t xn = search_pts[i];
                int iter;
                bool found = false;
                for (iter = 0; iter < 30; iter++) {
                    ce_float_t fx_val, fpx_val, fp_val, fm_val, xnew;
                    ce_float_t h = 1e-5f;
                    if (!eval_set_var(ctx, var, xn)) goto critical_done;
                    fx_val = eval_expr(ctx, fp);
                    if (ctx->error) { ctx->error = CE_OK; break; }

                    /* Numerical derivative of f' */
                    if (!eval_set_var(ctx, var, xn + h)) goto critical_done;
                    fp_val = eval_expr(ctx, fp);
                    if (!eval_set_var(ctx, var, xn - h)) goto critical_done;
                    fm_val = eval_expr(ctx, fp);
                    if (ctx->error) { ctx->error = CE_OK; break; }
                    fpx_val = (fp_val - fm_val) / (2.0f * h);

                    if (fabsf(fpx_val) < 1e-12f) break;
                    xnew = xn - fx_val / fpx_val;
                    if (fabsf(xnew - xn) < 1e-7f && fabsf(fx_val) < 1e-5f) {
                        /* Check for duplicates */
                        int j;
                        bool dup = false;
                        for (j = 0; j < n_crits; j++) {
                            if (fabsf(crits[j] - xnew) < 1e-4f) { dup = true; break; }
                        }
                        if (!dup) { crits[n_crits++] = xnew; found = true; }
                        break;
                    }
                    xn = xnew;
                }
                (void)found;
            }

            /* Classify each critical point */
            n_found = n_crits;
            for (i = 0; i < n_crits; i++) {
                ce_float_t x_val = crits[i];
                ce_float_t f_val, fpp_val;
                const char *type;

                if (!eval_set_var(ctx, var, x_val)) goto critical_done;
                f_val = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) { ctx->error = CE_OK; f_val = 0; }

                if (fpp) {
                    if (!eval_set_var(ctx, var, x_val)) goto critical_done;
                    fpp_val = eval_expr(ctx, fpp);
                    if (ctx->error) { ctx->error = CE_OK; fpp_val = 0; }

                    if (fpp_val > 1e-6f) type = "local minimum";
                    else if (fpp_val < -1e-6f) type = "local maximum";
                    else type = "inflection/inconclusive";
                } else {
                    type = "critical point";
                }
                steps_addf("%c = %.6g: f = %.6g (%s)", var, (double)x_val, (double)f_val, type);
            }

            if (n_crits == 0) {
                steps_addf("No critical points found in [-10, 10]");
            }

        critical_done:
            if (was_bound) eval_set_var(ctx, var, saved);
            else eval_clear_var(ctx, var);
            ast_free(fp);
            if (fpp) ast_free(fpp);
            return (ce_float_t)n_found;
        }

        if (pop == POLOP_ODE_SEP || pop == POLOP_ODE_LINEAR || pop == POLOP_DIFFEQ) {
            char xvar = node->d.polar.var ? node->d.polar.var : 'x';
            char yvar = (node->d.polar.expr2 && node->d.polar.expr2->type == NODE_VARIABLE)
                        ? node->d.polar.expr2->d.variable : 'y';
            char sol[256];
            bool ok = false;
            sol[0] = '\0';

            if (xvar == yvar) {
                set_err(ctx, CE_ERR_DOMAIN,
                        "ODE: independent and dependent variables must differ");
                return 0;
            }

            if (pop == POLOP_ODE_SEP) {
                ok = ode_solve_separable(node->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
            } else if (pop == POLOP_ODE_LINEAR) {
                ok = ode_solve_linear(node->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
            } else {
                ok = ode_solve_separable(node->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
                if (!ok)
                    ok = ode_solve_linear(node->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
                if (!ok && !sol[0]) {
                    strncpy(sol, "Unsolvable by current ODE methods (separable/linear)",
                            sizeof(sol) - 1);
                    sol[sizeof(sol) - 1] = '\0';
                }
            }

            if (!ok) {
                set_err(ctx, CE_ERR_TOO_COMPLEX, sol[0] ? sol :
                        "Unsolvable by current ODE methods");
                return 0;
            }
            steps_addf("ODE solution: %s", sol);
            return 0;
        }

        if (pop == POLOP_EULER) {
            char xvar = node->d.polar.var ? node->d.polar.var : 'x';
            char yvar = (node->d.polar.expr2 && node->d.polar.expr2->type == NODE_VARIABLE)
                        ? node->d.polar.expr2->d.variable : 'y';
            ce_float_t x0, y0, xf, h, x, y;
            ce_float_t saved_x = 0, saved_y = 0;
            bool x_was_bound, y_was_bound;
            int iter = 0;
            const int max_iter = 10000;

            if (xvar == yvar) {
                set_err(ctx, CE_ERR_DOMAIN, "Euler: x and y variables must differ");
                return 0;
            }
            if (!node->d.polar.lo || !node->d.polar.hi ||
                node->d.polar.lo->type != NODE_PAIR ||
                node->d.polar.hi->type != NODE_PAIR) {
                set_err(ctx, CE_ERR_SYNTAX, "Euler: invalid argument encoding");
                return 0;
            }

            x0 = eval_expr(ctx, node->d.polar.lo->d.pair.head);
            y0 = eval_expr(ctx, node->d.polar.lo->d.pair.tail);
            xf = eval_expr(ctx, node->d.polar.hi->d.pair.head);
            h  = eval_expr(ctx, node->d.polar.hi->d.pair.tail);
            if (ctx->error) return 0;

            if (fabsf(h) < 1e-12f) {
                set_err(ctx, CE_ERR_DOMAIN, "Euler: step size h cannot be zero");
                return 0;
            }
            if ((xf > x0 && h < 0) || (xf < x0 && h > 0)) {
                set_err(ctx, CE_ERR_DOMAIN, "Euler: step sign does not move toward xf");
                return 0;
            }

            x_was_bound = var_is_bound(ctx, xvar, &saved_x);
            y_was_bound = var_is_bound(ctx, yvar, &saved_y);

            x = x0;
            y = y0;
            steps_addf("Euler method: y' = f(%c,%c), x0=%.6g, y0=%.6g, xf=%.6g, h=%.6g",
                       xvar, yvar, (double)x0, (double)y0, (double)xf, (double)h);

            while (((h > 0 && x < xf - 1e-7f) || (h < 0 && x > xf + 1e-7f)) &&
                   iter < max_iter) {
                ce_float_t h_step = h;
                ce_float_t slope, x_next, y_next;

                if ((h > 0 && x + h_step > xf) || (h < 0 && x + h_step < xf))
                    h_step = xf - x;

                if (!eval_set_var(ctx, xvar, x)) goto euler_restore;
                if (!eval_set_var(ctx, yvar, y)) goto euler_restore;
                slope = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto euler_restore;

                x_next = x + h_step;
                y_next = y + h_step * slope;
                if (iter < 18) {
                    steps_addf("n=%d: x=%.6g, y=%.6g, f=%.6g -> y_next=%.6g",
                               iter, (double)x, (double)y, (double)slope, (double)y_next);
                }
                x = x_next;
                y = y_next;
                iter++;
            }

            if (iter >= max_iter) {
                set_err(ctx, CE_ERR_OVERFLOW, "Euler: exceeded maximum iteration budget");
                goto euler_restore;
            }

            steps_addf("Euler result: y(%.6g) ~= %.6g after %d steps",
                       (double)xf, (double)y, iter);

            if (x_was_bound) { if (!eval_set_var(ctx, xvar, saved_x)) return 0; }
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) { if (!eval_set_var(ctx, yvar, saved_y)) return 0; }
            else eval_clear_var(ctx, yvar);
            return y;

        euler_restore:
            if (x_was_bound) eval_set_var(ctx, xvar, saved_x);
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) eval_set_var(ctx, yvar, saved_y);
            else eval_clear_var(ctx, yvar);
            return 0;
        }

        if (pop == POLOP_SLOPEFIELD) {
            char xvar = node->d.polar.var ? node->d.polar.var : 'x';
            char yvar = (node->d.polar.expr2 && node->d.polar.expr2->type == NODE_VARIABLE)
                        ? node->d.polar.expr2->d.variable : 'y';
            ce_float_t xlo = -5.0f, xhi = 5.0f, ylo = -5.0f, yhi = 5.0f;
            ce_float_t saved_x = 0, saved_y = 0;
            bool x_was_bound, y_was_bound;
            ce_float_t sum_abs = 0.0f;
            int count = 0, shown = 0;
            int ix, iy;
            const int nx = 21, ny = 21;

            if (xvar == yvar) {
                set_err(ctx, CE_ERR_DOMAIN, "slopefield: x and y variables must differ");
                return 0;
            }

            if (node->d.polar.lo && node->d.polar.hi) {
                if (node->d.polar.lo->type != NODE_PAIR ||
                    node->d.polar.hi->type != NODE_PAIR) {
                    set_err(ctx, CE_ERR_SYNTAX, "slopefield: invalid bounds encoding");
                    return 0;
                }
                xlo = eval_expr(ctx, node->d.polar.lo->d.pair.head);
                xhi = eval_expr(ctx, node->d.polar.lo->d.pair.tail);
                ylo = eval_expr(ctx, node->d.polar.hi->d.pair.head);
                yhi = eval_expr(ctx, node->d.polar.hi->d.pair.tail);
                if (ctx->error) return 0;
            }

            if (xlo >= xhi || ylo >= yhi) {
                set_err(ctx, CE_ERR_DOMAIN, "slopefield: invalid window bounds");
                return 0;
            }

            x_was_bound = var_is_bound(ctx, xvar, &saved_x);
            y_was_bound = var_is_bound(ctx, yvar, &saved_y);

            steps_addf("Slope field for y' = f(%c,%c) on [%.3g,%.3g]x[%.3g,%.3g]",
                       xvar, yvar, (double)xlo, (double)xhi, (double)ylo, (double)yhi);

            for (ix = 0; ix < nx; ix++) {
                ce_float_t xv = xlo + ((ce_float_t)ix / (ce_float_t)(nx - 1)) * (xhi - xlo);
                for (iy = 0; iy < ny; iy++) {
                    ce_float_t yv = ylo + ((ce_float_t)iy / (ce_float_t)(ny - 1)) * (yhi - ylo);
                    ce_float_t m;
                    if (!eval_set_var(ctx, xvar, xv)) goto slope_restore;
                    if (!eval_set_var(ctx, yvar, yv)) goto slope_restore;
                    m = eval_expr(ctx, node->d.polar.expr1);
                    if (ctx->error) goto slope_restore;
                    if (ce_isfinite(m)) {
                        sum_abs += fabsf(m);
                        count++;
                        if (shown < 8) {
                            steps_addf("(% .3g,% .3g): slope %.4g",
                                       (double)xv, (double)yv, (double)m);
                            shown++;
                        }
                    }
                }
            }

            if (x_was_bound) { if (!eval_set_var(ctx, xvar, saved_x)) return 0; }
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) { if (!eval_set_var(ctx, yvar, saved_y)) return 0; }
            else eval_clear_var(ctx, yvar);

            steps_addf("Slope field samples: %d valid directions", count);
            if (count == 0) return 0;
            steps_addf("Average |slope| = %.6g", (double)(sum_abs / (ce_float_t)count));
            return sum_abs / (ce_float_t)count;

        slope_restore:
            if (x_was_bound) eval_set_var(ctx, xvar, saved_x);
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) eval_set_var(ctx, yvar, saved_y);
            else eval_clear_var(ctx, yvar);
            return 0;
        }

        if (pop == POLOP_INTPARTS || pop == POLOP_TRIGSUB ||
            pop == POLOP_USUB || pop == POLOP_INTSTRATEGY) {
            char var = node->d.polar.var ? node->d.polar.var : 'x';
            if (pop == POLOP_INTSTRATEGY) {
                char report[320];
                integrate_strategy_report(node->d.polar.expr1, var, report, sizeof(report));
                steps_addf("%s", report);
                return 0;
            } else {
                integ_strategy_t strategy = INTEG_STRATEGY_AUTO;
                const ast_node_t *u_hint = NULL;
                ast_node_t *anti;
                char abuf[128];
                ce_float_t bound_val;
                bool has_bound;

                if (pop == POLOP_INTPARTS) strategy = INTEG_STRATEGY_PARTS;
                else if (pop == POLOP_TRIGSUB) strategy = INTEG_STRATEGY_TRIGSUB;
                else {
                    strategy = INTEG_STRATEGY_USUB;
                    u_hint = node->d.polar.lo;
                }

                anti = integrate_with_strategy(node->d.polar.expr1, var, strategy, u_hint);
                if (!anti) {
                    set_err(ctx, CE_ERR_TOO_COMPLEX, integrate_last_failure_reason());
                    return 0;
                }
                ast_to_str(anti, abuf, sizeof(abuf));
                steps_addf("Antiderivative (%s): %s + C",
                           (strategy == INTEG_STRATEGY_PARTS) ? "parts" :
                           (strategy == INTEG_STRATEGY_TRIGSUB) ? "trigsub" : "u-sub",
                           abuf);

                has_bound = var_is_bound(ctx, var, &bound_val);
                if (!has_bound) {
                    steps_addf("Symbolic result only; bind %c to evaluate numerically", var);
                    ast_free(anti);
                    return 0;
                }

                {
                    ce_float_t out = eval_expr(ctx, anti);
                    ast_free(anti);
                    if (ctx->error) return 0;
                    return out;
                }
            }
        }
#endif /* CE_EVAL_ENABLE_ADV_POLAR */

        /* polydiv(p, q, var) — polynomial long division with steps */
#if CE_EVAL_ENABLE_POLYDIV
        if (pop == POLOP_POLYDIV) return eval_polydiv(ctx, node);
#endif /* CE_EVAL_ENABLE_POLYDIV */

        /* ── polararea(r, theta, a, b) — ½∫r²dθ (Simpson's rule, N=200) ── */
        if (pop == POLOP_POLARAREA) {
            char tvar = node->d.polar.var ? node->d.polar.var : 't';
            ce_float_t a_val = eval_expr(ctx, node->d.polar.lo);
            ce_float_t b_val = eval_expr(ctx, node->d.polar.hi);
            int i, N = 200;
            ce_float_t h, total = 0;
            ce_float_t saved;
            bool was_bound = false;
            int j;
            if (ctx->error) return 0;
            if (N % 2 != 0) N++;
            h = (b_val - a_val) / (ce_float_t)N;
            /* Save variable */
            for (j = 0; j < MAX_VARS; j++) {
                if (ctx->vars[j].bound && EVAL_VAR_SAME(ctx->vars[j].name, tvar))
                    { saved = ctx->vars[j].value; was_bound = true; break; }
            }
            for (i = 0; i <= N; i++) {
                ce_float_t th = a_val + i * h;
                ce_float_t w = (i == 0 || i == N) ? 1.0f : (i % 2 == 1) ? 4.0f : 2.0f;
                ce_float_t r_val;
                if (!eval_set_var(ctx, tvar, th)) goto polararea_done;
                r_val = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto polararea_done;
                total += w * r_val * r_val;
            }
            total *= h / 3.0f * 0.5f;
            steps_addf("Polar area = (1/2)*integral(%c^2 d%c) from %.6g to %.6g",
                       tvar, tvar, (double)a_val, (double)b_val);
            steps_addf("Area = %.6g", (double)total);
        polararea_done:
            if (was_bound) eval_set_var(ctx, tvar, saved);
            else eval_clear_var(ctx, tvar);
            return ctx->error ? 0 : total;
        }

        /* ── polararclen(r, theta, a, b) — ∫√(r²+r'²)dθ ── */
        if (pop == POLOP_POLARARCLEN) {
            char tvar = node->d.polar.var ? node->d.polar.var : 't';
            ce_float_t a_val = eval_expr(ctx, node->d.polar.lo);
            ce_float_t b_val = eval_expr(ctx, node->d.polar.hi);
            int i, N = 200;
            ce_float_t h, total = 0;
            ce_float_t saved;
            bool was_bound = false;
            int j;
            const ce_float_t dh = 1e-5f;
            if (ctx->error) return 0;
            if (N % 2 != 0) N++;
            h = (b_val - a_val) / (ce_float_t)N;
            for (j = 0; j < MAX_VARS; j++) {
                if (ctx->vars[j].bound && EVAL_VAR_SAME(ctx->vars[j].name, tvar))
                    { saved = ctx->vars[j].value; was_bound = true; break; }
            }
            for (i = 0; i <= N; i++) {
                ce_float_t th = a_val + i * h;
                ce_float_t w = (i == 0 || i == N) ? 1.0f : (i % 2 == 1) ? 4.0f : 2.0f;
                ce_float_t r_val, rp_val, rm_val, rprime;
                if (!eval_set_var(ctx, tvar, th)) goto polararclen_done;
                r_val = eval_expr(ctx, node->d.polar.expr1);
                if (!eval_set_var(ctx, tvar, th + dh)) goto polararclen_done;
                rp_val = eval_expr(ctx, node->d.polar.expr1);
                if (!eval_set_var(ctx, tvar, th - dh)) goto polararclen_done;
                rm_val = eval_expr(ctx, node->d.polar.expr1);
                if (ctx->error) goto polararclen_done;
                rprime = (rp_val - rm_val) / (2.0f * dh);
                total += w * sqrtf(r_val * r_val + rprime * rprime);
            }
            total *= h / 3.0f;
            steps_addf("Polar arc length = integral(sqrt(r^2+r'^2) d%c) from %.6g to %.6g",
                       tvar, (double)a_val, (double)b_val);
            steps_addf("Arc length = %.6g", (double)total);
        polararclen_done:
            if (was_bound) eval_set_var(ctx, tvar, saved);
            else eval_clear_var(ctx, tvar);
            return ctx->error ? 0 : total;
        }

        /* ── dot(a1,...,an, b1,...,bn) — numeric dot product ── */
        if (pop == POLOP_VEC_DOT) {
            /* Collect all args from PAIR chain */
            ce_float_t vals[8];
            int nv = 0;
            const ast_node_t *cur_node = node->d.polar.expr1;
            while (cur_node && nv < 8) {
                if (cur_node->type == NODE_PAIR) {
                    vals[nv++] = eval_expr(ctx, cur_node->d.pair.head);
                    if (ctx->error) return 0;
                    cur_node = cur_node->d.pair.tail;
                } else {
                    vals[nv++] = eval_expr(ctx, cur_node);
                    if (ctx->error) return 0;
                    cur_node = NULL;
                }
            }
            if (nv % 2 != 0 || nv < 2) {
                set_err(ctx, CE_ERR_DOMAIN, "dot: need even args (u1,...,un,v1,...,vn)");
                return 0;
            }
            {
                int dim = nv / 2, k;
                ce_float_t result = 0;
                for (k = 0; k < dim; k++) result += vals[k] * vals[dim + k];
                steps_addf("Dot product: sum of component products");
                steps_addf("dot = %.6g", (double)result);
                return result;
            }
        }

        /* ── div(Fx, Fy, x, y) — numeric divergence ∂Fx/∂x + ∂Fy/∂y ── */
        if (pop == POLOP_VEC_DIV) {
            char xvar = node->d.polar.var ? node->d.polar.var : 'x';
            char yvar = (node->d.polar.lo && node->d.polar.lo->type == NODE_VARIABLE)
                        ? node->d.polar.lo->d.variable : 'y';
            const ce_float_t dh = 1e-5f;
            ce_float_t xval = 0, yval = 0;
            ce_float_t fx_p, fx_m, fy_p, fy_m, div_val;
            int j;
            bool x_was_bound = false, y_was_bound = false;
            ce_float_t saved_x, saved_y;
            /* Get current variable values (for evaluation point) */
            for (j = 0; j < MAX_VARS; j++) {
                if (ctx->vars[j].bound && EVAL_VAR_SAME(ctx->vars[j].name, xvar))
                    { xval = ctx->vars[j].value; x_was_bound = true; }
                if (ctx->vars[j].bound && EVAL_VAR_SAME(ctx->vars[j].name, yvar))
                    { yval = ctx->vars[j].value; y_was_bound = true; }
            }
            saved_x = xval; saved_y = yval;
            /* ∂Fx/∂x */
            if (!eval_set_var(ctx, xvar, xval + dh)) return 0;
            if (!eval_set_var(ctx, yvar, yval)) return 0;
            fx_p = eval_expr(ctx, node->d.polar.expr1);
            if (ctx->error) goto div_restore;
            if (!eval_set_var(ctx, xvar, xval - dh)) goto div_restore;
            fx_m = eval_expr(ctx, node->d.polar.expr1);
            if (ctx->error) goto div_restore;
            /* ∂Fy/∂y */
            if (!eval_set_var(ctx, xvar, xval)) goto div_restore;
            if (!eval_set_var(ctx, yvar, yval + dh)) goto div_restore;
            fy_p = eval_expr(ctx, node->d.polar.expr2);
            if (ctx->error) goto div_restore;
            if (!eval_set_var(ctx, yvar, yval - dh)) goto div_restore;
            fy_m = eval_expr(ctx, node->d.polar.expr2);
            if (ctx->error) goto div_restore;
            div_val = (fx_p - fx_m) / (2.0f * dh) + (fy_p - fy_m) / (2.0f * dh);
            steps_addf("Divergence = dFx/d%c + dFy/d%c", xvar, yvar);
            steps_addf("div = %.6g", (double)div_val);
        div_restore:
            if (x_was_bound) eval_set_var(ctx, xvar, saved_x);
            else eval_clear_var(ctx, xvar);
            if (y_was_bound) eval_set_var(ctx, yvar, saved_y);
            else eval_clear_var(ctx, yvar);
            return ctx->error ? 0 : div_val;
        }

        /* dsolve/grad/cross/curl/laplace/ilaplace — handled by main.c (symbolic output) */
        if (pop == POLOP_DSOLVE || pop == POLOP_VEC_GRAD ||
            pop == POLOP_VEC_CROSS || pop == POLOP_VEC_CURL ||
            pop == POLOP_LAPLACE || pop == POLOP_ILAPLACE ||
            pop == POLOP_CONVERT) {
            set_err(ctx, CE_ERR_TOO_COMPLEX,
                    "Symbolic op (use at top level)");
            return 0;
        }

        /* Other polar ops handled by main.c */
        set_err(ctx, CE_ERR_TOO_COMPLEX,
                "Polar/advanced op (use at top level)");
        break;
    }

    default:
        set_err(ctx, CE_ERR_INTERNAL, "Unknown node type");
        break;
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────── */

void eval_init(eval_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
#if CE_SAFE_FLOAT_TINY
    g_ce_fops_tiny = 0;
    s_tiny_at = 0;
    s_tiny_mass = 0;
    s_tiny_bound = 0;
    s_tiny_val = 0;
    s_in_sample = 0;
    s_tiny_depth = 0;
#endif
}

bool eval_set_var(eval_ctx_t *ctx, char name, ce_float_t value)
{
    int i;
#if CE_EVAL_USER_VARS
    name = user_var_name(name);
#endif
    /* update existing */
    for (i = 0; i < MAX_VARS; i++) {
        if (ctx->vars[i].bound && ctx->vars[i].name == name) {
            ctx->vars[i].value = value;
            return true;
        }
    }
    /* find empty slot */
    for (i = 0; i < MAX_VARS; i++) {
        if (!ctx->vars[i].bound) {
            ctx->vars[i].name  = name;
            ctx->vars[i].value = value;
            ctx->vars[i].bound = true;
            return true;
        }
    }
    /* All slots full */
    set_err(ctx, CE_ERR_OVERFLOW, "Too many variables");
    return false;
}

void eval_clear_var(eval_ctx_t *ctx, char name)
{
    int i;
#if CE_EVAL_USER_VARS
    name = user_var_name(name);
#endif
    for (i = 0; i < MAX_VARS; i++) {
        if (ctx->vars[i].bound && ctx->vars[i].name == name) {
            ctx->vars[i].bound = false;
            return;
        }
    }
}

ce_float_t eval_at(const ast_node_t *expr, char var,
                   ce_float_t x, ce_error_t *err)
{
    eval_ctx_t ctx;
    ce_float_t val;
    eval_init(&ctx);
    eval_set_var(&ctx, var, x);
    val = eval_expr(&ctx, expr);
    *err = eval_error(&ctx);
    return val;
}

ce_error_t eval_error(const eval_ctx_t *ctx)
{
    return ctx->error;
}

const char *eval_error_msg(const eval_ctx_t *ctx)
{
    return ctx->error_msg;
}
