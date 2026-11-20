/*
 * test_ce_float_tiny.c - the tiny-arithmetic refusal contract of the
 * CE-safe float helpers (src/ce_float_ops.h, CE_SAFE_FLOAT_TINY=1, built by
 * CEBASE).
 *
 * The supported range is +-0 and |x| >= FLT_MIN. With the flag the helpers
 *   1. refuse (return 0, set g_ce_fops_tiny) a tiny operand: exponent field
 *      0, mantissa not 0;
 *   2. refuse a result whose exact value is nonzero and below FLT_MIN (a
 *      result that rounds up to exactly FLT_MIN may go either way);
 *   3. otherwise return exactly what the helpers returned without the flag:
 *      for + - * / that is the host's IEEE result bit for bit (the IEEE
 *      parity test_ce_float_ops.c proves for the flag-off helpers), and for
 *      pow/exp the flag-off result, read from a file the flag-off build of
 *      this test writes (argv[1]), so a normal answer cannot change;
 *   4. classify zero from the bytes: -0 is a supported operand, and an
 *      exact zero result of finite operands is +0 (what the CE's native
 *      + - * / return; -0 would print as "-0" through the CE's compare).
 *
 * Built three ways (tests/Makefile, target float-tiny-test):
 *   float_tiny_ref    CE_SAFE_FLOAT_TINY=0: writes the pow/exp reference.
 *   float_tiny_test   CE_SAFE_FLOAT_TINY=1 on host IEEE arithmetic.
 *   float_tiny_flush  the same, with every native operation the helpers
 *                     make (CE_FOPS_NATIVE, powf, expf, sinhf) flushing a
 *                     subnormal result to +0 the way the CE does. The
 *                     contract must hold unchanged: no decision may depend
 *                     on what a native routine does below FLT_MIN.
 *   float_tiny_bias   the flush model with TI-OS powf/expf/sinhf also
 *                     reading about 128 ulp low (TI-OS powf was measured
 *                     1..96 ulp off): a result just above FLT_MIN can then
 *                     flush to 0, which only the helpers' post-checks
 *                     catch. Values differ from the reference, so this
 *                     build checks the contract only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#ifdef TINY_CE_FLUSH
/* The CE flushes an underflowed product, quotient or OS powf/expf result
 * to +0. Model that for every native call. */
static float flush_(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    if ((u & 0x7F800000u) == 0u && (u & 0x007FFFFFu) != 0u) return 0.0f;
    return v;
}
#ifdef TINY_OS_LOW
#define OS_BIAS_ (1.0f - 0x1p-17f)
#else
#define OS_BIAS_ 1.0f
#endif
static float powf_flush_(float a, float b) { return flush_(powf(a, b) * OS_BIAS_); }
static float expf_flush_(float a) { return flush_(expf(a) * OS_BIAS_); }
static float sinhf_flush_(float a) { return flush_(sinhf(a) * OS_BIAS_); }
#define CE_FOPS_NATIVE(expr) flush_(expr)
#define CE_FOPS_NATIVE_IF(checked, expr) flush_(expr)
#define powf(a, b) powf_flush_((a), (b))
#define expf(a) expf_flush_(a)
#define sinhf(a) sinhf_flush_(a)
#endif

#include "../src/eval.c"
#include "parser.h"
#include "memory.h"
/* eval.c's recursion-refusal flag; the CE build defines it in src/split/ipc.c,
 * which this test does not link. */
bool g_eval_refused;
ce_float_t g_eval_refused_ans;

static unsigned long g_checks, g_failed;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        if (g_failed++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } \
} while (0)

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float from_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static int same(float a, float b)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    return bits_of(a) == bits_of(b);
}

/* 1: tiny (exponent field 0, mantissa not 0). */
static int is_tiny(float f)
{
    uint32_t u = bits_of(f);
    return (u & 0x7F800000u) == 0u && (u & 0x007FFFFFu) != 0u;
}

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

#if CE_SAFE_FLOAT_TINY
#define TINY_FLAG() (g_ce_fops_tiny != 0)
#define TINY_CLEAR() (g_ce_fops_tiny = 0)
#else
#define TINY_FLAG() 0
#define TINY_CLEAR() ((void)0)
#endif

/* Values: every exponent field in the ranges that matter, boundary and
 * random mantissas, both signs, and the specials. */
static int build_values(float *out, int cap)
{
    static const uint32_t mant[] = { 0u, 1u, 2u, 0x3FFFFFu, 0x400000u, 0x400001u,
                                     0x513E68u, 0x7FFFFEu, 0x7FFFFFu };
    static const uint32_t extra[] = { 0x01227B19u, 0x00DC56FFu, 0x00689F33u, 0x00513E68u,
                                      0x3F800000u, 0x40000000u, 0x3F000000u, 0x41200000u,
                                      0x7F800000u, 0x7FC00000u, 0x7F7FFFFFu };
    int n = 0;
    uint32_t e, s;
    unsigned i;
    for (s = 0; s < 2; s++) {
        for (e = 0; e < 256; e++) {
            if (!(e <= 70u || (e >= 120u && e <= 134u) || (e >= 186u && e <= 194u) || e >= 250u))
                continue;
            for (i = 0; i < sizeof(mant) / sizeof(mant[0]); i++)
                if (n < cap) out[n++] = from_bits((s << 31) | (e << 23) | mant[i]);
            if (n < cap) out[n++] = from_bits((s << 31) | (e << 23) | (rnd() & 0x7FFFFFu));
        }
        for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
            if (n < cap) out[n++] = from_bits((s << 31) | extra[i]);
    }
    return n;
}

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

/* Exact (+ - *) or correctly rounded to 53 bits (/) value. */
static double exact_op(int op, float a, float b)
{
    switch (op) {
    case 0: return (double)a + (double)b;
    case 1: return (double)a - (double)b;
    case 2: return (double)a * (double)b;
    default: return (double)a / (double)b;
    }
}

#if CE_SAFE_FLOAT_TINY
static unsigned long g_refused, g_answered, g_edge;

