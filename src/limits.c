/*
 * limits.c -- Symbolic limit engine
 *
 * Implements:
 *   - Direct substitution
 *   - L'Hopital's rule for 0/0 and inf/inf (via deriv_diff_quiet)
 *   - Special limits: sin(x)/x, tan(x)/x, (e^x-1)/x, (1-cos(x))/x^2
 *   - Limits at infinity for polynomial/rational expressions
 *   - One-sided limits via numerical approach
 *   - Squeeze theorem hints in step display
 *   - Step-by-step recording via steps module
 */
#include "limits.h"
#include "deriv.h"
#include "algebra.h"
#include "eval.h"
#include "steps.h"
#include "memory.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef CE_LIMITS_USE_SIMPLIFY
#define CE_LIMITS_USE_SIMPLIFY 1
#endif

#ifndef CE_LIMITS_ENABLE_SHIFTED_SPECIAL
#define CE_LIMITS_ENABLE_SHIFTED_SPECIAL 1
#endif

#ifndef CE_LIMITS_ENABLE_FACTOR_CANCEL
#define CE_LIMITS_ENABLE_FACTOR_CANCEL 1
#endif

#ifndef CE_LIMITS_ENABLE_RATIONALIZE
#define CE_LIMITS_ENABLE_RATIONALIZE 1
#endif

#ifndef CE_LIMITS_ENABLE_LHOPITAL
#define CE_LIMITS_ENABLE_LHOPITAL 1
#endif

#ifndef CE_LIMITS_ENABLE_ZERO_TIMES_INF
#define CE_LIMITS_ENABLE_ZERO_TIMES_INF 1
#endif

#ifndef CE_LIMITS_ENABLE_SPLIT_RULES
#define CE_LIMITS_ENABLE_SPLIT_RULES 1
#endif

#if CE_LIMITS_USE_SIMPLIFY
#include "simplify.h"
#else
static ast_node_t *ast_simplify(ast_node_t *expr)
{
    return expr;
}
#endif

/* Keep strategy scratch out of every recursive limit frame under LTO. */
#if defined(__GNUC__) || defined(__clang__)
#define LIMIT_NOINLINE __attribute__((noinline))
#else
#define LIMIT_NOINLINE
#endif

/* ── Tuning constants ───────────────────────────────────────── */
#define MAX_LHOPITAL  5       /* max L'Hopital applications    */
#define NEAR_ZERO     1e-4f   /* threshold for "approaches 0"  */
#define HUGE_THRESH   1e15f   /* threshold for "approaches inf"*/
#define CONVERGE_TOL  0.01f   /* relative convergence tolerance*/

/* ── Forward declarations ───────────────────────────────────── */
static lim_status_t limit_inner(const ast_node_t *expr, char var,
                                ce_float_t pt, int8_t dir,
                                ce_float_t *result, int depth);

/* eval_at() now lives in eval.c as a public API */

/* ── Helper: check if value is effectively zero ─────────────── */
static bool near_zero(ce_float_t v) { return fabsf(v) < NEAR_ZERO; }

/* ── Helper: check if value is huge (near infinity) ─────────── */
static bool is_huge(ce_float_t v)   { return fabsf(v) > HUGE_THRESH; }

/* ── 1. Direct Substitution ─────────────────────────────────── */

static lim_status_t try_direct_sub(const ast_node_t *expr, char var,
                                   ce_float_t pt, ce_float_t *result)
{
    ce_error_t err;
    ce_float_t val = eval_at(expr, var, pt, &err);

    if (err == CE_OK && ce_isfinite(val)) {
        *result = val;
        steps_addf("Direct sub: f(%.4g) = %.6g", (double)pt, (double)val);
        return LIM_OK;
    }
    return LIM_FAILED;
}

/* ── 2. Indeterminate form classification ───────────────────── */

typedef enum {
    FORM_NORMAL,
    FORM_0_OVER_0,
    FORM_INF_OVER_INF,
    FORM_OTHER
} indet_form_t;

#if CE_LIMITS_ENABLE_LHOPITAL
static indet_form_t classify_quotient(const ast_node_t *num_e,
                                      const ast_node_t *den_e,
                                      char var, ce_float_t pt, int8_t dir)
{
    ce_error_t en, ed;
    ce_float_t h = (dir >= 0) ? 1e-5f : -1e-5f;
    ce_float_t nv, dv;

    nv = eval_at(num_e, var, pt + h, &en);
    dv = eval_at(den_e, var, pt + h, &ed);

    if (en != CE_OK || ed != CE_OK) return FORM_OTHER;

    /* For two-sided limits, verify the left side agrees */
    if (dir == 0) {
        ce_error_t en2, ed2;
        ce_float_t nv2, dv2;
        nv2 = eval_at(num_e, var, pt - 1e-5f, &en2);
        dv2 = eval_at(den_e, var, pt - 1e-5f, &ed2);
        if (en2 == CE_OK && ed2 == CE_OK) {
            if (near_zero(nv) && near_zero(dv) &&
                near_zero(nv2) && near_zero(dv2))
                return FORM_0_OVER_0;
            if (is_huge(nv) && is_huge(dv) &&
                is_huge(nv2) && is_huge(dv2))
                return FORM_INF_OVER_INF;
            /* If sides disagree on form, use right-side classification */
        }
    }

    if (near_zero(nv) && near_zero(dv)) return FORM_0_OVER_0;
    if (is_huge(nv)   && is_huge(dv))   return FORM_INF_OVER_INF;
    return FORM_NORMAL;
}
#endif

