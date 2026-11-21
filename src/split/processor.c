#include "split/processor.h"

#include "memory.h"
#if CE_PROC_PARSE_INPUT
#include "token.h"
#include "parser.h"
#endif
#include "ast.h"
#include "eval.h"
#include "ce_float_ops.h"
#include "deriv.h"
#include "integrate.h"
#include "limits.h"
#include "series.h"
#include "algebra.h"
#include "steps.h"
#include "stats.h"
#include "matrix.h"
#include "polar.h"
#include "dsolve.h"
#include "vector.h"
#include "laplace.h"
#include "units.h"
#include "graph.h"
#if CE_PROC_USE_CLASSIFY
#include "split/classify.h"
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__) || defined(__GNUC__)
#define CE_MAYBE_UNUSED __attribute__((unused))
#define CE_PROC_NOINLINE __attribute__((noinline))
#define CE_PROC_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define CE_MAYBE_UNUSED
#define CE_PROC_NOINLINE
#define CE_PROC_ALWAYS_INLINE inline
#endif

/* Processor calls are synchronous and never re-enter this dispatcher.
 * Only module entry points call the public runners. Math/eval/formatting
 * callees do not call either runner. The parser and evaluator already use
 * static workspaces here. Keep response and dispatch scratch static too.
 * Reset response state on each call and write scratch before reading it. */
typedef struct {
    bool ok;
    bool show_steps;
    bool has_numeric;
    ce_float_t last_numeric;
    char result[MAX_RESULT_LEN];
    char error[128];
} proc_state_t;

#if CE_PROC_PARSE_INPUT
static tokenizer_t s_tokenizer;
static parser_t s_parser;
#endif
#if CE_PROC_USE_EVAL
static eval_ctx_t s_eval CE_MAYBE_UNUSED;
#endif

static void proc_format_decimal(char *buf, size_t size, ce_float_t value)
{
    double v;
    bool neg = false;
    unsigned long scale = 1000000UL;
    unsigned long scaled;
    unsigned long whole;
    unsigned long frac;
    char frac_buf[8];
    int frac_len = 6;

    if (!buf || size == 0) return;

    if (ce_isnan(value)) {
        snprintf(buf, size, "nan");
        return;
    }
    if (ce_isinf(value)) {
        snprintf(buf, size, "%sinf", value < 0 ? "-" : "");
        return;
    }

    v = (double)value;
    if (v < 0.0) {
        neg = true;
        v = -v;
    }

    if (v > 4294.0) {
        if (v > 4294967294.0) {
            snprintf(buf, size, "%s>4e9", neg ? "-" : "");
            return;
        }
        /* At 2^23 and above a 32-bit float (the CE's double) holds only
         * integers, and v + 0.5 would round odd values up: 16777215 printed
         * as 16777216. Below 2^23, v + 0.5 is exact. */
        whole = v >= 8388608.0 ? (unsigned long)v : (unsigned long)(v + 0.5);
        snprintf(buf, size, "%s%lu", neg ? "-" : "", whole);
        return;
    }

    if (v >= 100000.0) {
        scale = 1UL;
        frac_len = 0;
    } else if (v >= 10000.0) {
        scale = 10UL;
        frac_len = 1;
    } else if (v >= 1000.0) {
        scale = 100UL;
        frac_len = 2;
    } else if (v >= 100.0) {
        scale = 1000UL;
        frac_len = 3;
    } else if (v >= 10.0) {
        scale = 10000UL;
        frac_len = 4;
    } else if (v >= 1.0) {
        scale = 100000UL;
        frac_len = 5;
    }

    scaled = (unsigned long)(v * (double)scale + 0.5);
    whole = scaled / scale;
    frac = scaled % scale;
    if (frac == 0UL || frac_len == 0) {
        snprintf(buf, size, "%s%lu", neg ? "-" : "", whole);
        return;
    }

    snprintf(frac_buf, sizeof(frac_buf), "%06lu", frac);
    if (frac_len < 6) memmove(frac_buf, frac_buf + (6 - frac_len), (size_t)frac_len + 1U);
    while (frac_len > 0 && frac_buf[frac_len - 1] == '0') {
        frac_buf[--frac_len] = '\0';
    }

    snprintf(buf, size, "%s%lu.%s", neg ? "-" : "", whole, frac_buf);
}

static void proc_format_result(char *buf, size_t size, ce_float_t value)
{
    char number[32];

    if (!buf || size == 0) return;
    proc_format_decimal(number, sizeof(number), value);
    snprintf(buf, size, "= %s", number);
}

#if CE_PROC_GENERIC
/* Base-conversion display for a top-level hex()/bin()/oct() call.
 * eval.c returns the argument unchanged for FN_HEX/FN_BIN/FN_OCT, so the
 * base is applied here, at display time. Digits are built by hand: CEdev
 * printf has no %g and %X/%o support is not relied on.
 *
 * Returns 1 when buf holds "= 0xFF" style text, 0 when the root is not a
 * base call, and -1 when the value cannot be shown in that base (buf then
 * holds the error message).
 * - Integers only, |v| < 2^24. 2^24 itself is exact, but 2^24+1 already
 *   parses to it, so the boundary is excluded.
 * - Negatives are sign-magnitude ("-0xFF"), not two's complement.
 * - The whole line must fit one 26-column launcher row (HOME_COLS in
 *   src/core/launcher/main.c). The launcher clips longer rows without a
 *   marker, and a clipped bin() string reads as a different number, so a
 *   wider result is an error instead (bin() tops out at 2^22-1, or
 *   2^21-1 when negative). */
#define PROC_BASE_MAX_EXACT 16777216.0f
#define PROC_BASE_MAX_COLS  26U
static int proc_format_base_result(char *buf, size_t size,
                                   const ast_node_t *root, ce_float_t value)
{
    unsigned int base;
    const char *prefix;
    const char *name;
    char digits[32];
    int n = 0;
    unsigned long u;
    ce_float_t v;
    bool neg;

    if (!buf || size == 0 || !root || root->type != NODE_FUNC) return 0;
    switch (root->d.func.id) {
    case FN_HEX: base = 16U; prefix = "0x"; name = "hex"; break;
    case FN_BIN: base = 2U;  prefix = "0b"; name = "bin"; break;
    case FN_OCT: base = 8U;  prefix = "0o"; name = "oct"; break;
    default: return 0;
    }

    /* Plain comparisons only. CEdev v11.2's libc isinf() returned true for
     * exact integers such as 1048575 and 16777215 (real-ROM run, 2026-11-08;
     * see src/ce_float_class.h), so no classification macro is used here:
     * - NaN fails v == v. If the CE float compare were not IEEE, NaN's
     *   all-ones exponent still lands in the range check below.
     * - +/-inf is caught by the range check after the sign is removed.
     * - Integrality is an unsigned long round trip in ce_float_t. */
    v = value;
    if (!(v == v)) {
        snprintf(buf, size, "%s() needs an integer", name);
        return -1;
    }
    neg = v < 0;
    if (neg) v = -v;
    if (v >= (ce_float_t)PROC_BASE_MAX_EXACT) {
        snprintf(buf, size, "%s() range is +/-16777215", name);
        return -1;
    }
    u = (unsigned long)v;
    if ((ce_float_t)u != v) {
        snprintf(buf, size, "%s() needs an integer", name);
        return -1;
    }
    do {
        unsigned int d = (unsigned int)(u % base);
        digits[n++] = (char)(d < 10U ? '0' + d : 'A' + (d - 10U));
        u /= base;
    } while (u != 0UL && n < (int)sizeof(digits) - 1);

    {
        size_t pos = 0;
        size_t plen = strlen(prefix);
        size_t cols = 2U + (neg ? 1U : 0U) + plen + (size_t)n;
        if (cols > PROC_BASE_MAX_COLS) {
            snprintf(buf, size, "%s() result too wide", name);
            return -1;
        }
        if (size < cols + 1U) return 0;
        buf[pos++] = '=';
        buf[pos++] = ' ';
        if (neg) buf[pos++] = '-';
        memcpy(buf + pos, prefix, plen);
        pos += plen;
        while (n > 0) buf[pos++] = digits[--n];
        buf[pos] = '\0';
    }
    return 1;
}
#endif

#if CE_PROC_CMPLX
static void proc_polar_to_rect(ce_float_t r, ce_float_t theta,
                               ce_float_t *x_out, ce_float_t *y_out)
{
    if (x_out) *x_out = r * cosf(theta);
    if (y_out) *y_out = r * sinf(theta);
}

static void proc_rect_to_polar(ce_float_t x, ce_float_t y,
                               ce_float_t *r_out, ce_float_t *theta_out)
{
    if (r_out) *r_out = sqrtf(x * x + y * y);
    if (theta_out) *theta_out = atan2f(y, x);
}
#endif

#if CE_PROC_ALG_SOLVE || CE_PROC_GENERIC
static char find_var_in(const ast_node_t *e)
{
    char v;
    if (!e) return 0;
    switch (e->type) {
    case NODE_VARIABLE: return e->d.variable;
    case NODE_BINOP:
        v = find_var_in(e->d.binop.left);
        return v ? v : find_var_in(e->d.binop.right);
    case NODE_UNOP: return find_var_in(e->d.unop.operand);
    case NODE_FUNC: return find_var_in(e->d.func.arg);
    default: return 0;
    }
}
#endif

static void proc_failf(proc_state_t *st, const char *fmt, ...) STEPS_PRINTF(2, 3);
static void proc_failf(proc_state_t *st, const char *fmt, ...)
{
    va_list ap;
    if (!st) return;
    st->ok = false;
    st->show_steps = false;
    /* handle_solve passes st->result as the error text. Consume it before
     * clearing the result, or both clean refusals and routing tokens vanish. */
    va_start(ap, fmt);
    vsnprintf(st->error, sizeof(st->error), fmt, ap);
    va_end(ap);
    st->result[0] = '\0';
}

static void proc_okf(proc_state_t *st, bool show_steps, const char *fmt, ...) CE_MAYBE_UNUSED STEPS_PRINTF(3, 4);
static void proc_okf(proc_state_t *st, bool show_steps, const char *fmt, ...)
{
    va_list ap;
    if (!st) return;
    st->ok = true;
    st->show_steps = show_steps;
    st->error[0] = '\0';
    va_start(ap, fmt);
    vsnprintf(st->result, sizeof(st->result), fmt, ap);
    va_end(ap);
}
static void proc_ok_number(proc_state_t *st, bool show_steps,
                           const char *prefix, ce_float_t value, const char *suffix)
{
    char number[32];
    if (!st) return;
    proc_format_decimal(number, sizeof(number), value);
    proc_okf(st, show_steps, "%s%s%s", prefix ? prefix : "", number, suffix ? suffix : "");
}

static int proc_append_decimal(char *buf, size_t size, int pos, ce_float_t value)
{
    char number[32];
    if (!buf || pos < 0 || (size_t)pos >= size) return pos;
    proc_format_decimal(number, sizeof(number), value);
    return pos + snprintf(buf + pos, size - (size_t)pos, "%s", number);
}


#if CE_PROC_PARSE_INPUT
static bool parse_input(const char *input,
                        ast_node_t **out_root,
                        bool *out_had_equals,
                        char *err,
                        size_t err_size)
{
    const char *expr = input;
    const char *p;

    if (!input || !out_root || !out_had_equals) return false;

    *out_had_equals = false;
    for (p = expr; *p; p++) {
        if (*p == '=') {
            *out_had_equals = true;
            break;
        }
    }

    mem_free_all();
    steps_clear();
#if CE_PROC_USE_EVAL
    eval_init(&s_eval);
#endif

    tok_init(&s_tokenizer, expr);
    if (tok_tokenize(&s_tokenizer) != CE_OK) {
        snprintf(err, err_size, "%s", s_tokenizer.error == CE_ERR_PRECISION ?
                 CE_FOPS_TINY_MSG : "Check your input for typos or missing operators");
        return false;
    }

    parser_init(&s_parser, &s_tokenizer);
    *out_root = parser_parse(&s_parser);
    if (!*out_root) {
        const char *pmsg = parser_error_msg(&s_parser);
        if (pmsg && pmsg[0]) {
            snprintf(err, err_size, "%s", pmsg);
        } else {
            snprintf(err, err_size, "Could not understand expression");
        }
        return false;
    }

    return true;
}
#endif

#if CE_PROC_CALC_DERIV || CE_PROC_CALC_TANGENT || CE_PROC_CALC_INT || \
    CE_PROC_CALC_INT_STRATEGY || \
    CE_PROC_CALC_LIMIT || CE_PROC_CALC_SERIES_TF || CE_PROC_CALC_SERIES_SEQ
#if CE_PROC_CALC_DERIV
static bool handle_deriv(ast_node_t *root, proc_state_t *st)
{
    ast_node_t *deriv_result;
    char var = root->d.deriv.var;
    const ast_node_t *inner = root->d.deriv.expr;
    static char ast_str[128]; /* Processor is non-reentrant; keep OS stack free. */

    deriv_result = deriv_diff(inner, var);
    if (!deriv_result) {
        proc_failf(st, "Cannot differentiate this expression");
        ast_free(root);
        return true;
    }

    ast_to_display_str(deriv_result, ast_str, sizeof(ast_str));
    proc_okf(st, true, "d/d%c = %s", var, ast_str);

    ast_free(deriv_result);
    ast_free(root);
    return true;
}
#endif

#if CE_PROC_CALC_TANGENT
static bool handle_tangent(ast_node_t *root, proc_state_t *st)
{
    char var = root->d.tangent.var;
    const ast_node_t *inner = root->d.tangent.expr;
    ast_node_t *deriv_result;
    ce_float_t a_val, fa, slope, intercept;
    static char deriv_buf[128];

    eval_init(&s_eval);
    a_val = eval_expr(&s_eval, root->d.tangent.point);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Cannot evaluate point value");
        ast_free(root);
        return true;
    }

    eval_init(&s_eval);
    eval_set_var(&s_eval, var, a_val);
    fa = eval_expr(&s_eval, inner);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Cannot evaluate f(a)");
        ast_free(root);
        return true;
    }
    {
        static char a_buf[32], fa_buf[32], step_buf[80];
        proc_format_decimal(a_buf, sizeof(a_buf), a_val);
        proc_format_decimal(fa_buf, sizeof(fa_buf), fa);
        snprintf(step_buf, sizeof(step_buf), "f(%s) = %s", a_buf, fa_buf);
        steps_add(step_buf);
    }

    deriv_result = deriv_diff_quiet(inner, var);
    if (!deriv_result) {
        proc_failf(st, "Cannot differentiate function");
        ast_free(root);
        return true;
    }

    ast_to_str(deriv_result, deriv_buf, sizeof(deriv_buf));
    steps_addf("f'(%c) = %s", var, deriv_buf);

    eval_init(&s_eval);
    eval_set_var(&s_eval, var, a_val);
    slope = eval_expr(&s_eval, deriv_result);
    ast_free(deriv_result);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Cannot evaluate derivative at point");
        ast_free(root);
        return true;
    }

    intercept = fa - slope * a_val;
    {
        static char a_buf[32], slope_buf[32], intercept_buf[32], step_buf[96];
        proc_format_decimal(a_buf, sizeof(a_buf), a_val);
        proc_format_decimal(slope_buf, sizeof(slope_buf), slope);
        proc_format_decimal(intercept_buf, sizeof(intercept_buf), intercept);
        snprintf(step_buf, sizeof(step_buf), "f'(%s) = %s", a_buf, slope_buf);
        steps_add(step_buf);
        snprintf(step_buf, sizeof(step_buf), "b = f(a) - m*a = %s", intercept_buf);
        steps_add(step_buf);
    }
    {
        static char slope_buf[32];
        static char intercept_buf[32];
        proc_format_decimal(slope_buf, sizeof(slope_buf), slope);
        proc_format_decimal(intercept_buf, sizeof(intercept_buf),
                            intercept > 0 ? intercept : -intercept);
        if (fabsf(intercept) < 1e-6f)
            proc_okf(st, true, "y = %s*%c", slope_buf, var);
        else if (intercept > 0)
            proc_okf(st, true, "y = %s*%c + %s", slope_buf, var, intercept_buf);
        else
            proc_okf(st, true, "y = %s*%c - %s", slope_buf, var, intercept_buf);
    }

    ast_free(root);
    return true;
}
#endif

#if CE_PROC_CALC_INT_STRATEGY
static ast_node_t *try_integral_advanced(const ast_node_t *expr, char var)
{
    static const integ_strategy_t k_strategies[] = {
        INTEG_STRATEGY_PARTIAL_FRAC,
        INTEG_STRATEGY_TRIGSUB,
        INTEG_STRATEGY_USUB,
        INTEG_STRATEGY_PARTS
    };
    int base_steps = steps_count();
    int i;

    for (i = 0; i < (int)(sizeof(k_strategies) / sizeof(k_strategies[0])); i++) {
        ast_node_t *result;
        steps_restore(base_steps);
        result = integrate_with_strategy(expr, var, k_strategies[i], NULL);
        if (result) {
            return result;
        }
    }

    steps_restore(base_steps);
    return NULL;
}
#endif

#if CE_PROC_CALC_INT && CE_PROC_CALC_INT_DEFINITE
/* A definite-integral value, to six significant digits.
 * proc_format_decimal keeps six decimal places below 4294 (1e-7 printed as
 * 0), rounds to an integer above it (4321.5 printed as 4322) and prints
 * every integer digit (e^20 showed 9, 3 past float32's); review rounds
 * 9-10. "%.*g" is rendered by ce_fmt.c on the CE (force-included since
 * #95); the literal-%g ban in test_ce_numeric_formatting.py predates it. */
static void int_format_value(char *buf, size_t size, ce_float_t v)
{
    snprintf(buf, size, "%.*g", 6, (double)v);
}

/* True when n contains a function whose value depends on the angle mode:
 * sin..atan, asec..acot and angle (inverse trig and angle() return degrees
 * in degree mode). */
static bool int_has_angle(const ast_node_t *n)
{
    if (!n) return false;
    if (n->type == NODE_UNOP) return int_has_angle(n->d.unop.operand);
    if (n->type == NODE_BINOP)
        return int_has_angle(n->d.binop.left) || int_has_angle(n->d.binop.right);
    if (n->type == NODE_FUNC) {
        int id = n->d.func.id;
        if (id <= FN_ATAN || (id >= FN_ASEC && id <= FN_ACOT) || id == FN_ANGLE)
            return true;
        return int_has_angle(n->d.func.arg);
    }
    /* Any other compound node (nested integral, sum, ...) is treated as
     * angle-dependent: refusing is safe. */
    return n->type != NODE_NUMBER && n->type != NODE_VARIABLE &&
           n->type != NODE_CONSTANT;
}

/* Structural test for poles at an integration endpoint. A float
 * bound such as 2*pi or 5*pi/2 can sit a few ulps from a pole, where the
 * integrand is still finite and FTC prints a number for a divergent
 * integral. Testing the whole integrand cannot see this once it is scaled or
 * has regular terms added, so each subexpression that can have a pole is
 * tested on its own: a denominator, the base of a negative power, and
 * tan/sec/cot/csc/csch/coth. b is the bound and bin a point about 1000 ulps
 * inside it. A denominator ten times smaller at b than at bin, or a pole
 * function ten times larger, means a pole within about 100 ulps of b. Any
 * evaluation failure also counts. */