static void check_arith(int op, float a, float b)
{
    float got, want = host_op(op, a, b);
    int refused;
    double ex;
    if (op == 3 && !(bits_of(b) & 0x7FFFFFFFu)) return;      /* x/+-0: caller's */
    TINY_CLEAR();
    got = ce_op(op, a, b);
    refused = TINY_FLAG();
    if (refused) g_refused++; else g_answered++;
    CHECK(!refused || bits_of(got) == 0u, "%08X %c %08X refused but returned %08X",
          (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b), (unsigned)bits_of(got));
    if (!isfinite(a) || !isfinite(b)) {
        CHECK(!refused && same(got, want), "%08X %c %08X (inf/NaN): %08X, IEEE %08X",
              (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b),
              (unsigned)bits_of(got), (unsigned)bits_of(want));
        return;
    }
    if (is_tiny(a) || is_tiny(b)) {
        CHECK(refused, "%08X %c %08X: tiny operand not refused (%08X)",
              (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b), (unsigned)bits_of(got));
        return;
    }
    ex = exact_op(op, a, b);
    if (ex == 0.0) {
        CHECK(!refused && bits_of(got) == 0u, "%08X %c %08X: zero result %08X, want +0",
              (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b), (unsigned)bits_of(got));
        return;
    }
    if (fabs(ex) >= (double)FLT_MIN) {
        /* Products/quotients within about 2 ulp of FLT_MAX may count as an
         * overflow (ce_farith's threshold, unchanged by this flag). */
        if (op >= 2 && isinf(got) && isfinite(want) && fabsf(want) >= 0x1.fffff8p127f &&
            signbit(got) == signbit(want)) return;
        CHECK(!refused && same(got, want), "%08X %c %08X: %s %08X, IEEE %08X",
              (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b),
              refused ? "refused" : "got", (unsigned)bits_of(got), (unsigned)bits_of(want));
        return;
    }
    /* 0 < |exact| < FLT_MIN: refused, unless it rounds up to FLT_MIN. */
    if (fabsf(want) == FLT_MIN && !refused) {
        g_edge++;
        CHECK(same(got, want), "%08X %c %08X: edge %08X, IEEE %08X",
              (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b),
              (unsigned)bits_of(got), (unsigned)bits_of(want));
        return;
    }
    CHECK(refused, "%08X %c %08X: result below FLT_MIN not refused (%08X; IEEE %08X)",
          (unsigned)bits_of(a), "+-*/"[op], (unsigned)bits_of(b),
          (unsigned)bits_of(got), (unsigned)bits_of(want));
}

static void test_arith(void)
{
    static float v[4096];
    int n = build_values(v, 4096), i, j, op;
    unsigned long k;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            for (op = 0; op < 4; op++) check_arith(op, v[i], v[j]);
    /* Random normals near the bottom, where the scaled add decides. */
    for (k = 0; k < 2000000ul; k++) {
        uint32_t ea = 1u + rnd() % 40u, eb = 1u + rnd() % 40u;
        float a = from_bits(((rnd() & 1u) << 31) | (ea << 23) | (rnd() & 0x7FFFFFu));
        float b = from_bits(((rnd() & 1u) << 31) | (eb << 23) | (rnd() & 0x7FFFFFu));
        check_arith((int)(rnd() & 3u), a, b);
        /* Close magnitudes: cancellation down to and below FLT_MIN. */
        b = from_bits((bits_of(a) ^ 0x80000000u) + (rnd() % 64u) - 32u);
        if (!is_tiny(b)) check_arith(0, a, b);
    }
    printf("  arith: %lu answered, %lu refused, %lu rounded-up edge answers\n",
           g_answered, g_refused, g_edge);
}

