/*
 * test_ce_float_ops.c - checks for the CE-safe float helpers in
 * src/ce_float_ops.h (implemented in src/eval.c) and for the literal range
 * check in src/token.c.
 *
 * On the CE the native float routines wrap on overflow and do not handle
 * inf (ce_float_ops.h lists what was measured on the OS 5.8 ROM). The
 * helpers must therefore
 *   1. return exactly the IEEE-754 result of the host for + - * /, including
 *      inf/NaN operands and overflow (checked bit for bit here), and
 *   2. call a native operation only when its exact result is in range.
 * (2) is what makes them safe on the CE. eval.c wraps every such native
 * operation in CE_FOPS_NATIVE(); this file defines that macro to record any
 * call whose IEEE result is not finite, and every check below also requires
 * that count to stay 0.
 *
 * The file includes eval.c directly so the macro applies to it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static unsigned long g_native_calls;
static unsigned long g_native_bad;
static float fops_native_check_(float v)
{
    g_native_calls++;
    if (isinf(v)) g_native_bad++;      /* NaN is fine: 0/0 */
    return v;
}
#define CE_FOPS_NATIVE(expr) fops_native_check_(expr)
#define CE_FOPS_NATIVE_IF(checked, expr) ((checked) ? fops_native_check_(expr) : (expr))

#include "../src/eval.c"
/* eval.c's recursion-refusal flag; the CE build defines it in src/split/ipc.c,
 * which this test does not link. */
bool g_eval_refused;
ce_float_t g_eval_refused_ans;
#include "token.h"

static unsigned long g_checks, g_failed;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        if (g_failed++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } \
} while (0)

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float from_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Same bits, or both NaN (payloads may differ). */
static int same(float a, float b)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    return bits_of(a) == bits_of(b);
}

static uint32_t g_rng = 0x9E3779B9u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

/* Host IEEE results. volatile keeps the compiler from folding or fusing. */
static float host_op(int op, float a, float b)
{
    volatile float x = a, y = b, r;
    switch (op) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    default: r = x / y; break;
    }
    return r;
}

static float ce_op(int op, float a, float b)
{
    switch (op) {
    case 0: return ce_fadd(a, b);
    case 1: return ce_fsub(a, b);
    case 2: return ce_fmul(a, b);
    default: return ce_fdiv(a, b);
    }
}

static unsigned long g_pair_checks, g_band;
static void check_pair(float a, float b)
{
    int op;
    for (op = 0; op < 4; op++) {
        float want = host_op(op, a, b);
        float got = ce_op(op, a, b);
        g_pair_checks++;
        /* A product or quotient within about 2 ulp of FLT_MAX may count as
         * an overflow (ce_farith's threshold test). */
        if (op >= 2 && isinf(got) && isfinite(want) && fabsf(want) >= 0x1.fffff8p127f &&
            signbit(got) == signbit(want)) { g_band++; continue; }
        if (!same(want, got)) {
            CHECK(0, "%08X %c %08X: helper %08X, IEEE %08X", (unsigned)bits_of(a),
                  "+-*/"[op], (unsigned)bits_of(b), (unsigned)bits_of(got), (unsigned)bits_of(want));
        }
    }
}

/* Every exponent with boundary mantissas, plus the special values. */
static int build_edge_values(float *out, int cap)
{
    static const uint32_t mant[] = { 0u, 1u, 2u, 0x3FFFFFu, 0x400000u, 0x400001u, 0x7FFFFEu, 0x7FFFFFu };
    int n = 0;
    uint32_t e, s;
    unsigned i;
    for (s = 0; s < 2; s++) {
        for (e = 0; e < 256; e++) {
            for (i = 0; i < sizeof(mant) / sizeof(mant[0]); i++) {
                if (n < cap) out[n++] = from_bits((s << 31) | (e << 23) | mant[i]);
            }
            if (n < cap) out[n++] = from_bits((s << 31) | (e << 23) | (rnd() & 0x7FFFFFu));
        }
    }
    return n;
}