static bool int_pole_near(const ast_node_t *n, char var, ce_float_t b, ce_float_t bin)
{
    const ast_node_t *probe = NULL;
    const ast_node_t *kid[2] = { NULL, NULL };
    bool small = true;
    ce_float_t vb, vi;
    int i;

    if (!n) return false;
    if (n->type == NODE_UNOP) {
        kid[0] = n->d.unop.operand;
    } else if (n->type == NODE_BINOP) {
        kid[0] = n->d.binop.left;
        kid[1] = n->d.binop.right;
        /* A denominator, or the base of a power whose exponent is negative
         * (or cannot be evaluated) at the bound. The exponent is evaluated,
         * not pattern-matched: the parser keeps x^(-3/2) as
         * POW(x, DIV(NEG 3, 2)) and x^(1-2) unfolded (review round 7). */
        if (n->d.binop.op == OP_DIV) {
            probe = kid[1];
        } else if (n->d.binop.op == OP_POW) {
            ce_float_t e;
            if (!integrate_eval_at(kid[1], var, b, &e) || !(e >= 0)) probe = kid[0];
        }
    } else if (n->type == NODE_FUNC) {
        int id = n->d.func.id;
        kid[0] = n->d.func.arg;
        if (id == FN_TAN || (id >= FN_SEC && id <= FN_COT) ||
            (id >= FN_CSCH && id <= FN_COTH)) {
            probe = n;
            small = false;
        }
    }
    for (i = 0; i < 2; i++) {
        if (kid[i] && int_pole_near(kid[i], var, b, bin)) return true;
    }
    if (!probe) return false;
    if (!integrate_eval_at(probe, var, b, &vb) || !ce_isfinite(vb) ||
        !integrate_eval_at(probe, var, bin, &vi) || !ce_isfinite(vi))
        return true;
    vb = fabsf(vb);
    vi = fabsf(vi);
    /* A pole function near its pole is large, so vb > 1 avoids refusing
     * a zero of tan/cot just inside the bound. */
    return small ? (vi > 10.0f * vb) : (vb > 1.0f && vb > 10.0f * vi);
}

/* ── Interior-pole scan ──────────────────────────────────────────
 * Eight fixed probes saw a pole only when one landed on it, so FTC ran
 * across int(1/(x-1)^2,x,0,3) and printed -1.5. Every pole source of the
 * integrand is found structurally, as int_pole_near does: a denominator,
 * the base of a negative power, cos under tan/sec, sin under cot/csc, and
 * the argument of csch/coth. A source is split into factors (the operand of
 * a negation, abs or sqrt; both sides of a product; a numerator; the base of
 * a positive constant power), and each factor s is scanned on a grid of
 * INT_SCAN_N cells for a root:
 *  - a sign change, narrowed by bisection to adjacent floats (0 is tried
 *    first when the bracket spans it);
 *  - a local minimum of |s| at most a quarter of a neighbour, which
 *    golden-section search narrows; it is a root when |s| is within four
 *    rounding scales (integrate_err_scale) of zero, as for an expanded
 *    double root x^2-2x+1.
 * A root at which the integrand fails to evaluate is a pole on a float:
 * builds with limits.c split there and take one-sided limits, the others
 * refuse. A root where the integrand is finite is a pole between floats
 * (tan at the float pi/2, 1/(x^2-2) at sqrt 2): no float32 value decides
 * it, so it is refused, as is a second pole, a factor that fails to
 * evaluate, or an integrand that fails at a grid point other than the
 * pole. A feature narrower than a grid cell (two roots, or a dip to zero,
 * inside one cell with no sign change) is not seen; that is the limit of
 * any sampled scan. */
#define INT_SCAN_N 128

static struct {
    const ast_node_t *f;  /* the integrand */
    ce_float_t lo, hi;    /* grid: finite lo*(1-t) + hi*t; else lo + hi*u^3 */
    ce_float_t x;         /* the pole, when pole == 1 */
    char var;
    int8_t pole;          /* 0 none, 1 one pole on a float at x, 2 refuse */
    int8_t trig;          /* the factor is the argument of: 0 -, 1 cos, 2 sin */
#if !CE_INTEG_IMPROPER_USE_LIMITS
    int8_t zoom;          /* int_zoom depth */
#endif
    char sidx;            /* index of an enclosing sum or product from >= 0 */
    unsigned term_work;   /* shared bound for nested sum/product expansion */
#if CE_INTEG_IMPROPER_USE_LIMITS
    int8_t inf;           /* 0 finite; 1 one infinite bound; 2 both */
#endif
} s_ps;

/* The last static-context evaluation overflowed on an infinite interval
 * (e^x far out); a domain error or division by zero does not count. */
#if CE_INTEG_IMPROPER_USE_LIMITS
#define INT_SCAN_OVERFLOWED() (s_ps.inf && (eval_error(&s_eval) == CE_OK || \
                                            eval_error(&s_eval) == CE_ERR_OVERFLOW))
#else
#define INT_SCAN_OVERFLOWED() 0
#endif

/* Grid point k of INT_SCAN_N. One infinite bound: lo is the finite bound
 * and hi = +-(1 + |lo|) points to the infinite one, u = t/(1-t); both: lo = 0,
 * hi = 1, u = (2t-1)/(1-|2t-1|). Cubing u reaches about 2e6 (1 + |lo|). */
static ce_float_t int_scan_point(int k)
{
    ce_float_t t = (ce_float_t)k / INT_SCAN_N;
#if CE_INTEG_IMPROPER_USE_LIMITS
    if (s_ps.inf) {
        ce_float_t u = s_ps.inf == 2 ? t + t - 1 : t;
        u = u / (1 - fabsf(u));
        return s_ps.lo + s_ps.hi * u * u * u;
    }
#endif
    /* Weighted, not lo + (hi - lo) t: hi - lo can overflow, and CE
     * soft-float wraps instead of giving inf. */
    return s_ps.lo * (1 - t) + s_ps.hi * t;
}

/* n at var = x through the static evaluation context, which keeps an
 * eval_ctx_t (about 170 B) off the 4 KB stack. */
static CE_PROC_ALWAYS_INLINE bool int_eval_static(const ast_node_t *n, char var,
                                                  ce_float_t x, ce_float_t *v)
{
    eval_init(&s_eval);
    eval_set_var(&s_eval, var, x);
    *v = eval_expr(&s_eval, n);
    return eval_error(&s_eval) == CE_OK && ce_isfinite(*v);
}

/* Value of the factor s at x. With s_ps.trig set (a pole or zero of a
 * tan/sec/cot/csc whose argument is s) the factor is cos s or sin s,
 * applied to the evaluated argument: no copied node, and the evaluation
 * runs at the bound checks' stack depth. */
static CE_PROC_ALWAYS_INLINE bool int_factor_at(const ast_node_t *s, ce_float_t x,
                                                ce_float_t *v)
{
    if (!int_eval_static(s, s_ps.var, x, v)) return false;
    if (s_ps.trig) *v = s_ps.trig == 1 ? cosf(*v) : sinf(*v);
    return true;
}

#if CE_INTEG_IMPROPER_USE_LIMITS
/* Evaluate a factor; a failure refuses the integral. */
static bool int_scan_eval(const ast_node_t *s, ce_float_t x, ce_float_t *v)
{
    if (int_factor_at(s, x, v)) return true;
    s_ps.pole = 2;
    return false;
}

/* Every failure of n at x is a bounded 0/0: a quotient whose numerator
 * and denominator are exactly 0 at x, the numerator's zero provably of at
 * least the denominator's order (integrate_quot_bounded), reached
 * only through + - * unary minus, a nonzero denominator, constant powers
 * >= 0 and functions continuous everywhere. Structural, so a large term
 * cannot hide a pole the way it hides it from samples of the whole
 * integrand (1e17 + 1/(x-1)^2, review round 14) or inside the quotient
 * ((1e14 x^3 + x)/x^3, (1 - cos x)/x^4 whose rounded samples read 0, round
 * 15). */
static bool int_removable_at(const ast_node_t *n, ce_float_t x, int depth)
{
    ce_float_t v, w;
    if (depth > 32) return false;
    if (integrate_eval_at(n, s_ps.var, x, &v) && ce_isfinite(v)) return true;
    switch (n->type) {
    case NODE_UNOP:
        return n->d.unop.op == UOP_NEG && int_removable_at(n->d.unop.operand, x, depth + 1);
    case NODE_BINOP:
        switch (n->d.binop.op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
            return int_removable_at(n->d.binop.left, x, depth + 1) &&
                   int_removable_at(n->d.binop.right, x, depth + 1);
        case OP_DIV:
            if (!integrate_eval_at(n->d.binop.right, s_ps.var, x, &w) || !ce_isfinite(w))
                return false;
            if (w != 0) return int_removable_at(n->d.binop.left, x, depth + 1);
            return integrate_eval_at(n->d.binop.left, s_ps.var, x, &v) && v == 0 &&
                   integrate_quot_bounded(n, s_ps.var, x);
        case OP_POW:
            return !ast_contains_var(n->d.binop.right, s_ps.var) &&
                   integrate_eval_at(n->d.binop.right, s_ps.var, x, &w) && w >= 0 &&
                   int_removable_at(n->d.binop.left, x, depth + 1);
        default:
            return false;
        }
    case NODE_FUNC:
        switch (n->d.func.id) {
        case FN_SIN: case FN_COS: case FN_EXP: case FN_ATAN: case FN_ABS:
        case FN_SINH: case FN_COSH: case FN_TANH:
            return int_removable_at(n->d.func.arg, x, depth + 1);
        default:
            return false;
        }
    default:
        return false;
    }
}

/* The integrand fails at x only through removable 0/0 points
 * ((x^2-1)/(x-1) at 1, sin(x)/x and x^2/x at 0): not a pole. Limits
 * builds only (CECIDF has no room to locate the root, so it refuses). */
static bool int_removable(ce_float_t x)
{
    return int_removable_at(s_ps.f, x, 0);
}

/* A root of a pole source at x; zero when the factor is exactly 0 there.
 * Only an exact zero is a pole on a float: eval.c also fails a few ulps
 * from a pole (tan near the float pi/2), where F is still finite and the
 * one-sided limits would print a value for a divergent integral. */
static void int_root_at(const ast_node_t *s, ce_float_t x, bool zero)
{
    ce_float_t v, vl = 0, vr = 0;
    if (s_ps.pole == 2) return;
    /* The zero must also be isolated: x^2-2x+1 is 0 in float32 for
     * |x - 1| < 2.4e-4, and splitting at 1.0002 would let one-sided
     * limits evaluate F right next to the real pole. */
    if (zero && (!int_factor_at(s, nextafterf(x, -INFINITY), &vl) ||
                 !int_factor_at(s, nextafterf(x, INFINITY), &vr) ||
                 vl == 0 || vr == 0))
        zero = false;
    if (zero && !(integrate_eval_at(s_ps.f, s_ps.var, x, &v) && ce_isfinite(v)) &&
        int_removable(x))
        return;
    if (!zero || (integrate_eval_at(s_ps.f, s_ps.var, x, &v) && ce_isfinite(v)) ||
        (s_ps.pole == 1 && x != s_ps.x)) {
        s_ps.pole = 2;
    } else {
        s_ps.pole = 1;
        s_ps.x = x;
    }
}

/* s(a) has sign sa and s(b) the other sign. */
static CE_PROC_NOINLINE void int_bisect(const ast_node_t *s, ce_float_t a,
                                        int8_t sa, ce_float_t b)
{
    ce_float_t m, vm = 1;
    int i;
    for (i = 0; i < 100; i++) {
        m = a * b < 0 ? 0 : a + (b - a) * 0.5f;
        if (m == a || m == b) break;
        if (!int_scan_eval(s, m, &vm)) return;
        if (vm == 0) {
            a = m;
            break;
        }
        if ((vm < 0) == (sa < 0)) a = m; else b = m;
    }
    int_root_at(s, a, vm == 0);
}

/* |s| has a local minimum in [a, b]. Narrow it until the bracket stops
 * shrinking (about 55 steps from 1e6 wide to an ulp of 1); two roots
 * inside one grid cell show up as a sign change between the probes and
 * are bisected. */
static CE_PROC_NOINLINE void int_touch(const ast_node_t *s, ce_float_t a, ce_float_t b)
{
    ce_float_t vc, vd, e;
    int i;
    for (i = 0; i < 160; i++) {
        ce_float_t c = a + (b - a) * 0.381966f;
        ce_float_t d = b - (b - a) * 0.381966f;
        if (c == a || d == b || c == d) break;
        if (!int_scan_eval(s, c, &vc) || !int_scan_eval(s, d, &vd)) return;
        if (vc == 0 || vd == 0) {
            int_root_at(s, vc == 0 ? c : d, true);
            return;
        }
        if ((vc < 0) != (vd < 0)) {
            int_bisect(s, c, vc < 0 ? -1 : 1, d);
            return;
        }
        if (fabsf(vc) < fabsf(vd)) b = d; else a = c;
    }
    if (!int_scan_eval(s, a, &vc)) return;
    if (s_ps.trig) {
        ast_node_t fn;  /* cos(s) or sin(s), for its rounding scale */
        memset(&fn, 0, sizeof(fn));
        fn.type = NODE_FUNC;
        fn.d.func.id = s_ps.trig == 1 ? FN_COS : FN_SIN;
        fn.d.func.arg = (ast_node_t *)s;
        e = integrate_err_scale(&fn, s_ps.var, a);
    } else {
        e = integrate_err_scale(s, s_ps.var, a);
    }
    if (e < 0 || fabsf(vc) <= 4.0f * e) int_root_at(s, a, vc == 0);
}

#endif

#if CE_INTEG_IMPROPER_USE_LIMITS
/* Sign of n everywhere past the last scanned point x0 (> 0) toward
 * infinity, the variable having sign sg there, proved from n's structure:
 * 2 > 0, 1 >= 0, -1 <= 0, -2 < 0, 3 identically 0, 0 unknown. */
enum { TS_UNK = 0, TS_GE = 1, TS_GT = 2, TS_ZERO = 3 };

static int8_t ts_neg(int8_t a)
{
    return a == TS_ZERO ? a : -a;
}

static int8_t ts_add(int8_t a, int8_t b)
{
    if (a == TS_ZERO) return b;
    if (b == TS_ZERO) return a;
    if (a > 0 && b > 0) return a == TS_GT || b == TS_GT ? TS_GT : TS_GE;
    if (a < 0 && b < 0) return a == -TS_GT || b == -TS_GT ? -TS_GT : -TS_GE;
    return TS_UNK;
}

static int8_t ts_mul(int8_t a, int8_t b)
{
    int8_t m;
    if (!a || !b) return TS_UNK;  /* 0 * unknown may be 0 * infinity */
    if (a == TS_ZERO || b == TS_ZERO) return TS_ZERO;
    m = (a < 0) == (b < 0) ? 1 : -1;
    return m * (abs(a) == TS_GT && abs(b) == TS_GT ? TS_GT : TS_GE);
}

static int8_t int_tail_sign(const ast_node_t *n, int8_t sg, ce_float_t x0, int depth)
{
    int8_t a, b;
    ce_float_t v;

    if (depth > 24) return TS_UNK;
    if (!ast_contains_var(n, s_ps.var)) {
        if (!int_eval_static(n, s_ps.var, 0, &v) || !ce_isfinite(v)) return TS_UNK;
        return v > 0 ? TS_GT : v < 0 ? -TS_GT : TS_ZERO;
    }
    switch (n->type) {
    case NODE_VARIABLE:
        return sg * TS_GT;
    case NODE_UNOP:
        if (n->d.unop.op != UOP_NEG) return TS_UNK;
        return ts_neg(int_tail_sign(n->d.unop.operand, sg, x0, depth + 1));
    case NODE_BINOP:
        a = int_tail_sign(n->d.binop.left, sg, x0, depth + 1);
        switch (n->d.binop.op) {
        case OP_ADD:
            return ts_add(a, int_tail_sign(n->d.binop.right, sg, x0, depth + 1));
        case OP_SUB:
            return ts_add(a, ts_neg(int_tail_sign(n->d.binop.right, sg, x0, depth + 1)));
        case OP_MUL:
            return ts_mul(a, int_tail_sign(n->d.binop.right, sg, x0, depth + 1));
        case OP_DIV:
            b = int_tail_sign(n->d.binop.right, sg, x0, depth + 1);
            return abs(b) == TS_GT ? ts_mul(a, b) : TS_UNK;
        case OP_POW:
            if (!ast_contains_var(n->d.binop.right, s_ps.var)) {
                if (!int_eval_static(n->d.binop.right, s_ps.var, 0, &v) || !ce_isfinite(v))
                    return TS_UNK;
                if (v == floorf(v) && fabsf(v) < 1e6f) {
                    if (fmodf(v, 2.0f) == 0)  /* even: |base|^v */
                        return a == TS_ZERO ? (v > 0 ? TS_ZERO : TS_UNK)
                             : abs(a) == TS_GT ? TS_GT : TS_GE;
                    return v < 0 && abs(a) != TS_GT ? TS_UNK : a;  /* odd */
                }
                return a == TS_GT ? TS_GT : a == TS_GE && v > 0 ? TS_GE : TS_UNK;
            }
            /* b^g, b > 0 constant (e^g) */
            return a == TS_GT && !ast_contains_var(n->d.binop.left, s_ps.var) ? TS_GT : TS_UNK;
        default:
            return TS_UNK;
        }
    case NODE_FUNC:
        a = int_tail_sign(n->d.func.arg, sg, x0, depth + 1);
        switch (n->d.func.id) {
        case FN_EXP:
        case FN_COSH:
            return TS_GT;
        case FN_ABS:
            return abs(a) == TS_GT ? TS_GT : a == TS_ZERO ? a : TS_GE;
        case FN_SQRT:
            /* Undefined for a negative argument, not >= 0 there. */
            return a == TS_GT ? TS_GT : a == TS_ZERO ? a : a == TS_GE ? TS_GE : TS_UNK;
        case FN_SINH:
        case FN_TANH:
        case FN_ATAN:
        case FN_ASINH:
            return a;  /* odd, increasing */
        case FN_LN:
            /* ln x past x0 > 1 */
            return n->d.func.arg->type == NODE_VARIABLE && sg > 0 && x0 > 1 ? TS_GT : TS_UNK;
        default:
            return TS_UNK;
        }
    default:
        return TS_UNK;
    }
}

/* An infinite interval is sampled out to about 2e6 times its scale; past
 * that, a pole source needs a proof that it has no root (1/(x-3000000) on
 * [0,inf) printed 0.693148, review round 8). A polynomial factor has none
 * past its Fujiwara root bound; any other factor needs a structural sign
 * (int_tail_sign). Sampling further out proves nothing: 70 - ln x keeps
 * its sign up to 2.5e30 (review round 10). */
static CE_PROC_NOINLINE bool int_tail_clear(const ast_node_t *s)
{
    ce_float_t bnd, x;
    int end;
    bool poly = !s_ps.trig && integrate_poly_root_bound(s, s_ps.var, &bnd);

    for (end = 0; end < 2; end++) {
        if (end == 0 && s_ps.inf != 2) continue;
        x = int_scan_point(end ? INT_SCAN_N - 1 : 1);
        if (poly ? !(fabsf(x) > bnd)
                 : s_ps.trig || abs(int_tail_sign(s, x < 0 ? -1 : 1, fabsf(x), 0)) != TS_GT)
            return false;
    }
    return true;
}
#endif