/* ── 3. L'Hopital's Rule ───────────────────────────────────── */

#if CE_LIMITS_ENABLE_LHOPITAL
static LIMIT_NOINLINE lim_status_t try_lhopital(const ast_node_t *num_e,
                                 const ast_node_t *den_e,
                                 char var, ce_float_t pt, int8_t dir,
                                 ce_float_t *result, int depth)
{
    ast_node_t *fp, *gp, *quot;
    lim_status_t status;
    int sc;
    char fbuf[48], gbuf[48];

    if (depth >= MAX_LHOPITAL) {
        steps_addf("L'Hopital depth exceeded");
        return LIM_FAILED;
    }

    /* Differentiate numerator and denominator (silently) */
    sc = steps_count();
    fp = deriv_diff_quiet(num_e, var);
    gp = deriv_diff_quiet(den_e, var);
    steps_restore(sc);

    if (!fp || !gp) { ast_free(fp); ast_free(gp); return LIM_FAILED; }

    ast_to_str(fp, fbuf, sizeof(fbuf));
    ast_to_str(gp, gbuf, sizeof(gbuf));
    steps_addf("L'Hopital #%d: (%s)/(%s)", depth + 1, fbuf, gbuf);

    /* Form f'/g' and try the limit */
    quot = ast_binop(OP_DIV, fp, gp);
    if (!quot) return LIM_FAILED;
    status = limit_inner(quot, var, pt, dir, result, depth + 1);
    ast_free(quot);

    return status;
}
#endif

/* ── 4. Special Limits (pattern matching) ───────────────────── */

static bool is_var(const ast_node_t *n, char v)
{
    return n && n->type == NODE_VARIABLE && n->d.variable == v;
}

/* ast_is_num() now lives in ast.c as public API. */
#define is_num(n, v)  ast_is_num((n), (v))

/* Check if node is e^var or exp(var) */
static bool is_exp_var(const ast_node_t *n, char var)
{
    if (n->type == NODE_FUNC && n->d.func.id == FN_EXP &&
        is_var(n->d.func.arg, var))
        return true;
    if (n->type == NODE_BINOP && n->d.binop.op == OP_POW &&
        n->d.binop.left->type == NODE_CONSTANT &&
        n->d.binop.left->d.constant == CONST_E &&
        is_var(n->d.binop.right, var))
        return true;
    return false;
}

static lim_status_t try_special(const ast_node_t *expr, char var,
                                ce_float_t pt, ce_float_t *result)
{
    const ast_node_t *num_e, *den_e;

    /* Only for limit as var -> 0 */
    if (fabsf(pt) > NEAR_ZERO) return LIM_FAILED;
    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return LIM_FAILED;

    num_e = expr->d.binop.left;
    den_e = expr->d.binop.right;

    /* sin(x)/x -> 1 */
    if (num_e->type == NODE_FUNC && num_e->d.func.id == FN_SIN &&
        is_var(num_e->d.func.arg, var) && is_var(den_e, var)) {
        *result = 1.0f;
        steps_addf("Special: lim sin(%c)/%c = 1", var, var);
        return LIM_OK;
    }

    /* x/sin(x) -> 1 */
    if (is_var(num_e, var) &&
        den_e->type == NODE_FUNC && den_e->d.func.id == FN_SIN &&
        is_var(den_e->d.func.arg, var)) {
        *result = 1.0f;
        steps_addf("Special: lim %c/sin(%c) = 1", var, var);
        return LIM_OK;
    }

    /* tan(x)/x -> 1 */
    if (num_e->type == NODE_FUNC && num_e->d.func.id == FN_TAN &&
        is_var(num_e->d.func.arg, var) && is_var(den_e, var)) {
        *result = 1.0f;
        steps_addf("Special: lim tan(%c)/%c = 1", var, var);
        return LIM_OK;
    }

    /* (e^x - 1)/x -> 1 */
    if (is_var(den_e, var) &&
        num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB &&
        is_num(num_e->d.binop.right, 1) &&
        is_exp_var(num_e->d.binop.left, var)) {
        *result = 1.0f;
        steps_addf("Special: lim (e^%c-1)/%c = 1", var, var);
        return LIM_OK;
    }

    /* (1 - cos(x))/x -> 0 */
    if (is_var(den_e, var) &&
        num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB &&
        is_num(num_e->d.binop.left, 1) &&
        num_e->d.binop.right->type == NODE_FUNC &&
        num_e->d.binop.right->d.func.id == FN_COS &&
        is_var(num_e->d.binop.right->d.func.arg, var)) {
        *result = 0.0f;
        steps_addf("Special: lim (1-cos(%c))/%c = 0", var, var);
        return LIM_OK;
    }

    /* (1 - cos(x))/x^2 -> 1/2 */
    if (den_e->type == NODE_BINOP && den_e->d.binop.op == OP_POW &&
        is_var(den_e->d.binop.left, var) && is_num(den_e->d.binop.right, 2) &&
        num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB &&
        is_num(num_e->d.binop.left, 1) &&
        num_e->d.binop.right->type == NODE_FUNC &&
        num_e->d.binop.right->d.func.id == FN_COS &&
        is_var(num_e->d.binop.right->d.func.arg, var)) {
        *result = 0.5f;
        steps_addf("Special: lim (1-cos(%c))/%c^2 = 1/2", var, var);
        return LIM_OK;
    }

    return LIM_FAILED;
}