static void test_arith_parity(void)
{
    static float v[2 * 256 * 9];
    int n = build_edge_values(v, (int)(sizeof(v) / sizeof(v[0])));
    int i, j;
    unsigned long k;

    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            check_pair(v[i], v[j]);
    printf("  edge grid: %d values, %lu op checks\n", n, g_pair_checks);

    /* Random bit patterns. */
    for (k = 0; k < 4000000ul; k++)
        check_pair(from_bits(rnd()), from_bits(rnd()));

    /* Products and quotients near the top of the range: exponent sum or
     * difference within 2 of the overflow threshold. */
    for (k = 0; k < 4000000ul; k++) {
        uint32_t ea = 1u + rnd() % 254u;
        int eb_mul = 381 - (int)ea + (int)(rnd() % 5u) - 2;   /* ea+eb-254 ~ 127 */
        int eb_div = (int)ea - 128 + (int)(rnd() % 5u) - 2;   /* ea-eb-127 ~ 128 */
        uint32_t sa = rnd() & 0x80000000u, sb = rnd() & 0x80000000u;
        float a = from_bits(sa | (ea << 23) | (rnd() & 0x7FFFFFu));
        if (eb_mul >= 1 && eb_mul <= 254)
            check_pair(a, from_bits(sb | ((uint32_t)eb_mul << 23) | (rnd() & 0x7FFFFFu)));
        if (eb_div >= 1 && eb_div <= 254)
            check_pair(a, from_bits(sb | ((uint32_t)eb_div << 23) | (rnd() & 0x7FFFFFu)));
    }

    /* Sums near FLT_MAX and integers around 2^24. */
    for (k = 0; k < 2000000ul; k++) {
        uint32_t sa = rnd() & 0x80000000u, sb = (rnd() & 3u) ? sa : (sa ^ 0x80000000u);
        float a = from_bits(sa | (254u << 23) | (rnd() & 0x7FFFFFu));
        float b = from_bits(sb | ((uint32_t)(254 - (int)(rnd() % 30u)) << 23) | (rnd() & 0x7FFFFFu));
        long ia = (long)(rnd() % 50000000u) - 25000000L;
        long ib = (long)(rnd() % 50000000u) - 25000000L;
        check_pair(a, b);
        check_pair((float)ia, (float)ib);
        check_pair((float)ia, (float)(ib % 64));
    }
    CHECK(g_native_bad == 0, "%lu native ops had a non-finite IEEE result", g_native_bad);
    printf("  + - * /: %lu op checks (%lu in the FLT_MAX band), %lu native calls, %lu out of range\n",
           g_pair_checks, g_band, g_native_calls, g_native_bad);
}

