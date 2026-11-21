/*
 * split_cebase_test.c — desktop check of the CEBASE module's processor
 * path, compiled with the PROGRAM_CFLAGS from split/Makefile.base (the
 * tests/Makefile `split-cebase` target extracts them, so the flags cannot
 * drift from the .8xp build).
 *
 * CEBASE is built with CE_PROC_GENERIC=1, CE_PROC_PARSE_INPUT=0 and every
 * domain processor off. The launcher parses the input and sends the AST
 * over astwire, so this test does the same: parse, serialize, deserialize,
 * then ce_processor_run_preparsed(), as src/split/module_main.c does.
 *
 * Regression: hex(255) printed "= 255" on the CE because no
 * base-conversion display existed.
 * Regression: "16777215" printed "= inf" on the CE because CEdev's isinf()
 * misfires; the build defines CE_FLOAT_CLASS_REDIRECT so the CE-safe
 * ce_isnan/ce_isinf/ce_isfinite run here too.
 * Regression: "x^2*sin(x)+x" showed "Module CEBASE returned invalid IPC
 * data" on the CE. The generic processor answered any expression with a
 * free variable with an ok=2 action menu whose magic/version were zero, so
 * the launcher's ce_response_read() rejected it, and the CE launcher has no
 * menu for it anyway. Every response must now pass ce_response_read(), and
 * the split modules answer with "<var> has no value".
 */
#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "parser.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/processor.h"
#include "eval.h"
#include "ast.h"
#include <math.h>
#include <stdint.h>

#include <fileioc.h>
#include <tice.h>
#include <stdio.h>
#include <string.h>

#if !defined(HW_BUILD_SPLIT) || CE_PROC_PARSE_INPUT || !CE_PROC_GENERIC || CE_PROC_ALG || CE_PROC_CALC \
    || CE_PROC_STAT || CE_PROC_FIN || CE_PROC_MATRIX || CE_PROC_CMPLX || CE_PROC_GRAPH \
    || CE_PROC_USE_CLASSIFY || CE_EVAL_ENABLE_NODE_STAT || CE_EVAL_ENABLE_NODE_LIST \
    || CE_EVAL_ENABLE_ADV_POLAR || CE_EVAL_ENABLE_POLYDIV || CE_AST_ENABLE_STAT_NAMES \
    || CE_PROC_ALG_BASIC || CE_PROC_ALG_SOLVE || CE_PROC_ALG_ADV || CE_PROC_CALC_BASE \
    || CE_PROC_CALC_DERIV || CE_PROC_CALC_TANGENT || CE_PROC_CALC_LIMIT || CE_PROC_CALC_INT \
    || CE_PROC_CALC_INT_SYMBOLIC || CE_PROC_CALC_INT_POLAR || CE_PROC_CALC_SERIES \
    || CE_PROC_CALC_SERIES_TF || CE_PROC_CALC_SERIES_SEQ
#error "split_cebase_test.c must be built with the split/Makefile.base PROGRAM_CFLAGS"
#endif

/* ipc.c links os_RunPrgm; tice_mock.c also defines the hooks that
 * runtime_globals.c provides, so stub only this one here. */
int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t callback)
{
    (void)prgm; (void)data; (void)size; (void)callback;
    return -1;
}

static int g_run = 0;
static int g_fail = 0;

static void check(int cond, const char *what, const char *in, const char *got)
{
    g_run++;
    if (!cond) {
        g_fail++;
        printf("FAIL: %s [%s] got \"%s\"\n", what, in, got ? got : "(null)");
    }
}

static bool run_like_module(const char *input, ce_response_t *resp)
{
    static tokenizer_t tok;
    static parser_t par;
    static uint8_t wire[CE_AST_WIRE_MAX];
    uint16_t wire_size = 0;
    ast_node_t *parsed;
    ast_node_t *root;

    mem_free_all();
    memset(resp, 0, sizeof(*resp));
    tok_init(&tok, input);
    if (tok_tokenize(&tok) != CE_OK) return false;
    parser_init(&par, &tok);
    parsed = parser_parse(&par);
    if (!parsed) return false;
    if (!ce_ast_serialize(parsed, wire, sizeof(wire), &wire_size)) return false;
    mem_free_all();
    root = ce_ast_deserialize(wire, wire_size);
    if (!root) return false;
    return ce_processor_run_preparsed(input, root, false, resp);
}

/* What the launcher does with a worker's answer: CE_RESP must round-trip
 * through ce_response_write()/ce_response_read() (magic, version, size).
 * The real reader decides; there is no separate header pre-check here. */
static int launcher_accepts(const ce_response_t *resp)
{
    static ce_response_t back;
    int ok;
    ce_response_delete();
    if (!ce_response_write(resp)) return 0;
    memset(&back, 0, sizeof(back));
    ok = ce_response_read(&back) && back.ok == resp->ok &&
         strcmp(back.result, resp->result) == 0 &&
         strcmp(back.error, resp->error) == 0;
    ce_response_delete();
    return ok;
}

/* First result line must equal `want` exactly (the launcher shows line 1). */
static int first_line_is(const char *result, const char *want)
{
    size_t n = strlen(want);
    return strncmp(result, want, n) == 0 && (result[n] == '\n' || result[n] == '\0');
}

/* Repeat `unit` n times around `mid`, then `close` n times (127-char max). */
static const char *nest(const char *unit, int n, const char *mid, const char *close)
{
    static char buf[MAX_INPUT_LEN + 64];
    size_t pos = 0;
    int i;
    for (i = 0; i < n; i++) { strcpy(buf + pos, unit); pos += strlen(unit); }
    strcpy(buf + pos, mid); pos += strlen(mid);
    for (i = 0; i < n; i++) { strcpy(buf + pos, close); pos += strlen(close); }
    return buf;
}

/* `terms` copies of `leaf` joined by `op`: a left-nested chain of depth
 * terms-1 after parsing (or right-nested for ^). */