/* Measured CE cases and the zero rules. */
static void test_fixed_rows(void)
{
    static const struct { uint32_t a, b; int op; uint32_t want; int refuse; } rows[] = {
        { 0x01227B19u, 0x00DC56FFu, 1, 0, 1 },   /* the reported difference */
        { 0x00E00000u, 0x00800000u, 1, 0, 1 },
        { 0x00FFFFFFu, 0x00800000u, 1, 0, 1 },
        { 0x00C00000u, 0x00800000u, 1, 0, 1 },
        { 0x00BFFFFFu, 0x00800000u, 1, 0, 1 },
        { 0x00800001u, 0x00800000u, 1, 0, 1 },
        { 0x00800000u, 0x00800000u, 1, 0x00000000u, 0 },   /* exact cancellation */
        { 0x80800000u, 0x80800000u, 1, 0x00000000u, 0 },   /* -x - -x = +0 */
        { 0x01000000u, 0x00800000u, 1, 0x00800000u, 0 },   /* 2^-125-2^-126 */
        { 0x00800000u, 0x00800000u, 0, 0x01000000u, 0 },
        { 0x00400000u, 0x00400000u, 0, 0, 1 },              /* tiny operands */
        { 0x00000001u, 0x00000000u, 0, 0, 1 },
        { 0x00513E68u, 0x3F800000u, 2, 0, 1 },
        { 0x3F800000u, 0x00513E68u, 3, 0, 1 },
        { 0x00800000u, 0x3F000000u, 2, 0, 1 },              /* 2^-126*0.5 */
        { 0x00800000u, 0x40000000u, 3, 0, 1 },              /* 2^-126/2 */
        { 0x00800000u, 0x3F800000u, 2, 0x00800000u, 0 },
        { 0x01000000u, 0x3F000000u, 2, 0x00800000u, 0 },
        { 0x01000000u, 0x40000000u, 3, 0x00800000u, 0 },
        { 0x0DA24268u, 0x4F800000u, 3, 0, 1 },              /* 1E-30/2^32 */
        { 0x00000000u, 0x00000000u, 0, 0x00000000u, 0 },   /* signed zeros */
        { 0x80000000u, 0x80000000u, 0, 0x00000000u, 0 },   /* zero results: +0 */
        { 0x80000000u, 0x00000000u, 0, 0x00000000u, 0 },
        { 0x80000000u, 0x00000000u, 1, 0x00000000u, 0 },
        { 0x00000000u, 0x80000000u, 1, 0x00000000u, 0 },
        { 0x80000000u, 0x40A00000u, 0, 0x40A00000u, 0 },   /* -0 + 5 */
        { 0x40A00000u, 0x80000000u, 1, 0x40A00000u, 0 },   /* 5 - -0 */
        { 0x80000000u, 0xC0A00000u, 1, 0x40A00000u, 0 },   /* -0 - -5 */
        { 0x80000000u, 0x40A00000u, 2, 0x00000000u, 0 },   /* -0 * 5 */
        { 0xBF800000u, 0x00000000u, 2, 0x00000000u, 0 },   /* -1 * 0 (CE: +0) */
        { 0x00000000u, 0xC0A00000u, 3, 0x00000000u, 0 },   /* 0 / -5 */
        { 0x80000000u, 0xC0A00000u, 3, 0x00000000u, 0 },   /* -0 / -5 */
        { 0x7E967699u, 0x7E967699u, 1, 0x00000000u, 0 },   /* 1E38 - 1E38 */
        { 0x4B800000u, 0x3F800000u, 1, 0x4B7FFFFFu, 0 },   /* 16777216-1 */
    };
    unsigned i;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        float got;
        int refused;
        TINY_CLEAR();
        got = ce_op(rows[i].op, from_bits(rows[i].a), from_bits(rows[i].b));
        refused = TINY_FLAG();
        CHECK(refused == rows[i].refuse && (refused || bits_of(got) == rows[i].want),
              "row %u: %08X %c %08X gave %08X refused=%d, want %08X refused=%d", i,
              (unsigned)rows[i].a, "+-*/"[rows[i].op], (unsigned)rows[i].b,
              (unsigned)bits_of(got), refused, (unsigned)rows[i].want, rows[i].refuse);
    }
    /* The flag is sticky until eval_init(). */
    TINY_CLEAR();
    (void)ce_fmul(from_bits(0x00800000u), 0.5f);
    (void)ce_fadd(1.0f, 2.0f);
    CHECK(TINY_FLAG(), "refusal flag cleared by a later operation");
    {
        eval_ctx_t ctx;
        eval_init(&ctx);
        CHECK(!TINY_FLAG(), "eval_init clears the refusal flag");
    }
}
/* Evaluator paths around the helpers (eval.c): a tiny operand of a
 * function, acsc/acot's 1/x, degree-mode scaling, sqrt(-0), a refusal
 * inside a sum. Refused means ctx error CE_ERR_PRECISION with the message,
 * or (a refusal at the root, which processor.c reports) the flag set. */
static int eval_refused(ast_node_t *root, float *out)
{
    eval_ctx_t ctx;
    float v;
    eval_init(&ctx);
    v = eval_expr(&ctx, root);
    if (out) *out = v;
    if (ctx.error != CE_OK)
        return ctx.error == CE_ERR_PRECISION && strcmp(ctx.error_msg, CE_FOPS_TINY_MSG) == 0;
    return TINY_FLAG();
}

/* Parse and evaluate text; 1 when refused (as eval_refused), 0 when it
 * answers, -1 when it does not parse or fails otherwise. */
static int text_refused(const char *s, float *out)
{
    static tokenizer_t tok;
    static parser_t par;
    ast_node_t *root;
    eval_ctx_t ctx;
    float v;
    mem_free_all();
    tok_init(&tok, s);
    if (tok_tokenize(&tok) != CE_OK) return -1;
    parser_init(&par, &tok);
    root = parser_parse(&par);
    if (!root) return -1;
    eval_init(&ctx);
    v = eval_expr(&ctx, root);
    if (out) *out = v;
    if (ctx.error != CE_OK)
        return ctx.error == CE_ERR_PRECISION && strcmp(ctx.error_msg, CE_FOPS_TINY_MSG) == 0 ? 1 : -1;
    return TINY_FLAG() ? 1 : 0;
}

/* Review round 2: arguments of the polar-style functions, whose refusal
 * set only the flag, and their own arithmetic below FLT_MIN (native
 * __fmul/__fdiv flush that to +0 on the CE; the host's subnormal would be
 * caught only by processor.c, so these check eval itself). */
/* Review round 4: ft_sample_'s accounting and ft_drops_ok_'s margin,
 * checked directly (a row's verdict can hide a wrong weight). */
static ast_node_t *parse_(const char *s)
{
    static tokenizer_t tok;
    static parser_t par;
    mem_free_all();
    tok_init(&tok, s);
    if (tok_tokenize(&tok) != CE_OK) return NULL;
    parser_init(&par, &tok);
    return parser_parse(&par);
}