/* ── 4a2. Special shifted limits: sin(x-a)/(x-a) etc. ──────── */

/* Check if node matches (var - const) and return the constant */
static bool is_var_minus_const(const ast_node_t *n, char var, ce_float_t *c)
{
    if (n->type == NODE_BINOP && n->d.binop.op == OP_SUB &&
        is_var(n->d.binop.left, var) &&
        n->d.binop.right->type == NODE_NUMBER) {
        *c = n->d.binop.right->d.number;
        return true;
    }
    return false;
}

#if CE_LIMITS_ENABLE_SHIFTED_SPECIAL
static lim_status_t try_special_shifted(const ast_node_t *expr, char var,
                                        ce_float_t pt, ce_float_t *result)
{
    const ast_node_t *num_e, *den_e;
    ce_float_t a_num, a_den;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return LIM_FAILED;

    num_e = expr->d.binop.left;
    den_e = expr->d.binop.right;

    /* Denominator must be (x - a) where a == pt */
    if (!is_var_minus_const(den_e, var, &a_den))
        return LIM_FAILED;
    if (fabsf(a_den - pt) > NEAR_ZERO)
        return LIM_FAILED;

    /* sin(x-a)/(x-a) → 1 */
    if (num_e->type == NODE_FUNC && num_e->d.func.id == FN_SIN &&
        is_var_minus_const(num_e->d.func.arg, var, &a_num) &&
        fabsf(a_num - a_den) < NEAR_ZERO) {
        *result = 1.0f;
        steps_addf("Special: lim sin(%c-%.4g)/(%c-%.4g) = 1",
                   var, (double)a_den, var, (double)a_den);
        return LIM_OK;
    }

    /* tan(x-a)/(x-a) → 1 */
    if (num_e->type == NODE_FUNC && num_e->d.func.id == FN_TAN &&
        is_var_minus_const(num_e->d.func.arg, var, &a_num) &&
        fabsf(a_num - a_den) < NEAR_ZERO) {
        *result = 1.0f;
        steps_addf("Special: lim tan(%c-%.4g)/(%c-%.4g) = 1",
                   var, (double)a_den, var, (double)a_den);
        return LIM_OK;
    }

    /* (e^(x-a)-1)/(x-a) → 1 */
    if (num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB &&
        is_num(num_e->d.binop.right, 1)) {
        const ast_node_t *exp_part = num_e->d.binop.left;
        /* Check for exp(x-a) form */
        if (exp_part->type == NODE_FUNC && exp_part->d.func.id == FN_EXP &&
            is_var_minus_const(exp_part->d.func.arg, var, &a_num) &&
            fabsf(a_num - a_den) < NEAR_ZERO) {
            *result = 1.0f;
            steps_addf("Special: lim (e^(%c-%.4g)-1)/(%c-%.4g) = 1",
                       var, (double)a_den, var, (double)a_den);
            return LIM_OK;
        }
        /* Check for e^(x-a) as CONST_E^(x-a) */
        if (exp_part->type == NODE_BINOP && exp_part->d.binop.op == OP_POW &&
            exp_part->d.binop.left->type == NODE_CONSTANT &&
            exp_part->d.binop.left->d.constant == CONST_E &&
            is_var_minus_const(exp_part->d.binop.right, var, &a_num) &&
            fabsf(a_num - a_den) < NEAR_ZERO) {
            *result = 1.0f;
            steps_addf("Special: lim (e^(%c-%.4g)-1)/(%c-%.4g) = 1",
                       var, (double)a_den, var, (double)a_den);
            return LIM_OK;
        }
    }

    /* (1-cos(x-a))/(x-a) → 0 */
    if (num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB &&
        is_num(num_e->d.binop.left, 1) &&
        num_e->d.binop.right->type == NODE_FUNC &&
        num_e->d.binop.right->d.func.id == FN_COS &&
        is_var_minus_const(num_e->d.binop.right->d.func.arg, var, &a_num) &&
        fabsf(a_num - a_den) < NEAR_ZERO) {
        *result = 0.0f;
        steps_addf("Special: lim (1-cos(%c-%.4g))/(%c-%.4g) = 0",
                   var, (double)a_den, var, (double)a_den);
        return LIM_OK;
    }

    /* (1-cos(x-a))/(x-a)^2 → 1/2 */
    if (den_e->type == NODE_BINOP && den_e->d.binop.op == OP_POW &&
        is_num(den_e->d.binop.right, 2) &&
        is_var_minus_const(den_e->d.binop.left, var, &a_den) &&
        num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB &&
        is_num(num_e->d.binop.left, 1) &&
        num_e->d.binop.right->type == NODE_FUNC &&
        num_e->d.binop.right->d.func.id == FN_COS &&
        is_var_minus_const(num_e->d.binop.right->d.func.arg, var, &a_num) &&
        fabsf(a_num - a_den) < NEAR_ZERO &&
        fabsf(a_den - pt) < NEAR_ZERO) {
        *result = 0.5f;
        steps_addf("Special: lim (1-cos(%c-%.4g))/(%c-%.4g)^2 = 1/2",
                   var, (double)a_den, var, (double)a_den);
        return LIM_OK;
    }

    return LIM_FAILED;
}
#endif