static const char *chain(const char *leaf, char op, int terms)
{
    static char buf[MAX_INPUT_LEN + 64];
    size_t pos = 0;
    int i;
    for (i = 0; i < terms; i++) {
        if (i) buf[pos++] = op;
        strcpy(buf + pos, leaf);
        pos += strlen(leaf);
    }
    buf[pos] = '\0';
    return buf;
}

static void expect_value(const char *in, const char *want)
{
    ce_response_t resp;
    bool ran = run_like_module(in, &resp);
    check(strlen(in) <= MAX_INPUT_LEN - 1, "deep input fits the 127-char line", in, "");
    check(ran && resp.ok == 1, "deep input runs ok", in, resp.error);
    check(launcher_accepts(&resp), "launcher accepts CE_RESP", in, resp.result);
    check(first_line_is(resp.result, want), want, in, resp.result);
}

static void expect_eval_error(const char *in, const char *err)
{
    ce_response_t resp;
    bool ran = run_like_module(in, &resp);
    check(strlen(in) <= MAX_INPUT_LEN - 1, "deep input fits the 127-char line", in, "");
    check(!ran && resp.ok == 0, "deep input fails cleanly", in, resp.result);
    check(launcher_accepts(&resp), "launcher accepts CE_RESP", in, resp.error);
    check(strcmp(resp.error, err) == 0, err, in, resp.error);
    check(resp.result[0] == '\0', "no result text", in, resp.result);
}

/* The launcher's parser refuses these before anything reaches CEBASE. */
static void expect_parser_refusal(const char *in)
{
    static tokenizer_t tok;
    static parser_t par;
    ast_node_t *root;
    mem_free_all();
    tok_init(&tok, in);
    check(strlen(in) <= MAX_INPUT_LEN - 1, "deep input fits the 127-char line", in, "");
    check(tok_tokenize(&tok) == CE_OK, "deep input tokenizes", in, "");
    parser_init(&par, &tok);
    root = parser_parse(&par);
    check(root == NULL, "parser refuses deep input", in, "");
    check(strcmp(parser_error_msg(&par), "Expression too deeply nested") == 0,
          "Expression too deeply nested", in, parser_error_msg(&par));
    mem_free_all();
}

/*
 * Tiny arithmetic regression. On the OS 5.8 ROM,
 * "2.9843E-38-2.0235E-38" stored the non-IEEE 00513E68 and showed 0, its
 * ^0.5 stored the normal 1FE771A2 (9.8e-20) but printed 0, sqrt() and ^1
 * of it stored 0, and 10^(-40), 2^(-127)*2^127 and 1E20^(-2) gave 0.
 * CEBASE (CE_SAFE_FLOAT_TINY) now refuses a nonzero value below FLT_MIN
 * ("Tiny value unsupported"), keeps exact cancellation and +-0, and prints
 * a nonzero result below 1e-6 in scientific form. The CE's own arithmetic
 * is checked on the real ROM (tests/cemu/test_realrom_tiny.py); this pins
 * the module's answers and texts with the module's flags.
 */