static void test_eval_sample_unit(void)
{
    static const struct { const char *s; float w, mass, val; int err; } rows[] = {
        { "0.5^200",     256.0f, 256.0f, 0.0f, 0 },   /* own mark, bound 1 */
        { "0.5^200*3",   2.0f,   6.0f,   0.0f, 0 },   /* bound * |o| */
        { "0.5^200/4",   8.0f,   2.0f,   0.0f, 0 },   /* bound / |o| */
        { "-(0.5^200)",  2.0f,   2.0f,   0.0f, 0 },
        { "1+0.5^200",   4.0f,   0.0f,   1.0f, 0 },   /* exact: discharged */
        { "0.5^200-2",   1.0f,   0.0f,  -2.0f, 0 },
        { "1E-37+0.5^200", 4.0f, 4.0f,   1E-37f, 0 }, /* absorbed: value o */
        { "0.5^200-1E-37", 2.0f, 2.0f,  -1E-37f, 0 },
        { "exp(-200)",   1.0f,   1.0f,   0.0f, 0 },
        { "2+3",         1.0f,   0.0f,   5.0f, 0 },   /* nothing dropped */
        { "distance(0,0,1E-20,0)", 1.0f, 0.0f, 0.0f, 1 },  /* no mark: reported */
        { "sqrt(0.5^200)", 1.0f, 0.0f,   0.0f, 1 },   /* below the root */
        { "(1+0.5^200)*2", 1.0f, 0.0f,   2.0f, 0 },   /* discharged: used as usual */
        { "(1E-37+0.5^200)*2", 1.0f, 0.0f, 0.0f, 1 }, /* absorbed passes no further */
        /* review round 5 */
        { "0.5^200*cos(0)", 4.0f, 4.0f,  0.0f, 0 },   /* later sibling: as usual */
        { "0.5^200/(1+1)", 4.0f, 2.0f,   0.0f, 0 },
        { "0.5^200*0.5^200", 1.0f, 0.0f, 0.0f, 1 },   /* a second mark */
        { "0.5^200*1E30*1E30", 1.0f, 0.0f, 0.0f, 1 }, /* the bound overflows */
        { "geomsum(0.5^200,2,3)", 1.0f, 0.0f, 0.0f, 1 }, /* used by a function */
    };
    unsigned i;
    eval_ctx_t ctx;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        ast_node_t *root = parse_(rows[i].s);
        float v;
        CHECK(root != NULL, "%s parses", rows[i].s);
        if (!root) continue;
        eval_init(&ctx);
        v = ft_sample_(&ctx, root, rows[i].w);
        if (rows[i].err) {
            CHECK(ctx.error == CE_ERR_PRECISION && TINY_FLAG(), "%s: refused (%d)", rows[i].s,
                  (int)ctx.error);
        } else {
            CHECK(ctx.error == CE_OK && !TINY_FLAG(), "%s: kept (%d)", rows[i].s, (int)ctx.error);
            CHECK(s_tiny_mass == rows[i].mass, "%s: mass %g, want %g", rows[i].s,
                  (double)s_tiny_mass, (double)rows[i].mass);
            CHECK(v == rows[i].val, "%s: value %g, want %g", rows[i].s, (double)v,
                  (double)rows[i].val);
        }
        CHECK(s_in_sample == 0, "%s: sample depth restored", rows[i].s);
    }
    /* A refusal held before the sample survives it. */
    {
        ast_node_t *root = parse_("0.5^200");
        eval_init(&ctx);
        g_ce_fops_tiny = 1;
        (void)ft_sample_(&ctx, root, 1.0f);
        CHECK(TINY_FLAG(), "a held refusal survives a dropped sample");
    }
    /* The margin: mass * 2^-125 <= |scale| * 2^-25. */
    eval_init(&ctx);
    s_tiny_mass = 1.0f;
    CHECK(ft_drops_ok_(0x1p-99f) && !TINY_FLAG() && s_tiny_mass == 0.0f, "margin 2^-99 kept");
    s_tiny_mass = 1.0f;
    CHECK(!ft_drops_ok_(0x1p-100f) && TINY_FLAG() && s_tiny_mass == 0.0f, "margin 2^-100 refused");
    eval_init(&ctx);
    s_tiny_mass = 0x1p-60f;
    CHECK(!ft_drops_ok_(0.0f) && TINY_FLAG(), "a 0 result with drops is refused");
    eval_init(&ctx);
    s_tiny_mass = INFINITY;
    CHECK(!ft_drops_ok_(1E30f) && TINY_FLAG(), "an inf mass is refused");
    eval_init(&ctx);
    CHECK(ft_drops_ok_(0.0f) && !TINY_FLAG(), "no drops: any result kept");
    eval_init(&ctx);
}