/* ── 4b. Factor cancellation for 0/0 polynomial quotients ───── */

#if CE_LIMITS_ENABLE_FACTOR_CANCEL
static LIMIT_NOINLINE lim_status_t try_factor_cancel(const ast_node_t *expr, char var,
                                      ce_float_t pt, ce_float_t *result)
{
    poly_t num_p, den_p, divisor, q_num, r_num, q_den, r_den;
    ast_node_t *new_num, *new_den, *quot;
    ce_error_t err;
    ce_float_t val;

    /* Only for quotients */
    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return LIM_FAILED;

    /* Both must be polynomial */
    if (!algebra_ast_to_poly(expr->d.binop.left,  var, &num_p) ||
        !algebra_ast_to_poly(expr->d.binop.right, var, &den_p))
        return LIM_FAILED;

    /* Both must evaluate to 0 at the limit point (0/0 form) */
    if (fabsf(algebra_poly_eval(&num_p, pt)) > 0.01f ||
        fabsf(algebra_poly_eval(&den_p, pt)) > 0.01f)
        return LIM_FAILED;

    steps_addf("Factor cancel: both 0 at %c=%.4g", var, (double)pt);

    /* Build divisor (x - pt) */
    memset(&divisor, 0, sizeof(divisor));
    divisor.degree   = 1;
    divisor.coeff[0] = -pt;
    divisor.coeff[1] = 1;

    /* Divide out (x - pt) from numerator and denominator */
    if (!algebra_poly_divide(&num_p, &divisor, &q_num, &r_num) ||
        !algebra_poly_divide(&den_p, &divisor, &q_den, &r_den))
        return LIM_FAILED;

    /* Check remainders are ~0 (factor divides evenly) */
    if (fabsf(r_num.coeff[0]) > 0.01f || fabsf(r_den.coeff[0]) > 0.01f) {
        steps_addf("(%c-%.4g) is not a common factor", var, (double)pt);
        return LIM_FAILED;
    }

    steps_addf("Cancelled (%c-%.4g) from num & den", var, (double)pt);

    /* Build simplified quotient and evaluate */
    new_num = algebra_poly_to_ast(&q_num, var);
    new_den = algebra_poly_to_ast(&q_den, var);

    if (!new_num || !new_den) {
        ast_free(new_num);
        ast_free(new_den);
        return LIM_FAILED;
    }

    quot = ast_binop(OP_DIV, new_num, new_den);
    val = eval_at(quot, var, pt, &err);
    ast_free(quot);

    if (err == CE_OK && ce_isfinite(val)) {
        *result = val;
        steps_addf("After cancel: f(%.4g) = %.6g", (double)pt, (double)val);
        return LIM_OK;
    }

    return LIM_FAILED;
}
#endif

/* ── 4c. Rationalization for sqrt limits ──────────────────────── */

/*
 * Detects quotients where numerator is (sqrt(f(x)) - g(x)) or
 * (g(x) - sqrt(f(x))). Multiplies by conjugate to resolve 0/0.
 * Works for patterns like (sqrt(x+1)-1)/x -> 1/2 as x->0.
 */
#if CE_LIMITS_ENABLE_RATIONALIZE
static LIMIT_NOINLINE lim_status_t try_rationalize(const ast_node_t *expr, char var,
                                    ce_float_t pt, int8_t dir,
                                    ce_float_t *result)
{
    const ast_node_t *num_e, *den_e;
    const ast_node_t *sqrt_part = NULL;
    const ast_node_t *other_part = NULL;
    bool sqrt_first = false;
    ast_node_t *conj_num, *conj_den, *new_expr;
    ce_error_t err;
    ce_float_t val;
    ce_float_t probe_h = (dir < 0) ? -1e-5f : 1e-5f;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return LIM_FAILED;

    num_e = expr->d.binop.left;
    den_e = expr->d.binop.right;

    /* Check if numerator is: sqrt(f) - g  or  g - sqrt(f) */
    if (num_e->type == NODE_BINOP && num_e->d.binop.op == OP_SUB) {
        if (num_e->d.binop.left->type == NODE_FUNC &&
            num_e->d.binop.left->d.func.id == FN_SQRT) {
            sqrt_part = num_e->d.binop.left;
            other_part = num_e->d.binop.right;
            sqrt_first = true;
        } else if (num_e->d.binop.right->type == NODE_FUNC &&
                   num_e->d.binop.right->d.func.id == FN_SQRT) {
            sqrt_part = num_e->d.binop.right;
            other_part = num_e->d.binop.left;
            sqrt_first = false;
        }
    }

    if (!sqrt_part) return LIM_FAILED;

    /* Verify it's a 0/0 form at the limit point */
    {
        ce_float_t nv, dv;
        nv = eval_at(num_e, var, pt + probe_h, &err);
        if (err != CE_OK) return LIM_FAILED;
        dv = eval_at(den_e, var, pt + probe_h, &err);
        if (err != CE_OK) return LIM_FAILED;
        if (!near_zero(nv) || !near_zero(dv)) return LIM_FAILED;
    }

    steps_addf("Rationalize: multiply by conjugate");

    /*
     * If numerator = sqrt(f) - g, conjugate = sqrt(f) + g
     * New numerator = (sqrt(f)-g)(sqrt(f)+g) = f - g^2
     * New denominator = den * (sqrt(f) + g)
     */
    {
        ast_node_t *f_inner = ast_clone(sqrt_part->d.func.arg);
        ast_node_t *g_clone = ast_clone(other_part);
        ast_node_t *g_sq = ast_binop(OP_POW, ast_clone(other_part), ast_number(2));

        /* Rationalized numerator: f - g^2 (if sqrt_first)
         *                     or: g^2 - f (if !sqrt_first) */
        if (sqrt_first)
            conj_num = ast_binop(OP_SUB, f_inner, g_sq);
        else
            conj_num = ast_binop(OP_SUB, g_sq, f_inner);

        /* Conjugate denominator: den * (sqrt(f) + g) */
        {
            ast_node_t *sqrt_clone = ast_func(FN_SQRT,
                                              ast_clone(sqrt_part->d.func.arg));
            ast_node_t *conjugate = ast_binop(OP_ADD, sqrt_clone, g_clone);
            conj_den = ast_binop(OP_MUL, ast_clone(den_e), conjugate);
        }
    }

    conj_num = ast_simplify(conj_num);
    conj_den = ast_simplify(conj_den);

    new_expr = ast_binop(OP_DIV, conj_num, conj_den);

    /* Try direct substitution on the rationalized form */
    val = eval_at(new_expr, var, pt, &err);
    if (err != CE_OK || !ce_isfinite(val)) {
        /* Try slightly offset */
        val = eval_at(new_expr, var, pt + 1e-6f, &err);
    }
    ast_free(new_expr);

    if (err == CE_OK && ce_isfinite(val)) {
        *result = val;
        steps_addf("After rationalize: = %.6g", (double)val);
        return LIM_OK;
    }

    return LIM_FAILED;
}
#endif