static void tiny_cases(void)
{
    static const struct { const char *in; const char *out; uint32_t ans; } ok[] = {
        { "2.9843E-38*1",          "= 2.9843e-38",  0x01227B12u },   /* the CE parses 01227B19 */
        /* Normal terms, normal sum: a Kahan correction below FLT_MIN is
         * dropped, not refused. */
        /* Exactly rounded sums of the float terms: 041BF677, 07721C01. */
        { "0+sum(1E-37/3*k,k,1,10)", "= 1.83333e-36", 0x041BF678u },   /* 1 ulp */
        { "0+sum(1E-36*k/7,k,1,50)", "= 1.82143e-34", 0x07721C01u },
        { "1E-20*1E-17",           "= 1e-37",       0x02081CEAu },
        { "2^-30*2^-30*2^-30*2^-30*2^-6", "= 1.17549e-38", 0x00800000u },
        { "2^(-125)",              "= 2.35099e-38", 0x01000000u },
        { "(1E-10)^2",             "= 1e-20",       0x1E3CE509u },
        { "1E-30",                 "= 1e-30",       0x0DA24260u },
        { "-1E-30",                "= -1e-30",      0x8DA24260u },
        { "1E-7",                  "= 1e-07",       0x33D6BF95u },
        { "0.0000005",             "= 5e-07",       0x350637BDu },
        { "0.000001",              "= 0.000001",    0x358637BDu },
        { "0.0000015",             "= 0.000002",    0x35C9539Cu },
        { "sin(1E-30)",            "= 1e-30",       0x0DA24260u },
        { "exp(-87)",              "= 1.64581e-38", 0x00B33687u },
        /* Exact cancellation is zero, not a refusal. */
        { "1E-30-1E-30",           "= 0",           0x00000000u },
        { "2.9843E-38-2.9843E-38", "= 0",           0x00000000u },
        { "2^-30*2^-30*2^-30*2^-30*2^-5-2^-30*2^-30*2^-30*2^-30*2^-6",
                                   "= 1.17549e-38", 0x00800000u },
        /* Both zeros are operands (bytes, not the CE's -0 compare). */
        { "-0+5",                  "= 5",           0x40A00000u },
        { "5-(-0)",                "= 5",           0x40A00000u },
        { "0^0",                   "= 1",           0x3F800000u },
        { "(-0)^2",                "= 0",           0x00000000u },
        { "(-0)^0.5",              "= 0",           0x00000000u },
        { "-0",                    "= 0",           0x00000000u },   /* host -0 is kept as +0 */
        { "-0*5",                  "= 0",           0x00000000u },
        { "(-1)*0",                "= 0",           0x00000000u },
        { "0/(-5)",                "= 0",           0x00000000u },
        { "sqrt(-0)",              "= 0",           0x00000000u },
        { "sqrt((-1)*0)",          "= 0",           0x00000000u },
        /* Ordinary overflow and integer controls stay as before. */
        { "16777216-1",            "= 16777215",    0x4B7FFFFFu },
        { "2^30",                  "= 1073741824",  0x4E800000u },
        { "2E-38*1E38",            "= 2",           0x40000000u },
        /* review round 1: |expo*ln(base)| below FLT_MIN is a power of 1 */
        { "2^1.2E-38",             "= 1",           0x3F800000u },
        { "1.0000001^(1E-35)",     "= 1",           0x3F800000u },
        { "0+2^1.2E-38",           "= 1",           0x3F800000u },
        /* FLT_MIN itself is supported */
        { "2^(-126)",              "= 1.17549e-38", 0x00800000u },
        { "0.5^126",               "= 1.17549e-38", 0x00800000u },
        { "sech(87)",              "= 3.29162e-38", 0x01333687u },
        /* terms 1.5, 1E-37, -1.0000001E-37: term - comp cancels below
         * FLT_MIN against a total of 1.5, and is dropped */
        { "0+sum((k-2)*(k-3)*0.75+1E-37*(k-1)*(3-k)-1.0000001E-37*(k-1)*(k-2)/2,k,1,3)",
                                   "= 1.5",         0x3FC00000u },
        /* review round 3: same answers as main */
        { "0+geomsum(3,0.1,40)",   "= 3.33333",     0x40555556u },
        { "2*geomsum(1,0.01,20)",  "= 2.0202",      0x40014AFDu },
        { "0+distance(0,0,1E-20,1)", "= 1",         0x3F800000u },
        { "1+distance(0,0,1,1E-25)", "= 2",         0x40000000u },
        { "0+tangent(x^2,x,3)",    "= 5.99956",     0x40BFFC6Fu },
        /* review round 4: a sample whose own root result is below
         * FLT_MIN is dropped when the dropped mass stays under half an
         * ulp of the result (same answers as main) */
        { "int(exp(-x^2/2),x,-20,1.96)/sqrt(2*pi)", "= 0.975002", 0x3F7999BBu },
        { "2*int(exp(-x^2),x,-10,10)", "= 3.54491",  0x4062DFC4u },
        { "1+int(x^20,x,0,1)",     "= 1.04762",     0x3F861861u },
        { "1+sum(0.5^n,n,1,200)",  "= 2",           0x40000000u },
        { "1+sum(0.1^n,n,1,50)",   "= 1.11111",     0x3F8E38E4u },
        { "2*nderiv(x^4,x,4)",     "= 48",          0x423FFFFCu },
        /* the mark passes through *, / and unary minus */
        { "1+int(x*exp(-x^2),x,-20,20)", "= 1",     0x3F800000u },
        { "int(x^2*exp(-x^2/2),x,-10,10)/sqrt(2*pi)", "= 1", 0x3F800000u },
        { "1+sum(k*0.5^k,k,1,200)", "= 3",          0x40400000u },
        { "1+sum(-(0.5^k),k,1,200)", "= 0",         0x00000000u },
        { "1+sum(0.5^k/k,k,1,200)", "= 1.69315",    0x3FD8B90Cu },
        { "1+sum(exp(-k)*pi,k,1,200)", "= 2.82833", 0x4035036Cu },
        /* a sum or difference with o takes the value +-o */
        { "1+int(1+exp(-x^2),x,-10,10)", "= 22.7724", 0x41B62DF9u },
        { "1+sum(0.5^k+1,k,1,200)", "= 202",        0x434A0000u },
        { "0+sum(0.5^k-1,k,1,200)", "= -199",       0xC3470000u },
        { "1+sum(0.5^k+0,k,1,200)", "= 2",          0x40000000u },
        { "1+sum(1E-20*1E-20+k,k,1,3)", "= 7",      0x40E00000u },
        /* the margin: one dropped term (< 2^-125) against 2^-99 */
        { "0+sum(2^(-99-40*k),k,0,1)", "= 1.57772e-30", 0x0E000000u },
        { "0+int(2^(-90-x^2),x,-12,12)", "= 1.71974e-27", 0x13084076u },
        /* review round 5: a later sibling subtree evaluates as usual, so
         * the marked factor may come first */
        { "1+sum(0.5^k/(k+1),k,1,200)", "= 1.38629",   0x3FB17218u },
        { "2*int(e^(-x^2)*cos(x),x,-15,15)", "= 2.76078", 0x4030B093u },
        { "2*int(e^(-x^2)*x^2,x,-15,15)", "= 1.77245", 0x3FE2DFC5u },
        { "2*int(e^(-x^2)*(1+x),x,-15,15)", "= 3.54491", 0x4062DFC4u },
        { "2*int(e^(-x^2)/(1+x^2),x,-15,15)", "= 2.68659", 0x402BF10Cu },
        { "1+sum(0.5^k*sin(k),k,1,200)", "= 1.59284", 0x3FCBE21Au },
        { "1+sum(0.9^k*k^2,k,1,2000)", "= 1711",       0x44D5DFF5u },
        /* the passed mark's depth: (k+1) is then a later sibling */
        { "1+sum(0.5^k*2*(k+1),k,1,200)", "= 7",     0x40E00000u },
        /* review round 6: o +- mark that rounds to o exactly discharges
         * the mark, so the value can be used further */
        { "1+sum((0.5^k+1)*2,k,1,200)", "= 403",     0x43C98000u },
        { "2*int(1/(1+e^(-x)),x,0,100)", "= 198.614", 0x43469D1Cu },
        { "2*int(ln(1+e^(-x)),x,0,100)", "= 1.64493", 0x3FD28D33u },
        { "2*int(sqrt(1+e^(-x^2)),x,0,10)", "= 20.7719", 0x41A62CE0u },
        { "2*int(x^2*(1-e^(-x^2)),x,0,10)", "= 665.781", 0x442671F4u },
        { "1+sum(1/(1+0.5^k),k,1,200)", "= 200.236", 0x43483C4Au },
        { "1+sum((1+0.5^k)/k^2,k,1,200)", "= 3.22219", 0x404E3850u },
        /* after a discharge, cancellation follows float rounding (IEEE
         * gives the same 0); intended, not a missed refusal */
        { "0+sum(1+0.5^k-1,k,127,127)", "= 0",       0x00000000u },
        /* review round 9 controls: the guarded lim extrapolation keeps
         * normal answers */
        /* host IEEE bits; the OS 5.8 ROM's __fdiv gives 01881CEB */
        { "0+lim((1E-30*x+2E-36/10)-1.5E-36/10,x,0)", "= 5e-38", 0x01881CEAu },
        { "lim(sin(x)/x,x,0)",          "= 1",       0x3F800000u },
    };
    static const char *refused[] = {
        "2.9843E-38-2.0235E-38",
        "(2.9843E-38-2.0235E-38)^0.5",
        "sqrt(2.9843E-38-2.0235E-38)",
        "ln(2.9843E-38-2.0235E-38)",
        "1E-30/(2.9843E-38-2.0235E-38)",
        "(2.9843E-38-2.0235E-38)/(2.9843E-38-2.0235E-38)",
        "(2.9843E-38-2.0235E-38)^1",
        "2^(-127)*2^127",
        "10^(-40)",
        "10^(-44)",
        "1E20^(-2)",
        "1E-20*1E-20",
        "1E-20*1E-18",
        "1E-30/1E10",
        "2^-30*2^-30*2^-30*2^-30*2^-6*0.5",
        "2^-30*2^-30*2^-30*2^-30*2^-6/2",
        "exp(-88)",
        "e^(-88)",
        "0.5^1000",
        "acot(3E38)",
        "0+sum(1E-20*1E-20,k,1,2)",
        "0+sum(exp(-88),k,1,3)",
        "0+sum(3E-38*k/7,k,1,50)",           /* k=1: 4.3e-39 */
        "1+(1E-20*1E-20)",
        "sech(88.5)",                         /* native 1/c: 0 on the CE */
        "csch(-88.5)",
        "sech(88.5)*1E10",
        "e^(-87.3366)",
        "2^(-126.0001)",
        /* review round 2: polar-style functions */
        "geomsum(sech(88.5),0.5,40)",
        "slope(0,0,1E30,1E-10)",
        "distance(0,0,1E-20,0)",
        "midpoint(2E-38,0,-1.5E-38,0)",
        "0+distance(0,0,1E-20,1E-20)",
        "0+nderiv(1E-36*(x+1),x,0)",          /* main: Ans 03AA2208, = 0 */
        "0+tangent(1E-36*(x+1),x,0)",
        /* review round 4: drops that are not provably harmless */
        "1+sum(sqrt(1E-20*1E-20)+k,k,1,3)",    /* below the root: sqrt(t) is normal */
        "1+sum((1E-37+0.5^k)*2,k,127,128)",    /* an absorbed mark passes no further */
        "0+sum(2^-103+0.5^200,k,1,1)",         /* absorbed, not exact: mass 1 */
        "0+sum(1E36*0.5^k,k,126,130)",         /* the bound scales by |o| */
        "0+sum(0.5^k/1E-30,k,126,130)",        /* and by 1/|o| */
        "0+sum(distance(0,0,10^(-10*k),0),k,1,2)", /* a flag-only refusal, no mark */
        "0+sum(2^(-100-40*k),k,0,1)",          /* the margin, one step short */
        "0+sum(0.5^k/1E30,k,127,200)",         /* every term dropped: 0 */
        "0+int(2^(-100-x^2),x,-12,12)",        /* dropped mass near an ulp */
        "0+int(distance(0,0,1E-20,0),x,0,1)",  /* reported, not "no convergence" */
        /* review round 5: a bound that overflows bounds nothing */
        "0+sum(e^(-100*k)*1E30*1E30+1E9,k,1,1)",
        "0+sum(e^(-100*k)/1E-30/1E-30+1E9,k,1,1)",
        "0+sum(1E9-e^(-100*k)*1E30*1E30,k,1,1)",
        "2*int(e^(-100*x)*1E30*1E30+1E9,x,1,2)",
        "0+sum(e^(-100*k)*1E30*1E30,k,1,1)",
        "1+sum(0.5^k*0.5^k,k,60,70)",          /* dropped mass near an ulp */
        "1+sum(0.5^k*0.5^k,k,127,128)",        /* a second mark while one is pending */
        "1+sum(geomsum(0.5^k,2,3),k,126,127)", /* a function used the marked 0 */
        "0+int(exp(-x^2),x,100,200)",          /* every sample dropped */
        "0+lim((1E-19*x)*1E-19,x,1)",          /* a tiny limit */
        "1+lim(x^7,x,0)",                      /* 0 cannot be told from tiny */
        "1+nderiv(x^13,x,1)",
        "0+sum(0.5^k/k,k,100,200)",            /* mass reaches the 6th digit */
        "1+(0.5^200)*2",                       /* outside a sample: as before */
        /* review round 9: lim's extrapolation, 5e-39 from normal samples
         * (on the CE __fdiv flushed it to 0; the ROM printed = 0) */
        "0+lim((1E-30*x+2E-37/10)-1.5E-37/10,x,0)",
        "0+lim((1E-30*x+2E-37/10)-1.5E-37/10,x,0,1)",
        /* the extrapolation's rounding remainder of a 0 limit is below
         * FLT_MIN: 0 cannot be told from tiny (as 1+lim(x^7,x,0)); main
         * printed 0. Intended (round 9 nit) */
        "lim(x*1E-30,x,0)",
        "lim(x*1E-30,x,0,1)",
        /* a function consuming a marked 0 reports the refusal, not its
         * own error (1/0 here); kills mutant r5_orphan_kept */
        "1+sum(1/geomsum(0.5^k,1,3),k,127,127)",
    };
    static const struct { const char *in; const char *err; } other_err[] = {
        { "1/(-0)",    "Division by zero" },
        { "(-0)^(-1)", "Division by zero" },
        { "0^(-1)",    "Division by zero" },
        { "1/0",       "Division by zero" },
        { "1E38*10",   "Overflow" },
    };
    static const struct { const char *in; uint16_t pos; } lit[] = {
        { "1E-40", 0 }, { "2+1E-39", 2 }, { "1E-46", 0 }, { "0.1E-45", 0 },
        { "3*1E-99", 2 },
    };
    ce_response_t resp;
    size_t i;
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        uint32_t bits;
        bool ran = run_like_module(ok[i].in, &resp);
        memcpy(&bits, &resp.last_answer, 4);
        check(ran && resp.ok == 1, "tiny ok runs", ok[i].in, resp.error);
        check(launcher_accepts(&resp), "launcher accepts CE_RESP", ok[i].in, resp.result);
        check(first_line_is(resp.result, ok[i].out), ok[i].out, ok[i].in, resp.result);
        check(bits == ok[i].ans, "Ans bits", ok[i].in, resp.result);
        if (bits != ok[i].ans) printf("  Ans %08X, want %08X\n", (unsigned)bits, (unsigned)ok[i].ans);
    }
    /* Ans 5 must survive every refusal. */
    check(run_like_module("2+3", &resp) && resp.last_answer == 5.0f, "Ans = 5", "2+3", resp.result);
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        bool ran = run_like_module(refused[i], &resp);
        check(!ran && resp.ok == 0, "tiny refused", refused[i], resp.result);
        check(launcher_accepts(&resp), "launcher accepts CE_RESP", refused[i], resp.error);
        check(strcmp(resp.error, "Tiny value unsupported") == 0, "Tiny value unsupported",
              refused[i], resp.error);
        check(resp.result[0] == '\0', "no result text", refused[i], resp.result);
        check(resp.last_answer == 5.0f, "Ans kept after tiny refusal", refused[i], resp.error);
    }
    for (i = 0; i < sizeof(other_err) / sizeof(other_err[0]); i++) {
        bool ran = run_like_module(other_err[i].in, &resp);
        check(!ran && strcmp(resp.error, other_err[i].err) == 0, other_err[i].err,
              other_err[i].in, resp.error);
    }
    /* Degree mode scales the argument first: 1E-37 degrees is 1.7e-39
     * radians, below FLT_MIN (CE __fmul would flush it to 0). */
    {
        settings_t saved = g_settings;
        g_settings.angle_mode = ANGLE_DEGREES;
        check(!run_like_module("sin(1E-37)", &resp) &&
              strcmp(resp.error, "Tiny value unsupported") == 0,
              "degree-mode tiny argument refused", "sin(1E-37)", resp.error);
        check(run_like_module("sin(1E-30)", &resp) &&
              first_line_is(resp.result, "= 1.74533e-32"), "= 1.74533e-32", "sin(1E-30)",
              resp.result);
        /* cos and sec of such an angle are exactly 1 (review round 1);
         * tan keeps the refusal. */
        check(run_like_module("cos(1E-37)", &resp) && first_line_is(resp.result, "= 1"),
              "= 1", "degree cos(1E-37)", resp.result);
        check(run_like_module("sec(-1E-37)", &resp) && first_line_is(resp.result, "= 1"),
              "= 1", "degree sec(-1E-37)", resp.result);
        check(!run_like_module("tan(1E-37)", &resp) &&
              strcmp(resp.error, "Tiny value unsupported") == 0,
              "degree-mode tiny tan refused", "tan(1E-37)", resp.error);
        /* geomsum's own arguments set only the flag; its power-of-1
         * drop (n > 32 takes powf's path) must not clear it (round 2). */
        check(!run_like_module("geomsum(tan(1E-37),3,33)", &resp) &&
              strcmp(resp.error, "Tiny value unsupported") == 0,
              "degree geomsum(tan(1E-37),3,33) refused", "geomsum", resp.error);
        check(!run_like_module("geomsum(sin(1E-37),2,40)", &resp) &&
              strcmp(resp.error, "Tiny value unsupported") == 0,
              "degree geomsum(sin(1E-37),2,40) refused", "geomsum", resp.error);
        g_settings = saved;
    }
    /* A refusal must not leak into the next request. */
    check(run_like_module("2+3", &resp) && first_line_is(resp.result, "= 5"), "= 5 after refusal",
          "2+3", resp.result);
    /* Nonzero literals that would read as 0 or tiny stop in the tokenizer
     * (the launcher's), at the literal. */
    for (i = 0; i < sizeof(lit) / sizeof(lit[0]); i++) {
        static tokenizer_t tok;
        ce_error_t e;
        tok_init(&tok, lit[i].in);
        e = tok_tokenize(&tok);
        check(e == CE_ERR_PRECISION && tok.err_pos == lit[i].pos, "tiny literal refused",
              lit[i].in, "");
    }
    /* A literal whose exponent does not fit in a token (review round 9:
     * 1.0000000000E-99 read as 1e-9) is refused at the literal, whatever
     * its size; a literal that fits is read as before. */
    {
        static const struct { const char *in; uint16_t pos; ce_error_t e; } cut[] = {
            { "1.0000000000E-99", 0, CE_ERR_OVERFLOW },
            { "1.000000000000000E-99", 0, CE_ERR_OVERFLOW },
            { "2+1.0000000000E+38", 2, CE_ERR_OVERFLOW },
            { "1.0000000000E-10", 0, CE_ERR_OVERFLOW },
            { "1.00000000000000E5", 0, CE_ERR_OVERFLOW },
            { "2+00000000000000000001E-50", 2, CE_ERR_OVERFLOW },
            /* only the sign is cut (no exponent digit follows) */
            { "1.000000000000E-x", 0, CE_ERR_OVERFLOW },
            { "1.000000000E-99", 0, CE_ERR_PRECISION },   /* fits: tiny */
            { "1.00000000E+38", 0, CE_OK },
            { "1.000000000E-10", 0, CE_OK },
            /* every nonzero digit cut: too long, not tiny (1e-14, 1e-16) */
            { "0.00000000000001", 0, CE_ERR_OVERFLOW },
            { "0.0000000000000001", 0, CE_ERR_OVERFLOW },
            { "2+0000000000000001", 2, CE_ERR_OVERFLOW },
            /* integer digits cut: read short, as on main (follow-up; the
             * value is not checked here) */
            { "123456789012345678", 0, CE_OK },
            { "0000000000000000000E-50", 0, CE_OK },      /* a true zero */
            { "0.0000000000000E-99", 0, CE_OK },
        };
        for (i = 0; i < sizeof(cut) / sizeof(cut[0]); i++) {
            static tokenizer_t tok;
            ce_error_t e;
            tok_init(&tok, cut[i].in);
            e = tok_tokenize(&tok);
            check(e == cut[i].e && (e == CE_OK || tok.err_pos == cut[i].pos),
                  "long literal", cut[i].in, "");
        }
        check(run_like_module("1.000000000E-10*1E10", &resp) &&
              first_line_is(resp.result, "= 1"), "= 1", "1.000000000E-10*1E10", resp.result);
    }
    {
        static tokenizer_t tok;
        tok_init(&tok, "0E-50+0.000E-99");
        check(tok_tokenize(&tok) == CE_OK, "zero literals are not refused", "0E-50+0.000E-99", "");
    }
}