static void test_eval_polar(void)
{
    static const struct { const char *s; int deg; int want; float val; } rows[] = {
        { "geomsum(tan(1E-37),3,33)", 1, 1, 0 },
        { "geomsum(sin(1E-37),2,40)", 1, 1, 0 },
        { "geomsum(sech(88.5),0.5,40)", 0, 1, 0 },
        { "arithsum(sech(88.5),1,3)", 0, 1, 0 },
        { "distance(0,sech(88.5),1,1)", 0, 1, 0 },
        { "midpoint(sech(88.5),0,1,1)", 0, 1, 0 },
        { "slope(0,0,1E30,1E-10)", 0, 1, 0 },
        { "distance(0,0,1E-20,0)", 0, 1, 0 },
        { "midpoint(2E-38,0,-1.5E-38,0)", 0, 1, 0 },
        { "geomsum(2E-38,-0.9,2)", 0, 1, 0 },            /* 2e-38 - 1.8e-38 */
        /* review round 3: r^n below FLT_MIN leaves 1 - r^n = 1 */
        { "0+geomsum(3,0.1,40)", 0, 0, 3.3333333f },
        { "2*geomsum(1,0.01,20)", 0, 0, 2.0202020f },
        { "0+geomsum(1,0.5,127)", 0, 0, 2.0f },
        /* one square below FLT_MIN against a normal one is dropped */
        { "0+distance(0,0,1E-20,1)", 0, 0, 1.0f },
        { "1+distance(0,0,1,1E-25)", 0, 0, 2.0f },
        { "0+distance(0,0,1E-20,1E-20)", 0, 1, 0 },
        /* a central difference of two normals below FLT_MIN */
        { "0+nderiv(1E-36*(x+1),x,0)", 0, 1, 0 },
        { "0+tangent(1E-36*(x+1),x,0)", 0, 1, 0 },
        { "0+tangent(x^2,x,3)", 0, 0, 5.9995642f },
        /* an argument's refusal comes before a later domain error */
        { "geomsum(tan(1E-37),3,1.5)", 1, 1, 0 },
        { "arithsum(tan(1E-37),1,1.5)", 1, 1, 0 },
        { "geomsum(1E-20,1,1E-18)", 0, -1, 0 },          /* n not an integer */
        { "geomsum(1E-30,1,1E-8)", 0, -1, 0 },
        { "geomsum(1E-30,2,30)", 0, 0, 1.0737418e-21f },
        { "arithsum(2E-38,2E-38,2)", 0, 0, 6E-38f },
        { "geomsum(2,0.5,40)", 0, 0, 4.0f },
        { "distance(0,0,3,4)", 0, 0, 5.0f },
        { "slope(0,0,2,4)", 0, 0, 2.0f },
        { "midpoint(1,2,3,4)", 0, 0, 2.0f },
        /* review round 4: samples of int, sum, d/dx and lim whose own
         * value is below FLT_MIN (flushed here, as on the CE) */
        { "1+int(x^20,x,0,1)", 0, 0, 1.0476191f },
        { "1+int(x*exp(-x^2),x,-20,20)", 0, 0, 1.0f },
        { "1+sum(0.5^n,n,1,200)", 0, 0, 2.0f },
        { "1+sum(0.5^k/k,k,1,200)", 0, 0, 1.6931472f },
        { "1+sum(0.5^k+1,k,1,200)", 0, 0, 202.0f },
        { "2*nderiv(x^4,x,4)", 0, 0, 48.0f },
        { "0+sum(3E-38*k/7,k,1,50)", 0, 1, 0 },
        { "1+lim(x^7,x,0)", 0, 1, 0 },
        { "0+sum(1E36*0.5^k,k,126,130)", 0, 1, 0 },
        { "1+sum(0.5^k*(k+1),k,1,200)", 0, 0, 4.0f },
        { "0+sum(e^(-100*k)*1E30*1E30+1E9,k,1,1)", 0, 1, 0 },
        { "1+sum(sqrt(1E-20*1E-20)+k,k,1,3)", 0, 1, 0 },
    };
    unsigned i;
    float v = 0;
    settings_t saved = g_settings;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int got;
        g_settings.angle_mode = rows[i].deg ? ANGLE_DEGREES : ANGLE_RADIANS;
        got = text_refused(rows[i].s, &v);
        CHECK(got == rows[i].want, "%s: %d, want %d (%g)", rows[i].s, got, rows[i].want, (double)v);
        if (got == 0 && rows[i].want == 0)
            CHECK(fabsf(v - rows[i].val) <= fabsf(rows[i].val) * 1e-6f, "%s = %g, want %g",
                  rows[i].s, (double)v, (double)rows[i].val);
    }
    g_settings = saved;
    /* So must to_rad_even (eval_node's entry check keeps the flag clear
     * on its one path today; this pins the helper's own rule). */
    g_settings.angle_mode = ANGLE_DEGREES;
    g_ce_fops_tiny = 1;
    v = to_rad_even(1E-37f);
    CHECK(TINY_FLAG() && v == 0.0f, "to_rad_even keeps an earlier refusal");
    g_ce_fops_tiny = 0;
    v = to_rad_even(1E-37f);
    CHECK(!TINY_FLAG() && v == 0.0f, "to_rad_even drops its own refusal");
    g_settings = saved;
    /* The round-3 helpers, directly: each keeps a refusal made before it
     * (their callers check the arguments first, so the parser cannot reach
     * this) and keeps the refusal of a tiny r. */
    g_ce_fops_tiny = 1;
    v = ft_pow_rn_(0.1f, 40.0f);
    CHECK(TINY_FLAG() && v == 0.0f, "ft_pow_rn_ keeps an earlier refusal");
    g_ce_fops_tiny = 0;
    v = ft_pow_rn_(from_bits(0x00400000u), 2.0f);
    CHECK(TINY_FLAG(), "ft_pow_rn_ keeps a tiny r's refusal");
    g_ce_fops_tiny = 0;
    v = ft_pow_rn_(0.1f, 40.0f);
    CHECK(!TINY_FLAG() && v == 0.0f, "ft_pow_rn_ drops r^n below FLT_MIN");
    g_ce_fops_tiny = 1;
    v = ft_hyp2_(1E-20f, 1.0f);
    CHECK(TINY_FLAG() && v == 1.0f, "ft_hyp2_ keeps an earlier refusal");
    g_ce_fops_tiny = 0;
    /* Richardson's 4*d2 - d1 below FLT_MIN (d1 one ulp above 4*d2). */
    v = ft_richardson_(2E-38f, from_bits(bits_of(4.0f * 2E-38f) + 1u));
    CHECK(TINY_FLAG(), "Richardson step below FLT_MIN refused (%a)", (double)v);
    g_ce_fops_tiny = 0;
    v = ft_richardson_(1.5f, 2.0f);
    CHECK(!TINY_FLAG() && v == (4.0f * 1.5f - 2.0f) / 3.0f, "Richardson step unchanged");
    g_ce_fops_tiny = 0;
    /* lim's extrapolation and two-sided mean (review round 9): from
     * normal values each can fall below FLT_MIN, which __fdiv flushes to
     * 0 on the CE. 10*(1e-36+5e-39) - (1e-35+5e-39) = 4.5e-38, /9 = 5e-39. */
    v = ft_lim_rich_(1.005E-36f, 1.0005E-35f);
    CHECK(TINY_FLAG() && v == 0.0f, "lim extrapolation below FLT_MIN refused (%a)", (double)v);
    g_ce_fops_tiny = 0;
    v = ft_lim_rich_(1.5f, 2.0f);
    CHECK(!TINY_FLAG() && v == (10.0f * 1.5f - 2.0f) / 9.0f, "lim extrapolation unchanged");
    v = ft_lim_rich_(-2.5f, -2.5f);
    CHECK(!TINY_FLAG() && v == -2.5f, "lim extrapolation of equal samples unchanged");
    v = ft_mean2_(0x1p-126f, 0.0f);
    CHECK(TINY_FLAG() && v == 0.0f, "mean FLT_MIN/2 refused (%a)", (double)v);
    g_ce_fops_tiny = 0;
    v = ft_mean2_(0x1p-125f, from_bits(0x80000000u));
    CHECK(!TINY_FLAG() && v == 0x1p-126f, "mean (2^-125 + -0)/2 = FLT_MIN");
    v = ft_mean2_(3.0f, -3.0f);
    CHECK(!TINY_FLAG() && bits_of(v) == 0u, "mean of exact opposites is +0");
    v = ft_mean2_(1.5f, 2.5f);
    CHECK(!TINY_FLAG() && v == 2.0f, "mean unchanged");
    /* A power-of-1 drop must keep a refusal made before it. */
    mem_free_all();
    g_ce_fops_tiny = 1;
    v = ce_fpow(2.0f, 1.2E-38f);
    CHECK(TINY_FLAG() && v == 1.0f, "pow keeps an earlier refusal");
    g_ce_fops_tiny = 0;
    /* A t in [FOPS_LN_MIN, ln FLT_MIN) passes the pre-check; the exponent
     * check after powf must refuse it (review round 2, P3-1). */
    v = ce_fpow(2.0f, -126.00001f);
    CHECK(TINY_FLAG(), "2^-126.00001 refused (%a)", (double)v);
    g_ce_fops_tiny = 0;
    v = ce_fexpfn(-87.33655f, FN_EXP);
    CHECK(TINY_FLAG(), "exp(-87.33655) refused (%a)", (double)v);
    g_ce_fops_tiny = 0;
}