#if !CE_INTEG_IMPROPER_USE_LIMITS
static void int_root_scan(const ast_node_t *s);

/* Rescan the two cells around grid point j (CECIDF). A positive minimum is
 * smooth at the finer scale and passes; a zero keeps dipping until a sample
 * is 0 or changes sign, or the zoom cannot go deeper and the integral is
 * refused: INT_ZOOM_MAX levels, or cells too narrow for float32. */
#define INT_ZOOM_MAX 6
static CE_PROC_NOINLINE void int_zoom(const ast_node_t *s, int j)
{
    ce_float_t lo = s_ps.lo, hi = s_ps.hi;
    /* Compute both points before changing the grid's bounds. */
    ce_float_t a = int_scan_point(j > 0 ? j - 1 : 0);
    ce_float_t b = int_scan_point(j < INT_SCAN_N ? j + 1 : INT_SCAN_N);
    s_ps.lo = a;
    s_ps.hi = b;
    if (s_ps.zoom == INT_ZOOM_MAX ||
        !(fabsf(s_ps.hi - s_ps.lo) > 1e-5f * (fabsf(s_ps.lo) + fabsf(s_ps.hi)))) {
        s_ps.pole = 2;
    } else {
        s_ps.zoom++;
        int_root_scan(s);
        s_ps.zoom--;
    }
    s_ps.lo = lo;
    s_ps.hi = hi;
}
#endif

static CE_PROC_NOINLINE void int_root_scan(const ast_node_t *s)
{
    int k, k0 = 0;
    int8_t sg, sgp = 0;  /* signs of s at x and the previous sample xp */
    ce_float_t x, v;
    ce_float_t m, mp = 0, mpp = 0;  /* |s| at x and the two samples before */
#if CE_INTEG_IMPROPER_USE_LIMITS
    int8_t sgpp = 0;
    ce_float_t xp = 0, xpp = 0;
#endif

    if (!ast_contains_var(s, s_ps.var)) return;
#if CE_INTEG_IMPROPER_USE_LIMITS
    if (s_ps.inf == 2) k0 = 1;  /* t = 0 and 1 are the infinite bounds */
    if (s_ps.inf && !int_tail_clear(s)) {
        s_ps.pole = 2;
        return;
    }
#endif
    /* k = INT_SCAN_N + 1 is a virtual neighbour past the last sample, so a
     * minimum of |s| on the last sample is refined too; the first sample
     * gets one through mpp. One infinite bound: the last sample is k =
     * INT_SCAN_N - 1. */
    for (k = k0; k <= INT_SCAN_N + 1 && s_ps.pole != 2; k++) {
#if CE_INTEG_IMPROPER_USE_LIMITS
        if (s_ps.inf && k == INT_SCAN_N) continue;
        if (k > INT_SCAN_N) {
            x = xp;
            m = 3e38f;
            sg = sgp;
        } else
#else
        if (k > INT_SCAN_N) return;
#endif
        {
            x = int_scan_point(k);
            if (!int_factor_at(s, x, &v)) {
                /* On an infinite interval a factor may overflow far out
                 * (e^x + 1): the scan ends there. Any other failure
                 * refuses. */
                if (!INT_SCAN_OVERFLOWED()) s_ps.pole = 2;
                return;
            }
            sg = v < 0 ? -1 : v > 0;
#if CE_INTEG_IMPROPER_USE_LIMITS
            m = fabsf(v);
#endif
        }
#if CE_INTEG_IMPROPER_USE_LIMITS
        if (!sg) {
            int_root_at(s, x, true);
        } else if (sgp == -sg) {
            int_bisect(s, xp, sgp, x);
        } else if (sgp == sg && sgpp == sg && mp <= mpp && mp <= m &&
                   (mp <= 0.25f * mpp || mp <= 0.25f * m)) {
            int_touch(s, xpp, x);
        }
        if (k == k0) {
            mpp = 3e38f;
            sgpp = sg;
            xpp = x;
        } else {
            mpp = mp;
            sgpp = sgp;
            xpp = xp;
        }
        mp = m;
        xp = x;
#else
        /* Without endpoint limits every pole is refused, so where it sits
         * does not matter: a zero or a sign change is enough. A sharp
         * local minimum of |s| may hide a double root between samples
         * (x^2 - 2x + 1, review rounds 17-18): int_zoom rescans the cells
         * around it. Sharp means a quarter of the larger neighbour or less;
         * a double root at d <= h/2 from its nearest sample gives at most
         * (h/2)^2 / (3h/2)^2 = 1/9. A bound's sample is judged against the
         * second sample in, which a double root in the end cell also
         * keeps under 1/9 (a root nearer the inner sample makes that one
         * the minimum). */
        m = fabsf(v);
        if (!sg || sgp == -sg) {
            s_ps.pole = 2;
            return;
        }
        if (k > 1) {
            if (mp <= mpp && mp <= m && (mp <= 0.25f * mpp || mp <= 0.25f * m))
                int_zoom(s, k - 1);
            if (k == 2 && mpp <= mp && mpp <= 0.25f * m) int_zoom(s, 0);
            if (k == INT_SCAN_N && m <= mp && m <= 0.25f * mpp) int_zoom(s, INT_SCAN_N);
        }
        mpp = mp;
        mp = m;
#endif
        sgp = sg;
    }
}

/* The base of a power is a pole source unless the exponent is >= 0 (> 0
 * when it splits a factor). Evaluated, not pattern-matched: the parser
 * keeps x^(-3/2) as POW(x, DIV(NEG 3, 2)) and x^(1-2) unfolded. An
 * exponent with the variable in it (x^(0*x+2)) is evaluated on the grid. */
static bool int_pow_at_least(const ast_node_t *e, ce_float_t min)
{
    ce_float_t v;
    int k;
    if (!ast_contains_var(e, s_ps.var))
        return (integrate_eval_at(e, s_ps.var, 0, &v) && v >= min) ||
               (e->type == NODE_VARIABLE && e->d.variable == s_ps.sidx);
    for (k = 0; k <= INT_SCAN_N; k++) {
#if CE_INTEG_IMPROPER_USE_LIMITS
        if (s_ps.inf && (k == INT_SCAN_N || (k == 0 && s_ps.inf == 2))) continue;
#endif
        if (!integrate_eval_at(e, s_ps.var, int_scan_point(k), &v) || !(v >= min))
            return false;
    }
    return true;
}

static void int_pole_factors(const ast_node_t *s)
{
    while (s && s_ps.pole != 2) {
        if (s->type == NODE_UNOP && s->d.unop.op == UOP_NEG) {
            s = s->d.unop.operand;
        } else if (s->type == NODE_FUNC &&
                   (s->d.func.id == FN_ABS || s->d.func.id == FN_SQRT)) {
            s = s->d.func.arg;
        } else if (s->type == NODE_FUNC && s->d.func.id >= FN_TAN &&
                   s->d.func.id <= FN_COT) {
            /* tan u is zero where sin u is, cot u where cos u is; sec and
             * csc never are. Its poles change its sign but are no roots
             * (1/tan(x) on [1,2], review round 2). */
            if (s->d.func.id == FN_TAN || s->d.func.id == FN_COT) {
                s_ps.trig = s->d.func.id == FN_TAN ? 2 : 1;
                int_root_scan(s->d.func.arg);
                s_ps.trig = 0;
            }
            return;
        } else if (s->type == NODE_BINOP && s->d.binop.op == OP_MUL) {
            /* Recurse right and loop left: products parse left-associated,
             * so x*x*...*x stays one frame deep. */
            int_pole_factors(s->d.binop.right);
            s = s->d.binop.left;
        } else if (s->type == NODE_BINOP && (s->d.binop.op == OP_DIV ||
                   (s->d.binop.op == OP_POW &&
                    int_pow_at_least(s->d.binop.right, 1e-30f)))) {
            s = s->d.binop.left;
        } else if (s->type == NODE_BINOP && s->d.binop.op == OP_POW &&
                   !ast_contains_var(s->d.binop.right, s_ps.var) &&
                   !int_pow_at_least(s->d.binop.right, 0)) {
            /* b^-k is zero only where b is infinite: b's poles, which the
             * walk finds (1/(cos(x)^(-1)) on [1,2], review round 5). */
            return;
        } else {
            int_root_scan(s);
            return;
        }
    }
}

static CE_PROC_NOINLINE void int_pole_func(const ast_node_t *n)
{
    int id = n->d.func.id;
    if (id == FN_TAN || (id >= FN_SEC && id <= FN_COT)) {
        s_ps.trig = (id == FN_TAN || id == FN_SEC) ? 1 : 2;
        int_root_scan(n->d.func.arg);
        s_ps.trig = 0;
    } else if (id == FN_CSCH || id == FN_COTH) {
        int_pole_factors(n->d.func.arg);
    }
}

/* den: n sits in a denominator, reached from it through + - * abs sqrt and
 * powers >= 0 only. A tan/sec/cot/csc/csch/coth pole there makes the
 * denominator infinite and the integrand zero (1/tan(x), cos(x)/(1 +
 * tan(x)^2); review round 2), so it is not a pole source. Where poles
 * cancel inside it (tan(x) - sec(x) -> 0 at pi/2), the denominator's own
 * zero is found by int_pole_factors. A function in between (e^(tan x))
 * clears den. */

static void int_pole_walk(const ast_node_t *n, bool den);

/* Replace variable v by val in n, in place (a copy of a sum's term). An
 * inner sum, product or integral over v keeps its own v. False when n has
 * a node with v in it that this does not know (a limit, a derivative). */
static bool int_subst(ast_node_t *n, char v, ce_float_t val)
{
    while (n) {
        switch (n->type) {
        case NODE_VARIABLE:
            if (n->d.variable == v) {
                n->type = NODE_NUMBER;
                n->d.number = val;
            }
            return true;
        case NODE_NUMBER:
        case NODE_CONSTANT:
            return true;
        case NODE_BINOP:
            if (!int_subst(n->d.binop.right, v, val)) return false;
            n = n->d.binop.left;
            break;
        case NODE_UNOP:
            n = n->d.unop.operand;
            break;
        case NODE_FUNC:
            n = n->d.func.arg;
            break;
        case NODE_SUM:
        case NODE_PRODUCT:
        case NODE_INTEGRAL:
            if (!int_subst(n->d.sum.lo, v, val) || !int_subst(n->d.sum.hi, v, val))
                return false;
            if (n->d.sum.var == v) return true;
            n = n->d.sum.expr;
            break;
        case NODE_PIECEWISE:
            n = n->d.piecewise.pieces;
            break;
        case NODE_PAIR:
            if (!int_subst(n->d.pair.head, v, val)) return false;
            n = n->d.pair.tail;
            break;
        default:
            return !ast_contains_var(n, v);
        }
    }
    return true;
}

/* Poles of a finite sum's or product's terms and an inner integral's
 * integrand, and of their bounds (review rounds 17-18; the three nodes
 * share one layout, asserted below). Their own variable shadows the
 * integration variable in the body: int(int(1/x^2,x,1,2),x,0,3) has no
 * pole. A sum or product with bounds free of the variable and at most
 * INT_SUM_TERMS terms is walked term by term, its index replaced by each
 * value in a copy (n*x^(n-1), 1/(x^2+n)); all nested expansions share that
 * many term walks, and a sum that no longer fits is walked unbound
 * (sum(1/(x+n),n,1,17) answers, review round 20). Index bounds use eval's
 * CE int range, -2^23 inclusive to 2^23 exclusive. Otherwise the index or
 * inner variable stays unknown, so a factor with it in cannot be evaluated
 * and refuses; x^n is exempt for an index from a fixed bound >= 0. */
#define INT_SUM_TERMS 256  /* also the shared expansion budget (term_work) */
static CE_PROC_NOINLINE void int_pole_bound(const ast_node_t *n, bool den)
{
    char save = s_ps.sidx, iv = n->d.sum.var;
    ce_float_t lo, hi;
    int k;
    /* A nested binder may reuse an outer index's letter. Never inherit
     * its nonnegative-range exemption into the new scope. */
    s_ps.sidx = 0;
    int_pole_walk(n->d.sum.lo, false);
    int_pole_walk(n->d.sum.hi, false);
    if (iv == s_ps.var) {
        s_ps.sidx = save;
        return;
    }
    if (n->type != NODE_INTEGRAL && n->d.sum.lo && n->d.sum.hi &&
        !ast_contains_var(n->d.sum.lo, s_ps.var) &&
        !ast_contains_var(n->d.sum.hi, s_ps.var) &&
        integrate_eval_at(n->d.sum.lo, s_ps.var, 0, &lo) &&
        integrate_eval_at(n->d.sum.hi, s_ps.var, 0, &hi)) {
        /* Count with an integer loop, not i += 1 in float32: above 2^24
         * the latter can stall forever. Match eval's integer bounds. */
        if (!ce_isfinite(lo) || !ce_isfinite(hi) ||
            lo < -8388608.0f || lo >= 8388608.0f ||
            hi < -8388608.0f || hi >= 8388608.0f ||
            floorf(lo) != lo || floorf(hi) != hi) {
            s_ps.pole = 2;
            s_ps.sidx = save;
            return;
        }
        /* A body without the index is the same term each time: walk it
         * once, charging nothing (sum(sum(1/(x+m),m,1,1),n,1,129), review
         * round 21); an empty range has no term to walk. */
        if (!ast_contains_var(n->d.sum.expr, iv)) {
            if (hi >= lo) int_pole_walk(n->d.sum.expr, den);
            s_ps.sidx = save;
            return;
        }
        /* Reserve every term before walking them, so nested expansions
         * see only what is left; past that, walk the body unbound below. */
        if (hi - lo < (ce_float_t)(INT_SUM_TERMS - s_ps.term_work)) {
            if (hi >= lo) s_ps.term_work += (unsigned)(hi - lo) + 1;
            for (k = 0; lo + k <= hi && s_ps.pole != 2; k++) {
                ast_node_t *t;
                t = ast_clone(n->d.sum.expr);
                if (!t || !ast_tree_complete(t) || !int_subst(t, iv, lo + k))
                    s_ps.pole = 2;
                else
                    int_pole_walk(t, den);
                ast_free(t);
            }
            s_ps.sidx = save;
            return;
        }
        if (lo >= 0) s_ps.sidx = iv;
    }
    int_pole_walk(n->d.sum.expr, den);
    s_ps.sidx = save;
}

static void int_pole_walk(const ast_node_t *n, bool den)
{
    while (n && s_ps.pole != 2) {
        if (n->type == NODE_UNOP) {
            if (n->d.unop.op != UOP_NEG) den = false;
            n = n->d.unop.operand;
        } else if (n->type == NODE_FUNC) {
            if (!den) int_pole_func(n);
            /* abs and sqrt keep an infinite denominator infinite
             * (1/sqrt(sec(x)^4) on [1,2], review round 6). */
            if (n->d.func.id != FN_ABS && n->d.func.id != FN_SQRT) den = false;
            n = n->d.func.arg;
        } else if (n->type == NODE_BINOP) {
            bool rden = den;
            /* Inside a denominator, an inner denominator's zeros make it
             * infinite, which is harmless too (1/(1/cos(x)), review round
             * 4); the outer denominator's own zeros are its numerator's,
             * scanned where it was found. */
            if (n->d.binop.op == OP_DIV) {
                if (!den) int_pole_factors(n->d.binop.right);
                rden = !den;
            } else if (n->d.binop.op == OP_POW) {
                rden = false;  /* the exponent */
                if (!int_pow_at_least(n->d.binop.right, 0)) {
                    if (!den) int_pole_factors(n->d.binop.left);
                    den = !den;
                }
            }
            /* Recurse right and loop left: sums and products parse
             * left-associated, so a 60-term chain stays one frame deep on
             * the 4 KB stack. */
            int_pole_walk(n->d.binop.right, rden);
            n = n->d.binop.left;
        } else if (n->type == NODE_SUM || n->type == NODE_PRODUCT ||
                   n->type == NODE_INTEGRAL) {
            int_pole_bound(n, den);
            return;
        } else if (n->type == NODE_PIECEWISE) {
            n = n->d.piecewise.pieces;
        } else if (n->type == NODE_PAIR) {
            /* piecewise's values and conditions */
            int_pole_walk(n->d.pair.head, den);
            n = n->d.pair.tail;
        } else {
            /* Any other node with the variable in it (a limit, a
             * derivative) cannot be scanned: refuse. */
            if (n->type > NODE_CONSTANT && ast_contains_var(n, s_ps.var))
                s_ps.pole = 2;
            return;
        }
    }
}

typedef char int_sum_integral_layout[
    (offsetof(ast_node_t, d.sum.expr) == offsetof(ast_node_t, d.integral.expr) &&
     offsetof(ast_node_t, d.sum.expr) == offsetof(ast_node_t, d.product.expr) &&
     offsetof(ast_node_t, d.sum.var) == offsetof(ast_node_t, d.product.var) &&
     offsetof(ast_node_t, d.sum.lo) == offsetof(ast_node_t, d.product.lo) &&
     offsetof(ast_node_t, d.sum.hi) == offsetof(ast_node_t, d.product.hi) &&
     offsetof(ast_node_t, d.sum.lo) == offsetof(ast_node_t, d.integral.lo) &&
     offsetof(ast_node_t, d.sum.hi) == offsetof(ast_node_t, d.integral.hi)) ? 1 : -1];

/* Scan f for interior poles; see above. Returns s_ps.pole (pole at s_ps.x).
 * The grid's end points are the caller's: finite bounds are checked before,
 * and an infinite bound is never sampled. */
/* Inlined into handle_integral so the integrand is evaluated at the same
 * stack depth as the bound checks (see int_factor_at). */
static CE_PROC_ALWAYS_INLINE int8_t int_scan_poles(const ast_node_t *f, char var,
                                               ce_float_t lo, ce_float_t hi
#if CE_INTEG_IMPROPER_USE_LIMITS
                                               , int8_t inf
#endif
                                               )
{
    ce_float_t v, x;
    int k;
    s_ps.f = f;
    s_ps.var = var;
    s_ps.lo = lo;
    s_ps.hi = hi;
    s_ps.pole = 0;
    s_ps.trig = 0;
    s_ps.sidx = 0;
    s_ps.term_work = 0;
#if CE_INTEG_IMPROPER_USE_LIMITS
    s_ps.inf = inf;
#endif
    int_pole_walk(f, false);
    for (k = 1; k < INT_SCAN_N && s_ps.pole != 2; k++) {
        x = int_scan_point(k);
        if (!int_eval_static(f, var, x, &v) && !(s_ps.pole == 1 && x == s_ps.x)) {
            if (INT_SCAN_OVERFLOWED()) break;  /* see int_root_scan */
#if CE_INTEG_IMPROPER_USE_LIMITS
            if (int_removable(x)) continue;
#endif
            s_ps.pole = 2;
        }
    }
    return s_ps.pole;
}