/*
 * Stack regression: deep but legal input reset the CE. On the real
 * OS 5.8 ROM, "2^2^...^2^1" (31 levels, 63 chars) hit the stack-limit NMI
 * in CEBASE's evaluator, and 32-term sums already ran eval frames past the
 * stack limit without tripping it. Every operator at the 127-char maximum
 * must now give a value or a clean error. Stack depth itself is measured
 * on the real ROM (tests/cemu/test_realrom_cebase_stack.py); this desktop
 * check pins the answers and error text the CE must produce.
 */
static void deep_input_cases(void)
{
    static const struct { char op; const char *leaf; const char *value16; } ops[] = {
        { '+', "2", "= 32" }, { '-', "1", "= -14" }, { '*', "1", "= 1" }, { '/', "1", "= 1" },
    };
    size_t i;
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        /* 63 chars: 32 terms, depth 31. */
        expect_eval_error(chain(ops[i].leaf, ops[i].op, 32), "Eval recursion limit");
        /* Scalar persistence fixes CEBASE's evaluator cap at 16. */
        expect_eval_error(chain(ops[i].leaf, ops[i].op, 17), "Eval recursion limit");
        expect_value(chain(ops[i].leaf, ops[i].op, 16), ops[i].value16);
        /* Preserve maximum-length regression inputs as clean refusals. */
        expect_eval_error(chain(ops[i].leaf, ops[i].op, 64), "Eval recursion limit");
        /* Historical cap boundary also refuses under the fixed cap. */
        expect_eval_error(chain(ops[i].leaf, ops[i].op, 51), "Eval recursion limit");
    }
    expect_eval_error(chain("2", '+', 50), "Eval recursion limit");
    expect_eval_error(chain("1", '-', 50), "Eval recursion limit");
    /* Long right-nested powers refuse before reaching their leaves. */
    expect_eval_error(chain("1", '^', 50), "Eval recursion limit");
    expect_value(chain("2", '^', 4), "= 65536");   /* 2^(2^(2^2)) */
    /* Retain the reported reset and a 32-level tower as depth refusals. */
    expect_eval_error(nest("2^", 31, "1", ""), "Eval recursion limit");
    expect_eval_error(chain("2", '^', 32), "Eval recursion limit");
    /* 127 chars of ^ exceed the parser's nesting limit. */
    expect_parser_refusal(chain("2", '^', 64));
    /* Nested unary minus and parentheses. */
    expect_eval_error(nest("-(", 24, "2", ")"), "Eval recursion limit");
    expect_eval_error(nest("-(", 23, "2", ")"), "Eval recursion limit");
    expect_value(nest("-(", 15, "2", ")"), "= -2");
    expect_eval_error(nest("-(", 16, "2", ")"), "Eval recursion limit");
    expect_parser_refusal(nest("-(", 25, "2", ")"));
    expect_parser_refusal(nest("-(", 42, "2", ")"));
    expect_parser_refusal(nest("-", 126, "2", ""));
    expect_value(nest("(", 49, "2", ")"), "= 2");
    expect_parser_refusal(nest("(", 63, "2", ")"));
    /* Function nests: 25 levels fill the line. */
    expect_eval_error(nest("sin(", 25, "0", ")"), "Eval recursion limit");
    expect_eval_error(nest("abs(", 25, "-3", ")"), "Eval recursion limit");
    expect_value(nest("sin(", 15, "0", ")"), "= 0");
    expect_eval_error(nest("sin(", 16, "0", ")"), "Eval recursion limit");
    expect_value(nest("abs(", 14, "-3", ")"), "= 3");
    expect_eval_error(nest("abs(", 15, "-3", ")"), "Eval recursion limit");
    expect_parser_refusal(nest("2^(", 31, "1", ")"));
    expect_parser_refusal(nest("2(", 42, "2", ")"));
}