static void test_eval_level(void)
{
    static const struct { int fn; uint32_t arg; int refuse; } fns[] = {
        { FN_SQRT, 0x00513E68u, 1 }, { FN_LN, 0x00513E68u, 1 }, { FN_SIN, 0x00000001u, 1 },
        { FN_ABS, 0x80400000u, 1 }, { FN_ACOT, 0x7EE1C61Fu, 1 }, { FN_ACSC, 0x7EE1C61Fu, 1 },
        { FN_ACOT, 0x7E167699u, 0 }, { FN_EXP, 0xC2B00000u, 1 }, { FN_EXP, 0xC2AE0000u, 0 },
        { FN_SQRT, 0x00800000u, 0 }, { FN_SQRT, 0x80000000u, 0 }, { FN_SQRT, 0x00000000u, 0 },
        /* sech/csch near |a| = 88.5: 1/cosh is below FLT_MIN. The native
         * 1/c here flushed to +0 on the CE (review round 1). */
        { FN_SECH, 0x42B10000u, 1 }, { FN_CSCH, 0xC2B10000u, 1 }, { FN_SECH, 0x42AE0000u, 0 },
    };
    unsigned i;
    float v;
    settings_t saved = g_settings;
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        ast_node_t *root;
        int refused;
        mem_free_all();
        root = ast_func((func_id_t)fns[i].fn, ast_number(from_bits(fns[i].arg)));
        refused = eval_refused(root, &v);
        CHECK(refused == fns[i].refuse, "fn %d(%08X): refused=%d, want %d (value %08X)",
              fns[i].fn, (unsigned)fns[i].arg, refused, fns[i].refuse, (unsigned)bits_of(v));
    }
    /* sqrt(+-0) is +0, not a domain error and not -0. */
    mem_free_all();
    CHECK(!eval_refused(ast_func(FN_SQRT, ast_number(-0.0f)), &v) && bits_of(v) == 0u,
          "sqrt(-0) = +0 (%08X)", (unsigned)bits_of(v));
    /* A tiny operand of negation (an old Ans, say) is refused too. */
    mem_free_all();
    CHECK(eval_refused(ast_unop(UOP_NEG, ast_number(from_bits(0x00400000u))), &v),
          "-tiny refused");
    /* Degree mode: 1E-37 degrees is 1.7e-39 radians, below FLT_MIN. */
    g_settings.angle_mode = ANGLE_DEGREES;
    mem_free_all();
    CHECK(eval_refused(ast_func(FN_SIN, ast_number(1E-37f)), &v), "degree sin(1E-37) refused");
    /* cos and sec of it are exactly 1 (review round 1). */
    mem_free_all();
    CHECK(!eval_refused(ast_func(FN_COS, ast_number(-1E-37f)), &v) && v == 1.0f,
          "degree cos(-1E-37) = 1 (%g)", (double)v);
    mem_free_all();
    CHECK(!eval_refused(ast_func(FN_SEC, ast_number(1E-37f)), &v) && v == 1.0f,
          "degree sec(1E-37) = 1 (%g)", (double)v);
    mem_free_all();
    CHECK(!eval_refused(ast_func(FN_SIN, ast_number(1E-30f)), &v) && fabsf(v - 1.7453293e-32f) < 1e-37f,
          "degree sin(1E-30) answered (%g)", (double)v);
    g_settings = saved;
    /* A refused term stops a sum, and the error is the refusal. */
    mem_free_all();
    CHECK(eval_refused(ast_binop(OP_ADD, ast_number(1.0f),
              ast_binop(OP_MUL, ast_number(1E-20f), ast_number(1E-20f))), &v), "1+1E-20*1E-20 refused");
    /* A running total below FLT_MIN (normal terms 2E-38 and -1.5E-38) is an
     * error of the sum itself, also at the root: the loop must not finish
     * and return 0 with only the flag set. */
    {
        static tokenizer_t tok;
        static parser_t par;
        eval_ctx_t ctx;
        ast_node_t *root;
        mem_free_all();
        tok_init(&tok, "sum(2E-38-3.5E-38*(k-1),k,1,2)");
        CHECK(tok_tokenize(&tok) == CE_OK, "sum input tokenizes");
        parser_init(&par, &tok);
        root = parser_parse(&par);
        CHECK(root && root->type == NODE_SUM, "sum input parses");
        if (root) {
            eval_init(&ctx);
            (void)eval_expr(&ctx, root);
            CHECK(ctx.error == CE_ERR_PRECISION && strcmp(ctx.error_msg, CE_FOPS_TINY_MSG) == 0,
                  "tiny running total is the sum's error (%d)", (int)ctx.error);
        }
    }
    /* An operator's refusal is an evaluation error at that node, even at the
     * root: eval_expr's caller need not know about the flag, and a later
     * eval_init() (which clears it) cannot drop the refusal. */
    {
        eval_ctx_t ctx;
        mem_free_all();
        eval_init(&ctx);
        (void)eval_expr(&ctx, ast_binop(OP_MUL, ast_number(1E-20f), ast_number(1E-20f)));
        CHECK(ctx.error == CE_ERR_PRECISION && strcmp(ctx.error_msg, CE_FOPS_TINY_MSG) == 0,
              "root operator refusal sets the eval error (%d)", (int)ctx.error);
    }
}
#endif /* CE_SAFE_FLOAT_TINY */