/* ── 4d. Progressive probing helpers for 0*inf detection ─────── */

static bool tends_to_zero(const ast_node_t *expr, char var,
                          ce_float_t pt, int8_t dir)
{
    static const ce_float_t probes[] = { 1e-2f, 1e-4f, 1e-6f, 1e-8f };
    int i;
    int n_probes = (int)(sizeof(probes) / sizeof(probes[0]));
    ce_float_t h = (dir < 0) ? -1.0f : 1.0f;
    ce_float_t prev = 1e30f;
    /* Check that |f| is shrinking toward 0 as we approach the point */
    for (i = 0; i < n_probes; i++) {
        ce_error_t err;
        ce_float_t v = eval_at(expr, var, pt + h * probes[i], &err);
        if (err != CE_OK || !ce_isfinite(v)) return false;
        if (fabsf(v) > 1.0f) return false;
        if (i > 0 && fabsf(v) >= fabsf(prev)) return false;  /* not shrinking */
        prev = v;
    }
    return fabsf(prev) <= NEAR_ZERO;
}

static bool tends_to_inf(const ast_node_t *expr, char var,
                         ce_float_t pt, int8_t dir)
{
    static const ce_float_t probes[] = { 1e-2f, 1e-4f, 1e-6f };
    int i;
    ce_float_t h = (dir < 0) ? -1.0f : 1.0f;
    ce_float_t prev = 0;
    /* Check that |f| is growing as we approach the point */
    for (i = 0; i < 3; i++) {
        ce_error_t err;
        ce_float_t v = eval_at(expr, var, pt + h * probes[i], &err);
        if (err != CE_OK) return false;
        if (!ce_isfinite(v)) continue;  /* inf/nan counts as huge */
        if (i > 0 && fabsf(v) <= fabsf(prev)) return false;  /* not growing */
        prev = v;
    }
    /* Must be at least moderately large at the closest probe */
    return fabsf(prev) > 3.0f;
}

/*
 * Detect 0 * infinity form: rewrite f*g where f→0, g→inf.
 * Rewrites as f/(1/g) and directly applies L'Hopital (since we already
 * know it's 0/0 form), bypassing the probe-based classify_quotient
 * which can fail to detect the form at moderate probe distances.
 */
#if CE_LIMITS_ENABLE_ZERO_TIMES_INF
static LIMIT_NOINLINE lim_status_t try_zero_times_inf(const ast_node_t *expr, char var,
                                       ce_float_t pt, int8_t dir,
                                       ce_float_t *result, int depth)
{
    const ast_node_t *zero_part, *inf_part;
    ast_node_t *recip;
    lim_status_t st;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_MUL)
        return LIM_FAILED;

    zero_part = NULL;
    inf_part = NULL;

    /* Check f→0, g→±inf */
    if (tends_to_zero(expr->d.binop.left, var, pt, dir) &&
        tends_to_inf(expr->d.binop.right, var, pt, dir)) {
        zero_part = expr->d.binop.left;
        inf_part  = expr->d.binop.right;
    }
    /* Check g→0, f→±inf */
    else if (tends_to_zero(expr->d.binop.right, var, pt, dir) &&
             tends_to_inf(expr->d.binop.left, var, pt, dir)) {
        zero_part = expr->d.binop.right;
        inf_part  = expr->d.binop.left;
    }

    if (!zero_part) return LIM_FAILED;

    /* Rewrite as inf_part / (1/zero_part) → inf/inf form.
     * Apply L'Hopital directly since we know the form. */
    steps_addf("0*inf form: rewrite as g/(1/f) (inf/inf)");
    recip = ast_binop(OP_DIV, ast_number(1), ast_clone(zero_part));
    st = try_lhopital(inf_part, recip, var, pt, dir, result, depth);
    ast_free(recip);
    return st;
}
#endif