#if CE_INTEG_IMPROPER_USE_LIMITS
/* The integral across a pole at x: DIVERGES when a one-sided half
 * (lo..x or x..hi) proves it, otherwise refused. */
static improp_status_t int_split_improper(const ast_node_t *f, char var,
                                          ce_float_t lo, bool lo_inf, ce_float_t x,
                                          ce_float_t hi, bool hi_inf, ce_float_t *out)
{
    ce_float_t r = 0;
    /* limits.c reads a finite point past 1e15 as infinity, so a pole or a
     * finite bound out there cannot be decided (int(1/(x-1e16),x,0,3e16)). */
    if (fabsf(x) >= 1e15f || (!lo_inf && fabsf(lo) >= 1e15f) ||
        (!hi_inf && fabsf(hi) >= 1e15f))
        return IMPROP_FAILED;
    /* A finite value through the pole would rest on compute_limit's
     * numeric limit of F there, which F's last probe fools: 300 + 1/(x-1)
     * on [0,3] gave 900.753 (review round 14). As at a singular bound
     * (end_bad), only divergence is trusted. */
    if (integrate_improper(f, var, lo, lo_inf, x, false, out) == IMPROP_DIVERGES ||
        integrate_improper(f, var, x, false, hi, hi_inf, &r) == IMPROP_DIVERGES)
        return IMPROP_DIVERGES;
    return IMPROP_FAILED;
}
#endif
#endif

#if CE_PROC_CALC_INT
/* A worker that neither simplifies nor folds (CECIDF, for size) shows the
 * value only; integrate.h CE_INTEG_DEFINITE_PRINT_F decides (and leaves the
 * printer out of that worker). */
#define INT_SHOW_ANTIDERIV CE_INTEG_DEFINITE_PRINT_F
#if CE_PROC_CALC_INT_DEFINITE && CE_PROC_CALC_INT_POLAR_LOWER
/* Two callers (run_preparsed and handle_int_polar): keep one copy of the
 * body, or LTO inlines all of it into handle_int_polar (+6.8 KB, CECIDF over
 * 51,200 B) and stacks run_preparsed's frame on the int path. */
#define INT_BODY_ATTR __attribute__((noinline))
#else
#define INT_BODY_ATTR
#endif
static INT_BODY_ATTR bool handle_integral_body(ast_node_t *root, proc_state_t *st) CE_MAYBE_UNUSED;

/* int() and bounded improper(). A definite refusal after an evaluation hit
 * eval's depth cap says so, as #128's other refusals do (the pole scan
 * refuses such inputs before ipc.c's rewrite of an answer would). Not an
 * indefinite one: CEIS answers CEIADVREQ to forward it (review round 17).
 * root is freed inside, so read it first. */
static CE_PROC_ALWAYS_INLINE bool handle_integral(ast_node_t *root, proc_state_t *st)
    CE_MAYBE_UNUSED;
static CE_PROC_ALWAYS_INLINE bool handle_integral(ast_node_t *root, proc_state_t *st)
{
#if CE_PROC_CALC_INT_DEFINITE
    bool definite = root->d.integral.lo != NULL;
    bool r = handle_integral_body(root, st);
    if (definite && !st->ok && g_eval_refused) proc_failf(st, "Eval recursion limit");
    return r;
#else
    return handle_integral_body(root, st);
#endif
}

static INT_BODY_ATTR bool handle_integral_body(ast_node_t *root, proc_state_t *st)
{
    char var = root->d.integral.var;
    const ast_node_t *inner = root->d.integral.expr;
    char ast_str[128];
    (void)var;
    (void)inner;
    (void)ast_str;

    if (root->d.integral.lo && root->d.integral.hi) {
#if CE_PROC_CALC_INT_DEFINITE
        ce_float_t lo_val, hi_val, result_val;
        ce_error_t ierr;
        char antideriv_str[128];
        bool end_bad = false;  /* f fails or blows up at a bound */

        eval_init(&s_eval);
        lo_val = eval_expr(&s_eval, root->d.integral.lo);
        hi_val = eval_expr(&s_eval, root->d.integral.hi);
        if (eval_error(&s_eval) != CE_OK || ce_isnan(lo_val) || ce_isnan(hi_val)) {
            proc_failf(st, "Could not evaluate integral bounds");
            ast_free(root);
            return true;
        }
        /* Bounds typed differently that round to the same float32
         * (int(e^(x^2),x,1,1+1/100000000)) need not be a zero-width
         * interval: refuse rather than print 0 (review rounds 20-21; an
         * exact structural compare, because printed text keeps only six
         * digits). Bounds typed alike (int(f,x,2,2), int(f,x,pi,pi)) are 0.
         * A literal that float32 cannot hold (1.00000001) is already
         * rounded by the parser; that is the engine's resolution. */
        if (lo_val == hi_val &&
            !ast_same_bound(root->d.integral.lo, root->d.integral.hi)) {
            proc_failf(st, "Could not decide integral");
            ast_free(root);
            return true;
        }

        /* integrate() antiderivatives (and the improper paths built on
         * them) assume radians: in degree mode int(cos(x),x,0,90) is
         * 180/pi, not 1. An integrand with no trig function is angle-
         * independent; one with trig is refused rather than given the
         * radian answer or an unchecked numeric one (review round 8). */
        if (g_settings.angle_mode != ANGLE_RADIANS && int_has_angle(inner)) {
            proc_failf(st, "Trig integral: use RADIAN mode");
            ast_free(root);
            return true;
        }

        /* ── Detect improper integral (infinite or singular bounds) ── */
        {
            bool lo_inf = !ce_isfinite(lo_val);
            bool hi_inf = !ce_isfinite(hi_val);

            if (lo_inf || hi_inf) {
#if CE_INTEG_IMPROPER_USE_LIMITS
                /* Type 1: one or both bounds are ±infinity. Pass the real
                 * values: integrate_improper() orients them (int(f,x,0,-inf)
                 * is -int(f,x,-inf,0); equal infinite bounds give 0). */
                ce_float_t improp_result = 0;
                improp_status_t improp_st = IMPROP_FAILED;
                ce_float_t fin = lo_inf ? hi_val : lo_val;
                int8_t pole = 0;
                /* Poles inside the interval, on a grid that reaches about
                 * 2e6 (1 + |bound|): int(1/(x-2)^2,x,1,inf) printed
                 * -1 (converges). Equal infinite bounds give 0 below. */
                if (!(lo_inf && hi_inf && (lo_val > 0) == (hi_val > 0))) {
                    pole = lo_inf && hi_inf
                         ? int_scan_poles(inner, var, 0, 1, 2)
                         : int_scan_poles(inner, var, fin,
                                          ((lo_inf ? lo_val : hi_val) > 0 ? 1 : -1) *
                                          (1 + fabsf(fin)), 1);
                    /* A pole at the finite bound is that bound's business. */
                    if (pole == 1 && !(lo_inf && hi_inf) && s_ps.x == fin) pole = 0;
                }
                if (pole == 1) {
                    improp_st = int_split_improper(inner, var, lo_val, lo_inf, s_ps.x,
                                                   hi_val, hi_inf, &improp_result);
                } else if (pole == 0) {
                    improp_st = integrate_improper(inner, var,
                                                   lo_val, lo_inf,
                                                   hi_val, hi_inf,
                                                   &improp_result);
                }
                switch (improp_st) {
                case IMPROP_OK:
                    st->has_numeric  = true;
                    st->last_numeric = improp_result;
                    proc_ok_number(st, true, "= ", improp_result, "  (converges)");
                    break;
                case IMPROP_DIVERGES:
                    proc_okf(st, true, "Integral DIVERGES");
                    break;
                default:
                    proc_failf(st, "Could not determine improper integral convergence");
                    break;
                }
#else
                /* Infinite bounds need limits at infinity. Sampling F there
                 * is fooled both ways (int(1e-9/x,x,1,inf) "converged" to 0,
                 * int(e^(-x/1000),x,0,inf) looked divergent), and this
                 * build has no limits.c, so it cannot decide the integral. */
                proc_failf(st, "Could not decide integral");
#endif
                ast_free(root);
                return true;
            }

            /* Type 2: finite bounds. A bound where f fails, or where
             * int_pole_near() finds a subexpression pole within a few ulps
             * (tan at the float pi/2, cot at the float 2*pi), is an improper
             * endpoint: FTC across it cannot be trusted, so such integrals
             * are refused below unless an endpoint limit proves divergence.
             * Poles inside (lo, hi) are found by int_scan_poles(); eight
             * fixed probes missed the pole in int(1/(x-1)^2,x,0,3). */
            {
                int k;
                for (k = 0; k < 2; k++) {
                    ce_float_t xk = k ? lo_val : hi_val;
                    ce_float_t vk;
                    /* Step inward about 1000 ulps of the bound (at least
                     * 1e-7 of the width), never past the midpoint. */
                    ce_float_t w = hi_val - lo_val;
                    ce_float_t d = 1.2e-4f * (fabsf(xk) + 1e-3f * fabsf(w));
                    bool bad = !int_eval_static(inner, var, xk, &vk);
                    if (d > 0.5f * fabsf(w)) d = 0.5f * fabsf(w);
                    if (w < 0) d = -d;
                    if (bad || int_pole_near(inner, var, xk, k ? xk + d : xk - d))
                        end_bad = true;
                }
                if (!end_bad) {
#if CE_INTEG_IMPROPER_USE_LIMITS
                    int8_t pole = int_scan_poles(inner, var, lo_val, hi_val, 0);
                    if (pole == 1) {
                        ce_float_t improp_result = 0;
                        steps_addf("Detected singularity in integrand - using improper integral limits");
                        /* Split at the pole so each half takes a one-sided
                         * endpoint limit; F(hi)-F(lo) across it is wrong
                         * (int(1/x^2,x,-1,1) is not -2). */
                        switch (int_split_improper(inner, var, lo_val, false, s_ps.x,
                                                   hi_val, false, &improp_result)) {
                        case IMPROP_OK:
                            st->has_numeric  = true;
                            st->last_numeric = improp_result;
                            proc_ok_number(st, true, "= ", improp_result, "  (converges)");
                            break;
                        case IMPROP_DIVERGES:
                            proc_okf(st, true, "Integral DIVERGES");
                            break;
                        default:
                            proc_failf(st, "Could not evaluate Type 2 improper integral");
                            break;
                        }
                        ast_free(root);
                        return true;
                    }
#else
                    /* No endpoint limits here: a pole inside the interval
                     * is refused like one at a bound. */
                    int8_t pole = int_scan_poles(inner, var, lo_val, hi_val);
#endif
                    if (pole) end_bad = true;
                }
            }
        }

        if (end_bad) {
#if CE_INTEG_IMPROPER_USE_LIMITS
            /* A symbolic endpoint limit can still prove divergence (F -> inf
             * at the bound). Any finite value from here is not trusted. */
            ce_float_t improp_result = 0;
            /* limits.c treats |point| > 1e15 as infinity, so only smaller
             * finite bounds can prove divergence here. */
            if (fabsf(lo_val) < 1e15f && fabsf(hi_val) < 1e15f &&
                integrate_improper(inner, var, lo_val, false, hi_val, false,
                                   &improp_result) == IMPROP_DIVERGES) {
                proc_okf(st, true, "Integral DIVERGES");
                ast_free(root);
                return true;
            }
#endif
            proc_failf(st, "Could not decide integral");
            ast_free(root);
            return true;
        }

        {
            /* A failed symbolic attempt must not leave its step lines in
             * front of the numeric fallback's Simpson steps. */
            int steps_before = steps_save();
            /* A worker that shows only the value (below) does not print
             * F at all, which leaves ast_print (3.6 KB) out of CECIDF. */
            result_val = integrate_definite(inner, var, lo_val, hi_val,
                                            INT_SHOW_ANTIDERIV ? antideriv_str : NULL,
                                            (int)sizeof(antideriv_str), &ierr);
            if (ierr != CE_OK) steps_restore(steps_before);
        }
        if (ierr == CE_ERR_PRECISION) {
            /* float32 rounding swamps F(b) - F(a); a Simpson value would be
             * no better (its sample points round the same way). */
            proc_failf(st, "Could not decide integral");
            ast_free(root);
            return true;
        }
        if (ierr == CE_ERR_OVERFLOW) {
            /* The antiderivative is undefined or overflows at a bound (e.g.
             * -1/x at 0 for int(1/x^2,x,0,1)). Take one-sided endpoint
             * limits where this build has them; otherwise refuse. */
#if CE_INTEG_IMPROPER_USE_LIMITS
            ce_float_t improp_result = 0;
            /* limits.c treats |point| > 1e15 as infinity: with such a finite
             * bound an overflowing F would be read as divergence
             * (int(x^2,x,0,1e16)). Refuse instead. */
            improp_status_t improp_st =
                (fabsf(lo_val) < 1e15f && fabsf(hi_val) < 1e15f)
                    ? integrate_improper(inner, var, lo_val, false,
                                         hi_val, false, &improp_result)
                    : IMPROP_FAILED;
            /* The same rounding budget as the plain FTC (review round 12). */
            if (improp_st == IMPROP_OK &&
                (integrate_improper_unc() < 0 ||
                 integrate_improper_unc() * 8192.0f > fabsf(improp_result)))
                improp_st = IMPROP_FAILED;
            if (improp_st == IMPROP_OK) {
                st->has_numeric  = true;
                st->last_numeric = improp_result;
                proc_ok_number(st, true, "= ", improp_result, "  (converges)");
                ast_free(root);
                return true;
            }
            if (improp_st == IMPROP_DIVERGES) {
                proc_okf(st, true, "Integral DIVERGES");
                ast_free(root);
                return true;
            }
#endif
            /* F is undefined or overflows at a bound and the endpoint limits
             * could not decide. A Simpson value here is not trustworthy
             * either (int(tanh(x),x,-1e6,1e6+1) gave 2875), so refuse. */
            proc_failf(st, "Could not decide integral");
            ast_free(root);
            return true;
        }
        if (ierr != CE_OK) {
            ce_float_t num_val;
            eval_init(&s_eval);
            num_val = eval_expr(&s_eval, root);

            if (eval_error(&s_eval) != CE_OK || !ce_isfinite(num_val)) {
                proc_failf(st, "Integral failed: %s",
                           eval_error(&s_eval) != CE_OK ? eval_error_msg(&s_eval)
                                                        : "result overflow");
                ast_free(root);
                return true;
            }
            st->has_numeric = true;
            st->last_numeric = num_val;
            {
                char nbuf[32];
                /* Grid agreement is ~2e-4, not six-digit accuracy.
                 * Four-digit rounding still exposes CE noise near a
                 * rounding boundary (int(1/sqrt(sec(x)^4),x,1,2)).
                 * Show three significant digits; retain guard digits in
                 * Ans. This is not a certified quadrature error bound. */
                snprintf(nbuf, sizeof(nbuf), "%.*g", 3, (double)num_val);
                proc_okf(st, true, "= %s  (numeric)", nbuf);
            }
            ast_free(root);
            return true;
        }

        st->has_numeric = true;
        st->last_numeric = result_val;
        {
            char result_buf[32];
            int_format_value(result_buf, sizeof(result_buf), result_val);
            /* The launcher transcript shows only a result's first line, so
             * the value goes first and the antiderivative second. A worker
             * that neither simplifies nor folds (CECIDF, for size) would
             * print raw rule output such as ((X^(2+1))/(2+1)), so it
             * shows the value only. */
#if !INT_SHOW_ANTIDERIV
            (void)antideriv_str;
            proc_okf(st, true, "= %s", result_buf);
#else
            proc_okf(st, true, "= %s\nF(%c) = %s", result_buf, var, antideriv_str);
#endif
        }
        ast_free(root);
        return true;
#else
        ce_float_t num_val;

        eval_init(&s_eval);
        num_val = eval_expr(&s_eval, root);
        if (eval_error(&s_eval) != CE_OK || !ce_isfinite(num_val)) {
            proc_failf(st, "Integral failed: %s",
                       eval_error(&s_eval) != CE_OK ? eval_error_msg(&s_eval)
                                                    : "result overflow");
            ast_free(root);
            return true;
        }

        st->has_numeric = true;
        st->last_numeric = num_val;
        proc_ok_number(st, true, "= ", num_val, "  (numeric)");
        ast_free(root);
        return true;
#endif
    }

#if CE_PROC_CALC_INT_SYMBOLIC
    {
        ast_node_t *integ_result = integrate(inner, var);
        if (!integ_result) {
            if (integrate_hit_depth_cap()) {
                /* Too deep for this worker: the strategy workers would hit
                 * the same INTEG_MAX_DEPTH cap, so answer here instead of
                 * forwarding. */
                proc_failf(st, "%s", integrate_last_failure_reason());
            } else if (CE_PROC_CALC_INT_ROUTE_ADV) {
                proc_failf(st, "CEIADVREQ");
            } else {
                proc_failf(st, "Cannot find symbolic antiderivative");
            }
            ast_free(root);
            return true;
        }
        ast_to_display_str(integ_result, ast_str, sizeof(ast_str));
        proc_okf(st, true, "int = %s + C", ast_str);
        ast_free(integ_result);
    }
#elif CE_PROC_CALC_INT_STRATEGY
    {
        ast_node_t *integ_result = try_integral_advanced(inner, var);
        if (!integ_result) {
            proc_failf(st, "%s", integrate_last_failure_reason());
            ast_free(root);
            return true;
        }
        ast_to_display_str(integ_result, ast_str, sizeof(ast_str));
        proc_okf(st, true, "int = %s + C", ast_str);
        ast_free(integ_result);
    }
#else
    if (CE_PROC_CALC_INT_ROUTE_ADV) {
        proc_failf(st, "CEIADVREQ");
    } else {
        proc_failf(st, "Symbolic antiderivative not enabled");
    }
#endif

    ast_free(root);
    return true;
}
#endif

#if CE_PROC_CALC_INT && CE_PROC_CALC_INT_DEFINITE && CE_PROC_CALC_INT_POLAR_LOWER
/* CE handlers for improper() and areabetween() (see processor.h).
 * Both reuse handle_integral(), so every check int(f,x,a,b) makes (bounds,
 * angle mode, poles at and between the bounds, FTC precision, the checked
 * numeric fallback) applies to them as well. */
#define INT_AREA_N 64  /* f-g samples; each interval adds at most one bound */
static proc_state_t s_area_piece;  /* static: off the 4 KB CE stack */
static ce_float_t s_area_bnd[INT_AREA_N + 2];
static ce_float_t s_area_est[INT_AREA_N + 2];  /* sampled |f-g| per piece */

/* A NODE_INTEGRAL that owns expr, lo and hi (all freed on failure). */
static ast_node_t *int_node(ast_node_t *expr, char var, ast_node_t *lo, ast_node_t *hi)
{
    ast_node_t *n = (expr && lo && hi) ? mem_alloc_node() : NULL;
    if (!n) {
        ast_free(expr);
        ast_free(lo);
        ast_free(hi);
        return NULL;
    }
    n->type = NODE_INTEGRAL;
    n->d.integral.var = var;
    n->d.integral.expr = expr;
    n->d.integral.lo = lo;
    n->d.integral.hi = hi;
    return n;
}

/* diff at x into *v; false when it fails or is not finite. */
static bool area_at(const ast_node_t *diff, char var, ce_float_t x, ce_float_t *v)
{
    return integrate_eval_at(diff, var, x, v) && ce_isfinite(*v);
}