/* The rows measured on the ROM (bits as the CE parsed them). */
static void test_rom_rows(void)
{
    const float e38 = from_bits(0x7E96769Cu);     /* CE atof("1E38") */
    const float e3_38 = from_bits(0x7F61B1EDu);   /* CE atof("3E38") */
    const float e30 = from_bits(0x7149F2CDu);
    CHECK(isinf(ce_fmul(e38, 10.0f)), "1E38*10 must be inf");
    CHECK(isinf(ce_fmul(e3_38, 2.0f)), "3E38*2 must be inf");
    CHECK(isinf(ce_fadd(e3_38, e3_38)), "3E38+3E38 must be inf");
    CHECK(isinf(ce_fmul(e30, e30)), "1E30*1E30 must be inf");
    CHECK(isinf(ce_fdiv(e38, 0.1f)), "1E38/0.1 must be inf");
    CHECK(bits_of(ce_fsub(16777216.0f, 1.0f)) == 0x4B7FFFFFu, "16777216-1 = 16777215");
    CHECK(bits_of(ce_fsub(16777218.0f, 3.0f)) == 0x4B7FFFFFu, "16777218-3 = 16777215");
    CHECK(bits_of(ce_fadd(-16777216.0f, 1.0f)) == 0xCB7FFFFFu, "-16777216+1");
    CHECK(isinf(ce_fmul(CE_INF, 2.0f)) && isinf(ce_fadd(CE_INF, CE_INF)), "inf*2, inf+inf");
    CHECK(isnan(ce_fsub(CE_INF, CE_INF)) && isnan(ce_fmul(CE_INF, 0.0f)), "inf-inf, inf*0");
    CHECK(isnan(ce_fdiv(CE_INF, CE_INF)) && ce_fdiv(1.0f, CE_INF) == 0.0f, "inf/inf, 1/inf");
    /* CE __fsub truncates 2.9843E-38-2.0235E-38 to the subnormal 00513E68,
     * and CE __fdiv returns NaN for any x/subnormal. */
    {
        const float sub = from_bits(0x00513E68u);
        CHECK(isinf(ce_fdiv(10.0f, sub)), "10/subnormal must be inf");
        CHECK(same(ce_fdiv(1e-30f, sub), host_op(3, 1e-30f, sub)), "1E-30/subnormal");
        CHECK(same(ce_fdiv(-1e-30f, sub), host_op(3, -1e-30f, sub)) &&
              same(ce_fdiv(1e-30f, -sub), host_op(3, 1e-30f, -sub)), "signs");
        CHECK(same(ce_fdiv(from_bits(1u), from_bits(1u)), 1.0f) &&
              same(ce_fdiv(from_bits(1u), from_bits(0x007FFFFFu)), host_op(3, from_bits(1u), from_bits(0x007FFFFFu))),
              "subnormal/subnormal");
        CHECK(same(ce_fdiv(sub, sub), 1.0f) && same(ce_fdiv(-sub, sub), -1.0f) &&
              same(ce_fdiv(sub, -from_bits(3u)), host_op(3, sub, -from_bits(3u))),
              "subnormal/subnormal without a native op on a subnormal");
    }
}

static double ulps(float got, double want)
{
    float w = (float)want;
    if (isinf(w)) return isinf(got) ? 0.0 : 1e9;
    {
        float up = nextafterf(w, INFINITY);
        double ulp = (double)up - (double)w;
        if (ulp == 0.0 || isinf(up)) ulp = (double)w - (double)nextafterf(w, -INFINITY);
        return fabs((double)got - want) / ulp;
    }
}