/* pow/exp/sinh records, compared with the flag-off build (argv[1]). */
typedef struct { uint32_t r; uint8_t refused, tiny_operand; } rec_t;

static int build_pow_bases(float *out, int cap)
{
    static const uint32_t extra[] = { 0x00513E68u, 0x00000001u, 0x007FFFFFu, 0x00800000u,
                                      0x01000000u, 0x0DA24268u, 0x00A00000u };
    static const float base_list[] = { 0.0f, 1.0f, 2.0f, 10.0f, 0.5f, 0.1f, 1E20f, 1E-20f,
                                        3.0f, 1.5f, 0.999f, 1.001f, 1E-30f, 1E-37f, 2E-38f,
                                        65536.0f, 1E30f, 7.0f, 0.25f, 1.17549435E-38f };
    int n = 0;
    unsigned i;
    uint32_t e;
    for (i = 0; i < sizeof(base_list) / sizeof(base_list[0]); i++) {
        if (n < cap) out[n++] = base_list[i];
        if (n < cap) out[n++] = -base_list[i];
    }
    for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        if (n < cap) out[n++] = from_bits(extra[i]);
        if (n < cap) out[n++] = from_bits(extra[i] | 0x80000000u);
    }
    for (e = 1; e < 254u; e += 3u)
        if (n < cap) out[n++] = from_bits((e << 23) | (rnd() & 0x7FFFFFu));
    return n;
}

static int build_pow_exps(float *out, int cap)
{
    static const float list[] = { 0.5f, -0.5f, 1.5f, -2.5f, 0.25f, 1E-3f, 100.5f, -100.5f,
                                  1000.0f, -1000.0f, 126.0f, -126.0f, 127.0f, -127.0f,
                                  149.0f, -149.0f, 1E10f, -1E10f, 0.0f, -0.0f };
    static const uint32_t extra[] = { 0x00513E68u, 0x00000001u, 0x7F800000u, 0xFF800000u };
    int n = 0, k;
    unsigned i;
    for (k = -40; k <= 40; k++)
        if (n < cap) out[n++] = (float)k;
    for (i = 0; i < sizeof(list) / sizeof(list[0]); i++)
        if (n < cap) out[n++] = list[i];
    for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        if (n < cap) out[n++] = from_bits(extra[i]);
    return n;
}

static int build_exp_args(float *out, int cap)
{
    int n = 0;
    double x;
    uint32_t u;
    for (x = -120.0; x <= 120.0; x += 0.0625)
        if (n < cap) out[n++] = (float)x;
    /* Around ln(FLT_MIN) = -87.336545 and the refusal bound. */
    for (u = 0xC2AEAC00u; u <= 0xC2AEAD80u; u++)
        if (n < cap) out[n++] = from_bits(u);
    if (n < cap) out[n++] = -INFINITY;
    if (n < cap) out[n++] = 1E-30f;
    if (n < cap) out[n++] = -1E-30f;
    if (n < cap) out[n++] = 1.17549435E-38f;
    if (n < cap) out[n++] = -0.0f;
    return n;
}

static rec_t *g_recs;
static size_t g_nrec, g_caprec;

static void rec_add(float r, int refused, int tiny_operand)
{
    if (g_nrec == g_caprec) {
        g_caprec = g_caprec ? g_caprec * 2u : 4096u;
        g_recs = (rec_t *)realloc(g_recs, g_caprec * sizeof(rec_t));
        if (!g_recs) { printf("out of memory\n"); exit(2); }
    }
    g_recs[g_nrec].r = bits_of(r);
    g_recs[g_nrec].refused = (uint8_t)refused;
    g_recs[g_nrec].tiny_operand = (uint8_t)tiny_operand;
    g_nrec++;
}

/* True |base|^expo, with a ~1e-12 relative error at most. */
static double true_pow(float base, float expo)
{
    return pow(fabs((double)base), (double)expo);
}