/* Frees root. */
static void __attribute__((noinline)) handle_int_polar(ast_node_t *root, proc_state_t *st)
{
    char var = root->d.polar.var ? root->d.polar.var : 'x';
    bool area = root->d.polar.op == POLOP_AREABETWEEN;
    ast_node_t *f = root->d.polar.expr1;
    ast_node_t *g = root->d.polar.expr2;
    ast_node_t *lo = root->d.polar.lo;
    ast_node_t *hi = root->d.polar.hi;
    ast_node_t *diff = NULL;
    ce_float_t a, b, total = 0, tsum = 0;
    bool numeric = false;
    int nb = 0, i;

    root->d.polar.expr1 = NULL;
    root->d.polar.expr2 = NULL;
    root->d.polar.lo = NULL;
    root->d.polar.hi = NULL;
    ast_free(root);

    if (!area) {
        /* improper(f,x,a,b) is int(f,x,a,b). This worker has no limits.c,
         * so infinite bounds and singular endpoints are refused exactly as
         * int() refuses them, never sampled. */
        ast_free(g);
        root = int_node(f, var, lo, hi);
        if (root) handle_integral(root, st);
        else proc_failf(st, "Out of memory");
        return;
    }

    if (!lo || !hi) {
        /* The parser accepts areabetween(f,g); the desktop handler then
         * assumes [0,1]. Ask for the bounds instead of guessing them. */
        proc_failf(st, "Use areabetween(f,g,x,a,b)");
        goto out;
    }
    eval_init(&s_eval);
    a = eval_expr(&s_eval, lo);
    b = eval_expr(&s_eval, hi);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Could not evaluate integral bounds");
        goto out;
    }
    if (!ce_isfinite(a) || !ce_isfinite(b)) {
        /* Like int(), reject infinite bounds in this build without limits.c. */
        proc_failf(st, "Could not decide integral");
        goto out;
    }
    if (a == b) {
        /* As in int(): bounds typed alike are an empty interval, but
         * different bounds that round to one float32 are not. */
        if (ast_same_bound(lo, hi)) {
            st->has_numeric = true;  /* Ans = 0, as int(f,x,1,1) */
            st->last_numeric = 0;
            proc_okf(st, true, "Area = 0");
        } else {
            proc_failf(st, "Could not decide integral");
        }
        goto out;
    }

    diff = ast_binop(OP_SUB, f, g);  /* frees f and g on failure */
    f = g = NULL;
    if (!diff) {
        proc_failf(st, "Out of memory");
        goto out;
    }
    if (g_settings.angle_mode != ANGLE_RADIANS && int_has_angle(diff)) {
        proc_failf(st, "Trig integral: use RADIAN mode");
        goto out;
    }

    /* Split [a, b] where f-g changes sign. Samples run from a to b (b < a
     * works the same way). A sample where f-g fails or is not finite is a
     * pole or a domain gap, refused like one inside int(). A sign change
     * is taken between the last nonzero sample and the next one of the
     * other sign: at the zero sample in between if there is one (so a
     * tangency or f == g adds no bound), otherwise at a bisected root.
     * A root within h/1024 of a sample is moved onto it, because int()
     * rightly refuses a piece a few ulps wide. Each piece also gets a
     * trapezoid estimate of the area from the samples, s_area_est. */
    {
        ce_float_t h = (b - a) / (ce_float_t)INT_AREA_N;
        ce_float_t ah = fabsf(h), tol = ah * (1.0f / 1024.0f);
        ce_float_t xp = a, dp, ds, x, d, acc = 0;

        s_area_bnd[nb++] = a;
        if (!area_at(diff, var, a, &dp)) goto grid;
        ds = dp;
        for (i = 1; i <= INT_AREA_N; i++) {
            x = i == INT_AREA_N ? b : a + (ce_float_t)i * h;
            if (!area_at(diff, var, x, &d)) goto grid;
            /* Signs, not ds*d: CE soft-float wraps on overflow instead of
             * giving inf. */
            if (d != 0 && ds != 0 && (ds < 0) != (d < 0)) {
                ce_float_t cut = xp;  /* dp == 0: the zero sample */
                if (dp != 0) {
                    /* 30 halvings take the bracket below float32
                     * resolution; l stays on dp's side. */
                    ce_float_t l = xp, r = x, m, v;
                    int k;
                    for (k = 0; k < 30; k++) {
                        m = 0.5f * (l + r);
                        if (!area_at(diff, var, m, &v)) goto grid;
                        if ((v < 0) == (dp < 0)) l = m;
                        else r = m;
                    }
                    cut = l;
                    if (fabsf(cut - xp) < tol) cut = xp;
                    else if (fabsf(x - cut) < tol) cut = x;
                }
                acc += 0.5f * fabsf(dp) * fabsf(cut - xp);
                if (!(i == INT_AREA_N && cut == x)) {  /* b is added below */
                    s_area_est[nb - 1] = acc;
                    tsum += acc;
                    s_area_bnd[nb++] = cut;
                    acc = 0;
                }
                acc += 0.5f * fabsf(d) * fabsf(x - cut);
            } else {
                acc += 0.5f * (fabsf(dp) + fabsf(d)) * ah;
            }
            if (d != 0) ds = d;
            xp = x;
            dp = d;
        }
        s_area_est[nb - 1] = acc;
        tsum += acc;
        s_area_bnd[nb++] = b;
    }

    /* f-g keeps one sign on each piece, so |int(f-g)| over the piece is its
     * area. Each piece goes through handle_integral() with all its checks;
     * the first refusal is the answer. */
    for (i = 0; i + 1 < nb; i++) {
        ast_node_t *piece;
        ce_float_t v, t;
        if (s_area_bnd[i + 1] == s_area_bnd[i]) continue;
        piece = int_node(ast_clone(diff), var, ast_number(s_area_bnd[i]),
                         ast_number(s_area_bnd[i + 1]));
        if (!piece) {
            proc_failf(st, "Out of memory");
            goto out;
        }
        memset(&s_area_piece, 0, sizeof(s_area_piece));
        handle_integral(piece, &s_area_piece);
        if (!s_area_piece.ok) {
            proc_failf(st, "%s", s_area_piece.error);
            goto out;
        }
        /* Only a value counts; "DIVERGES" (a build with limits.c) is not.
         * A value from the checked Simpson fallback ends "(numeric)". */
        if (!s_area_piece.has_numeric) goto grid;
        if (strchr(s_area_piece.result, '(')) numeric = true;
        /* The value must match the sampled area to 25%. Crossings missed
         * between samples (areabetween(sin(100x),0,x,0,pi) gave 0.52, not
         * 2) cancel inside a piece; a feature narrower than the grid is
         * missing from the samples. Either way the grid cannot resolve
         * f-g, so refuse rather than print a wrong area. */
        v = fabsf(s_area_piece.last_numeric);
        t = s_area_est[i];
        if (fabsf(v - t) > 0.25f * (v > t ? v : t) + 1e-5f * tsum) goto grid;
        total += v;
    }
    if (!ce_isfinite(total)) goto grid;

    st->has_numeric = true;
    st->last_numeric = total;
    {
        /* s_area_piece is done with: its error field holds the text, which
         * keeps this frame under 128 B (short IX offsets on the eZ80). */
        snprintf(s_area_piece.error, sizeof(s_area_piece.error), "%.*g",
                 numeric ? 3 : 6, (double)total);
        proc_okf(st, true, "Area = %s%s", s_area_piece.error, numeric ? "  (numeric)" : "");
    }
    goto out;

grid:
    /* As int(): name eval's depth cap when it is the cause (#128). */
    proc_failf(st, g_eval_refused ? "Eval recursion limit" : "Could not decide area");
out:
    ast_free(diff);
    ast_free(f);
    ast_free(g);
    ast_free(lo);
    ast_free(hi);
}
#endif

#if CE_PROC_CALC_LIMIT
static bool handle_limit(ast_node_t *root, proc_state_t *st)
{
    lim_status_t ls;
    ce_float_t lim_val;

    if (!root->d.limit.point) {
        proc_failf(st, "Limit needs a point");
        ast_free(root);
        return true;
    }

    ls = compute_limit(root->d.limit.expr, root->d.limit.var,
                       root->d.limit.point, root->d.limit.direction, &lim_val);
    switch (ls) {
    case LIM_OK:
        st->has_numeric = true;
        st->last_numeric = lim_val;
        proc_ok_number(st, true, "= ", lim_val, "");
        break;
    case LIM_POS_INF:
        proc_okf(st, true, "= +inf");
        break;
    case LIM_NEG_INF:
        proc_okf(st, true, "= -inf");
        break;
    case LIM_DNE:
        proc_okf(st, true, "= DNE (does not exist)");
        break;
    default:
        if (CE_PROC_CALC_LIMIT_ROUTE_ADV) {
            proc_failf(st, "CELADVREQ");
        } else {
            proc_failf(st, "Could not evaluate limit");
        }
        break;
    }

    ast_free(root);
    return true;
}
#endif

#if CE_PROC_CALC_SERIES_TF
static bool handle_taylor(ast_node_t *root, proc_state_t *st)
{
    ast_node_t *result_ast;
    ce_float_t center, degf;
    int degree;
    char ast_str[128];

    eval_init(&s_eval);
    center = eval_expr(&s_eval, root->d.taylor.center);
    degf = eval_expr(&s_eval, root->d.taylor.degree);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Taylor args must be numeric");
        ast_free(root);
        return true;
    }

    degree = (int)degf;
    result_ast = taylor_series(root->d.taylor.expr, root->d.taylor.var, center, degree);
    if (!result_ast) {
        proc_failf(st, "Could not compute Taylor series");
        ast_free(root);
        return true;
    }

    ast_to_display_str(result_ast, ast_str, sizeof(ast_str));
    proc_okf(st, true, "T_%d = %s", degree, ast_str);

    ast_free(result_ast);
    ast_free(root);
    return true;
}

static bool handle_fourier(ast_node_t *root, proc_state_t *st)
{
    ast_node_t *result_ast;
    ce_float_t nval;
    int nterms;
    char ast_str[128];

    eval_init(&s_eval);
    nval = eval_expr(&s_eval, root->d.fourier.nterms);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Fourier nterms must be numeric");
        ast_free(root);
        return true;
    }

    nterms = (int)nval;
    result_ast = fourier_series(root->d.fourier.expr, root->d.fourier.var, nterms);
    if (!result_ast) {
        /* A sample failed: undefined on [-pi, pi], or the input is too deep. */
        proc_failf(st, g_eval_refused
                           ? "Eval recursion limit" : "Could not compute Fourier series");
        ast_free(root);
        return true;
    }

    ast_to_display_str(result_ast, ast_str, sizeof(ast_str));
    proc_okf(st, true, "Fourier_%d = %s", nterms, ast_str);

    ast_free(result_ast);
    ast_free(root);
    return true;
}
#endif

#if CE_PROC_CALC_SERIES_SEQ
static bool handle_sum(ast_node_t *root, proc_state_t *st)
{
    conv_info_t ci;
    ce_float_t v;
    if (!root->d.sum.hi) return false;

    /* Only sum(f,n,a,inf) is a series. Any other upper bound is a finite
     * sum: eval.c adds the terms (integer bounds, span at most 10,000,
     * any failed term or overflow is an error), so sum(n,n,1,10) = 55
     * rather than the series test's "diverges". */
    eval_init(&s_eval);
    v = eval_expr(&s_eval, root->d.sum.hi);
    if (eval_error(&s_eval) != CE_OK || !ce_isinf(v) || v < 0) {
        /* Keep a failed classification, including a one-shot cancellation. */
        if (eval_error(&s_eval) == CE_OK) {
            eval_init(&s_eval);
            v = eval_expr(&s_eval, root);
        }
        if (eval_error(&s_eval) == CE_ERR_UNDEFINED && s_eval.undefined_var) {
            /* Report an undefined bound or summand variable by name. */
            proc_failf(st, "%c has no value", s_eval.undefined_var);
        } else if (eval_error(&s_eval) != CE_OK) {
            proc_failf(st, "%s", eval_error_msg(&s_eval));
        } else if (!ce_isfinite(v)) {
            proc_failf(st, "Sum overflow");
        } else {
            proc_ok_number(st, true, "= ", v, "");
            st->has_numeric = true;
            st->last_numeric = v;
        }
        ast_free(root);
        return true;
    }

    ci = series_converge(root->d.sum.expr, root->d.sum.var);
    if (ci.result == CONV_INCONCLUSIVE) {
        proc_okf(st, true, "Convergence: inconclusive%s%s",
                 ci.test_name ? " (" : "",
                 ci.test_name ? ci.test_name : "");
        if (ci.test_name) {
            size_t n = strlen(st->result);
            if (n + 1 < sizeof(st->result)) {
                st->result[n++] = ')';
                st->result[n] = '\0';
            }
        }
    } else {
        proc_okf(st, true, "Convergence: %s%s%s",
                 (ci.result == CONV_CONVERGES) ? "converges" : "diverges",
                 ci.test_name ? " (" : "",
                 ci.test_name ? ci.test_name : "");
        if (ci.test_name) {
            size_t n = strlen(st->result);
            if (n + 1 < sizeof(st->result)) {
                st->result[n++] = ')';
                st->result[n] = '\0';
            }
        }
        if (ci.has_sum) {
            char sum_buf[48];
            char sum_num[32];
            proc_format_decimal(sum_num, sizeof(sum_num), ci.sum_estimate);
            snprintf(sum_buf, sizeof(sum_buf), "\nSum ~= %s", sum_num);
            strncat(st->result, sum_buf, sizeof(st->result) - strlen(st->result) - 1);
        }
    }
    ast_free(root);
    return true;
}
static bool handle_recseq(ast_node_t *root, proc_state_t *st)
{
    ce_float_t init_val;
    static ce_float_t terms[201];
    int k_val, n_terms, i, pos;

    eval_init(&s_eval);
    init_val = eval_expr(&s_eval, root->d.polar.lo);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "recseq: bad init value");
        ast_free(root);
        return true;
    }

    eval_init(&s_eval);
    k_val = (int)eval_expr(&s_eval, root->d.polar.hi);
    if (eval_error(&s_eval) != CE_OK || k_val < 0) {
        proc_failf(st, "recseq: bad k value");
        ast_free(root);
        return true;
    }
    if (k_val > 200) k_val = 200;

    n_terms = series_recseq(root->d.polar.expr1, root->d.polar.var,
                            init_val, k_val, terms, k_val + 1);
    if (n_terms <= k_val) {
        /* s(n_terms) failed to evaluate: s(k) is unknown, not the last
         * term that was computed. */
        proc_failf(st, g_eval_refused ? "Eval recursion limit"
                                      : "recseq: s(%d) could not be evaluated", n_terms);
        ast_free(root);
        return true;
    }

    pos = snprintf(st->result, sizeof(st->result), "s(%d) = ",
                   n_terms - 1);
    pos = proc_append_decimal(st->result, sizeof(st->result), pos, terms[n_terms - 1]);
    if (n_terms >= 2 && pos < (int)sizeof(st->result) - 20) {
        pos += snprintf(st->result + pos, sizeof(st->result) - pos, "\nTerms: ");
        for (i = 0; i < n_terms && i < 8 && pos < (int)sizeof(st->result) - 12; i++) {
            if (i > 0) pos += snprintf(st->result + pos, sizeof(st->result) - pos, ", ");
            pos = proc_append_decimal(st->result, sizeof(st->result), pos, terms[i]);
        }
        if (n_terms > 8) {
            snprintf(st->result + pos, sizeof(st->result) - pos, ", ...");
        }
    }

    st->ok = true;
    st->show_steps = true;
    st->error[0] = '\0';
    ast_free(root);
    return true;
}
#endif
#endif

#if CE_PROC_ALG_EXPFAC
static bool handle_expand(ast_node_t *root, proc_state_t *st)
{
    ast_node_t *result_ast = algebra_expand(root->d.expand.expr, root->d.expand.var);
    char ast_str[128];

    if (!result_ast) {
        proc_failf(st, "Expand failed");
        ast_free(root);
        return true;
    }

    ast_to_display_str(result_ast, ast_str, sizeof(ast_str));
    proc_okf(st, true, "= %s", ast_str);
    ast_free(result_ast);
    ast_free(root);
    return true;
}
#endif

#if CE_PROC_ALG_EXPFAC
static bool handle_factor(ast_node_t *root, proc_state_t *st)
{
    ast_node_t *result_ast = algebra_factor(root->d.factor.expr, root->d.factor.var);
    char ast_str[128];

    if (!result_ast) {
        proc_failf(st, "Factor failed");
        ast_free(root);
        return true;
    }

    ast_to_display_str(result_ast, ast_str, sizeof(ast_str));
    proc_okf(st, true, "= %s", ast_str);
    ast_free(result_ast);
    ast_free(root);
    return true;
}
#endif

#if CE_PROC_ALG_SOLVE
static bool handle_solve(ast_node_t *root, proc_state_t *st)
{
    int solve_status = algebra_solve(root->d.solve.expr, root->d.solve.var,
                                     st->result, MAX_RESULT_LEN);
    if (solve_status < 0) {
        proc_failf(st, "%s", st->result[0] ? st->result : "CEAADVREQ");
        ast_free(root);
        return true;
    }
    st->ok = true;
    st->show_steps = true;
    st->error[0] = '\0';
    ast_free(root);
    return true;
}
#endif

#if CE_PROC_STAT || CE_PROC_FIN
static bool is_finance_fn(stat_fn_t fn)
{
    switch (fn) {
    case STAT_FIN_PV:
    case STAT_FIN_FV:
    case STAT_FIN_PMT:
    case STAT_FIN_NPER:
    case STAT_FIN_RATE:
    case STAT_FIN_NOM:
    case STAT_FIN_EFF:
    case STAT_FIN_BAL:
    case STAT_FIN_SUMPRN:
    case STAT_FIN_SUMINT:
        return true;
    default:
        return false;
    }
}

static bool handle_stat(ast_node_t *root, proc_state_t *st)
{
    ce_float_t val;

    if (root->d.stat.fn == STAT_LINREG) {
#if CE_PROC_ENABLE_LINREG
        static ce_float_t sbuf[MAX_STAT_DATA];
        int count = 0;
        const ast_node_t *pair = root->d.stat.args;
        ce_float_t m, b, r;
        ce_error_t serr;

        eval_init(&s_eval);
        while (pair && pair->type == NODE_PAIR && count < MAX_STAT_DATA) {
            sbuf[count] = eval_expr(&s_eval, pair->d.pair.head);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(st, "Eval: %s", eval_error_msg(&s_eval));
                ast_free(root);
                return true;
            }
            count++;
            pair = pair->d.pair.tail;
        }

        serr = stat_linreg(sbuf, count, &m, &b, &r);
        if (serr != CE_OK) {
            proc_failf(st, "linreg: need >= 2 (x,y) pairs");
            ast_free(root);
            return true;
        }

        {
            char m_buf[32], b_buf[32], r_buf[32], r2_buf[32];
            proc_format_decimal(m_buf, sizeof(m_buf), m);
            proc_format_decimal(b_buf, sizeof(b_buf), b);
            proc_format_decimal(r_buf, sizeof(r_buf), r);
            proc_format_decimal(r2_buf, sizeof(r2_buf), r * r);
            proc_okf(st, false, "y = %sx + %s\nr = %s  r^2 = %s",
                     m_buf, b_buf, r_buf, r2_buf);
        }
        ast_free(root);
        return true;
#else
        proc_failf(st, "linreg not enabled in this module");
        ast_free(root);
        return true;
#endif
    }

    eval_init(&s_eval);
    val = eval_expr(&s_eval, root);
    if (eval_error(&s_eval) != CE_OK) {
        proc_failf(st, "Stat: %s", eval_error_msg(&s_eval));
        ast_free(root);
        return true;
    }

    st->has_numeric = true;
    st->last_numeric = val;