static void test_pow(void)
{
    int b, n;
    unsigned long k;
    double worst_int = 0.0, worst_gen = 0.0;
    unsigned long native_before = g_native_bad;

    /* Exact integer powers (TI-OS powf gave 2^30 = 1073743360). */
    for (b = 2; b <= 300; b++) {
        double p = 1.0;
        for (n = 0; n <= 40; n++) {
            if (p >= 16777216.0) break;
            CHECK((double)ce_fpow((float)b, (float)n) == p, "%d^%d", b, n);
            CHECK((double)ce_fpow((float)-b, (float)n) == ((n & 1) ? -p : p), "(-%d)^%d", b, n);
            p *= b;
        }
    }
    for (n = -149; n <= 127; n++)
        CHECK(ce_fpow(2.0f, (float)n) == ldexpf(1.0f, n), "2^%d", n);   /* subnormals too */
    CHECK(same(ce_fpow(100.0f, -20.0f), powf(100.0f, -20.0f)) && ce_fpow(100.0f, -20.0f) > 0.0f,
          "100^-20: subnormal, though 100^20 overflows");
    /* Odd integer exponents in [2^23, 2^24) keep the sign. */
    CHECK(ce_fpow(-1.0f, 8388609.0f) == -1.0f && ce_fpow(-1.0f, 8388608.0f) == 1.0f, "(-1)^(2^23+1)");
    CHECK(same(ce_fpow(-0.9999999f, 8388609.0f), powf(-0.9999999f, 8388609.0f)), "(-0.9999999)^8388609");
    CHECK(same(ce_fpow(-0.0f, 8388609.0f), -0.0f) && same(ce_fpow(-CE_INF, 8388609.0f), -CE_INF), "-0, -inf ^ odd");
    CHECK(bits_of(ce_fpow(2.0f, 30.0f)) == 0x4E800000u, "2^30");
    CHECK(ce_fpow(3.0f, 15.0f) == 14348907.0f, "3^15");
    CHECK(ce_fpow(10.0f, 10.0f) == 1e10f, "10^10");
    CHECK(ce_fpow(1.5f, 2.0f) == 2.25f && ce_fpow(0.5f, 10.0f) == 0.0009765625f, "1.5^2, 0.5^10");
    CHECK(ce_fpow(-2.0f, 3.0f) == -8.0f, "(-2)^3");
    CHECK(ce_fpow(10.0f, -5.0f) == 1e-5f, "10^-5");

    /* Overflow and underflow. */
    CHECK(isinf(ce_fpow(10.0f, 39.0f)) && isinf(ce_fpow(2.0f, 128.0f)), "10^39, 2^128");
    CHECK(isinf(ce_fpow(2.0f, 1000.0f)) && isinf(ce_fpow(1e19f, 3.0f)), "2^1000, 1e19^3");
    CHECK(bits_of(ce_fpow(-2.0f, 129.0f)) == 0xFF800000u, "(-2)^129 = -inf");
    CHECK(isinf(ce_fpow(1.0001f, 1e6f)) && isinf(ce_fpow(0.5f, -129.0f)), "1.0001^1e6, 0.5^-129");
    CHECK(isinf(ce_fpow(CE_E, 100.0f)) && isinf(ce_fpow(10.0f, 38.6f)), "e^100, 10^38.6");
    CHECK(ce_fpow(10.0f, -46.0f) == 0.0f && ce_fpow(2.0f, -150.0f) == 0.0f, "underflow to 0");
    CHECK(ce_fpow(0.5f, 1e30f) == 0.0f && isinf(ce_fpow(2.0f, 3e38f)), "huge exponents");
    CHECK(isinf(ce_fpow(0.0f, -1.0f)) && ce_fpow(0.0f, 2.0f) == 0.0f && ce_fpow(0.0f, 0.0f) == 1.0f, "0^x");
    CHECK(bits_of(ce_fpow(-0.0f, -3.0f)) == 0xFF800000u && bits_of(ce_fpow(-0.0f, 3.0f)) == 0x80000000u, "-0^odd");
    CHECK(isnan(ce_fpow(-8.0f, 1.0f / 3.0f)), "(-8)^(1/3) has no real value");
    CHECK(ce_fpow(-1.0f, 1e30f) == 1.0f && ce_fpow(-1.0f, 7.0f) == -1.0f, "(-1)^n");

    /* inf / NaN operands follow C99 Annex F like the host powf. */
    {
        const float sp[] = { 0.0f, -0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f,
                             1.5f, -1.5f, 2.5f, -2.5f, 3.5f, -3.5f, 8388609.0f, -8388609.0f,
                             16777216.0f, INFINITY, -INFINITY, NAN };
        unsigned i, j;
        /* Every pair: also the finite ones, which hit the signs of
         * (-0)^1.5 and (-inf)^-1.5. */
        for (i = 0; i < sizeof(sp) / sizeof(sp[0]); i++)
            for (j = 0; j < sizeof(sp) / sizeof(sp[0]); j++) {
                CHECK(same(ce_fpow(sp[i], sp[j]), powf(sp[i], sp[j])),
                      "pow(%g, %g): %g, host %g", sp[i], sp[j], ce_fpow(sp[i], sp[j]), powf(sp[i], sp[j]));
            }
    }

    /* Random cases against a double reference. Results that are clearly
     * in range must be finite and close; clearly out of range must be inf. */
    for (k = 0; k < 2000000ul; k++) {
        int intexp = (int)(rnd() & 1u);
        float base = from_bits(((rnd() % 64u) + 96u) << 23 | (rnd() & 0x7FFFFFu));   /* 2^-31..2^32 */
        float expo = intexp ? (float)((int)(rnd() % 129u) - 64)
                            : from_bits(((rnd() % 12u) + 120u) << 23 | (rnd() & 0x7FFFFFu)) * ((rnd() & 1u) ? 1.0f : -1.0f);
        double want = pow((double)base, (double)expo);
        float got = ce_fpow(base, expo);
        if (want > (double)FLT_MAX * 1.0001) {
            CHECK(isinf(got), "%a^%a = %g, want inf", base, expo, got);
        } else if (want < (double)FLT_MAX * 0.9999 && want > (double)FLT_MIN * 1.0001) {
            double u = ulps(got, want);
            CHECK(isfinite(got) && u < 64.0, "%a^%a = %a, want %a (%.1f ulp)", base, expo, got, want, u);
            if (intexp) { if (u > worst_int) worst_int = u; }
            else if (u > worst_gen) worst_gen = u;
        }
    }
    CHECK(g_native_bad == native_before, "pow made %lu out-of-range native calls", g_native_bad - native_before);
    printf("  pow: worst error %.1f ulp (integer exponent), %.1f ulp (general)\n", worst_int, worst_gen);
}