static void run_pow_exp(void)
{
    static float bases[512], exps[256], xs[8192];
    int nb, ne = build_pow_exps(exps, 256);
    int nx, i, j;
    unsigned long refused_n = 0;
    (void)refused_n;
    nb = (g_rng = 0x2545F491u, build_pow_bases(bases, 512));   /* same in both builds */
    nx = build_exp_args(xs, 8192);
    for (i = 0; i < nb; i++) {
        for (j = 0; j < ne; j++) {
            float b = bases[i], e = exps[j], got;
            int refused;
            TINY_CLEAR();
            got = ce_fpow(b, e);
            refused = TINY_FLAG();
            rec_add(got, refused, is_tiny(b) || is_tiny(e));
#if CE_SAFE_FLOAT_TINY
            refused_n += (unsigned long)refused;
            CHECK(!refused || !(bits_of(got) & 0x7FFFFFFFu), "pow %08X^%08X refused but returned %08X",
                  (unsigned)bits_of(b), (unsigned)bits_of(e), (unsigned)bits_of(got));
            if (is_tiny(b) || is_tiny(e)) {
                CHECK(refused, "pow %08X^%08X: tiny operand not refused",
                      (unsigned)bits_of(b), (unsigned)bits_of(e));
            } else if (!(bits_of(b) & 0x7FFFFFFFu) || !isfinite(e) || !isfinite(b) ||
                       (b < 0.0f && e != floorf(e))) {
                CHECK(!refused, "pow %08X^%08X: zero/inf/NaN case refused",
                      (unsigned)bits_of(b), (unsigned)bits_of(e));
            } else {
                double t = true_pow(b, e);
                if (t != 0.0 && t < (double)FLT_MIN * (1.0 - 1e-4)) {
                    CHECK(refused, "pow %08X^%08X: |result| %g below FLT_MIN not refused (%08X)",
                          (unsigned)bits_of(b), (unsigned)bits_of(e), t, (unsigned)bits_of(got));
                } else if (t >= (double)FLT_MIN * (1.0 + 1e-4)) {
                    CHECK(!refused, "pow %08X^%08X: normal result %g refused",
                          (unsigned)bits_of(b), (unsigned)bits_of(e), t);
                }
                /* A nonzero base to a finite power is never 0 or tiny. */
                CHECK(refused || isnan(got) || ce_fops_class(got) == 2u,
                      "pow %08X^%08X answered %08X (0 or tiny)", (unsigned)bits_of(b),
                      (unsigned)bits_of(e), (unsigned)bits_of(got));
            }
#endif
        }
    }
    for (i = 0; i < nx; i++) {
        float x = xs[i], got;
        int refused, fn;
        for (fn = 0; fn < 2; fn++) {
            TINY_CLEAR();
            got = fn ? ce_fsinh(x) : ce_fexp(x);
            refused = TINY_FLAG();
            rec_add(got, refused, is_tiny(x));
#if CE_SAFE_FLOAT_TINY
            refused_n += (unsigned long)refused;
            if (fn == 0 && isfinite(x)) {
                double t = exp((double)x);
                if (t < (double)FLT_MIN * (1.0 - 1e-4)) {
                    CHECK(refused, "exp(%a) = %g not refused", x, t);
                } else if (t >= (double)FLT_MIN * (1.0 + 1e-4)) {
                    CHECK(!refused, "exp(%a) = %g refused", x, t);
                }
                CHECK(refused || ce_fops_class(got) == 2u, "exp(%a) answered %08X (0 or tiny)",
                      x, (unsigned)bits_of(got));
            } else if (fn == 0) {
                CHECK(!refused && bits_of(got) == 0u, "exp(-inf) must be +0, not refused");
            } else {
#ifdef TINY_OS_LOW
                /* The low model reads sinh(FLT_MIN) as tiny: refusing it is right. */
                CHECK(!refused || fabsf(x) < FLT_MIN * 1.0001f, "sinh(%a) refused", x);
#else
                CHECK(!refused, "sinh(%a) refused", x);
#endif
                CHECK(refused || !(bits_of(x) & 0x7FFFFFFFu) || ce_fops_class(got) == 2u,
                      "sinh(%a) answered %08X (0 or tiny)", x, (unsigned)bits_of(got));
            }
#endif
        }
    }
#if CE_SAFE_FLOAT_TINY
    printf("  pow/exp/sinh: %lu records, %lu refused\n", (unsigned long)g_nrec, refused_n);
#endif
}

#if CE_SAFE_FLOAT_TINY
/* Every answered record must equal the flag-off build's, bit for bit. */
static void compare_with_reference(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t i, n = 0, refused_lost = 0, refused_kept = 0, refused_operand = 0;
    rec_t r;
    CHECK(f != NULL, "cannot open reference %s", path);
    if (!f) return;
    for (i = 0; i < g_nrec && fread(&r, sizeof(r), 1, f) == 1; i++, n++) {
        CHECK(g_recs[i].tiny_operand == r.tiny_operand, "record %lu: inputs differ", (unsigned long)i);
        if (g_recs[i].refused) {
            /* What the flag refuses, the flag-off build answered from a
             * tiny operand (anything), with 0 or a tiny value (a loss on
             * the CE), or with a normal on the conservative band just
             * above FLT_MIN (powf path, t < ln(FLT_MIN) + 5e-5). */
            float v = fabsf(from_bits(r.r));
            if (g_recs[i].tiny_operand) refused_operand++;
            else if ((r.r & 0x7F800000u) == 0u) refused_lost++;
            else {
                refused_kept++;
                CHECK(v <= FLT_MIN * 1.0001f, "record %lu: refused a normal %08X off the band",
                      (unsigned long)i, (unsigned)r.r);
            }
            continue;
        }
        CHECK(g_recs[i].r == r.r || (isnan(from_bits(g_recs[i].r)) && isnan(from_bits(r.r))),
              "record %lu: %08X, flag-off %08X", (unsigned long)i,
              (unsigned)g_recs[i].r, (unsigned)r.r);
    }
    fclose(f);
    CHECK(n == g_nrec, "reference has %lu records, want %lu", (unsigned long)n,
          (unsigned long)g_nrec);
    printf("  vs flag-off: %lu records; refused %lu tiny-operand, %lu flag-off zero/tiny, "
           "%lu band (|x| <= 1.0001 FLT_MIN) answers\n", (unsigned long)n,
           (unsigned long)refused_operand, (unsigned long)refused_lost, (unsigned long)refused_kept);
}
#endif

int main(int argc, char **argv)
{
#if CE_SAFE_FLOAT_TINY
    printf("=== CE tiny-arithmetic refusal (CE_SAFE_FLOAT_TINY)%s ===\n",
#if defined(TINY_OS_LOW)
           ", CE flush model, TI-OS pow/exp/sinh 128 ulp low"
#elif defined(TINY_CE_FLUSH)
           ", CE flush model"
#else
           ""
#endif
           );
    test_fixed_rows();
    test_eval_level();
    test_eval_polar();
    test_eval_sample_unit();
    test_arith();
    run_pow_exp();
#ifdef TINY_OS_LOW
    (void)argc; (void)argv;
    printf("  (OS-low model: contract only, no reference comparison)\n");
#else
    if (argc > 1) compare_with_reference(argv[1]);
    else CHECK(0, "usage: %s <flag-off reference file>", argv[0]);
#endif
    printf("float_tiny_test: %lu checks, %lu failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
#else
    FILE *f;
    if (argc < 2) { printf("usage: %s <output file>\n", argv[0]); return 2; }
    (void)argv;
    run_pow_exp();
    f = fopen(argv[1], "wb");
    if (!f || fwrite(g_recs, sizeof(rec_t), g_nrec, f) != g_nrec) {
        printf("cannot write %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    printf("float_tiny_ref: %lu pow/exp/sinh records written\n", (unsigned long)g_nrec);
    (void)g_checks; (void)g_failed;
    return 0;
#endif
}