int main(void)
{
    static const struct { const char *in; const char *out; } ok_cases[] = {
        { "hex(255)",       "= 0xFF" },
        { "hex(16)",        "= 0x10" },
        { "hex(0)",         "= 0x0" },
        { "hex(-255)",      "= -0xFF" },
        { "hex(65535)",     "= 0xFFFF" },
        { "hex(65536)",     "= 0x10000" },
        { "hex(1048575)",   "= 0xFFFFF" },
        { "hex(4194303)",   "= 0x3FFFFF" },
        { "hex(8388607)",   "= 0x7FFFFF" },
        { "hex(8388608)",   "= 0x800000" },
        { "hex(16777215)",  "= 0xFFFFFF" },
        { "hex(-16777215)", "= -0xFFFFFF" },
        { "oct(16777215)",  "= 0o77777777" },
        { "bin(2097151)",   "= 0b111111111111111111111" },
        { "bin(-2097151)",  "= -0b111111111111111111111" },
        { "bin(4194303)",   "= 0b1111111111111111111111" },
        { "bin(10)",        "= 0b1010" },
        { "bin(0)",         "= 0b0" },
        { "oct(8)",         "= 0o10" },
        { "oct(255)",       "= 0o377" },
        { "dec(255)",       "= 255" },
        { "hex(15)+1",      "= 16" },
        { "hex(-0)",        "= 0x0" },
        { "hex(3*5)",       "= 0xF" },
        { "2+3",            "= 5" },
        /* CEdev v11.2 isinf() was true for these (mantissa bits 7..14 all
         * ones); a plain 16777215 showed "= inf" on the CE. */
        { "16777215",       "= 16777215" },
        { "-16777215",      "= -16777215" },
        { "16777214",       "= 16777214" },
        { "8388609",        "= 8388609" },
        { "1048575",        "= 1048575" },
        { "131071",         "= 131071" },
        { "65791",          "= 65791" },
        { "65790",          "= 65790" },
        { "65536*256-1",    "= 16777215" },
        { "floor(16777215)", "= 16777215" },
        { "1000000",        "= 1000000" },
        { "1E6",            "= 1000000" },
        { "12345678",       "= 12345678" },
        { "2^20",           "= 1048576" },
        { "0.99999237",     "= 0.999992" },
        /* CE float ops: exact integer results that CE
         * __fsub and TI-OS powf got wrong. */
        { "16777216-1",     "= 16777215" },
        { "2^24-1",         "= 16777215" },
        { "2^24",           "= 16777216" },
        { "2^30",           "= 1073741824" },
        { "3^15",           "= 14348907" },
        { "(-3)^15",        "= -14348907" },
        { "7^8",            "= 5764801" },
        { "1.5^2",          "= 2.25" },
        { "10^-5",          "= 0.00001" },
        { "1E38*3.4",       "= >4e9" },   /* in range; proc_format_decimal caps */
        { "1E39",           "= inf" },
        { "inf*2",          "= inf" },
    };
    static const struct { const char *in; const char *err; } err_cases[] = {
        { "hex(2.5)",   "hex() needs an integer" },
        { "bin(-0.5)",  "bin() needs an integer" },
        { "hex(65535.5)", "hex() needs an integer" },
        { "hex(0.001)", "hex() needs an integer" },
        { "oct(2^25)",      "oct() range is +/-16777215" },
        { "hex(16777216)",  "hex() range is +/-16777215" },
        { "hex(16777217)",  "hex() range is +/-16777215" },
        { "bin(4194304)",   "bin() result too wide" },
        { "bin(-4194303)",  "bin() result too wide" },
        { "bin(-2097152)",  "bin() result too wide" },
        { "hex(1/0)",       "Division by zero" },
        { "bin(16777215)",  "bin() result too wide" },
        { "exp(89)",        "exp overflow" },
        { "sinh(90)",       "sinh overflow" },
        /* Overflows the CE used to show as 0 or nan. */
        { "1E38*10",        "Overflow" },
        { "3E38*2",         "Overflow" },
        { "3E38+3E38",      "Overflow" },
        { "1E30*1E30",      "Overflow" },
        { "1E38/0.1",       "Overflow" },
        { "-3E38-3E38",     "Overflow" },
        { "exp(100)",       "exp overflow" },
        { "10^39",          "Power overflow" },
        { "2^128",          "Power overflow" },
        { "cosh(100)",      "cosh overflow" },
        { "sum(10^38,k,1,10)", "Sum overflow" },
    };
    /* Expressions with a free variable: no numeric value in CEBASE. */
    static const char NOVAL_HINTS[] =
        "factor(...) or expand(...)";
    static const struct { const char *in; char var; } noval_cases[] = {
        { "x^2*sin(x)+x", 'x' },
        { "x^2*sin(x)*cos(x)*e^x+x^3*cos(x)+x*sin(x)+x", 'x' },
        { "X^2*SIN(X)+X", 'X' },
        { "x", 'x' },
        { "x+1", 'x' },
        { "-x", 'x' },
        { "sin(x)", 'x' },
        { "2x+3x", 'x' },
        { "y^2+1", 'y' },
        { "3+t", 't' },
        { "2*3+a*b", 'a' },
        /* The derivative's "save the binding" lookup of x must not be
         * reported; y is the unbound variable. */
        { "d/dx(x^2)+y", 'y' },
        { "1+d/dx(y)", 'y' },
        { "sum(k,k,1,3)+w", 'w' },
    };
    ce_response_t resp;
    size_t i;

    fileioc_mock_reset();
    fileioc_mock_enable();   /* launcher_accepts() writes/reads CE_RESP */

    for (i = 0; i < sizeof(ok_cases) / sizeof(ok_cases[0]); i++) {
        bool ran = run_like_module(ok_cases[i].in, &resp);
        check(ran && resp.ok == 1, "runs ok", ok_cases[i].in, resp.error);
        check(launcher_accepts(&resp), "launcher accepts CE_RESP", ok_cases[i].in, resp.result);
        check(first_line_is(resp.result, ok_cases[i].out), ok_cases[i].out,
              ok_cases[i].in, resp.result);
        check(strcspn(resp.result, "\n") <= 26, "fits a 26-col launcher row",
              ok_cases[i].in, resp.result);
    }
    for (i = 0; i < sizeof(err_cases) / sizeof(err_cases[0]); i++) {
        bool ran = run_like_module(err_cases[i].in, &resp);
        check(!ran && resp.ok == 0, "fails", err_cases[i].in, resp.result);
        check(launcher_accepts(&resp), "launcher accepts CE_RESP", err_cases[i].in, resp.error);
        check(strcmp(resp.error, err_cases[i].err) == 0, err_cases[i].err,
              err_cases[i].in, resp.error);
        check(strlen(resp.error) <= 26, "error fits a 26-col launcher row",
              err_cases[i].in, resp.error);
    }

    for (i = 0; i < sizeof(noval_cases) / sizeof(noval_cases[0]); i++) {
        char want[32];
        char hint[32];
        const char *line;
        size_t n;
        int lines_ok = 1;
        bool ran = run_like_module(noval_cases[i].in, &resp);
        snprintf(want, sizeof(want), "%c has no value", noval_cases[i].var);
        snprintf(hint, sizeof(hint), "Try d/d%c(...), int(...,%c),",
                 noval_cases[i].var, noval_cases[i].var);
        check(!ran && resp.ok == 0, "no-value fails cleanly (no ok=2 menu)",
              noval_cases[i].in, resp.result);
        check(launcher_accepts(&resp), "launcher accepts CE_RESP",
              noval_cases[i].in, resp.error);
        check(first_line_is(resp.error, want), want, noval_cases[i].in, resp.error);
        check(strstr(resp.error, hint) != NULL, hint, noval_cases[i].in, resp.error);
        check(strstr(resp.error, NOVAL_HINTS) != NULL, NOVAL_HINTS,
              noval_cases[i].in, resp.error);
        check(resp.result[0] == '\0', "no result text", noval_cases[i].in, resp.result);
        /* Every line fits a 26-col row, and the whole text fits the field. */
        for (line = resp.error; *line; line += n + (line[n] == '\n')) {
            n = strcspn(line, "\n");
            if (n > 26) lines_ok = 0;
        }
        check(lines_ok, "each line fits 26 cols", noval_cases[i].in, resp.error);
        check(strlen(resp.error) < sizeof(resp.error) - 1, "fits the error field",
              noval_cases[i].in, resp.error);
    }

    /* Ans survives a no-value answer (the old ok=2 path reset it to 0). */
    {
        bool ran = run_like_module("2+3", &resp);
        check(ran && resp.last_answer == 5.0f, "Ans = 5", "2+3", resp.result);
        ran = run_like_module("x+1", &resp);
        check(!ran && resp.last_answer == 5.0f, "Ans kept after no-value", "x+1", resp.error);
    }

    /* Special values the parser cannot produce: build the AST directly. */
    {
        static const struct { float v; const char *err; } special[] = {
            { NAN,       "hex() needs an integer" },
            { INFINITY,  "hex() range is +/-16777215" },
            { -INFINITY, "hex() range is +/-16777215" },
        };
        for (i = 0; i < sizeof(special) / sizeof(special[0]); i++) {
            ast_node_t *arg;
            ast_node_t *root;
            bool ran;
            mem_free_all();
            memset(&resp, 0, sizeof(resp));
            arg = ast_number(special[i].v);
            root = arg ? ast_func(FN_HEX, arg) : NULL;
            check(root != NULL, "special AST builds", special[i].err, "");
            if (!root) continue;
            ran = ce_processor_run_preparsed("hex(special)", root, false, &resp);
            check(!ran && strcmp(resp.error, special[i].err) == 0, special[i].err,
                  "hex(special)", resp.error);
        }
        mem_free_all();
        memset(&resp, 0, sizeof(resp));
        {
            ast_node_t *arg = ast_number(-0.0f);
            ast_node_t *root = arg ? ast_func(FN_HEX, arg) : NULL;
            bool ran = root && ce_processor_run_preparsed("hex(-0.0)", root, false, &resp);
            check(ran && first_line_is(resp.result, "= 0x0"), "= 0x0", "hex(-0.0f)", resp.result);
        }
    }

    deep_input_cases();
    tiny_cases();

    /* An eval refusal (depth cap or eZ80 stack backstop) marks the run. No
     * answer of that run may succeed, and Ans keeps its value from before
     * the refusal. The desktop processor clears
     * the flag at request entry, so set it by hand and build the response
     * the way a worker does after a caller swallowed the error. End to end
     * (a real swallowed refusal): sum/recseq/nlsolve in
     * tests/cemu/test_eval_refusal_workers.py and the real-ROM
     * test_realrom_eval_refusals.py / test_realrom_eval_backstop.py. */
    {
        bool ran;
        /* Ans was 7 when eval refused; the worker then changed it to 9
         * (limits_series/main.c sets Ans before writing its answer). */
        g_last_answer = 9.0f;
        g_eval_refused_ans = 7.0f;
        g_eval_refused = true;
        ce_response_from_runtime(&resp, true, true, "= 5", "", 9.0f);
        check(resp.ok == 0, "refused run never reports success", "2+3", resp.result);
        check(strcmp(resp.error, "Eval recursion limit") == 0, "Eval recursion limit",
              "2+3", resp.error);
        check(resp.result[0] == '\0', "no result text after refusal", "2+3", resp.result);
        check(resp.last_answer == 7.0f, "CE_RESP keeps the old Ans", "2+3", resp.error);
        check(g_last_answer == 7.0f, "Ans unchanged after refusal", "2+3", resp.error);
        check(launcher_accepts(&resp), "launcher accepts CE_RESP", "2+3", resp.error);
        /* A refused request leaves nothing behind for the next one. */
        ran = run_like_module("2+3", &resp);
        check(ran && resp.ok == 1 && first_line_is(resp.result, "= 5"), "= 5 once clear",
              "2+3", resp.result);
        check(resp.last_answer == 5.0f && g_last_answer == 5.0f, "Ans = 5 once clear",
              "2+3", resp.result);
    }

    printf("split_cebase_test: %d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