static void test_exp_hyp(void)
{
    double x;
    float f;
    /* The bounds are the largest floats with a finite IEEE result. */
    CHECK(isfinite(ce_fexp(CE_FOPS_EXP_MAX)) && isinf(ce_fexp(nextafterf(CE_FOPS_EXP_MAX, 100.0f))), "exp bound");
    CHECK(isfinite(expf(CE_FOPS_EXP_MAX)) && isinf(expf(nextafterf(CE_FOPS_EXP_MAX, 100.0f))), "exp bound is IEEE's");
    CHECK(isfinite(coshf(CE_FOPS_HYP_MAX)) && isinf(coshf(nextafterf(CE_FOPS_HYP_MAX, 100.0f))), "hyp bound is IEEE's");
    CHECK(bits_of(CE_FOPS_EXP_MAX) == 0x42B17217u, "exp bound bits");
    CHECK(bits_of(CE_FOPS_HYP_MAX) == 0x42B2D4FCu, "hyp bound bits");
    CHECK(isfinite(ce_fcosh(CE_FOPS_HYP_MAX)) && isinf(ce_fcosh(nextafterf(CE_FOPS_HYP_MAX, 100.0f))), "cosh bound");
    CHECK(isfinite(ce_fsinh(-CE_FOPS_HYP_MAX)) && bits_of(ce_fsinh(-90.0f)) == 0xFF800000u, "sinh(-90) = -inf");
    CHECK(isinf(ce_fexp(100.0f)) && isinf(ce_fexp(89.0f)) && isinf(ce_fcosh(100.0f)) && isinf(ce_fsinh(90.0f)), "rom rows");
    CHECK(ce_fexp(-200.0f) == 0.0f && ce_fexp(-INFINITY) == 0.0f && isinf(ce_fexp(INFINITY)), "exp tails");
    CHECK(isnan(ce_fexp(NAN)) && isnan(ce_fsinh(NAN)) && isnan(ce_fcosh(NAN)), "NaN");
    for (x = -120.0; x <= 120.0; x += 0.0625) {
        f = (float)x;
        CHECK(same(ce_fexp(f), f > CE_FOPS_EXP_MAX ? INFINITY : expf(f)), "exp(%g)", x);
        CHECK(same(ce_fcosh(f), fabsf(f) > CE_FOPS_HYP_MAX ? INFINITY : coshf(f)), "cosh(%g)", x);
        CHECK(same(ce_fsinh(f), fabsf(f) > CE_FOPS_HYP_MAX ? copysignf(INFINITY, f) : sinhf(f)), "sinh(%g)", x);
    }
}

/* Literal parsing: token.c must give the host's (float)strtod for every
 * literal, without relying on atof() for out-of-range ones, except that a
 * literal with a nonzero digit whose float would be 0 or tiny (exponent
 * field 0) is refused with CE_ERR_PRECISION at its first character
 * (internal review); the CE's atof() read 1.3E-38 as 0). lit() returns
 * -2 for that refusal and -1 for any other failure. */
static float lit(const char *s)
{
    tokenizer_t t;
    tok_init(&t, s);
    if (tok_tokenize(&t) != CE_OK) {
        if (t.err_pos != 0) return -1.0f;
        /* -3: a nonzero literal whose exponent did not fit (round 9) */
        return t.error == CE_ERR_PRECISION ? -2.0f : t.error == CE_ERR_OVERFLOW ? -3.0f : -1.0f;
    }
    if (t.count < 1 || t.tokens[0].type != TOK_NUMBER) return -1.0f;
    return t.tokens[0].num_val;
}