/* ── 5. Limits at Infinity ──────────────────────────────────── */

static LIMIT_NOINLINE lim_status_t try_infinity(const ast_node_t *expr, char var,
                                 int8_t sign, ce_float_t *result)
{
    ce_error_t e1, e2, e3;
    ce_float_t x1, x2, x3, v1, v2, v3;

    x1 = sign * 1e3f;
    x2 = sign * 1e5f;
    x3 = sign * 1e7f;

    v1 = eval_at(expr, var, x1, &e1);
    v2 = eval_at(expr, var, x2, &e2);
    v3 = eval_at(expr, var, x3, &e3);

    if (e1 != CE_OK || e2 != CE_OK || e3 != CE_OK) return LIM_FAILED;

    /* Check convergence to finite value */
    if (ce_isfinite(v2) && ce_isfinite(v3)) {
        ce_float_t diff = fabsf(v2 - v3);
        ce_float_t scale = 1 + fabsf(v3);
        if (diff < CONVERGE_TOL * scale) {
            *result = v3;
            steps_addf("f(%.0g)=%.4g, f(%.0g)=%.4g",
                       (double)x2, (double)v2, (double)x3, (double)v3);
            steps_addf("Converges to %.6g", (double)v3);
            return LIM_OK;
        }
    }

    /* Divergence to +/-infinity */
    if (v3 > HUGE_THRESH) {
        steps_addf("f(%.0g)=%.4g -> +inf", (double)x3, (double)v3);
        return LIM_POS_INF;
    }
    if (v3 < -HUGE_THRESH) {
        steps_addf("f(%.0g)=%.4g -> -inf", (double)x3, (double)v3);
        return LIM_NEG_INF;
    }

    /* Oscillation check */
    if (ce_isfinite(v1) && ce_isfinite(v2) && ce_isfinite(v3)) {
        ce_float_t d12 = fabsf(v1 - v2);
        ce_float_t d23 = fabsf(v2 - v3);
        if (d12 > 0.1f && d23 > 0.1f) {
            steps_addf("Values oscillate: DNE");
            return LIM_DNE;
        }
    }

    return LIM_FAILED;
}

/* ── 6. Numerical Approach from One Side ────────────────────── */

static lim_status_t approach_side(const ast_node_t *expr, char var,
                                  ce_float_t pt, int8_t side,
                                  ce_float_t *result)
{
    static const ce_float_t hs[] = {
        0.01f, 0.001f, 1e-4f, 1e-5f, 1e-6f
    };
    ce_float_t vals[5];
    ce_error_t err;
    int i, valid = 0;

    for (i = 0; i < 5; i++) {
        ce_float_t x = pt + side * hs[i];
        vals[i] = eval_at(expr, var, x, &err);
        if (err != CE_OK || !ce_isfinite(vals[i])) break;
        valid++;

        if (is_huge(vals[i])) {
            steps_addf("  %c%+.1g: f=%.4g (diverging)",
                       var, (double)(side * hs[i]), (double)vals[i]);
            return (vals[i] > 0) ? LIM_POS_INF : LIM_NEG_INF;
        }
    }

    if (valid >= 3) {
        /* Check convergence of last two */
        ce_float_t v1 = vals[valid - 2];
        ce_float_t v2 = vals[valid - 1];
        ce_float_t d  = fabsf(v1 - v2);
        if (d < CONVERGE_TOL * (1 + fabsf(v2))) {
            *result = v2;
            steps_addf("  %s: f -> %.6g",
                       side > 0 ? "from right" : "from left",
                       (double)v2);
            return LIM_OK;
        }

        /* Check for divergence: values growing rapidly in magnitude */
        if (fabsf(v2) > 10.0f * fabsf(vals[0]) &&
            fabsf(v2) > fabsf(v1) &&
            fabsf(v1) > fabsf(vals[valid - 3])) {
            steps_addf("  %s: f diverging (%.4g -> %.4g)",
                       side > 0 ? "from right" : "from left",
                       (double)vals[0], (double)v2);
            return (v2 > 0) ? LIM_POS_INF : LIM_NEG_INF;
        }
    }

    return LIM_FAILED;
}

static LIMIT_NOINLINE lim_status_t try_numerical(const ast_node_t *expr, char var,
                                  ce_float_t pt, int8_t dir,
                                  ce_float_t *result)
{
    steps_addf("Numerical approach:");

    /* Two-sided: must agree from both sides */
    if (dir == 0) {
        ce_float_t lval, rval;
        lim_status_t ls = approach_side(expr, var, pt, -1, &lval);
        lim_status_t rs = approach_side(expr, var, pt,  1, &rval);

        if (ls == LIM_OK && rs == LIM_OK) {
            if (fabsf(lval - rval) < CONVERGE_TOL * (1 + fabsf(lval))) {
                *result = (lval + rval) / 2;
                steps_addf("Both sides -> %.6g", (double)*result);
                return LIM_OK;
            }
            steps_addf("Left=%.4g, Right=%.4g => DNE",
                       (double)lval, (double)rval);
            return LIM_DNE;
        }

        /* Both diverge same way */
        if (ls == LIM_POS_INF && rs == LIM_POS_INF) return LIM_POS_INF;
        if (ls == LIM_NEG_INF && rs == LIM_NEG_INF) return LIM_NEG_INF;
        if ((ls == LIM_POS_INF || ls == LIM_NEG_INF) &&
            (rs == LIM_POS_INF || rs == LIM_NEG_INF)) {
            steps_addf("Left/right disagree => DNE");
            return LIM_DNE;
        }

        return LIM_FAILED;
    }

    /* One-sided */
    return approach_side(expr, var, pt, dir, result);
}