#if CE_PROC_STAT_NAMES
    {
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s = ", stat_fn_name(root->d.stat.fn));
        proc_ok_number(st, false, prefix, val, "");
    }
#else
    proc_ok_number(st, false, "Result = ", val, "");
#endif
    ast_free(root);
    return true;
}
#endif

#if CE_PROC_MATRIX
/* Evaluate the elements of a mat(...) literal into *out.  On failure the
 * error is in *st and false is returned; the caller frees the tree. */
static bool proc_read_matrix(const ast_node_t *lit, ce_matrix_t *out,
                             proc_state_t *st)
{
    const ast_node_t *pair;
    int idx = 0;

    if (!lit || lit->type != NODE_MATRIX_OP || lit->d.matrix_op.op != MAT_LITERAL) {
        proc_failf(st, "Expected mat(...) argument");
        return false;
    }
    pair = lit->d.matrix_op.arg;
    matrix_init(out, lit->d.matrix_op.rows, lit->d.matrix_op.cols);
    eval_init(&s_eval);
    while (pair && pair->type == NODE_PAIR && idx < out->rows * out->cols) {
        out->data[idx] = eval_expr(&s_eval, pair->d.pair.head);
        if (eval_error(&s_eval) != CE_OK) {
            proc_failf(st, "Eval: %s", eval_error_msg(&s_eval));
            return false;
        }
        idx++;
        pair = pair->d.pair.tail;
    }
    return true;
}

static bool handle_matrix(ast_node_t *root, proc_state_t *st)
{
    static ce_matrix_t mat_a, mat_result;
    ast_node_t *inner = root;
    matrix_op_t mop;

    if (root->d.matrix_op.op != MAT_LITERAL) {
        mop = root->d.matrix_op.op;
        inner = root->d.matrix_op.arg;
    } else {
        mop = MAT_LITERAL;
    }

    if (!proc_read_matrix(inner, &mat_a, st)) {
        ast_free(root);
        return true;
    }

    if (mop == MAT_LITERAL) {
        matrix_to_str(&mat_a, st->result, sizeof(st->result));
        st->ok = true;
        st->show_steps = false;
        st->error[0] = '\0';
    } else if (mop == MAT_DET) {
        ce_float_t d = matrix_det(&mat_a);
        st->has_numeric = true;
        st->last_numeric = d;
        proc_ok_number(st, false, "det = ", d, "");
    } else if (mop == MAT_TRANSPOSE) {
        matrix_transpose(&mat_a, &mat_result);
        matrix_to_str(&mat_result, st->result, sizeof(st->result));
        st->ok = true;
        st->show_steps = false;
        st->error[0] = '\0';
    } else if (mop == MAT_INV) {
        if (!matrix_inverse(&mat_a, &mat_result)) {
            proc_failf(st, "Matrix is singular (no inverse)");
            ast_free(root);
            return true;
        }
        matrix_to_str(&mat_result, st->result, sizeof(st->result));
        st->ok = true;
        st->show_steps = false;
        st->error[0] = '\0';
    } else if (mop == MAT_RREF) {
        mat_result = mat_a;
        matrix_rref(&mat_result);
        matrix_to_str(&mat_result, st->result, sizeof(st->result));
        st->ok = true;
        st->show_steps = false;
        st->error[0] = '\0';
    }

    ast_free(root);
    return true;
}

/* Append s to st->result at *pos, never past the buffer. */
static void proc_eig_put(proc_state_t *st, int *pos, const char *s)
{
    int n;
    if (*pos < 0 || *pos >= MAX_RESULT_LEN - 1) return;
    n = snprintf(st->result + *pos, (size_t)(MAX_RESULT_LEN - *pos), "%s", s);
    if (n < 0) return;
    *pos += n;
    if (*pos > MAX_RESULT_LEN - 1) *pos = MAX_RESULT_LEN - 1;
}

/* An eigenvalue.  proc_format_decimal shows 6 fixed decimals and caps at
 * 4e9, so a value at or above 1e6, or one below 0.1 that needs more digits
 * than 6 decimals show (0.000123456, 3e-8), is written as mantissa "e"
 * exponent with 5 significant digits.  The scaling multiplies by up to 6
 * of the powers 1e1..1e32 (1e16 and 1e32 are inexact), which on the CE
 * leaves about 1e-6 relative error: enough to flip a 6th digit (6e-30
 * showed as 6.00001e-30), not a 5th.  (A float conversion in snprintf would
 * link ce_fmt's float formatter, about 3.5 KB in CEMAT.) */
static void proc_eig_fmt(char *buf, size_t size, ce_float_t v)
{
    static const ce_float_t p10[6] = { 1e1f, 1e2f, 1e4f, 1e8f, 1e16f, 1e32f };
    char frac[6];
    ce_float_t a = v < 0 ? -v : v;
    unsigned long m;
    int e = 0, k, digits;

    if (a == 0 || !(a < 1e38f)) {          /* 0, inf, nan */
        proc_format_decimal(buf, size, v);
        return;
    }
    if (a >= 10.0f) {
        for (k = 5; k >= 0; k--) {
            if (a >= p10[k]) { a /= p10[k]; e += 1 << k; }
        }
    } else if (a < 1.0f) {
        for (k = 5; k >= 0; k--) {
            if (a * p10[k] < 10.0f) { a *= p10[k]; e -= 1 << k; }
        }
    }
    if (e >= -1 && e < 6) {                 /* 0.1 <= |v| < 1e6 */
        proc_format_decimal(buf, size, v);
        return;
    }
    m = (unsigned long)(a * 10000.0f + 0.5f);       /* 5 digits, d.dddd */
    if (m >= 100000UL) { m /= 10; e++; }
    /* Significant digits actually needed, without trailing zeros. */
    for (digits = 5; digits > 1 && m % 10 == 0; digits--) m /= 10;
    if (e < 0 && digits <= 7 + e) {
        /* 6 decimals hold every digit (0.05, 0.0001). */
        proc_format_decimal(buf, size, v);
        return;
    }
    for (k = digits - 1; k >= 1; k--) {     /* m = d[.dddd] */
        frac[k - 1] = (char)('0' + m % 10);
        m /= 10;
    }
    frac[digits > 1 ? digits - 1 : 0] = '\0';
    snprintf(buf, size, "%s%lu%s%se%d", v < 0 ? "-" : "", m,
             digits > 1 ? "." : "", frac, e);
}

static void proc_eig_num(proc_state_t *st, int *pos, ce_float_t v)
{
    char number[32];
    proc_eig_fmt(number, sizeof(number), v);
    proc_eig_put(st, pos, number);
}

/* eigenval(mat(...)) / eigenvec(mat(...)): real eigenvalues and eigenvectors
 * of a square 1x1..3x3 matrix literal (src/matrix.c).  Runs in CEMAT; the
 * launcher routes both ops there (route_polar_program).  The argument must
 * be a mat(...) literal itself: a wrapped one such as eigenval(inv(mat(...)))
 * is rejected instead of silently using the unwrapped matrix.  Scratch is
 * static so the eZ80 frame stays small.  Frees root. */
static void handle_eigen(ast_node_t *root, proc_state_t *st)
{
    static ce_matrix_t mat;
    static ce_float_t vals[MAX_MATRIX_DIM];
    static ce_float_t vecs[MAX_MATRIX_DIM][MAX_MATRIX_DIM];
    char label[24];
    int n, i, j, k, pos = 0;
    bool want_vec = root->d.polar.op == POLOP_EIGENVEC;

    if (!proc_read_matrix(root->d.polar.expr1, &mat, st)) goto done;
    if (mat.rows != mat.cols) {
        proc_failf(st, "Matrix must be square");
        goto done;
    }
    n = matrix_eigenvalues(&mat, vals, MAX_MATRIX_DIM);
    if (n == -2) {
        proc_failf(st, "Eigen: entry out of range");
        goto done;
    }
    if (n < 0) {
        proc_failf(st, "Eigen: up to 3x3 only");
        goto done;
    }
    if (n == 0) {
        proc_failf(st, "No real eigenvalues");
        goto done;
    }

    st->result[0] = '\0';
    steps_clear();
    if (!want_vec) {
        steps_addf("Matrix %dx%d - eigenvalues:", mat.rows, mat.cols);
        proc_eig_put(st, &pos, "lambda = {");
        for (i = 0; i < n; i++) {
            char eig_buf[32];
            proc_eig_fmt(eig_buf, sizeof(eig_buf), vals[i]);
            steps_addf("  lambda%d = %s", i + 1, eig_buf);
            if (i > 0) proc_eig_put(st, &pos, ", ");
            proc_eig_put(st, &pos, eig_buf);
        }
        proc_eig_put(st, &pos, "}");
        /* A real 3x3 with one real eigenvalue has a complex pair. */
        if (n < (int)mat.rows) proc_eig_put(st, &pos, " + complex pair");
    } else {
        n = matrix_eigenvectors(&mat, vals, vecs, n);
        steps_addf("Matrix %dx%d - eigenvectors:", mat.rows, mat.cols);
        for (i = 0; i < n; i++) {
            bool found = false;
            if (i > 0) proc_eig_put(st, &pos, "\n");
            snprintf(label, sizeof(label), "v%d(lambda=", i + 1);
            proc_eig_put(st, &pos, label);
            proc_eig_num(st, &pos, vals[i]);
            for (j = 0; j < (int)mat.rows; j++) {
                if (vecs[i][j] != 0) found = true;
            }
            if (!found) {
                /* matrix_eigenvectors found no vector that checks out. */
                proc_eig_put(st, &pos, ") not found");
                continue;
            }
            /* A defective eigenvalue (fewer independent vectors than its
             * multiplicity) repeats an earlier vector; say so instead of
             * printing it again as if it were new. */
            for (k = 0; k < i; k++) {
                int same = vals[k] == vals[i];
                for (j = 0; j < (int)mat.rows && same; j++) {
                    if (vecs[k][j] != vecs[i][j]) same = 0;
                }
                if (same) break;
            }
            if (k < i) {
                snprintf(label, sizeof(label), ")=same as v%d", k + 1);
                proc_eig_put(st, &pos, label);
                continue;
            }
            proc_eig_put(st, &pos, ")=[");
            for (j = 0; j < (int)mat.rows; j++) {
                /* Components are relative to the largest (1) and good to
                 * about 1e-4: fixed decimals, never an exponent. */
                char number[32];
                if (j > 0) proc_eig_put(st, &pos, ",");
                proc_format_decimal(number, sizeof(number), vecs[i][j]);
                proc_eig_put(st, &pos, number);
            }
            proc_eig_put(st, &pos, "]");
        }
        /* A real 3x3 with one real eigenvalue has a complex pair. */
        if (n < (int)mat.rows) proc_eig_put(st, &pos, "\n+ complex pair");
    }
    st->ok = true;
    st->show_steps = true;
    st->error[0] = '\0';
done:
    ast_free(root);
}
#endif

static void finalize_response(proc_state_t *st, ce_response_t *resp)
{
    ce_response_from_runtime(resp,
                             st->ok,
                             st->show_steps,
                             st->ok ? st->result : "",
                             st->ok ? "" : st->error,
                             st->has_numeric ? st->last_numeric : g_last_answer);
    /* resp->ok, not st->ok: ce_response_from_runtime can refuse a success
     * (eval stack backstop), and Ans must then keep its old value. */
    if (resp->ok && st->has_numeric) {
        g_last_answer = st->last_numeric;
    }
}