/* What lit() must return: the host's float, or -2 (refused) when that
 * float is 0 or tiny while the digits are not all 0. */
static float lit_want(const char *s)
{
    float want = (float)strtod(s, NULL);
    const char *p;
    if ((bits_of(want) & 0x7F800000u) != 0u) return want;
    for (p = s; *p && *p != 'e' && *p != 'E'; p++)
        if (*p >= '1' && *p <= '9') return -2.0f;
    return want;
}

static void test_literals(void)
{
    static const char *cases[] = {
        "1E38", "1E39", "1E99", "1E200", "3.5E38", "3.4E38", "3.4028234E38", "3.4028235E38",
        "3.40282356E38", "3.402823567E38", "3.402823568E38", "3.4028236E38", "34028235E31",
        "34028236E31", "0.34028236E39", "0.3402823E39", "1E-37", "1E-38", "1E-45", "1E-46",
        "7E-46", "7.1E-46", "1E-47", "1E-99", "0.0000000001", "0", "0.000", "0E99",
        "000123.4500", ".5", "5.", "1e+10", "1e-0", "123456789", "16777215", "16777217",
        "1.17549435E-38", "1.1754942E-38", "1E-40", "0.00E-50", "00000", "0.0E-99",
    };
    char buf[48];
    unsigned i;
    unsigned long k;
    /* Literals are at most MAX_TOKEN_LEN - 1 = 15 characters (longer ones are
     * cut short by the scanner, a separate issue). */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float want = lit_want(cases[i]);
        float got = lit(cases[i]);
        CHECK(same(got, want), "literal %s: %a, want %a", cases[i], got, want);
    }
    /* A longer literal keeps its first 15 characters. When those read as 0
     * but a cut-off significand digit is nonzero, it is refused, not 0. */
    /* Its only nonzero digit is cut: too long, not tiny (round 9 nit). */
    CHECK(lit("0.0000000000000001") == -3.0f, "cut 0.0000000000000001 refused");
    /* Its exponent is cut too: refused as too long (review round 9). */
    CHECK(lit("00000000000000000001E-50") == -3.0f, "cut 0...01E-50 refused");
    CHECK(bits_of(lit("0.00000000000000000")) == 0u, "long true zero is +0");
    CHECK(bits_of(lit("0000000000000000000E-50")) == 0u, "long true zero with exponent is +0");
    CHECK(lit("3.14159265358979323846") == 3.14159265358979f, "long pi keeps its digits");
    /* The contract's own fixed points (host IEEE conversion). */
    CHECK(lit("1E-38") == -2.0f && lit("1E-45") == -2.0f && lit("1E-99") == -2.0f,
          "nonzero tiny literals are refused");
    CHECK(bits_of(lit("1.17549435E-38")) == 0x00800000u, "FLT_MIN literal is kept");
    CHECK(bits_of(lit("0E99")) == 0u && bits_of(lit("0.000")) == 0u &&
          bits_of(lit("0.0E-99")) == 0u, "zero literals read as +0");
    for (k = 0; k < 200000ul; k++) {
        int nd = 1 + (int)(rnd() % 8u), d, pos = 0;
        int dot = (int)(rnd() % (unsigned)(nd + 1));
        for (d = 0; d < nd; d++) {
            if (d == dot && d > 0) buf[pos++] = '.';
            buf[pos++] = (char)('0' + rnd() % 10u);
        }
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "E%d", (int)(rnd() % 120u) - 60);
        {
            float want = lit_want(buf);
            float got = lit(buf);
            CHECK(same(got, want), "literal %s: %a, want %a", buf, got, want);
        }
    }
}

int main(void)
{
    printf("=== CE-safe float ops (src/ce_float_ops.h) ===\n");
    test_rom_rows();
    test_arith_parity();
    test_pow();
    test_exp_hyp();
    test_literals();
    printf("float_ops_test: %lu checks, %lu failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