/* ── 7. Squeeze theorem hint ────────────────────────────────── */

static void squeeze_hint(const ast_node_t *expr)
{
    /* If expr involves sin or cos bounded by [-1,1], note it */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        const ast_node_t *L = expr->d.binop.left;
        const ast_node_t *R = expr->d.binop.right;

        bool l_trig = (L->type == NODE_FUNC &&
                       (L->d.func.id == FN_SIN || L->d.func.id == FN_COS));
        bool r_trig = (R->type == NODE_FUNC &&
                       (R->d.func.id == FN_SIN || R->d.func.id == FN_COS));

        if (l_trig || r_trig)
            steps_addf("Hint: |sin|,|cos| <= 1 (squeeze)");
    }
}

/* ── Core recursive limit computation ───────────────────────── */

static lim_status_t limit_inner(const ast_node_t *expr, char var,
                                ce_float_t pt, int8_t dir,
                                ce_float_t *result, int depth)
{
    lim_status_t status;

    if (depth > MAX_LHOPITAL + 3) return LIM_FAILED;

    /* ── Infinity limit ─────────────────────────────────── */
    if (pt > HUGE_THRESH || pt < -HUGE_THRESH) {
        int8_t sign = (pt > 0) ? 1 : -1;
        return try_infinity(expr, var, sign, result);
    }

    /* ── Special limits (before direct sub to avoid 0/0 eval) ── */
    status = try_special(expr, var, pt, result);
    if (status != LIM_FAILED) return status;

    /* ── Direct substitution ────────────────────────────── */
    status = try_direct_sub(expr, var, pt, result);
    if (status == LIM_OK) return LIM_OK;

    /* ── Special shifted limits: sin(x-a)/(x-a) etc. ──── */
#if CE_LIMITS_ENABLE_SHIFTED_SPECIAL
    status = try_special_shifted(expr, var, pt, result);
    if (status != LIM_FAILED) return status;
#endif

    /* ── Factor cancellation (polynomial 0/0) ──────────── */
#if CE_LIMITS_ENABLE_FACTOR_CANCEL
    status = try_factor_cancel(expr, var, pt, result);
    if (status != LIM_FAILED) return status;
#endif

    /* ── Rationalization (sqrt 0/0 forms) ──────────────── */
#if CE_LIMITS_ENABLE_RATIONALIZE
    status = try_rationalize(expr, var, pt, dir, result);
    if (status != LIM_FAILED) return status;
#endif

    /* ── Squeeze theorem hint ───────────────────────────── */
    squeeze_hint(expr);

    /* ── L'Hopital for quotients ────────────────────────── */
#if CE_LIMITS_ENABLE_LHOPITAL
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_DIV) {
        indet_form_t form = classify_quotient(
            expr->d.binop.left, expr->d.binop.right,
            var, pt, dir);

        if (form == FORM_0_OVER_0 || form == FORM_INF_OVER_INF) {
            steps_addf("Indeterminate: %s",
                       form == FORM_0_OVER_0 ? "0/0" : "inf/inf");
            status = try_lhopital(expr->d.binop.left, expr->d.binop.right,
                                  var, pt, dir, result, depth);
            if (status != LIM_FAILED) return status;
        }
    }
#endif

    /* ── Limit of sum/diff: split ───────────────────────── */
#if CE_LIMITS_ENABLE_SPLIT_RULES
    if (expr->type == NODE_BINOP &&
        (expr->d.binop.op == OP_ADD || expr->d.binop.op == OP_SUB)) {
        ce_float_t lval, rval;
        lim_status_t ls = limit_inner(expr->d.binop.left, var, pt, dir,
                                      &lval, depth + 1);
        lim_status_t rs = limit_inner(expr->d.binop.right, var, pt, dir,
                                      &rval, depth + 1);
        if (ls == LIM_OK && rs == LIM_OK) {
            *result = (expr->d.binop.op == OP_ADD) ?
                      lval + rval : lval - rval;
            return LIM_OK;
        }
    }
#endif

    /* ── 0 * infinity form ─────────────────────────────── */
#if CE_LIMITS_ENABLE_ZERO_TIMES_INF
    status = try_zero_times_inf(expr, var, pt, dir, result, depth);
    if (status != LIM_FAILED) return status;
#endif

    /* ── Limit of product: split if both finite ─────────── */
#if CE_LIMITS_ENABLE_SPLIT_RULES
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        ce_float_t lval, rval;
        lim_status_t ls = limit_inner(expr->d.binop.left, var, pt, dir,
                                      &lval, depth + 1);
        lim_status_t rs = limit_inner(expr->d.binop.right, var, pt, dir,
                                      &rval, depth + 1);
        if (ls == LIM_OK && rs == LIM_OK) {
            *result = lval * rval;
            return LIM_OK;
        }
    }
#endif

    /* ── Numerical fallback ─────────────────────────────── */
    return try_numerical(expr, var, pt, dir, result);
}