static bool ce_processor_run_root(const char *input,
                                  ast_node_t *root,
                                  bool input_had_equals,
                                  ce_response_t *resp)
{
    static proc_state_t st;
#if CE_PROC_USE_CLASSIFY
    ce_domain_t domain;
#endif

    memset(&st, 0, sizeof(st));
    st.ok = false;
#ifdef DESKTOP_BUILD
    /* A CE program run serves one request, so g_eval_refused starts clear.
     * Desktop tests serve many per process (see calcengine.h). */
    g_eval_refused = false;
#endif

    if (!resp) {
        if (root) ast_free(root);
        return false;
    }
    if (!root) {
        proc_failf(&st, "Could not parse input");
        finalize_response(&st, resp);
        return false;
    }
    if (!input || !input[0]) {
        proc_failf(&st, "Empty input");
        ast_free(root);
        finalize_response(&st, resp);
        return false;
    }

#if !CE_PROC_ALG_SOLVE
    (void)input_had_equals;
#endif

#if CE_PROC_USE_CLASSIFY
    domain = ce_classify_expression(root, input_had_equals);
#endif

#if CE_PROC_ALG_EXPFAC
    if (root->type == NODE_EXPAND) {
        handle_expand(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
    if (root->type == NODE_FACTOR) {
        handle_factor(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_ALG_SOLVE
    if (root->type == NODE_SOLVE) {
        handle_solve(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_CALC_DERIV
    if (root->type == NODE_DERIV) {
        handle_deriv(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_CALC_TANGENT
    if (root->type == NODE_TANGENT) {
        handle_tangent(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_CALC_LIMIT
    if (root->type == NODE_LIMIT) {
        handle_limit(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_CALC_INT
    /* Guarded by CE_PROC_CALC_INT, not _SYMBOLIC: CECIDF (definite
     * integrals) builds with SYMBOLIC=0, and under the old guard it never
     * reached handle_integral, so every int(f,x,a,b) on the CE answered
     * "Feature is routed to another module" (real OS 5.8 ROM, 2026-11-09). */
    if (root->type == NODE_INTEGRAL) {
        handle_integral(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_CALC_SERIES_TF
    if (root->type == NODE_TAYLOR) {
        handle_taylor(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
    if (root->type == NODE_FOURIER) {
        handle_fourier(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_CALC_SERIES_SEQ
    if (root->type == NODE_SUM && root->d.sum.hi) {
        handle_sum(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
    if (root->type == NODE_POLAR && root->d.polar.op == POLOP_RECSEQ) {
        handle_recseq(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_STAT || CE_PROC_FIN
    if (root->type == NODE_STAT) {
        bool finance = is_finance_fn(root->d.stat.fn);
        (void)finance;
#if CE_PROC_STAT && !CE_PROC_FIN
        if (finance) {
            proc_failf(&st, "Finance function routed to wrong module");
            ast_free(root);
            finalize_response(&st, resp);
            return false;
        }
#elif CE_PROC_FIN && !CE_PROC_STAT
        if (!finance) {
            proc_failf(&st, "Stat function routed to wrong module");
            ast_free(root);
            finalize_response(&st, resp);
            return false;
        }
#endif
        handle_stat(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

#if CE_PROC_MATRIX
    if (root->type == NODE_MATRIX_OP) {
        handle_matrix(root, &st);
        finalize_response(&st, resp);
        return st.ok;
    }
#endif

    if (root->type == NODE_POLAR) {
        polar_op_t pop = root->d.polar.op;
        (void)pop;

#if CE_PROC_MATRIX
        if (pop == POLOP_EIGENVAL || pop == POLOP_EIGENVEC) {
            handle_eigen(root, &st);
            finalize_response(&st, resp);
            return st.ok;
        }
#endif

#if CE_PROC_ALG_POLYDIV
        /* polydiv(p, q, var) is routed to CEAPDV, which builds with
         * CE_PROC_ALG_SOLVE=0.  The OP_DIV detection further down lives
         * inside the SOLVE block, so before this branch every polydiv()
         * on the CE answered "Feature is routed to another module", as
         * observed on the OS 5.8 ROM. */
        if (pop == POLOP_POLYDIV) {
            static char pd_quot[MAX_RESULT_LEN / 2];
            static char pd_rem[MAX_RESULT_LEN / 2];
            ast_node_t *quot = algebra_poly_div(root->d.polar.expr1,
                                                root->d.polar.expr2,
                                                root->d.polar.var,
                                                pd_rem, (int)sizeof(pd_rem));
            if (!quot) {
                proc_failf(&st, "Cannot perform polynomial division");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            ast_to_display_str(quot, pd_quot, (int)sizeof(pd_quot));
            ast_free(quot);
            proc_okf(&st, true, "Q = %s\nR = %s", pd_quot, pd_rem);
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CMPLX
        if (pop == POLOP_TO_RECT) {
            ce_float_t r_val, theta_val, xr, yr;
            eval_init(&s_eval);
            r_val = eval_expr(&s_eval, root->d.polar.expr1);
            theta_val = eval_expr(&s_eval, root->d.polar.expr2);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "Eval error");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            proc_polar_to_rect(r_val, theta_val, &xr, &yr);
            {
                char r_buf[32], theta_buf[32], x_buf[32], y_buf[32], step_buf[96];
                proc_format_decimal(r_buf, sizeof(r_buf), r_val);
                proc_format_decimal(theta_buf, sizeof(theta_buf), theta_val);
                proc_format_decimal(x_buf, sizeof(x_buf), xr);
                proc_format_decimal(y_buf, sizeof(y_buf), yr);
                snprintf(step_buf, sizeof(step_buf), "r = %s, theta = %s", r_buf, theta_buf);
                steps_add(step_buf);
                snprintf(step_buf, sizeof(step_buf), "x = r*cos(theta) = %s", x_buf);
                steps_add(step_buf);
                snprintf(step_buf, sizeof(step_buf), "y = r*sin(theta) = %s", y_buf);
                steps_add(step_buf);
            }
            {
                char x_buf[32], y_buf[32];
                proc_format_decimal(x_buf, sizeof(x_buf), xr);
                proc_format_decimal(y_buf, sizeof(y_buf), yr);
                proc_okf(&st, true, "x = %s\ny = %s", x_buf, y_buf);
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

        if (pop == POLOP_FROM_RECT) {
            ce_float_t x_val, y_val, rp, tp;
            eval_init(&s_eval);
            x_val = eval_expr(&s_eval, root->d.polar.expr1);
            y_val = eval_expr(&s_eval, root->d.polar.expr2);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "Eval error");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            proc_rect_to_polar(x_val, y_val, &rp, &tp);
            {
                char r_buf[32], theta_buf[32];
                proc_format_decimal(r_buf, sizeof(r_buf), rp);
                proc_format_decimal(theta_buf, sizeof(theta_buf), tp);
                proc_okf(&st, true, "r = %s\ntheta = %s", r_buf, theta_buf);
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

#endif

#if CE_PROC_ALG_ADV_SYM || CE_PROC_ALG_ADV_GEO
#if CE_PROC_ALG_ADV_SYM
        if (pop == POLOP_CSQUARE) {
            int ok = algebra_complete_square(root->d.polar.expr1, root->d.polar.var,
                                             st.result, sizeof(st.result));
            if (!ok) {
                proc_failf(&st, "csquare: not a quadratic");
            } else {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }

        if (pop == POLOP_PFRAC) {
            ast_node_t *pf = algebra_partial_fractions(root->d.polar.expr1, root->d.polar.var,
                                                       st.result, sizeof(st.result));
            if (!pf) {
                proc_failf(&st, "pfrac: decomposition failed");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            ast_free(pf);
            st.ok = true;
            st.show_steps = true;
            st.error[0] = '\0';
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

        if (pop == POLOP_DESCARTES) {
            int ok = algebra_descartes(root->d.polar.expr1, root->d.polar.var,
                                       st.result, sizeof(st.result));
            if (!ok) {
                proc_failf(&st, "descartes: not a polynomial");
            } else {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }
#endif

#if CE_PROC_ALG_ADV_GEO
        if (pop == POLOP_LINE2PT) {
            ce_float_t x1v, y1v, x2v, y2v;
            eval_init(&s_eval);
            x1v = eval_expr(&s_eval, root->d.polar.expr1);
            y1v = eval_expr(&s_eval, root->d.polar.expr2);
            x2v = eval_expr(&s_eval, root->d.polar.lo);
            y2v = eval_expr(&s_eval, root->d.polar.hi);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "line: eval error");
            } else if (!algebra_line_two_points(x1v, y1v, x2v, y2v,
                                                st.result, sizeof(st.result))) {
                proc_failf(&st, "line: duplicate points");
            } else {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }

        if (pop == POLOP_LINEPT) {
            ce_float_t mv, x1v, y1v;
            eval_init(&s_eval);
            mv = eval_expr(&s_eval, root->d.polar.expr1);
            x1v = eval_expr(&s_eval, root->d.polar.expr2);
            y1v = eval_expr(&s_eval, root->d.polar.lo);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "linept: eval error");
            } else {
                algebra_line_slope_point(mv, x1v, y1v, st.result, sizeof(st.result));
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }

        if (pop == POLOP_PARABOLA) {
            ce_float_t vx, vy, px, py;
            eval_init(&s_eval);
            vx = eval_expr(&s_eval, root->d.polar.expr1);
            vy = eval_expr(&s_eval, root->d.polar.expr2);
            px = eval_expr(&s_eval, root->d.polar.lo);
            py = eval_expr(&s_eval, root->d.polar.hi);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "parabola: eval error");
            } else if (!algebra_parabola_vertex(vx, vy, px, py, st.result, sizeof(st.result))) {
                proc_failf(&st, "parabola: vertex = point x");
            } else {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }

        if (pop == POLOP_TOFRAC) {
            ce_float_t val;
            eval_init(&s_eval);
            val = eval_expr(&s_eval, root->d.polar.expr1);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "tofrac: eval error");
            } else if (!algebra_dec_to_frac(val, 10000, st.result, sizeof(st.result))) {
                proc_failf(&st, "tofrac: no simple fraction");
            } else {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }

        if (pop == POLOP_UNITCIRC) {
            ce_float_t angle;
            eval_init(&s_eval);
            angle = eval_expr(&s_eval, root->d.polar.expr1);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "unitcirc: eval error");
            } else if (!algebra_unit_circle(angle, st.result, sizeof(st.result))) {
                proc_failf(&st, "%s", st.result);
            } else {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }
#endif

#if CE_PROC_ALG_ADV_SYM
        if (pop == POLOP_NLSOLVE) {
            const ast_node_t *eqs[2];
            char vars[3];
            eqs[0] = root->d.polar.expr1;
            eqs[1] = root->d.polar.expr2;
            vars[0] = root->d.polar.var;
            vars[1] = root->d.polar.lo ? (char)(int)root->d.polar.lo->d.number : 'y';
            vars[2] = '\0';
            algebra_solve_nonlinear(eqs, vars, st.result, sizeof(st.result));
            st.ok = true;
            st.show_steps = true;
            st.error[0] = '\0';
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_ALG_ADV_GEO
        if (pop == POLOP_MIDPOINT) {
            ce_float_t x1v, y1v, x2v, y2v, mx, my;
            eval_init(&s_eval);
            x1v = eval_expr(&s_eval, root->d.polar.expr1);
            y1v = eval_expr(&s_eval, root->d.polar.expr2);
            x2v = eval_expr(&s_eval, root->d.polar.lo);
            y2v = eval_expr(&s_eval, root->d.polar.hi);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "midpoint: eval error");
            } else {
                mx = (x1v + x2v) / 2.0f;
                my = (y1v + y2v) / 2.0f;
                {
                    char mx_buf[32], my_buf[32];
                    proc_format_decimal(mx_buf, sizeof(mx_buf), mx);
                    proc_format_decimal(my_buf, sizeof(my_buf), my);
                    proc_okf(&st, true, "Midpoint = (%s, %s)", mx_buf, my_buf);
                }
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }

        if (pop == POLOP_DISTANCE || pop == POLOP_SLOPE) {
            ce_float_t val;
            eval_init(&s_eval);
            val = eval_expr(&s_eval, root);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "%s", eval_error_msg(&s_eval));
            } else {
                st.has_numeric = true;
                st.last_numeric = val;
                proc_ok_number(&st, true,
                               (pop == POLOP_DISTANCE) ? "Distance = " : "Slope = ",
                               val, "");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }
#endif
#endif

#if CE_PROC_CALC_LIMIT_CONTINUITY
        if (pop == POLOP_CONTINUITY) {
            continuity_t ct;
            if (!root->d.polar.expr2) {
                proc_failf(&st, "continuity: need point");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            ct = limits_continuity(root->d.polar.expr1, root->d.polar.var, root->d.polar.expr2);
            switch (ct) {
            case CONT_CONTINUOUS: proc_okf(&st, true, "Continuous"); break;
            case CONT_REMOVABLE:  proc_okf(&st, true, "Removable discontinuity"); break;
            case CONT_JUMP:       proc_okf(&st, true, "Jump discontinuity"); break;
            case CONT_INFINITE:   proc_okf(&st, true, "Infinite discontinuity"); break;
            case CONT_ESSENTIAL:  proc_okf(&st, true, "Essential discontinuity"); break;
            default:              proc_okf(&st, true, "Continuity: could not determine"); break;
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_LIMIT_RELRATE
        if (pop == POLOP_RELRATE) {
            const ast_node_t *expr = root->d.polar.expr1;
            const char *vars = "xyzrswuvab";
            char found[10];
            int nfound = 0;
            int i, rlen;

            for (i = 0; vars[i] && nfound < 10; i++) {
                if (ast_contains_var(expr, vars[i])) found[nfound++] = vars[i];
            }
            if (nfound == 0) {
                proc_failf(&st, "No variables found");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            steps_add("Related rates: d/dt of expression");
            steps_add("Chain rule: sum of df/dv * dv/dt");
            snprintf(st.result, sizeof(st.result), "d/dt = ");
            rlen = (int)strlen(st.result);

            for (i = 0; i < nfound; i++) {
                ast_node_t *partial = deriv_diff_quiet(expr, found[i]);
                if (partial) {
                    char partial_str[64];
                    ast_to_display_str(partial, partial_str, sizeof(partial_str));
                    if (strstr(partial_str, "...")) {
                        ast_free(partial);
                        break;
                    }
                    if (i > 0 && rlen + 3 < MAX_RESULT_LEN) {
                        rlen += snprintf(st.result + rlen, sizeof(st.result) - rlen, " + ");
                    }
                    rlen += snprintf(st.result + rlen, sizeof(st.result) - rlen,
                                     "(%s)*d%c/dt", partial_str, found[i]);
                    ast_free(partial);
                    /* snprintf reports the required length, not bytes written.
                     * Stop before another term can use an invalid offset. */
                    if (rlen >= (int)sizeof(st.result)) break;
                } else break;
            }
            if (i < nfound) {
                proc_failf(&st, "Related rate result incomplete");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            st.ok = true;
            st.show_steps = true;
            st.error[0] = '\0';
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_ODE
        if (pop == POLOP_ODE_SEP || pop == POLOP_ODE_LINEAR || pop == POLOP_DIFFEQ) {
            char xvar = root->d.polar.var ? root->d.polar.var : 'x';
            char yvar = (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                      ? root->d.polar.expr2->d.variable : 'y';
            char sol[256];
            bool ok = false;
            sol[0] = '\0';

            if (xvar == yvar) {
                proc_failf(&st, "ODE: independent and dependent variables must differ");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            if (pop == POLOP_ODE_SEP) {
                ok = ode_solve_separable(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
            } else if (pop == POLOP_ODE_LINEAR) {
                ok = ode_solve_linear(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
            } else {
                ok = ode_solve_separable(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
                if (!ok)
                    ok = ode_solve_linear(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
                if (!ok && !sol[0]) {
                    strncpy(sol, "Unsolvable by current ODE methods (separable/linear)",
                            sizeof(sol) - 1);
                    sol[sizeof(sol) - 1] = '\0';
                }
            }

            if (!ok) {
                proc_failf(&st, "%s", sol[0] ? sol : "Unsolvable ODE by current methods");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            proc_okf(&st, true, "%s", sol);
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_EULER
        if (pop == POLOP_EULER || pop == POLOP_SLOPEFIELD) {
            ce_float_t val;
            eval_init(&s_eval);
            val = eval_expr(&s_eval, root);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "%s", eval_error_msg(&s_eval));
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            if (pop == POLOP_EULER) {
                st.has_numeric = true;
                st.last_numeric = val;
                proc_ok_number(&st, true, "Euler y(xf) ~= ", val, "");
            } else {
                proc_ok_number(&st, true, "Slope field generated\navg |slope| = ", val, "");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_SERIES_SEQ
        if (pop == POLOP_ARITHSUM || pop == POLOP_GEOMSUM) {
            ce_float_t val;
            eval_init(&s_eval);
            val = eval_expr(&s_eval, root);
            if (eval_error(&s_eval) != CE_OK) {
                proc_failf(&st, "%s", eval_error_msg(&s_eval));
            } else {
                st.has_numeric = true;
                st.last_numeric = val;
                proc_ok_number(&st, true,
                               (pop == POLOP_ARITHSUM) ? "Arithmetic sum = " : "Geometric sum = ",
                               val, "");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return st.ok;
        }
#endif

#if CE_PROC_CALC_INT_STRATEGY
        if (pop == POLOP_INTPARTS || pop == POLOP_TRIGSUB || pop == POLOP_USUB) {
            integ_strategy_t strategy = INTEG_STRATEGY_AUTO;
            const ast_node_t *u_hint = NULL;
            ast_node_t *anti;
            char abuf[160];
            char var = root->d.polar.var ? root->d.polar.var : 'x';

            if (pop == POLOP_INTPARTS) strategy = INTEG_STRATEGY_PARTS;
            else if (pop == POLOP_TRIGSUB) strategy = INTEG_STRATEGY_TRIGSUB;
            else { strategy = INTEG_STRATEGY_USUB; u_hint = root->d.polar.lo; }

            anti = integrate_with_strategy(root->d.polar.expr1, var, strategy, u_hint);
            if (!anti) {
                proc_failf(&st, "%s", integrate_last_failure_reason());
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            ast_to_display_str(anti, abuf, sizeof(abuf));
            proc_okf(&st, true, "int = %s + C", abuf);
            ast_free(anti);
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_INT_STRATEGY
        if (pop == POLOP_INTSTRATEGY) {
            char var = root->d.polar.var ? root->d.polar.var : 'x';
            integrate_strategy_report(root->d.polar.expr1, var,
                                      st.result, (int)sizeof(st.result));
            st.ok = true;
            st.show_steps = true;
            st.error[0] = '\0';
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_INT && CE_PROC_CALC_INT_DEFINITE && CE_PROC_CALC_INT_POLAR_LOWER
        if (pop == POLOP_IMPROPER || pop == POLOP_AREABETWEEN) {
            handle_int_polar(root, &st);
            finalize_response(&st, resp);
            return st.ok;
        }
#endif

#if CE_PROC_CALC_INT_DEFINITE_POLAR
        if (pop == POLOP_AREABETWEEN) {
            char var = root->d.polar.var ? root->d.polar.var : 'x';
            ce_float_t a_val = 0, b_val = 1;
            ce_error_t aerr = CE_OK;
            ce_float_t area_result;

            if (!root->d.polar.expr1 || !root->d.polar.expr2) {
                proc_failf(&st, "areabetween: requires two expressions");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            if (root->d.polar.lo) {
                eval_init(&s_eval);
                a_val = eval_expr(&s_eval, root->d.polar.lo);
            }
            if (root->d.polar.hi) {
                eval_init(&s_eval);
                b_val = eval_expr(&s_eval, root->d.polar.hi);
            }

            area_result = area_between_curves(root->d.polar.expr1,
                                              root->d.polar.expr2,
                                              var, a_val, b_val, &aerr);
            if (aerr != CE_OK) {
                proc_failf(&st, "areabetween: computation failed");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            st.has_numeric  = true;
            st.last_numeric = area_result;
            proc_ok_number(&st, true, "Area between curves = ", area_result, "");
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_INT_DEFINITE_POLAR
        if (pop == POLOP_IMPROPER) {
            /* improper(expr, var, lo, hi) — direct improper integral call.
             * With both bounds it is int(expr, var, lo, hi), which owns the
             * pole scan, the splitting and the rounding checks: improper()
             * ran F(b) - F(a) across 1/(x-1)^2 on [0,3] (review round 13). */
            char var = root->d.polar.var ? root->d.polar.var : 'x';
            if (root->d.polar.lo && root->d.polar.hi) {
                ast_node_t *in = mem_alloc_node();
                if (in) {  /* zeroed by mem_alloc_node */
                    in->type = NODE_INTEGRAL;
                    in->d.integral.var = var;
                    in->d.integral.expr = root->d.polar.expr1;
                    in->d.integral.lo = root->d.polar.lo;
                    in->d.integral.hi = root->d.polar.hi;
                    root->d.polar.expr1 = NULL;
                    root->d.polar.lo = NULL;
                    root->d.polar.hi = NULL;
                    ast_free(root);
                    handle_integral(in, &st);
                    finalize_response(&st, resp);
                    return st.ok;
                }
            }
            ce_float_t lo_val = 0, hi_val = 0;
            bool lo_inf = false, hi_inf = false;
            improp_status_t improp_st;
            ce_float_t improp_result = 0;

            if (root->d.polar.lo) {
                eval_init(&s_eval);
                lo_val = eval_expr(&s_eval, root->d.polar.lo);
                lo_inf = !ce_isfinite(lo_val);
            }
            if (root->d.polar.hi) {
                eval_init(&s_eval);
                hi_val = eval_expr(&s_eval, root->d.polar.hi);
                hi_inf = !ce_isfinite(hi_val);
            }

            steps_addf("Improper integral: evaluating convergence for bounds [%s, %s]",
                       lo_inf ? "-inf" : "finite",
                       hi_inf ? "+inf" : "finite");

            improp_st = integrate_improper(root->d.polar.expr1, var,
                                           lo_inf ? 0 : lo_val, lo_inf,
                                           hi_inf ? 0 : hi_val, hi_inf,
                                           &improp_result);
            switch (improp_st) {
            case IMPROP_OK:
                st.has_numeric  = true;
                st.last_numeric = improp_result;
                proc_ok_number(&st, true, "= ", improp_result, "  (converges)");
                break;
            case IMPROP_DIVERGES:
                proc_okf(&st, true, "Integral DIVERGES");
                break;
            default:
                proc_failf(&st, "Could not determine convergence of improper integral");
                break;
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

#if CE_PROC_CALC_POLAR_NUM
        if (pop == POLOP_ARCLEN || pop == POLOP_ARCLEN_PARAM ||
            pop == POLOP_SURFREV || pop == POLOP_DISK ||
            pop == POLOP_WASHER || pop == POLOP_SHELL) {
            ce_float_t a_val = 0, b_val = 1, result_val;
            ce_error_t perr = CE_OK;
            const char *label = "";

            eval_init(&s_eval);
            if (root->d.polar.lo) a_val = eval_expr(&s_eval, root->d.polar.lo);
            if (root->d.polar.hi) b_val = eval_expr(&s_eval, root->d.polar.hi);

            switch (pop) {
            case POLOP_ARCLEN:
                label = "Arc length";
                result_val = polar_arclen(root->d.polar.expr1, root->d.polar.var, a_val, b_val, &perr);
                break;
            case POLOP_ARCLEN_PARAM:
                label = "Parametric arc length";
                result_val = polar_arclen_param(root->d.polar.expr1, root->d.polar.expr2,
                                                root->d.polar.var, a_val, b_val, &perr);
                break;
            case POLOP_SURFREV:
                label = "Surface area";
                result_val = polar_surfrev(root->d.polar.expr1, root->d.polar.var, a_val, b_val, &perr);
                break;
            case POLOP_DISK:
                label = "Volume (disk)";
                result_val = polar_disk(root->d.polar.expr1, root->d.polar.var, a_val, b_val, &perr);
                break;
            case POLOP_WASHER:
                label = "Volume (washer)";
                result_val = polar_washer(root->d.polar.expr1, root->d.polar.expr2,
                                          root->d.polar.var, a_val, b_val, &perr);
                break;
            case POLOP_SHELL:
                label = "Volume (shell)";
                result_val = polar_shell(root->d.polar.expr1, root->d.polar.var, a_val, b_val, &perr);
                break;
            default:
                perr = CE_ERR_INTERNAL;
                result_val = 0;
                break;
            }

            /* A bound that failed to evaluate is not 0 (polar.c keeps its
             * own contexts, so s_eval still holds the bounds' status). A
             * failed sample makes perr CE_ERR_DOMAIN (polar.c), and a
             * non-finite total is no volume or length either, never "= nan". */
            if (perr != CE_OK || eval_error(&s_eval) != CE_OK ||
                !ce_isfinite(result_val)) {
                proc_failf(&st, "%s: computation failed", label);
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            st.has_numeric = true;
            st.last_numeric = result_val;
            {
                char prefix[96];
                snprintf(prefix, sizeof(prefix), "%s = ", label);
                proc_ok_number(&st, true, prefix, result_val, "");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
#endif

/* New Pass 74/75 features: not linked in split modules, handled by main.c */
#ifndef HW_BUILD_SPLIT
        /* ── dsolve ────────────────────────────────────────────────────── */
        if (pop == POLOP_DSOLVE) {
            char xvar = root->d.polar.var ? root->d.polar.var : 'x';
            char yvar = 'y';
            if (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE) {
                yvar = root->d.polar.expr2->d.variable;
            }
            ode_method_t method = ODE_METHOD_UNKNOWN;
            bool ok = dsolve_auto(root->d.polar.expr1, xvar, yvar,
                                  st.result, MAX_RESULT_LEN, &method);
            if (ok) {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            } else {
                proc_failf(&st, "dsolve: tried separable, linear, exact - no solution found");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok;
        }

        /* ── polararea / polararclen ────────────────────────────────────── */
        if (pop == POLOP_POLARAREA || pop == POLOP_POLARARCLEN) {
            char pvar = root->d.polar.var ? root->d.polar.var : 'x';
            ce_float_t a_val = 0, b_val = 0;
            if (root->d.polar.lo) {
                eval_init(&s_eval);
                a_val = eval_expr(&s_eval, root->d.polar.lo);
            }
            if (root->d.polar.hi) {
                eval_init(&s_eval);
                b_val = eval_expr(&s_eval, root->d.polar.hi);
            }
            ce_error_t perr = CE_OK;
            ce_float_t result_val;
            const char *label2;
            if (pop == POLOP_POLARAREA) {
                label2 = "Polar area";
                result_val = polar_area(root->d.polar.expr1, pvar,
                                        a_val, b_val, &perr);
            } else {
                label2 = "Polar arc length";
                result_val = polar_arclen_polar(root->d.polar.expr1, pvar,
                                                a_val, b_val, &perr);
            }
            if (perr != CE_OK) {
                proc_failf(&st, "%s: computation failed", label2);
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            st.has_numeric = true;
            st.last_numeric = result_val;
            {
                char prefix[96];
                snprintf(prefix, sizeof(prefix), "%s = ", label2);
                proc_ok_number(&st, true, prefix, result_val, "");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

        /* ── vector dot product ─────────────────────────────────────────── */
        if (pop == POLOP_VEC_DOT) {
            /* expr1 is a flat PAIR chain: u0,u1,...,un, v0,v1,...,vn (even count) */
            ce_float_t vals[6] = {0,0,0,0,0,0};
            eval_init(&s_eval);
            int total = vector_eval_pair_chain(root->d.polar.expr1, vals, 6, &s_eval);
            if (total < 2 || (total % 2) != 0) {
                proc_failf(&st, "dot: need equal-length vectors, e.g. dot(1,2,3,4,5,6)");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            int half = total / 2;
            ce_float_t dot_val = vector_dot(vals, vals + half, half);
            st.has_numeric = true;
            st.last_numeric = dot_val;
            proc_ok_number(&st, true, "u\xb7v = ", dot_val, "");
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

        /* ── vector cross product ───────────────────────────────────────── */
        if (pop == POLOP_VEC_CROSS) {
            /* expr1 is a flat PAIR chain of exactly 6 values: u0,u1,u2,v0,v1,v2 */
            ce_float_t vals[6] = {0,0,0,0,0,0};
            eval_init(&s_eval);
            int total = vector_eval_pair_chain(root->d.polar.expr1, vals, 6, &s_eval);
            if (total != 6) {
                proc_failf(&st, "cross: need exactly 6 components (u0,u1,u2,v0,v1,v2)");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            ce_float_t res[3];
            vector_cross(vals, vals + 3, res);
            {
                char x_buf[32], y_buf[32], z_buf[32];
                proc_format_decimal(x_buf, sizeof(x_buf), res[0]);
                proc_format_decimal(y_buf, sizeof(y_buf), res[1]);
                proc_format_decimal(z_buf, sizeof(z_buf), res[2]);
                proc_okf(&st, false, "u\xd7v = <%s, %s, %s>",
                         x_buf, y_buf, z_buf);
            }
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

        /* ── gradient ───────────────────────────────────────────────────── */
        if (pop == POLOP_VEC_GRAD) {
            char v1 = root->d.polar.var ? root->d.polar.var : 'x';
            char v2 = 'y';
            if (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE) {
                v2 = root->d.polar.expr2->d.variable;
            }
            const char vars_arr[3] = { v1, v2, '\0' };
            bool ok = vector_gradient(root->d.polar.expr1, vars_arr, 2,
                                      st.result, MAX_RESULT_LEN);
            if (ok) {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            } else {
                proc_failf(&st, "grad: cannot compute symbolic gradient");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok;
        }

        /* ── divergence ─────────────────────────────────────────────────── */
        if (pop == POLOP_VEC_DIV) {
            char v1 = root->d.polar.var ? root->d.polar.var : 'x';
            char v2 = 'y';
            if (root->d.polar.lo && root->d.polar.lo->type == NODE_VARIABLE) {
                v2 = root->d.polar.lo->d.variable;
            }
            const char vars_arr[3] = { v1, v2, '\0' };
            const ast_node_t *fields[2];
            fields[0] = root->d.polar.expr1;
            fields[1] = root->d.polar.expr2;
            bool ok = vector_divergence(fields, vars_arr, 2,
                                        st.result, MAX_RESULT_LEN);
            if (ok) {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            } else {
                proc_failf(&st, "div: cannot compute symbolic divergence");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok;
        }

        /* ── curl ───────────────────────────────────────────────────────── */
        if (pop == POLOP_VEC_CURL) {
            char vx = root->d.polar.var ? root->d.polar.var : 'x';
            char vy = 'y', vz = 'z';
            if (root->d.polar.lo && root->d.polar.lo->type == NODE_PAIR) {
                const ast_node_t *vp = root->d.polar.lo;
                if (vp->d.pair.head && vp->d.pair.head->type == NODE_VARIABLE)
                    vy = vp->d.pair.head->d.variable;
                if (vp->d.pair.tail && vp->d.pair.tail->type == NODE_VARIABLE)
                    vz = vp->d.pair.tail->d.variable;
            }
            const ast_node_t *Fx = root->d.polar.expr1;
            const ast_node_t *Fy = NULL, *Fz = NULL;
            if (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_PAIR) {
                Fy = root->d.polar.expr2->d.pair.head;
                Fz = root->d.polar.expr2->d.pair.tail;
            }
            bool ok = vector_curl(Fx, Fy, Fz, vx, vy, vz,
                                  st.result, MAX_RESULT_LEN);
            if (ok) {
                st.ok = true;
                st.show_steps = true;
                st.error[0] = '\0';
            } else {
                proc_failf(&st, "curl: cannot compute symbolic curl");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok;
        }

        /* ── Laplace transform ──────────────────────────────────────────── */
        if (pop == POLOP_LAPLACE) {
            char tvar = root->d.polar.var ? root->d.polar.var : 't';
            char svar = 's';
            if (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE) {
                svar = root->d.polar.expr2->d.variable;
            }
            bool ok = laplace_forward(root->d.polar.expr1, tvar, svar,
                                      st.result, MAX_RESULT_LEN);
            if (ok) {
                st.ok = true;
                st.show_steps = false;
                st.error[0] = '\0';
            } else {
                proc_failf(&st, "Laplace: no matching form found");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok;
        }

        /* ── Inverse Laplace transform ──────────────────────────────────── */
        if (pop == POLOP_ILAPLACE) {
            char svar = root->d.polar.var ? root->d.polar.var : 's';
            char tvar = 't';
            if (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE) {
                tvar = root->d.polar.expr2->d.variable;
            }
            bool ok = laplace_inverse(root->d.polar.expr1, svar, tvar,
                                      st.result, MAX_RESULT_LEN);
            if (ok) {
                st.ok = true;
                st.show_steps = false;
                st.error[0] = '\0';
            } else {
                proc_failf(&st, "Inv Laplace: no matching form found");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok;
        }

        /* ── dsolve2: 2nd-order linear ODE ────────────────────────────── */
        if (pop == POLOP_DSOLVE2) {
            /*
             * Packed: expr1=a_node, expr2=b_node, lo=c_node, hi=f_node, var=xvar.
             * Evaluate a,b,c numerically (they must be constant).
             * f may contain x; pass as AST to dsolve2_const_coeff.
             */
            char xvar2 = root->d.polar.var ? root->d.polar.var : 'x';
            eval_ctx_t ec2;
            ce_float_t a_v, b_v, c_v;
            bool ok2;

            eval_init(&ec2);
            a_v = root->d.polar.expr1 ? eval_expr(&ec2, root->d.polar.expr1) : 1.0f;
            eval_init(&ec2);
            b_v = root->d.polar.expr2 ? eval_expr(&ec2, root->d.polar.expr2) : 0.0f;
            eval_init(&ec2);
            c_v = root->d.polar.lo    ? eval_expr(&ec2, root->d.polar.lo)    : 0.0f;

            if (!ce_isfinite(a_v) || !ce_isfinite(b_v) || !ce_isfinite(c_v)) {
                proc_failf(&st, "dsolve2: coefficients a,b,c must be numeric constants");
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }

            steps_clear();
            ok2 = dsolve2_const_coeff(a_v, b_v, c_v,
                                       root->d.polar.hi,   /* f(x) AST */
                                       xvar2,
                                       st.result, MAX_RESULT_LEN);
            if (ok2) {
                st.ok         = true;
                st.show_steps = true;
                st.error[0]   = '\0';
            } else {
                proc_failf(&st, "dsolve2: could not solve ODE");
            }
            ast_free(root);
            finalize_response(&st, resp);
            return ok2;
        }

        /* ── graphimpl: implicit curve graphing ─────────────────────────── */
        if (pop == POLOP_GRAPHIMPL) {
            char xvar3 = root->d.polar.var ? root->d.polar.var : 'x';
            char yvar3 = 'y';
            ce_float_t xmin3 = -10.0f, xmax3 = 10.0f;
            eval_ctx_t ec4;

            /* Extract yvar from expr2 (NODE_VARIABLE) */
            if (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                yvar3 = root->d.polar.expr2->d.variable;

            /* Evaluate xmin/xmax if provided */
            if (root->d.polar.lo) {
                eval_init(&ec4);
                ce_float_t v = eval_expr(&ec4, root->d.polar.lo);
                if (ce_isfinite(v)) xmin3 = v;
            }
            if (root->d.polar.hi) {
                eval_init(&ec4);
                ce_float_t v = eval_expr(&ec4, root->d.polar.hi);
                if (ce_isfinite(v)) xmax3 = v;
            }

            steps_clear();
            graph_implicit_result(root->d.polar.expr1,
                                   xvar3, yvar3,
                                   xmin3, xmax3, -10.0f, 10.0f,
                                   st.result, MAX_RESULT_LEN);
            st.ok         = true;
            st.show_steps = false;
            st.error[0]   = '\0';
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }

        /* ── Unit conversion ────────────────────────────────────────────── */
        if (pop == POLOP_CONVERT) {
            /* convert() is pre-processed entirely in main.c before IPC dispatch.
             * If it reaches processor.c it means the request format is wrong. */
            proc_failf(&st, "convert: use convert(value, from_unit, to_unit) syntax");
            ast_free(root);
            finalize_response(&st, resp);
            return false;
        }
#endif /* !HW_BUILD_SPLIT */

    }

#if CE_PROC_GRAPH
#if CE_PROC_USE_CLASSIFY
    if (domain == CE_DOMAIN_GRAPH) {
        proc_okf(&st, false,
                 "Graph module loaded. Enter graph() expressions in CALCENG.");
        ast_free(root);
        finalize_response(&st, resp);
        return true;
    }
#else
    proc_okf(&st, false,
             "Graph module loaded. Enter graph() expressions in CALCENG.");
    ast_free(root);
    finalize_response(&st, resp);
    return true;
#endif
#endif

#if CE_PROC_ALG_SOLVE
    if (input_had_equals && root->type != NODE_SOLVE) {
        char var = find_var_in(root);
        if (var) {
            algebra_solve(root, var, st.result, MAX_RESULT_LEN);
            st.ok = true;
            st.show_steps = true;
            st.error[0] = '\0';
            ast_free(root);
            finalize_response(&st, resp);
            return true;
        }
    }

#if CE_PROC_ALG_POLYDIV
    if (root->type == NODE_BINOP && root->d.binop.op == OP_DIV) {
        char var = find_var_in(root);
        if (var) {
            char rem_buf[64];
            ast_node_t *quot = algebra_poly_div(root->d.binop.left, root->d.binop.right,
                                                var, rem_buf, sizeof(rem_buf));
            if (quot) {
                char ast_str[128];
                ast_to_display_str(quot, ast_str, sizeof(ast_str));
                if (strcmp(rem_buf, "0") == 0) {
                    proc_okf(&st, true, "= %s", ast_str);
                } else {
                    proc_okf(&st, true, "Q = %s\nR = %s", ast_str, rem_buf);
                }
                ast_free(quot);
                ast_free(root);
                finalize_response(&st, resp);
                return true;
            }
        }
    }
#endif
#endif

#if CE_PROC_GENERIC
#if CE_PROC_DISAMBIG_MENU
    /* ── Disambiguation: offer choices when expression has a variable ── */
    {
        char var = find_var_in(root);
        if (var && input && input[0] &&
            (root->type == NODE_BINOP || root->type == NODE_UNOP ||
             root->type == NODE_FUNC)) {
            /* Build disambiguation options:
             * Format: "Label1\nExpr1\nLabel2\nExpr2\n..." */
            static char dbuf[MAX_RESULT_LEN];
            static char solve_expr[MAX_INPUT_LEN];
            static char factor_expr[MAX_INPUT_LEN];
            static char expand_expr[MAX_INPUT_LEN];
            int pos = 0;
            int ilen = (int)strlen(input);

            snprintf(solve_expr,  sizeof(solve_expr),  "solve(%s=%c0,%c)",
                     input, ilen > 0 ? ' ' : '0', var);
            snprintf(factor_expr, sizeof(factor_expr), "factor(%s)", input);
            snprintf(expand_expr, sizeof(expand_expr), "expand(%s)", input);

#define DISAMBIG_APPEND(label, expr) do { \
    int ll = (int)strlen(label); int el = (int)strlen(expr); \
    if (pos + ll + el + 2 < (int)sizeof(dbuf)) { \
        memcpy(dbuf + pos, label, (size_t)ll); pos += ll; \
        dbuf[pos++] = '\n'; \
        memcpy(dbuf + pos, expr, (size_t)el); pos += el; \
        dbuf[pos++] = '\n'; \
    } \
} while (0)

            DISAMBIG_APPEND("Evaluate numerically", input);
            DISAMBIG_APPEND("Simplify",             input);
            DISAMBIG_APPEND("Solve for variable",   solve_expr);
            DISAMBIG_APPEND("Factor",               factor_expr);
            DISAMBIG_APPEND("Expand",               expand_expr);
#undef DISAMBIG_APPEND

            dbuf[pos] = '\0';

            /* Only offer disambiguation if we have at least 2 options encoded */
            if (pos > 4) {
                /* Stamp magic/version/settings like every other response;
                 * this path once left them zero, and ce_response_read()
                 * rejected it with "Module CEBASE returned invalid IPC data"
                 * on the CE. Keep the previous Ans. */
                ce_response_from_runtime(resp, false, false, dbuf, "",
                                         g_last_answer);
                resp->ok = 2;   /* signal: disambiguation needed */
                resp->step_count = 0;   /* no stale steps on the menu */
                ast_free(root);
                return true;
            }
        }
    }
#endif /* CE_PROC_DISAMBIG_MENU */

    {
        char ast_str[128];
        ce_float_t val;

        ast_to_display_str(root, ast_str, sizeof(ast_str));
        eval_init(&s_eval);
        val = eval_expr(&s_eval, root);
        if (eval_error(&s_eval) != CE_OK) {
            char uv = s_eval.undefined_var;
            if (eval_error(&s_eval) == CE_ERR_UNDEFINED && uv) {
                /* The variable has no local binding and no stored value
                 * (`expr->X` stores one in modules built with
                 * CE_EVAL_USER_VARS). Name the variable on
                 * line 1 (the home row) and list what can be done with the
                 * expression; each line fits 26 columns. */
#if CE_EVAL_USER_VARS
                proc_failf(&st,
                           "%c has no value\n"
                           "Try d/d%c(...), int(...,%c),\n"
                           "solve(...=0,%c),\n"
                           "factor(...) or expand(...)\n"
                           "Or store a value: 5->%c",
                           uv, uv, uv, uv, uv);
#else
                proc_failf(&st,
                           "%c has no value\n"
                           "Try d/d%c(...), int(...,%c),\n"
                           "solve(...=0,%c),\n"
                           "factor(...) or expand(...)",
                           uv, uv, uv, uv);
#endif
            } else {
                proc_failf(&st, "%s", eval_error_msg(&s_eval));
            }
            ast_free(root);
            finalize_response(&st, resp);
            return false;
        }

        {
            int base_rc = proc_format_base_result(st.result, sizeof(st.result),
                                                  root, val);
            if (base_rc < 0) {
                /* proc_failf clears st.result before formatting, so move
                 * the message out first. */
                char base_err[48];
                strncpy(base_err, st.result, sizeof(base_err) - 1);
                base_err[sizeof(base_err) - 1] = '\0';
                proc_failf(&st, "%s", base_err);
                ast_free(root);
                finalize_response(&st, resp);
                return false;
            }
            if (base_rc == 0) {
#if CE_SAFE_FLOAT_TINY
                /* A tiny value that got through is no answer (eval.c
                 * refuses tiny operands and results; ce_float_ops.h). */
                if (g_ce_fops_tiny || ce_fops_class(val) == 1u) {
                    proc_failf(&st, "%s", CE_FOPS_TINY_MSG);
                    ast_free(root);
                    finalize_response(&st, resp);
                    return false;
                }
                /* -0 (only an OS routine could return it) is shown and
                 * kept as 0: the CE's compare would print it as "-0". */
                if (!ce_fops_class(val)) val = 0.0f;
                /* Six fixed decimals print a nonzero value below 1e-6 as
                 * 0 (9.80203e-20 did), so show it in scientific form.
                 * "%.*g" is rendered by ce_fmt.c, as in int_format_value. */
                if (ce_fops_class(val) && fabsf(val) < 1e-6f)
                    snprintf(st.result, sizeof(st.result), "= %.*g", 6, (double)val);
                else
#endif
                proc_format_result(st.result, sizeof(st.result), val);
            }
        }
        st.has_numeric = true;
        st.last_numeric = val;
        {
            int rlen = (int)strlen(st.result);
            if (rlen + 20 < MAX_RESULT_LEN) {
                snprintf(st.result + rlen, sizeof(st.result) - rlen, "\nParsed: %s", ast_str);
            }
        }
        st.ok = true;
        st.show_steps = false;
        st.error[0] = '\0';

        ast_free(root);
        finalize_response(&st, resp);
        return true;
    }
#else
#if CE_PROC_USE_CLASSIFY
    (void)domain;
#endif
    proc_failf(&st, "Feature is routed to another module");
    ast_free(root);
    finalize_response(&st, resp);
    return false;
#endif
}

bool ce_processor_run_preparsed(const char *input,
                                ast_node_t *root,
                                bool input_had_equals,
                                ce_response_t *resp)
{
    return ce_processor_run_root(input, root, input_had_equals, resp);
}

bool ce_processor_run(const char *input, ce_response_t *resp)
{
#if CE_PROC_PARSE_INPUT
    ast_node_t *root = NULL;
    bool input_had_equals = false;
    char parse_err[128] = {0};
    static proc_state_t st;

    memset(&st, 0, sizeof(st));
    st.ok = false;

    if (!resp) return false;
    if (!input || !input[0]) {
        proc_failf(&st, "Empty input");
        finalize_response(&st, resp);
        return false;
    }

    if (!parse_input(input, &root, &input_had_equals, parse_err, sizeof(parse_err))) {
        proc_failf(&st, "%s", parse_err);
        finalize_response(&st, resp);
        return false;
    }

    return ce_processor_run_root(input, root, input_had_equals, resp);
#else
    static proc_state_t st;
    (void)input;
    memset(&st, 0, sizeof(st));
    st.ok = false;
    if (!resp) return false;
    proc_failf(&st, "Parser is disabled in this module");
    finalize_response(&st, resp);
    return false;
#endif
}