static LIMIT_NOINLINE bool limit_point(const ast_node_t *point, ce_float_t *value)
{
    eval_ctx_t ctx;
    eval_init(&ctx);
    *value = eval_expr(&ctx, point);
    return eval_error(&ctx) == CE_OK;
}

/* ── Public API ─────────────────────────────────────────────── */

/*
 * compute_limit — Top-level entry point for limit computation.
 * Clears and rebuilds the step buffer from scratch.
 * Callers who need to preserve existing steps should save/restore
 * via steps_count() / steps_restore() around this call.
 */
lim_status_t compute_limit(const ast_node_t *expr, char var,
                           const ast_node_t *point, int8_t direction,
                           ce_float_t *result)
{
    ce_float_t pt_val;
    char buf[80], pt_buf[80];
    bool refused = g_eval_refused;
    lim_status_t status;

    *result = 0;

    /* Evaluate the limit point */
    if (!limit_point(point, &pt_val)) return LIM_FAILED;

    /* Log what we're computing */
    ast_to_str(expr, buf, sizeof(buf));
    ast_to_str(point, pt_buf, sizeof(pt_buf));

    if (direction > 0)
        steps_addf("lim %c->%s+ of %s", var, pt_buf, buf);
    else if (direction < 0)
        steps_addf("lim %c->%s- of %s", var, pt_buf, buf);
    else
        steps_addf("lim %c->%s of %s", var, pt_buf, buf);

    status = limit_inner(expr, var, pt_val, direction, result, 0);
    /* Every evaluation in this file treats an error as "this method does
     * not apply" (or "could not determine") and takes values only from
     * successful evaluations, so a recursion refusal in a probe here is handled
     * (calcengine.h). The point itself failing is reported by the caller. */
    g_eval_refused = refused;
    return status;
}

/* ── Continuity Analysis ───────────────────────────────────── */

/*
 * limits_continuity — Top-level entry point for continuity analysis.
 * Clears and rebuilds the step buffer from scratch.
 */
static LIMIT_NOINLINE continuity_t continuity_inner(const ast_node_t *expr, char var,
                                                    const ast_node_t *point);

continuity_t limits_continuity(const ast_node_t *expr, char var,
                               const ast_node_t *point)
{
    /* As in compute_limit(): a recursion refusal here ends in a classification
     * from successful evaluations or in CONT_FAILED ("could not
     * determine"), never in a value made from the error (calcengine.h). */
    bool refused = g_eval_refused;
    continuity_t c = continuity_inner(expr, var, point);
    g_eval_refused = refused;
    return c;
}

static LIMIT_NOINLINE continuity_t continuity_inner(const ast_node_t *expr, char var,
                                                    const ast_node_t *point)
{
    ce_float_t pt_val, f_a, left_val, right_val;
    ce_error_t err;
    lim_status_t ls, rs;
    bool f_a_ok;
    char buf[80], pt_buf[80];

    /* Evaluate the point */
    if (!limit_point(point, &pt_val)) return CONT_FAILED;

    ast_to_str(expr, buf, sizeof(buf));
    ast_to_str(point, pt_buf, sizeof(pt_buf));
    steps_addf("Continuity of %s at %c=%s", buf, var, pt_buf);

    /* Evaluate f(a) directly */
    f_a = eval_at(expr, var, pt_val, &err);
    if (err == CE_ERR_DEPTH) return CONT_FAILED;
    f_a_ok = (err == CE_OK && ce_isfinite(f_a));
    if (f_a_ok)
        steps_addf("f(%s) = %.6g", pt_buf, (double)f_a);
    else
        steps_addf("f(%s) undefined", pt_buf);

    /* Compute left and right limits using limit_inner directly */
    ls = limit_inner(expr, var, pt_val, -1, &left_val, 0);
    rs = limit_inner(expr, var, pt_val,  1, &right_val, 0);

    /* Both sides diverge to infinity */
    if ((ls == LIM_POS_INF || ls == LIM_NEG_INF) ||
        (rs == LIM_POS_INF || rs == LIM_NEG_INF)) {
        steps_addf("Infinite discontinuity");
        return CONT_INFINITE;
    }

    /* DNE on either side */
    if (ls == LIM_DNE || rs == LIM_DNE) {
        steps_addf("Essential discontinuity (DNE)");
        return CONT_ESSENTIAL;
    }

    /* Both sides failed */
    if (ls == LIM_FAILED || rs == LIM_FAILED) {
        return CONT_FAILED;
    }

    /* Both limits are finite — do they agree? */
    if (ls == LIM_OK && rs == LIM_OK) {
        if (fabsf(left_val - right_val) > CONVERGE_TOL * (1 + fabsf(left_val))) {
            steps_addf("Jump: left=%.6g, right=%.6g",
                       (double)left_val, (double)right_val);
            return CONT_JUMP;
        }

        /* Limits agree — does f(a) match? */
        ce_float_t lim_val = (left_val + right_val) / 2;
        if (f_a_ok && fabsf(f_a - lim_val) < CONVERGE_TOL * (1 + fabsf(lim_val))) {
            steps_addf("Continuous: f(a) = lim = %.6g", (double)lim_val);
            return CONT_CONTINUOUS;
        }

        steps_addf("Removable: lim=%.6g but f(a)%s",
                   (double)lim_val, f_a_ok ? " differs" : " undefined");
        return CONT_REMOVABLE;
    }

    return CONT_FAILED;
}
