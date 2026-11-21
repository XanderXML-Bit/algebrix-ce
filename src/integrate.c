/*
 * integrate.c -- Symbolic integration engine
 *
 * Implements:
 *   - Constant rule, power rule, sum/difference rule
 *   - Trig integrals: sin, cos, tan, sec, csc, cot, sec^2, csc^2
 *   - Exponential / logarithmic: e^x, a^x, 1/x -> ln|x|
 *   - Simple u-substitution for linear inner functions (ax+b)
 *   - Integration by parts for recognizable patterns
 *   - Step-by-step recording via steps module
 *   - Definite integral evaluation via Fundamental Theorem of Calculus
 */
#include "integrate.h"
#include "steps.h"
#include "eval.h"
#include "deriv.h"
#include "limits.h"
#include "polar.h"
#include "memory.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>


#ifndef CE_INTEG_USE_SIMPLIFY
#define CE_INTEG_USE_SIMPLIFY 1
#endif

#ifndef CE_INTEG_ENABLE_NONLINEAR_USUB
#define CE_INTEG_ENABLE_NONLINEAR_USUB 1
#endif

#ifndef CE_INTEG_ENABLE_TRIG_SUB
#define CE_INTEG_ENABLE_TRIG_SUB 1
#endif

#ifndef CE_INTEG_ENABLE_PARTIAL_FRAC
#define CE_INTEG_ENABLE_PARTIAL_FRAC 1
#endif

#ifndef CE_INTEG_ENABLE_CYCLIC_PARTS
#define CE_INTEG_ENABLE_CYCLIC_PARTS 1
#endif

#ifndef CE_INTEG_ENABLE_PARTS
#define CE_INTEG_ENABLE_PARTS 1
#endif

#ifndef CE_INTEG_ENABLE_STRATEGY_API
#define CE_INTEG_ENABLE_STRATEGY_API 1
#endif

#ifndef CE_INTEG_ENABLE_STRATEGY_REPORT
#define CE_INTEG_ENABLE_STRATEGY_REPORT CE_INTEG_ENABLE_STRATEGY_API
#endif

#ifndef CE_INTEG_ENABLE_ODE
#define CE_INTEG_ENABLE_ODE 1
#endif

#ifndef CE_INTEG_ENABLE_DEFINITE
#define CE_INTEG_ENABLE_DEFINITE 1
#endif

/* CE workers that cannot fit simplify.c fold the finished antiderivative
 * with the small exact pass in fold_lite.c instead (see fold_lite.h). */
#ifndef CE_INTEG_USE_FOLD_LITE
#define CE_INTEG_USE_FOLD_LITE 0
#endif

/* Keep size-sensitive probes out of line: LTO inlines single-call statics,
 * which made CECIDF's integrate_improper() larger, not smaller. */
#if defined(__GNUC__) || defined(__clang__)
#define CE_INTEG_NOINLINE __attribute__((noinline))
#else
#define CE_INTEG_NOINLINE
#endif


#if CE_INTEG_USE_SIMPLIFY
#include "simplify.h"
#define integ_final_simplify(e) ast_simplify(e)
#else
static ast_node_t *ast_simplify(ast_node_t *expr)
{
    return expr;
}
#if CE_INTEG_USE_FOLD_LITE
#include "fold_lite.h"
#define integ_final_simplify(e) ast_fold_lite(e)
#else
#define integ_final_simplify(e) (e)
#endif
#endif

/* A constant subtree equal to v (-1 for the power rule, 1 for the base of
 * a^u). The parser keeps x^(-1) as POW(x, NEG 1), and an unfolded constant
 * such as (0-1) or (2-1) stays as typed; is_num() sees only a literal, so
 * the power rule built x^(-1+1)/(-1+1) (review round 12) and a^u built
 * (2-1)^x/ln(2-1) (round 15). Literals and NEG literals are matched
 * directly. Builds with the definite path evaluate other constant subtrees
 * (they link the evaluator anyway); the others fold a copy with the
 * worker's own simplify/fold_lite pass, because calling the evaluator
 * pulled all of eval.c into CEIPRT (+12.8 KB). */
static bool integ_is_literal(const ast_node_t *e, ce_float_t v)
{
    return ast_is_num(e, v) ||
           (e && e->type == NODE_UNOP && e->d.unop.op == UOP_NEG &&
            ast_is_num(e->d.unop.operand, -v));
}

static bool integ_const_equals(const ast_node_t *e, char var, ce_float_t v)
{
    if (integ_is_literal(e, v)) return true;
    if (!e || ast_contains_var(e, var)) return false;
#if CE_INTEG_ENABLE_DEFINITE
    {
        ce_float_t x;
        return integrate_eval_at(e, var, 0, &x) && x == v;
    }
#elif CE_INTEG_USE_SIMPLIFY || CE_INTEG_USE_FOLD_LITE
    {
        ast_node_t *c = ast_clone(e);
        bool r = false;
        /* ast_tree_complete(NULL) is true, so test the clone itself. */
        if (c && ast_tree_complete(c)) {
            c = integ_final_simplify(c);
            r = c && ast_tree_complete(c) && integ_is_literal(c, v);
        }
        if (c) ast_free(c);
        return r;
    }
#else
    return false;
#endif
}

#define integ_exp_is_minus_one(e, var) integ_const_equals((e), (var), -1.0f)

/* ── Forward declarations ────────────────────────────────── */
static ast_node_t *integ(const ast_node_t *expr, char var, int depth);
#if CE_INTEG_ENABLE_NONLINEAR_USUB
static ast_node_t *try_nonlinear_usub(const ast_node_t *expr, char var, int depth);
#endif

/* Zero-initialised (.bss) rather than initialised to the default text, so
 * the 192-byte buffer is not stored in every .8xp that links this file.
 * Empty means the default reason; see integrate_last_failure_reason(). */
#define CE_INTEG_DEFAULT_FAIL_REASON "No symbolic solution found"
static char g_integrate_fail_reason[192];

static void set_integrate_fail_reason(const char *msg)
{
    if (!msg || !msg[0]) {
        g_integrate_fail_reason[0] = '\0';
        return;
    }
    strncpy(g_integrate_fail_reason, msg, sizeof(g_integrate_fail_reason) - 1);
    g_integrate_fail_reason[sizeof(g_integrate_fail_reason) - 1] = '\0';
}

/* ── Recursion bound ─────────────────────────────────────────
 * The CE runs every worker on the 4 KB OS stack, and a write past its
 * limit is an NMI reset ("RAM Cleared").  integ() is the only recursive
 * entry into the rule set.  Two things keep it inside the stack:
 *  - the linear rules (sums, negation, constant factors and denominators)
 *    recurse through integ_node(), whose frame is small; the other rules
 *    run out of line in integ_rules(), whose frame is large because LTO
 *    inlines the rule helpers into it.  A 61-term sum, the longest that
 *    fits the 127-character input line, then costs about 30 B per term;
 *  - integ() counts the calls that are open and fails cleanly past
 *    INTEG_MAX_DEPTH, as deriv.c does with DIFF_MAX_DEPTH, with the reason
 *    INTEG_TOO_DEEP_MSG.  Sums and constant-factor chains that fit the
 *    127-character line nest at most 61 calls, so they stay under the cap;
 *    a run of unary minus signs costs one character per level and can
 *    reach it, e.g. 48 signs in front of a 35-term sum.  The cap also
 *    bounds the depth of the antiderivative that the processor prints.
 * tests/cemu/test_module_flag_parity.py checks the cap with the module
 * flags. The stack high-water probe measures how much stack remains
 * on the real ROM. */
#ifndef INTEG_MAX_DEPTH
#define INTEG_MAX_DEPTH 64
#endif
/* INTEG_TOO_DEEP_MSG and INTEG_OOM_MSG are in integrate.h. */

static uint8_t g_integ_nest;
/* Set when any integ() call in the current request reached the cap,
 * including a strategy attempt (try_parts, nonlinear u-sub) that was then
 * abandoned.  A request that fails for another reason after such an attempt
 * still reports INTEG_TOO_DEEP_MSG; only the reason text is affected. */
static bool g_integ_too_deep;
static uint16_t g_integ_alloc_start;

/* Start of a public integration request: the cap has not been hit.  Each
 * entry point sets its own failure reason on every path. */
static void integ_begin(void)
{
    g_integ_too_deep = false;
    g_integ_alloc_start = mem_alloc_failures();
}

/* Text for a step that names an expression: n printed in at most size-1
 * characters (size <= 120), or "..." when n is too deep to print safely.
 * ast_print() recurses through the whole tree even once its buffer is full,
 * about 36 B of CE stack per level, so a step about an 84-deep input used
 * to take most of the 4 KB stack.  The buffer is static, so it never sits
 * in a recursive integ() frame; this is a leaf, and each caller hands the
 * text to steps_addf() before anything prints again.  Compiled out when
 * the worker records no steps. */
#ifdef CE_DISABLE_STEP_RECORDING
#define integ_text(n, size) ""
#else
#ifndef INTEG_STEP_PRINT_DEPTH
#define INTEG_STEP_PRINT_DEPTH 24
#endif
/* integ_text() relies on this order of node_type_t. */
typedef char integ_text_node_order[(NODE_NUMBER == 0 && NODE_VARIABLE == 1 &&
                                    NODE_CONSTANT == 2 && NODE_BINOP == 3 &&
                                    NODE_UNOP == 4 && NODE_FUNC == 5) ? 1 : -1];

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static const char *integ_text(const ast_node_t *n, int size)
{
    static char buf[120];
    const ast_node_t *m = n;
    int d = INTEG_STEP_PRINT_DEPTH;

    /* Depth of the leftmost path (left operands and single children):
     * left-assoc chains and unary-minus runs at the left grow it.  A chain
     * inside a right operand (x+x*x*...*x) is not counted and still prints
     * in full, as on main; with the 127-character line it is at most ~60
     * deep and left at least 740 B of stack on the real ROM. A full bounded
     * walk did not fit in CECISM. */
    /* Leaves (number, variable, constant) end the walk.  Any other node
     * kind on the leftmost path (an integral, derivative, limit, ...) is
     * named "..." outright: its own children can be as deep as anything
     * (int(int(-----x+x+...)) overflowed in review).  One in a right
     * operand still prints, like the right-operand chains above. */
    while (m && m->type > NODE_CONSTANT) {
        if (m->type > NODE_FUNC || --d < 0) return "...";
        m = m->type == NODE_BINOP ? m->d.binop.left
          : m->type == NODE_UNOP  ? m->d.unop.operand
          :                         m->d.func.arg;
    }
    ast_to_str(n, buf, size);
    return buf;
}
#endif

/* When the node pool runs out while a rule builds its result, ast_binop()
 * keeps a NULL operand.  The final simplify/fold dereferences operands, and
 * a hole would print as "(0+)" or "cos()", so the finished antiderivative is
 * checked (ast_tree_complete) before and after the final pass; an incomplete
 * tree is an out-of-memory error.  Takes ownership of `result`. */
static ast_node_t *integ_finish(ast_node_t *result)
{
    if (ast_tree_complete(result)) {
        result = integ_final_simplify(result);
        if (ast_tree_complete(result)) return result;
    }
    ast_free(result);
    set_integrate_fail_reason(INTEG_OOM_MSG);
    return NULL;
}

/* ── Helpers ─────────────────────────────────────────────── */
/* ast_is_num() now lives in ast.c as public API. */
#define is_num(n, v)  ast_is_num((n), (v))

/* Check if node is the integration variable */
static bool is_var(const ast_node_t *n, char var)
{
    return n && n->type == NODE_VARIABLE && n->d.variable == var;
}

/* Check if expression is constant w.r.t. var */
static bool is_const_wrt(const ast_node_t *n, char var)
{
    return !ast_contains_var(n, var);
}

static bool eval_const_numeric(const ast_node_t *expr, char var, ce_float_t *out)
{
    eval_ctx_t ctx;
    if (!expr || !out || ast_contains_var(expr, var))
        return false;
    eval_init(&ctx);
    *out = eval_expr(&ctx, expr);
    if (eval_error(&ctx) != CE_OK || !ce_isfinite(*out))
        return false;
    return true;
}

static void split_const_multiple(const ast_node_t *expr, char var,
                                 ce_float_t *coeff, const ast_node_t **core)
{
    ce_float_t cval;
    *coeff = 1.0f;
    *core = expr;
    if (!expr) return;

    if (expr->type == NODE_UNOP && expr->d.unop.op == UOP_NEG) {
        *coeff = -1.0f;
        *core = expr->d.unop.operand;
        return;
    }

    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        if (eval_const_numeric(expr->d.binop.left, var, &cval) &&
            ast_contains_var(expr->d.binop.right, var)) {
            *coeff = cval;
            *core = expr->d.binop.right;
            return;
        }
        if (eval_const_numeric(expr->d.binop.right, var, &cval) &&
            ast_contains_var(expr->d.binop.left, var)) {
            *coeff = cval;
            *core = expr->d.binop.left;
            return;
        }
    }
}

/* Copy expr while replacing every instance of `var` with `sub_expr`. */
static ast_node_t *substitute_var(const ast_node_t *expr, char var,
                                  const ast_node_t *sub_expr)
{
    if (!expr) return NULL;

    switch (expr->type) {
    case NODE_NUMBER:
        return ast_number(expr->d.number);
    case NODE_CONSTANT:
        return ast_constant(expr->d.constant);
    case NODE_VARIABLE:
        if (expr->d.variable == var)
            return ast_clone(sub_expr);
        return ast_variable(expr->d.variable);
    case NODE_BINOP:
        return ast_binop(expr->d.binop.op,
            substitute_var(expr->d.binop.left, var, sub_expr),
            substitute_var(expr->d.binop.right, var, sub_expr));
    case NODE_UNOP:
        return ast_unop(expr->d.unop.op,
            substitute_var(expr->d.unop.operand, var, sub_expr));
    case NODE_FUNC:
        return ast_func(expr->d.func.id,
            substitute_var(expr->d.func.arg, var, sub_expr));
    default:
        return ast_clone(expr);
    }
}

/* Copy expr while replacing every exact subtree equal to target by repl_var. */
static ast_node_t *replace_subexpr_with_var(const ast_node_t *expr,
                                            const ast_node_t *target,
                                            char repl_var)
{
    if (!expr) return NULL;
    if (target && ast_equal(expr, target))
        return ast_variable(repl_var);

    switch (expr->type) {
    case NODE_NUMBER:
        return ast_number(expr->d.number);
    case NODE_CONSTANT:
        return ast_constant(expr->d.constant);
    case NODE_VARIABLE:
        return ast_variable(expr->d.variable);
    case NODE_BINOP:
        return ast_binop(expr->d.binop.op,
            replace_subexpr_with_var(expr->d.binop.left, target, repl_var),
            replace_subexpr_with_var(expr->d.binop.right, target, repl_var));
    case NODE_UNOP:
        return ast_unop(expr->d.unop.op,
            replace_subexpr_with_var(expr->d.unop.operand, target, repl_var));
    case NODE_FUNC:
        return ast_func(expr->d.func.id,
            replace_subexpr_with_var(expr->d.func.arg, target, repl_var));
    default:
        return ast_clone(expr);
    }
}

static bool match_derivative_multiple(const ast_node_t *factor,
                                      const ast_node_t *inner,
                                      char var, ce_float_t *k_out)
{
    ast_node_t *du_raw, *du, *fcopy;
    const ast_node_t *core_f, *core_du;
    ce_float_t cf, cd;

    if (!factor || !inner || !k_out) return false;
    du_raw = deriv_diff_quiet(inner, var);
    if (!du_raw) return false;
    du = ast_simplify(du_raw);
    if (!du) return false;
    fcopy = ast_simplify(ast_clone(factor));
    if (!fcopy) { ast_free(du); return false; }

    split_const_multiple(fcopy, var, &cf, &core_f);
    split_const_multiple(du, var, &cd, &core_du);
    if (core_f && core_du && ast_equal(core_f, core_du) && fabsf(cd) > 1e-12f) {
        *k_out = cf / cd;
        ast_free(fcopy);
        ast_free(du);
        return ce_isfinite(*k_out) && fabsf(*k_out) > 1e-12f;
    }

    if (ast_equal(fcopy, du)) {
        *k_out = 1.0f;
        ast_free(fcopy);
        ast_free(du);
        return true;
    }

    ast_free(fcopy);
    ast_free(du);
    return false;
}

/*
 * Check if expression is a linear function of var: a*var + b
 * Returns true and sets *a_out, *b_out if so.
 * Only handles simple forms: var, c*var, var+c, c*var+d, c+var
 */
static bool is_linear(const ast_node_t *n, char var,
                       const ast_node_t **a_out, const ast_node_t **b_out)
{
    /* NON-pool-allocated static sentinel nodes (safe to pass through ast_clone) */
    static ast_node_t one_node     = { NODE_NUMBER, { .number = 1.0f } };
    static ast_node_t neg_one_node = { NODE_NUMBER, { .number = -1.0f } };
    static ast_node_t zero_node    = { NODE_NUMBER, { .number = 0.0f } };

    /*
     * NOTE: These static nodes are NOT pool-allocated.  Callers must
     * never call ast_free() on the pointers returned through a_out/b_out
     * when they point to these sentinels.  All existing callers only
     * pass them to ast_clone(), which is safe because it copies the
     * node data without touching pool bookkeeping on the source.
     * The sentinel values (1, -1, 0) are never modified.
     */

    if (!n) return false;

    /* Just the variable: 1*var + 0 */
    if (is_var(n, var)) {
        *a_out = &one_node;
        *b_out = &zero_node;
        return true;
    }

    /* Negated variable: -var = (-1)*var + 0 */
    if (n->type == NODE_UNOP && n->d.unop.op == UOP_NEG &&
        is_var(n->d.unop.operand, var)) {
        *a_out = &neg_one_node;
        *b_out = &zero_node;
        return true;
    }

    /* c * var or var * c */
    if (n->type == NODE_BINOP && n->d.binop.op == OP_MUL) {
        if (is_var(n->d.binop.right, var) &&
            is_const_wrt(n->d.binop.left, var)) {
            *a_out = n->d.binop.left;
            *b_out = &zero_node;
            return true;
        }
        if (is_var(n->d.binop.left, var) &&
            is_const_wrt(n->d.binop.right, var)) {
            *a_out = n->d.binop.right;
            *b_out = &zero_node;
            return true;
        }
        return false;
    }

    /* (a*var + b), (var + b), (b + a*var), (b + var) */
    if (n->type == NODE_BINOP &&
        (n->d.binop.op == OP_ADD || n->d.binop.op == OP_SUB)) {
        const ast_node_t *L = n->d.binop.left;
        const ast_node_t *R = n->d.binop.right;
        binop_t op = n->d.binop.op;

        /* L contains var, R is constant */
        if (ast_contains_var(L, var) && !ast_contains_var(R, var)) {
            if (is_var(L, var)) {
                *a_out = &one_node;
                *b_out = R;
                return true;  /* works for both OP_ADD (x+c) and OP_SUB (x-c) */
            }
            if (L->type == NODE_BINOP && L->d.binop.op == OP_MUL) {
                if (is_var(L->d.binop.right, var) &&
                    is_const_wrt(L->d.binop.left, var)) {
                    *a_out = L->d.binop.left;
                    *b_out = R;
                    return true;
                }
                if (is_var(L->d.binop.left, var) &&
                    is_const_wrt(L->d.binop.right, var)) {
                    *a_out = L->d.binop.right;
                    *b_out = R;
                    return true;
                }
            }
        }

        /* R contains var, L is constant */
        if (!ast_contains_var(L, var) && ast_contains_var(R, var)) {
            if (op == OP_ADD) {
                if (is_var(R, var)) {
                    *a_out = &one_node;
                    *b_out = L;
                    return true;
                }
                if (R->type == NODE_BINOP && R->d.binop.op == OP_MUL) {
                    if (is_var(R->d.binop.right, var) &&
                        is_const_wrt(R->d.binop.left, var)) {
                        *a_out = R->d.binop.left;
                        *b_out = L;
                        return true;
                    }
                }
            }
            /* b - x pattern: a=-1, b=L */
            if (op == OP_SUB && is_var(R, var)) {
                *a_out = &neg_one_node;
                *b_out = L;
                return true;
            }
        }
    }

    return false;
}

/*
 * Try to match f(u) where u is linear in var.
 * If expr = func(a*var + b), sets *fn_out, *a_coeff, *inner.
 */
static bool is_func_of_linear(const ast_node_t *expr, char var,
                               func_id_t *fn_out,
                               const ast_node_t **a_coeff,
                               const ast_node_t **inner)
{
    const ast_node_t *a, *b;

    if (!expr) return false;
    if (expr->type != NODE_FUNC) return false;
    if (!ast_contains_var(expr->d.func.arg, var)) return false;

    if (is_linear(expr->d.func.arg, var, &a, &b)) {
        *fn_out  = expr->d.func.id;
        *a_coeff = a;
        *inner   = expr->d.func.arg;
        return true;
    }
    return false;
}

/* ── Constant rule: int(c, x) = c*x ─────────────────────── */

static ast_node_t *integ_const(const ast_node_t *expr, char var)
{
    const char *text = integ_text(expr, 60);
    steps_addf("Constant rule: int(%s) = %s*%c", text, text, var);
    return ast_binop(OP_MUL, ast_clone(expr), ast_variable(var));
}

/* ── Power rule: int(x^n) = x^(n+1)/(n+1) ───────────────── */

static ast_node_t *integ_power_rule(const ast_node_t *base, char var,
                                     const ast_node_t *exp_node)
{
    const char *text;
    ast_node_t *np1;

    /* int(x^(-1)) = ln|x| */
    if (integ_exp_is_minus_one(exp_node, var)) {
        steps_addf("int(1/%c) = ln|%c|", var, var);
        return ast_func(FN_LN, ast_func(FN_ABS, ast_variable(var)));
    }

    np1 = ast_binop(OP_ADD, ast_clone(exp_node), ast_number(1));
    np1 = ast_simplify(np1);

    text = integ_text(base, 60);
    steps_addf("Power rule: int(%s^n) = %s^(n+1)/(n+1)", text, text);

    return ast_binop(OP_DIV,
        ast_binop(OP_POW, ast_clone(base), ast_clone(np1)),
        np1);
}

/* ── Trig integrals (direct, variable = var) ─────────────── */

static ast_node_t *integ_trig_basic(func_id_t fn, char var)
{
    switch (fn) {
    case FN_SIN:
        steps_addf("int(sin(%c)) = -cos(%c)", var, var);
        return ast_unop(UOP_NEG, ast_func(FN_COS, ast_variable(var)));

    case FN_COS:
        steps_addf("int(cos(%c)) = sin(%c)", var, var);
        return ast_func(FN_SIN, ast_variable(var));

    case FN_TAN:
        steps_addf("int(tan(%c)) = -ln|cos(%c)|", var, var);
        return ast_unop(UOP_NEG,
            ast_func(FN_LN, ast_func(FN_ABS,
                ast_func(FN_COS, ast_variable(var)))));

    case FN_SEC:
        steps_addf("int(sec(%c)) = ln|sec(%c)+tan(%c)|", var, var, var);
        return ast_func(FN_LN, ast_func(FN_ABS,
            ast_binop(OP_ADD,
                ast_func(FN_SEC, ast_variable(var)),
                ast_func(FN_TAN, ast_variable(var)))));

    case FN_CSC:
        steps_addf("int(csc(%c)) = -ln|csc(%c)+cot(%c)|", var, var, var);
        return ast_unop(UOP_NEG,
            ast_func(FN_LN, ast_func(FN_ABS,
                ast_binop(OP_ADD,
                    ast_func(FN_CSC, ast_variable(var)),
                    ast_func(FN_COT, ast_variable(var))))));

    case FN_COT:
        steps_addf("int(cot(%c)) = ln|sin(%c)|", var, var);
        return ast_func(FN_LN, ast_func(FN_ABS,
            ast_func(FN_SIN, ast_variable(var))));

    case FN_EXP:
        steps_addf("int(exp(%c)) = exp(%c)", var, var);
        return ast_func(FN_EXP, ast_variable(var));

    case FN_LN:
        /* int(ln(x)) = x*ln(x) - x  (integration by parts) */
        steps_addf("int(ln(%c)) = %c*ln(%c) - %c", var, var, var, var);
        return ast_binop(OP_SUB,
            ast_binop(OP_MUL, ast_variable(var),
                ast_func(FN_LN, ast_variable(var))),
            ast_variable(var));

    /* ── Hyperbolic integrals ─────────────────────────────── */
    case FN_SINH:
        steps_addf("int(sinh(%c)) = cosh(%c)", var, var);
        return ast_func(FN_COSH, ast_variable(var));

    case FN_COSH:
        steps_addf("int(cosh(%c)) = sinh(%c)", var, var);
        return ast_func(FN_SINH, ast_variable(var));

    case FN_TANH:
        /* int(tanh(x)) = ln(cosh(x)) */
        steps_addf("int(tanh(%c)) = ln(cosh(%c))", var, var);
        return ast_func(FN_LN, ast_func(FN_COSH, ast_variable(var)));

    /* Inverse trig integrals (by parts results) */
    case FN_ASIN:
        /* int(asin(x)) = x*asin(x) + sqrt(1-x^2) */
        steps_addf("int(asin(%c)) = %c*asin(%c) + sqrt(1-%c^2)", var, var, var, var);
        return ast_binop(OP_ADD,
            ast_binop(OP_MUL, ast_variable(var),
                ast_func(FN_ASIN, ast_variable(var))),
            ast_func(FN_SQRT,
                ast_binop(OP_SUB, ast_number(1),
                    ast_binop(OP_POW, ast_variable(var), ast_number(2)))));

    case FN_ACOS:
        /* int(acos(x)) = x*acos(x) - sqrt(1-x^2) */
        steps_addf("int(acos(%c)) = %c*acos(%c) - sqrt(1-%c^2)", var, var, var, var);
        return ast_binop(OP_SUB,
            ast_binop(OP_MUL, ast_variable(var),
                ast_func(FN_ACOS, ast_variable(var))),
            ast_func(FN_SQRT,
                ast_binop(OP_SUB, ast_number(1),
                    ast_binop(OP_POW, ast_variable(var), ast_number(2)))));

    case FN_ATAN:
        /* int(atan(x)) = x*atan(x) - ln(1+x^2)/2 */
        steps_addf("int(atan(%c)) = %c*atan(%c) - ln(1+%c^2)/2", var, var, var, var);
        return ast_binop(OP_SUB,
            ast_binop(OP_MUL, ast_variable(var),
                ast_func(FN_ATAN, ast_variable(var))),
            ast_binop(OP_DIV,
                ast_func(FN_LN,
                    ast_binop(OP_ADD, ast_number(1),
                        ast_binop(OP_POW, ast_variable(var), ast_number(2)))),
                ast_number(2)));

    case FN_SQRT:
        /* int(sqrt(x)) = (2/3)*x^(3/2) — same as power rule for x^0.5 */
        steps_addf("int(sqrt(%c)) = (2/3)*%c^(3/2)", var, var);
        return ast_binop(OP_MUL,
            ast_binop(OP_DIV, ast_number(2), ast_number(3)),
            ast_binop(OP_POW, ast_variable(var),
                ast_binop(OP_DIV, ast_number(3), ast_number(2))));

    default:
        return NULL;
    }
}

/* ── Trig integrals with u-substitution for linear arg ───── */

static ast_node_t *integ_func_linear(func_id_t fn, const ast_node_t *arg,
                                     const ast_node_t *a_coeff)
{
    ast_node_t *body = NULL;
    const char *buf = integ_text(a_coeff, 80);

    switch (fn) {
    case FN_SIN:
        steps_addf("u-sub: int(sin(u)) = -cos(u)/a, a=%s", buf);
        body = ast_unop(UOP_NEG, ast_func(FN_COS, ast_clone(arg)));
        break;

    case FN_COS:
        steps_addf("u-sub: int(cos(u)) = sin(u)/a, a=%s", buf);
        body = ast_func(FN_SIN, ast_clone(arg));
        break;

    case FN_TAN:
        steps_addf("u-sub: int(tan(u)) = -ln|cos(u)|/a, a=%s", buf);
        body = ast_unop(UOP_NEG,
            ast_func(FN_LN, ast_func(FN_ABS,
                ast_func(FN_COS, ast_clone(arg)))));
        break;

    case FN_SEC:
        steps_addf("u-sub: int(sec(u)) = ln|sec(u)+tan(u)|/a, a=%s", buf);
        body = ast_func(FN_LN, ast_func(FN_ABS,
            ast_binop(OP_ADD,
                ast_func(FN_SEC, ast_clone(arg)),
                ast_func(FN_TAN, ast_clone(arg)))));
        break;

    case FN_CSC:
        steps_addf("u-sub: int(csc(u)) = -ln|csc+cot|/a, a=%s", buf);
        body = ast_unop(UOP_NEG,
            ast_func(FN_LN, ast_func(FN_ABS,
                ast_binop(OP_ADD,
                    ast_func(FN_CSC, ast_clone(arg)),
                    ast_func(FN_COT, ast_clone(arg))))));
        break;

    case FN_COT:
        steps_addf("u-sub: int(cot(u)) = ln|sin(u)|/a, a=%s", buf);
        body = ast_func(FN_LN, ast_func(FN_ABS,
            ast_func(FN_SIN, ast_clone(arg))));
        break;

    case FN_EXP:
        steps_addf("u-sub: int(exp(u)) = exp(u)/a, a=%s", buf);
        body = ast_func(FN_EXP, ast_clone(arg));
        break;

    case FN_SINH:
        steps_addf("u-sub: int(sinh(u)) = cosh(u)/a, a=%s", buf);
        body = ast_func(FN_COSH, ast_clone(arg));
        break;

    case FN_COSH:
        steps_addf("u-sub: int(cosh(u)) = sinh(u)/a, a=%s", buf);
        body = ast_func(FN_SINH, ast_clone(arg));
        break;

    case FN_TANH:
        steps_addf("u-sub: int(tanh(u)) = ln(cosh(u))/a, a=%s", buf);
        body = ast_func(FN_LN, ast_func(FN_COSH, ast_clone(arg)));
        break;

    case FN_LN:
        /* int(ln(u)) = u*ln(u) - u, then divide by a */
        steps_addf("u-sub + IBP: int(ln(u))/a, u=%s, a=%s",
                   buf, buf);
        body = ast_binop(OP_SUB,
            ast_binop(OP_MUL, ast_clone(arg),
                ast_func(FN_LN, ast_clone(arg))),
            ast_clone(arg));
        break;

    case FN_SQRT:
        /* int(sqrt(u)) = (2/3)*u^(3/2), then divide by a */
        steps_addf("u-sub: int(sqrt(u)) = (2/3)*u^(3/2)/a, a=%s", buf);
        body = ast_binop(OP_MUL,
            ast_binop(OP_DIV, ast_number(2), ast_number(3)),
            ast_binop(OP_POW, ast_clone(arg),
                ast_binop(OP_DIV, ast_number(3), ast_number(2))));
        break;

    default:
        return NULL;
    }

    if (!body) return NULL;

    /* Divide by 'a' coefficient */
    return ast_binop(OP_DIV, body, ast_clone(a_coeff));
}

/* ── Check for sec^2(u) pattern → tan(u) ────────────────── */

static ast_node_t *try_sec2(const ast_node_t *expr, char var)
{
    /* Pattern: sec(u)^2 where u is linear in var */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW &&
        is_num(expr->d.binop.right, 2) &&
        expr->d.binop.left->type == NODE_FUNC &&
        expr->d.binop.left->d.func.id == FN_SEC) {

        const ast_node_t *u = expr->d.binop.left->d.func.arg;
        const ast_node_t *a, *b;

        if (is_var(u, var)) {
            steps_addf("int(sec^2(%c)) = tan(%c)", var, var);
            return ast_func(FN_TAN, ast_variable(var));
        }
        if (is_linear(u, var, &a, &b)) {
            steps_addf("u-sub: int(sec^2(u)) = tan(u)/a");
            return ast_binop(OP_DIV,
                ast_func(FN_TAN, ast_clone(u)),
                ast_clone(a));
        }
    }
    return NULL;
}

/* ── Check for csc^2(u) pattern → -cot(u) ───────────────── */

static ast_node_t *try_csc2(const ast_node_t *expr, char var)
{
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW &&
        is_num(expr->d.binop.right, 2) &&
        expr->d.binop.left->type == NODE_FUNC &&
        expr->d.binop.left->d.func.id == FN_CSC) {

        const ast_node_t *u = expr->d.binop.left->d.func.arg;
        const ast_node_t *a, *b;

        if (is_var(u, var)) {
            steps_addf("int(csc^2(%c)) = -cot(%c)", var, var);
            return ast_unop(UOP_NEG, ast_func(FN_COT, ast_variable(var)));
        }
        if (is_linear(u, var, &a, &b)) {
            steps_addf("u-sub: int(csc^2(u)) = -cot(u)/a");
            return ast_binop(OP_DIV,
                ast_unop(UOP_NEG, ast_func(FN_COT, ast_clone(u))),
                ast_clone(a));
        }
    }
    return NULL;
}

/* ── Exponential: int(e^(ax+b)) = e^(ax+b)/a ────────────── */

static ast_node_t *try_exp_rule(const ast_node_t *expr, char var)
{
    /* e^x or e^(linear) */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW) {
        const ast_node_t *base = expr->d.binop.left;
        const ast_node_t *exp_node = expr->d.binop.right;

        /* e^(something) where something contains var */
        if (base->type == NODE_CONSTANT && base->d.constant == CONST_E &&
            ast_contains_var(exp_node, var)) {
            const ast_node_t *a, *b;

            if (is_var(exp_node, var)) {
                steps_addf("int(e^%c) = e^%c", var, var);
                return ast_binop(OP_POW,
                    ast_constant(CONST_E), ast_variable(var));
            }
            if (is_linear(exp_node, var, &a, &b)) {
                steps_addf("u-sub: int(e^u) = e^u/a");
                return ast_binop(OP_DIV,
                    ast_binop(OP_POW,
                        ast_constant(CONST_E), ast_clone(exp_node)),
                    ast_clone(a));
            }
        }

        /* a^x where a is constant, exponent = x */
        if (!ast_contains_var(base, var) && ast_contains_var(exp_node, var)) {
            const ast_node_t *a, *b;

            /* 1^u = 1: a^u/ln(a) would divide by ln(1) = 0
             * (int(1^x,x,0,1) and int((2-1)^x,x,0,1) were refused,
             * review rounds 14-15). */
            if (integ_const_equals(base, var, 1.0f)) return ast_variable(var);

            if (is_var(exp_node, var)) {
                const char *buf = integ_text(base, 80);
                steps_addf("int(%s^%c) = %s^%c/ln(%s)", buf, var, buf, var, buf);
                return ast_binop(OP_DIV,
                    ast_binop(OP_POW, ast_clone(base), ast_variable(var)),
                    ast_func(FN_LN, ast_clone(base)));
            }
            if (is_linear(exp_node, var, &a, &b)) {
                const char *buf = integ_text(base, 80);
                steps_addf("u-sub: int(%s^u) = %s^u/(a*ln(%s))", buf, buf, buf);
                return ast_binop(OP_DIV,
                    ast_binop(OP_POW, ast_clone(base), ast_clone(exp_node)),
                    ast_binop(OP_MUL,
                        ast_clone(a),
                        ast_func(FN_LN, ast_clone(base))));
            }
        }
    }
    return NULL;
}

/* ── 1/x → ln|x| ────────────────────────────────────────── */

static ast_node_t *try_recip(const ast_node_t *expr, char var)
{
    /* Pattern: 1/x or c/x */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_DIV) {
        const ast_node_t *num = expr->d.binop.left;
        const ast_node_t *den = expr->d.binop.right;

        /* c / x  where c is constant, den is just var */
        if (is_const_wrt(num, var) && is_var(den, var)) {
            steps_addf("int(1/%c) = ln|%c|  (times const)", var, var);
            return ast_binop(OP_MUL, ast_clone(num),
                ast_func(FN_LN, ast_func(FN_ABS, ast_variable(var))));
        }

        /* c / (ax+b) where den is linear */
        if (is_const_wrt(num, var) && ast_contains_var(den, var)) {
            const ast_node_t *a, *b;
            if (is_linear(den, var, &a, &b)) {
                steps_addf("u-sub: int(1/u) = ln|u|/a");
                return ast_binop(OP_DIV,
                    ast_binop(OP_MUL, ast_clone(num),
                        ast_func(FN_LN, ast_func(FN_ABS, ast_clone(den)))),
                    ast_clone(a));
            }
        }
    }
    return NULL;
}

/* ── Power with linear base: int((ax+b)^n) ──────────────── */

static ast_node_t *try_linear_power(const ast_node_t *expr, char var)
{
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW) {
        const ast_node_t *base = expr->d.binop.left;
        const ast_node_t *exp_node = expr->d.binop.right;

        if (ast_contains_var(base, var) && !ast_contains_var(exp_node, var)) {
            const ast_node_t *a, *b;
            if (is_linear(base, var, &a, &b)) {
                /* int((ax+b)^n) = (ax+b)^(n+1) / (a*(n+1)) for n != -1 */
                if (integ_exp_is_minus_one(exp_node, var)) {
                    steps_addf("u-sub: int(1/u) = ln|u|/a");
                    return ast_binop(OP_DIV,
                        ast_func(FN_LN, ast_func(FN_ABS, ast_clone(base))),
                        ast_clone(a));
                }

                ast_node_t *np1 = ast_simplify(
                    ast_binop(OP_ADD, ast_clone(exp_node), ast_number(1)));

                steps_addf("u-sub: int(u^n) = u^(n+1)/(a*(n+1))");
                return ast_binop(OP_DIV,
                    ast_binop(OP_POW, ast_clone(base), ast_clone(np1)),
                    ast_binop(OP_MUL, ast_clone(a), np1));
            }
        }
    }
    return NULL;
}

/* ── Trig product: sec(x)*tan(x), csc(x)*cot(x) ────────── */

static const ast_node_t *usub_candidate_from_node(const ast_node_t *n, char var)
{
    if (!n || !ast_contains_var(n, var))
        return NULL;

    if (n->type == NODE_FUNC && ast_contains_var(n->d.func.arg, var))
        return n->d.func.arg;

    if (n->type == NODE_BINOP && n->d.binop.op == OP_POW &&
        ast_contains_var(n->d.binop.left, var) &&
        !ast_contains_var(n->d.binop.right, var))
        return n->d.binop.left;

    if (n->type == NODE_BINOP && n->d.binop.op == OP_DIV &&
        is_num(n->d.binop.left, 1) &&
        ast_contains_var(n->d.binop.right, var))
        return n->d.binop.right;

    if (n->type == NODE_BINOP &&
        (n->d.binop.op == OP_ADD || n->d.binop.op == OP_SUB) &&
        ast_contains_var(n, var))
        return n;

    return n;
}

static ast_node_t *try_usub_with_inner(const ast_node_t *expr, char var,
                                       const ast_node_t *inner, int depth)
{
    const ast_node_t *g_expr = NULL;
    ast_node_t *g_alloc = NULL;
    ast_node_t *g_u, *F_u, *result;
    ce_float_t k = 0.0f;

    if (!expr || !inner || is_var(inner, var))
        return NULL;

    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        if (match_derivative_multiple(expr->d.binop.left, inner, var, &k))
            g_expr = expr->d.binop.right;
        else if (match_derivative_multiple(expr->d.binop.right, inner, var, &k))
            g_expr = expr->d.binop.left;
    } else if (expr->type == NODE_BINOP && expr->d.binop.op == OP_DIV) {
        if (match_derivative_multiple(expr->d.binop.left, inner, var, &k)) {
            g_alloc = ast_binop(OP_DIV, ast_number(1),
                                ast_clone(expr->d.binop.right));
            if (!g_alloc) return NULL;
            g_expr = g_alloc;
        }
    } else {
        return NULL;
    }

    if (!g_expr || fabsf(k) < 1e-12f) {
        if (g_alloc) ast_free(g_alloc);
        return NULL;
    }

    g_u = replace_subexpr_with_var(g_expr, inner, 'u');
    if (g_alloc) ast_free(g_alloc);
    if (!g_u) return NULL;
    if (ast_contains_var(g_u, var) || !ast_contains_var(g_u, 'u')) {
        ast_free(g_u);
        return NULL;
    }

    F_u = integ(g_u, 'u', depth + 1);
    ast_free(g_u);
    if (!F_u) return NULL;

    result = substitute_var(F_u, 'u', inner);
    ast_free(F_u);
    if (!result) return NULL;

#ifndef CE_DISABLE_STEP_RECORDING
    /* Step text through integ_text(): no 96 B buffers in this frame, which
     * sits on the recursive integ() path, and no deep print. */
    {
        ast_node_t *du = deriv_diff_quiet(inner, var);
        steps_addf("Nonlinear u-sub: u = %s", integ_text(inner, 96));
        if (du) {
            steps_addf("du/d%c = %s", var, integ_text(du, 96));
            ast_free(du);
        } else {
            steps_addf("du/d%c = %s", var, "du/dx");
        }
        if (fabsf(k - 1.0f) > 1e-6f)
            steps_addf("Derivative factor scale: %.6g", (double)k);
    }
#endif

    if (fabsf(k - 1.0f) > 1e-6f) {
        /* match_derivative_multiple gives factor = k * u', so
         * int(factor * g(u)) = k * G(u), not G(u) / k. */
        ast_node_t *scaled = ast_simplify(
            ast_binop(OP_MUL, result, ast_number(k)));
        if (!scaled) {
            ast_free(result);
            return NULL;
        }
        result = scaled;
    }

    return result;
}

/* Non-linear u-substitution for patterns like f'(x)*g(f(x)). */
#if CE_INTEG_ENABLE_NONLINEAR_USUB
static ast_node_t *try_nonlinear_usub(const ast_node_t *expr, char var, int depth)
{
    const ast_node_t *cands[4];
    int n_cands = 0;
    int i, j;

    if (!expr || expr->type != NODE_BINOP)
        return NULL;

    if (expr->d.binop.op != OP_MUL && expr->d.binop.op != OP_DIV)
        return NULL;

    cands[n_cands++] = usub_candidate_from_node(expr->d.binop.left, var);
    cands[n_cands++] = usub_candidate_from_node(expr->d.binop.right, var);

    for (i = 0; i < n_cands; i++) {
        if (!cands[i] || is_var(cands[i], var)) continue;
        for (j = 0; j < i; j++) {
            if (cands[j] && ast_equal(cands[i], cands[j])) {
                cands[i] = NULL;
                break;
            }
        }
        if (!cands[i]) continue;
        {
            ast_node_t *res = try_usub_with_inner(expr, var, cands[i], depth);
            if (res) return res;
        }
    }

    return NULL;
}
#endif

static ast_node_t *try_trig_product(const ast_node_t *expr, char var)
{
    const ast_node_t *L, *R;
    func_id_t lf, rf;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_MUL)
        return NULL;

    L = expr->d.binop.left;
    R = expr->d.binop.right;

    /* Both must be functions of just the variable */
    if (L->type != NODE_FUNC || R->type != NODE_FUNC) return NULL;
    if (!is_var(L->d.func.arg, var) || !is_var(R->d.func.arg, var))
        return NULL;

    lf = L->d.func.id;
    rf = R->d.func.id;

    /* sec(x)*tan(x) or tan(x)*sec(x) → sec(x) */
    if ((lf == FN_SEC && rf == FN_TAN) || (lf == FN_TAN && rf == FN_SEC)) {
        steps_addf("int(sec(%c)*tan(%c)) = sec(%c)", var, var, var);
        return ast_func(FN_SEC, ast_variable(var));
    }

    /* csc(x)*cot(x) or cot(x)*csc(x) → -csc(x) */
    if ((lf == FN_CSC && rf == FN_COT) || (lf == FN_COT && rf == FN_CSC)) {
        steps_addf("int(csc(%c)*cot(%c)) = -csc(%c)", var, var, var);
        return ast_unop(UOP_NEG, ast_func(FN_CSC, ast_variable(var)));
    }

    return NULL;
}

/* ── Inverse trig: 1/(1+x^2)→atan, 1/sqrt(1-x^2)→asin ── */

static ast_node_t *try_inverse_trig(const ast_node_t *expr, char var)
{
    const ast_node_t *num, *den;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return NULL;

    num = expr->d.binop.left;
    den = expr->d.binop.right;

    /* Numerator must be constant w.r.t. var */
    if (ast_contains_var(num, var)) return NULL;

    /* Pattern: c/(1+x^2) or c/(x^2+1) → c*atan(x) */
    if (den->type == NODE_BINOP && den->d.binop.op == OP_ADD) {
        const ast_node_t *a = den->d.binop.left;
        const ast_node_t *b = den->d.binop.right;
        bool match = false;

        /* 1+x^2 */
        if (is_num(a, 1) && b->type == NODE_BINOP &&
            b->d.binop.op == OP_POW &&
            is_var(b->d.binop.left, var) && is_num(b->d.binop.right, 2))
            match = true;

        /* x^2+1 */
        if (is_num(b, 1) && a->type == NODE_BINOP &&
            a->d.binop.op == OP_POW &&
            is_var(a->d.binop.left, var) && is_num(a->d.binop.right, 2))
            match = true;

        if (match) {
            steps_addf("int(1/(1+%c^2)) = atan(%c)", var, var);
            if (is_num(num, 1))
                return ast_func(FN_ATAN, ast_variable(var));
            return ast_binop(OP_MUL, ast_clone(num),
                ast_func(FN_ATAN, ast_variable(var)));
        }
    }

    /* Pattern: c/sqrt(1-x^2) → c*asin(x) */
    if (den->type == NODE_FUNC && den->d.func.id == FN_SQRT) {
        const ast_node_t *inner = den->d.func.arg;
        if (inner->type == NODE_BINOP && inner->d.binop.op == OP_SUB) {
            const ast_node_t *a = inner->d.binop.left;
            const ast_node_t *b = inner->d.binop.right;

            if (is_num(a, 1) && b->type == NODE_BINOP &&
                b->d.binop.op == OP_POW &&
                is_var(b->d.binop.left, var) &&
                is_num(b->d.binop.right, 2)) {
                steps_addf("int(1/sqrt(1-%c^2)) = asin(%c)", var, var);
                if (is_num(num, 1))
                    return ast_func(FN_ASIN, ast_variable(var));
                return ast_binop(OP_MUL, ast_clone(num),
                    ast_func(FN_ASIN, ast_variable(var)));
            }
        }
    }

    /* Pattern: c/(1-x^2)^(1/2) alternate form */
    if (den->type == NODE_BINOP && den->d.binop.op == OP_POW) {
        const ast_node_t *base = den->d.binop.left;
        const ast_node_t *exp_n = den->d.binop.right;

        if (exp_n->type == NODE_NUMBER && fabsf(exp_n->d.number - 0.5f) < 0.01f &&
            base->type == NODE_BINOP && base->d.binop.op == OP_SUB &&
            is_num(base->d.binop.left, 1) &&
            base->d.binop.right->type == NODE_BINOP &&
            base->d.binop.right->d.binop.op == OP_POW &&
            is_var(base->d.binop.right->d.binop.left, var) &&
            is_num(base->d.binop.right->d.binop.right, 2)) {
            steps_addf("int(1/sqrt(1-%c^2)) = asin(%c)", var, var);
            if (is_num(num, 1))
                return ast_func(FN_ASIN, ast_variable(var));
            return ast_binop(OP_MUL, ast_clone(num),
                ast_func(FN_ASIN, ast_variable(var)));
        }
    }

    return NULL;
}

/* ── Trig power: sin^2(x), cos^2(x), tan^2(x) ──────────── */

static ast_node_t *try_trig_power(const ast_node_t *expr, char var)
{
    const ast_node_t *base, *exp_n;
    func_id_t fn;
    int n;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_POW)
        return NULL;

    base = expr->d.binop.left;
    exp_n = expr->d.binop.right;

    if (base->type != NODE_FUNC || exp_n->type != NODE_NUMBER) return NULL;
    if (!is_var(base->d.func.arg, var)) return NULL;

    n = (int)exp_n->d.number;
    if (fabsf(exp_n->d.number - (ce_float_t)n) > 1e-6f) return NULL;
    if (n < 2 || n > 4) return NULL;

    fn = base->d.func.id;

    /* ── Power 2 ────────────────────────────────────── */
    if (n == 2) {
        /* int(sin^2(x)) = x/2 - sin(x)*cos(x)/2 */
        if (fn == FN_SIN) {
            steps_addf("sin^2(%c) = (1-cos(2%c))/2", var, var);
            steps_addf("int = %c/2 - sin(%c)*cos(%c)/2", var, var, var);
            return ast_binop(OP_SUB,
                ast_binop(OP_DIV, ast_variable(var), ast_number(2)),
                ast_binop(OP_DIV,
                    ast_binop(OP_MUL,
                        ast_func(FN_SIN, ast_variable(var)),
                        ast_func(FN_COS, ast_variable(var))),
                    ast_number(2)));
        }
        /* int(cos^2(x)) = x/2 + sin(x)*cos(x)/2 */
        if (fn == FN_COS) {
            steps_addf("cos^2(%c) = (1+cos(2%c))/2", var, var);
            steps_addf("int = %c/2 + sin(%c)*cos(%c)/2", var, var, var);
            return ast_binop(OP_ADD,
                ast_binop(OP_DIV, ast_variable(var), ast_number(2)),
                ast_binop(OP_DIV,
                    ast_binop(OP_MUL,
                        ast_func(FN_SIN, ast_variable(var)),
                        ast_func(FN_COS, ast_variable(var))),
                    ast_number(2)));
        }
        /* int(tan^2(x)) = tan(x) - x */
        if (fn == FN_TAN) {
            steps_addf("tan^2(%c) = sec^2(%c) - 1", var, var);
            steps_addf("int = tan(%c) - %c", var, var);
            return ast_binop(OP_SUB,
                ast_func(FN_TAN, ast_variable(var)),
                ast_variable(var));
        }
    }

    /* ── Power 3 ────────────────────────────────────── */
    if (n == 3) {
        /* int(sin^3(x)) = -cos(x) + cos^3(x)/3
         * via sin^3 = sin*(1-cos^2), let u=cos(x) */
        if (fn == FN_SIN) {
            steps_addf("sin^3(%c) = sin(%c)*(1-cos^2(%c))", var, var, var);
            steps_addf("u = cos(%c), du = -sin(%c)d%c", var, var, var);
            steps_addf("int = -cos(%c) + cos^3(%c)/3", var, var);
            return ast_binop(OP_ADD,
                ast_unop(UOP_NEG, ast_func(FN_COS, ast_variable(var))),
                ast_binop(OP_DIV,
                    ast_binop(OP_POW,
                        ast_func(FN_COS, ast_variable(var)),
                        ast_number(3)),
                    ast_number(3)));
        }
        /* int(cos^3(x)) = sin(x) - sin^3(x)/3
         * via cos^3 = cos*(1-sin^2), let u=sin(x) */
        if (fn == FN_COS) {
            steps_addf("cos^3(%c) = cos(%c)*(1-sin^2(%c))", var, var, var);
            steps_addf("u = sin(%c), du = cos(%c)d%c", var, var, var);
            steps_addf("int = sin(%c) - sin^3(%c)/3", var, var);
            return ast_binop(OP_SUB,
                ast_func(FN_SIN, ast_variable(var)),
                ast_binop(OP_DIV,
                    ast_binop(OP_POW,
                        ast_func(FN_SIN, ast_variable(var)),
                        ast_number(3)),
                    ast_number(3)));
        }
    }

    /* ── Power 4 ────────────────────────────────────── */
    if (n == 4) {
        /* int(sin^4(x)) = 3x/8 - sin(2x)/4 + sin(4x)/32
         * via double half-angle: sin^4 = (3 - 4cos(2x) + cos(4x))/8 */
        if (fn == FN_SIN) {
            steps_addf("sin^4(%c) = (3-4cos(2%c)+cos(4%c))/8", var, var, var);
            steps_addf("int = 3%c/8 - sin(2%c)/4 + sin(4%c)/32", var, var, var);
            return ast_binop(OP_ADD,
                ast_binop(OP_SUB,
                    ast_binop(OP_DIV,
                        ast_binop(OP_MUL, ast_number(3), ast_variable(var)),
                        ast_number(8)),
                    ast_binop(OP_DIV,
                        ast_func(FN_SIN, ast_binop(OP_MUL, ast_number(2), ast_variable(var))),
                        ast_number(4))),
                ast_binop(OP_DIV,
                    ast_func(FN_SIN, ast_binop(OP_MUL, ast_number(4), ast_variable(var))),
                    ast_number(32)));
        }
        /* int(cos^4(x)) = 3x/8 + sin(2x)/4 + sin(4x)/32
         * via double half-angle: cos^4 = (3 + 4cos(2x) + cos(4x))/8 */
        if (fn == FN_COS) {
            steps_addf("cos^4(%c) = (3+4cos(2%c)+cos(4%c))/8", var, var, var);
            steps_addf("int = 3%c/8 + sin(2%c)/4 + sin(4%c)/32", var, var, var);
            return ast_binop(OP_ADD,
                ast_binop(OP_ADD,
                    ast_binop(OP_DIV,
                        ast_binop(OP_MUL, ast_number(3), ast_variable(var)),
                        ast_number(8)),
                    ast_binop(OP_DIV,
                        ast_func(FN_SIN, ast_binop(OP_MUL, ast_number(2), ast_variable(var))),
                        ast_number(4))),
                ast_binop(OP_DIV,
                    ast_func(FN_SIN, ast_binop(OP_MUL, ast_number(4), ast_variable(var))),
                    ast_number(32)));
        }
    }

    return NULL;
}

/* ── Trigonometric substitution patterns ──────────────────── */
/*
 * Detect and integrate expressions involving:
 *   sqrt(a^2 - x^2)  →  x = a*sin(θ)
 *   sqrt(a^2 + x^2)  →  x = a*tan(θ)
 *   sqrt(x^2 - a^2)  →  x = a*sec(θ)
 *
 * Common integral results used:
 *   int(1/sqrt(a^2-x^2)) = asin(x/a)
 *   int(sqrt(a^2-x^2))   = (x/2)*sqrt(a^2-x^2) + (a^2/2)*asin(x/a)
 *   int(1/sqrt(x^2+a^2)) = ln|x+sqrt(x^2+a^2)|
 *   int(1/sqrt(x^2-a^2)) = ln|x+sqrt(x^2-a^2)|
 *   int(x^2/sqrt(a^2-x^2)) = -(x/2)*sqrt(a^2-x^2) + (a^2/2)*asin(x/a)
 */

/*
 * Check if inner expression matches a^2 - x^2.
 * Returns true if pattern matches and sets *a2 to the constant a^2.
 */
static bool match_a2_minus_x2(const ast_node_t *inner, char var, ce_float_t *a2)
{
    /* a^2 - x^2 */
    if (inner->type == NODE_BINOP && inner->d.binop.op == OP_SUB) {
        const ast_node_t *L = inner->d.binop.left;
        const ast_node_t *R = inner->d.binop.right;

        /* L is a constant, R is x^2 */
        if (!ast_contains_var(L, var) &&
            R->type == NODE_BINOP && R->d.binop.op == OP_POW &&
            is_var(R->d.binop.left, var) && is_num(R->d.binop.right, 2)) {
            /* Evaluate L to get a^2 */
            eval_ctx_t ctx;
            eval_init(&ctx);
            *a2 = eval_expr(&ctx, L);
            return (eval_error(&ctx) == CE_OK && *a2 > 0);
        }
    }
    return false;
}

/*
 * Check if inner expression matches x^2 + a^2 or a^2 + x^2.
 */
static bool match_x2_plus_a2(const ast_node_t *inner, char var, ce_float_t *a2)
{
    if (inner->type == NODE_BINOP && inner->d.binop.op == OP_ADD) {
        const ast_node_t *L = inner->d.binop.left;
        const ast_node_t *R = inner->d.binop.right;
        eval_ctx_t ctx;

        /* x^2 + a^2 */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            is_var(L->d.binop.left, var) && is_num(L->d.binop.right, 2) &&
            !ast_contains_var(R, var)) {
            eval_init(&ctx);
            *a2 = eval_expr(&ctx, R);
            return (eval_error(&ctx) == CE_OK && *a2 > 0);
        }

        /* a^2 + x^2 */
        if (R->type == NODE_BINOP && R->d.binop.op == OP_POW &&
            is_var(R->d.binop.left, var) && is_num(R->d.binop.right, 2) &&
            !ast_contains_var(L, var)) {
            eval_init(&ctx);
            *a2 = eval_expr(&ctx, L);
            return (eval_error(&ctx) == CE_OK && *a2 > 0);
        }
    }
    return false;
}

/*
 * Check if inner expression matches x^2 - a^2.
 */
static bool match_x2_minus_a2(const ast_node_t *inner, char var, ce_float_t *a2)
{
    if (inner->type == NODE_BINOP && inner->d.binop.op == OP_SUB) {
        const ast_node_t *L = inner->d.binop.left;
        const ast_node_t *R = inner->d.binop.right;

        /* x^2 - a^2 */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            is_var(L->d.binop.left, var) && is_num(L->d.binop.right, 2) &&
            !ast_contains_var(R, var)) {
            eval_ctx_t ctx;
            eval_init(&ctx);
            *a2 = eval_expr(&ctx, R);
            return (eval_error(&ctx) == CE_OK && *a2 > 0);
        }
    }
    return false;
}

#if CE_INTEG_ENABLE_TRIG_SUB || CE_INTEG_ENABLE_STRATEGY_REPORT
static ast_node_t *try_trig_sub(const ast_node_t *expr, char var)
{
    const ast_node_t *num, *den, *sqrt_inner;
    ce_float_t a2, a;

    /* ── Pattern: c / sqrt(a^2 - x^2) → c * asin(x/a) ──── */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_DIV) {
        num = expr->d.binop.left;
        den = expr->d.binop.right;

        if (is_const_wrt(num, var)) {
            /* den = sqrt(stuff) */
            if (den->type == NODE_FUNC && den->d.func.id == FN_SQRT) {
                sqrt_inner = den->d.func.arg;

                /* 1/sqrt(a^2 - x^2) → asin(x/a) */
                if (match_a2_minus_x2(sqrt_inner, var, &a2)) {
                    a = sqrtf(a2);
                    steps_addf("Trig sub: 1/sqrt(%.4g-%c^2), a=%.4g", (double)a2, var, (double)a);
                    steps_addf("  x = %.4g*sin(t), int = asin(%c/%.4g)", (double)a, var, (double)a);

                    if (fabsf(a - 1.0f) < 0.001f) {
                        if (is_num(num, 1))
                            return ast_func(FN_ASIN, ast_variable(var));
                        return ast_binop(OP_MUL, ast_clone(num),
                            ast_func(FN_ASIN, ast_variable(var)));
                    }
                    if (is_num(num, 1))
                        return ast_func(FN_ASIN,
                            ast_binop(OP_DIV, ast_variable(var), ast_number(a)));
                    return ast_binop(OP_MUL, ast_clone(num),
                        ast_func(FN_ASIN,
                            ast_binop(OP_DIV, ast_variable(var), ast_number(a))));
                }

                /* 1/sqrt(x^2 + a^2) → ln|x + sqrt(x^2+a^2)| */
                if (match_x2_plus_a2(sqrt_inner, var, &a2)) {
                    a = sqrtf(a2);
                    steps_addf("Trig sub: 1/sqrt(%c^2+%.4g), a=%.4g", var, (double)a2, (double)a);
                    steps_addf("  x = %.4g*tan(t), int = ln|%c+sqrt(%c^2+%.4g)|",
                               (double)a, var, var, (double)a2);

                    ast_node_t *result = ast_func(FN_LN, ast_func(FN_ABS,
                        ast_binop(OP_ADD,
                            ast_variable(var),
                            ast_func(FN_SQRT, ast_clone(sqrt_inner)))));

                    if (is_num(num, 1)) return result;
                    return ast_binop(OP_MUL, ast_clone(num), result);
                }

                /* 1/sqrt(x^2 - a^2) → ln|x + sqrt(x^2-a^2)| */
                if (match_x2_minus_a2(sqrt_inner, var, &a2)) {
                    a = sqrtf(a2);
                    steps_addf("Trig sub: 1/sqrt(%c^2-%.4g), a=%.4g", var, (double)a2, (double)a);
                    steps_addf("  x = %.4g*sec(t), int = ln|%c+sqrt(%c^2-%.4g)|",
                               (double)a, var, var, (double)a2);

                    ast_node_t *result = ast_func(FN_LN, ast_func(FN_ABS,
                        ast_binop(OP_ADD,
                            ast_variable(var),
                            ast_func(FN_SQRT, ast_clone(sqrt_inner)))));

                    if (is_num(num, 1)) return result;
                    return ast_binop(OP_MUL, ast_clone(num), result);
                }
            }

            /* den = (a^2 + x^2) — already handled by try_inverse_trig for a=1,
             * here handle general a: c/(a^2+x^2) → (c/a)*atan(x/a) */
            if (match_x2_plus_a2(den, var, &a2)) {
                a = sqrtf(a2);
                steps_addf("Trig sub: 1/(%c^2+%.4g) = (1/%.4g)*atan(%c/%.4g)",
                           var, (double)a2, (double)a, var, (double)a);

                return ast_binop(OP_MUL,
                    ast_binop(OP_DIV, ast_clone(num), ast_number(a)),
                    ast_func(FN_ATAN,
                        ast_binop(OP_DIV, ast_variable(var), ast_number(a))));
            }
        }
    }

    /* ── Pattern: sqrt(a^2 - x^2) alone ──────────────────── */
    if (expr->type == NODE_FUNC && expr->d.func.id == FN_SQRT) {
        sqrt_inner = expr->d.func.arg;

        if (match_a2_minus_x2(sqrt_inner, var, &a2)) {
            a = sqrtf(a2);
            steps_addf("Trig sub: sqrt(%.4g-%c^2), a=%.4g", (double)a2, var, (double)a);
            steps_addf("  = (%c/2)*sqrt(%.4g-%c^2) + (%.4g/2)*asin(%c/%.4g)",
                       var, (double)a2, var, (double)a2, var, (double)a);

            /* (x/2)*sqrt(a^2-x^2) + (a^2/2)*asin(x/a) */
            return ast_binop(OP_ADD,
                ast_binop(OP_MUL,
                    ast_binop(OP_DIV, ast_variable(var), ast_number(2)),
                    ast_func(FN_SQRT, ast_clone(sqrt_inner))),
                ast_binop(OP_MUL,
                    ast_number(a2 / 2.0f),
                    ast_func(FN_ASIN,
                        ast_binop(OP_DIV, ast_variable(var), ast_number(a)))));
        }
    }

    return NULL;
}
#endif

/* ── Partial fraction decomposition for rational integrals ── */
/*
 * Handle rational functions where denominator has known factored forms:
 *   c / ((x-a)(x-b))       → A/(x-a) + B/(x-b)
 *   c / (x^2 - d^2)        → split using difference of squares
 *   (cx+d) / (x^2+bx+c)   → ln + atan decomposition
 *   c / (x*(x+a))          → partial fractions
 *   c / (x^2 + a)          → (c/sqrt(a)) * atan(x/sqrt(a))  [already in trig_sub]
 */

#if CE_INTEG_ENABLE_PARTIAL_FRAC || CE_INTEG_ENABLE_STRATEGY_REPORT
static ast_node_t *try_partial_fractions(const ast_node_t *expr, char var)
{
    const ast_node_t *num, *den;
    const ast_node_t *L, *R;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return NULL;

    num = expr->d.binop.left;
    den = expr->d.binop.right;

    /* ── Pattern: c / (x * (x + a)) ─────────────────────── */
    if (den->type == NODE_BINOP && den->d.binop.op == OP_MUL) {
        L = den->d.binop.left;
        R = den->d.binop.right;

        /* x * (x + a) where a is constant */
        if (is_var(L, var) && R->type == NODE_BINOP &&
            (R->d.binop.op == OP_ADD || R->d.binop.op == OP_SUB) &&
            is_var(R->d.binop.left, var) && is_const_wrt(R->d.binop.right, var)) {

            eval_ctx_t ctx;
            ce_float_t c_val, a_val, A, B;

            eval_init(&ctx);
            c_val = is_const_wrt(num, var) ? eval_expr(&ctx, num) : 0;
            if (eval_error(&ctx) != CE_OK || c_val == 0) return NULL;

            eval_init(&ctx);
            a_val = eval_expr(&ctx, R->d.binop.right);
            if (eval_error(&ctx) != CE_OK) return NULL;
            if (R->d.binop.op == OP_SUB) a_val = -a_val;

            if (fabsf(a_val) < 1e-10f) return NULL;

            /* c / (x(x+a)) = A/x + B/(x+a) where A = c/a, B = -c/a (Heaviside) */
            /* Actually: c/(x(x+a)) → cover-up: A = c/(0+a) = c/a, B = c/((-a)(0)) ... */
            /* Better: A*x + A*a + B*x = c → A+B=0, Aa=c → A=c/a, B=-c/a */
            A = c_val / a_val;
            B = -c_val / a_val;

            steps_addf("Partial fractions: %.4g/(%c*(%c%+.4g))",
                       (double)c_val, var, var, (double)a_val);
            steps_addf("  = %.4g/%c + %.4g/(%c%+.4g)",
                       (double)A, var, (double)B, var, (double)a_val);

            /* int = A*ln|x| + B*ln|x+a| */
            return ast_binop(OP_ADD,
                ast_binop(OP_MUL, ast_number(A),
                    ast_func(FN_LN, ast_func(FN_ABS, ast_variable(var)))),
                ast_binop(OP_MUL, ast_number(B),
                    ast_func(FN_LN, ast_func(FN_ABS,
                        ast_clone(R)))));
        }

        /* (x + a) * (x + b) where both are linear in var
         * Numerator can be constant or linear in var (Heaviside cover-up) */
        if (L->type == NODE_BINOP &&
            (L->d.binop.op == OP_ADD || L->d.binop.op == OP_SUB) &&
            is_var(L->d.binop.left, var) && is_const_wrt(L->d.binop.right, var) &&
            R->type == NODE_BINOP &&
            (R->d.binop.op == OP_ADD || R->d.binop.op == OP_SUB) &&
            is_var(R->d.binop.left, var) && is_const_wrt(R->d.binop.right, var)) {

            eval_ctx_t ctx;
            ce_float_t a_val, b_val, A, B;

            eval_init(&ctx);
            a_val = eval_expr(&ctx, L->d.binop.right);
            if (eval_error(&ctx) != CE_OK) return NULL;
            if (L->d.binop.op == OP_SUB) a_val = -a_val;

            eval_init(&ctx);
            b_val = eval_expr(&ctx, R->d.binop.right);
            if (eval_error(&ctx) != CE_OK) return NULL;
            if (R->d.binop.op == OP_SUB) b_val = -b_val;

            if (fabsf(a_val - b_val) < 1e-10f) return NULL; /* repeated root */

            /* Heaviside cover-up: evaluate numerator at roots
             * A = num(-a) / (b-a), B = num(-b) / (a-b) */
            if (is_const_wrt(num, var)) {
                ce_float_t c_val;
                eval_init(&ctx);
                c_val = eval_expr(&ctx, num);
                if (eval_error(&ctx) != CE_OK) return NULL;
                A = c_val / (b_val - a_val);
                B = c_val / (a_val - b_val);
            } else {
                ce_float_t num_neg_a, num_neg_b;
                eval_init(&ctx);
                eval_set_var(&ctx, var, -a_val);
                num_neg_a = eval_expr(&ctx, num);
                if (eval_error(&ctx) != CE_OK) return NULL;

                eval_init(&ctx);
                eval_set_var(&ctx, var, -b_val);
                num_neg_b = eval_expr(&ctx, num);
                if (eval_error(&ctx) != CE_OK) return NULL;

                A = num_neg_a / (b_val - a_val);
                B = num_neg_b / (a_val - b_val);
            }

            steps_addf("Partial fractions: A/(%c%+.4g) + B/(%c%+.4g)",
                       var, (double)a_val, var, (double)b_val);
            steps_addf("  A = %.4g, B = %.4g",
                       (double)A, (double)B);

            return ast_binop(OP_ADD,
                ast_binop(OP_MUL, ast_number(A),
                    ast_func(FN_LN, ast_func(FN_ABS, ast_clone(L)))),
                ast_binop(OP_MUL, ast_number(B),
                    ast_func(FN_LN, ast_func(FN_ABS, ast_clone(R)))));
        }
    }

    /* ── Pattern: c / (x^2 - a^2) = c / ((x-a)(x+a)) ───── */
    {
        ce_float_t a2;
        if (is_const_wrt(num, var) && match_x2_minus_a2(den, var, &a2)) {
            ce_float_t a_val = sqrtf(a2);
            eval_ctx_t ctx;
            ce_float_t c_val;
            ce_float_t A, B;

            eval_init(&ctx);
            c_val = eval_expr(&ctx, num);
            if (eval_error(&ctx) != CE_OK) return NULL;

            /* c/(x^2-a^2) = c/((x-a)(x+a)) = A/(x-a) + B/(x+a)
             * A = c/(2a), B = -c/(2a) */
            A = c_val / (2.0f * a_val);
            B = -c_val / (2.0f * a_val);

            steps_addf("Partial fractions: %.4g/(%c^2-%.4g)",
                       (double)c_val, var, (double)a2);
            steps_addf("  = %.4g/(%c-%.4g) + %.4g/(%c+%.4g)",
                       (double)A, var, (double)a_val, (double)B, var, (double)a_val);

            return ast_binop(OP_ADD,
                ast_binop(OP_MUL, ast_number(A),
                    ast_func(FN_LN, ast_func(FN_ABS,
                        ast_binop(OP_SUB, ast_variable(var), ast_number(a_val))))),
                ast_binop(OP_MUL, ast_number(B),
                    ast_func(FN_LN, ast_func(FN_ABS,
                        ast_binop(OP_ADD, ast_variable(var), ast_number(a_val))))));
        }
    }

    return NULL;
}
#endif

/* ── Cyclic integration by parts ─────────────────────────── */
/*
 * Closed-form results for cyclic IBP patterns:
 *   int(e^(ax)*sin(bx)) = e^(ax)*(a*sin(bx) - b*cos(bx))/(a^2+b^2)
 *   int(e^(ax)*cos(bx)) = e^(ax)*(a*cos(bx) + b*sin(bx))/(a^2+b^2)
 * Special case a=1:
 *   int(e^x*sin(x)) = e^x*(sin(x)-cos(x))/2
 *   int(e^x*cos(x)) = e^x*(sin(x)+cos(x))/2
 */

/* Check if expr matches e^(a*var) or e^var. Returns true and sets *a_coeff. */
static bool match_exp_linear(const ast_node_t *expr, char var, ce_float_t *a_coeff)
{
    /* e^x */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW &&
        expr->d.binop.left->type == NODE_CONSTANT &&
        expr->d.binop.left->d.constant == CONST_E &&
        is_var(expr->d.binop.right, var)) {
        *a_coeff = 1.0f;
        return true;
    }
    /* exp(x) */
    if (expr->type == NODE_FUNC && expr->d.func.id == FN_EXP &&
        is_var(expr->d.func.arg, var)) {
        *a_coeff = 1.0f;
        return true;
    }
    /* e^(a*x) or exp(a*x) */
    {
        const ast_node_t *inner = NULL;
        if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW &&
            expr->d.binop.left->type == NODE_CONSTANT &&
            expr->d.binop.left->d.constant == CONST_E)
            inner = expr->d.binop.right;
        else if (expr->type == NODE_FUNC && expr->d.func.id == FN_EXP)
            inner = expr->d.func.arg;
        if (inner && inner->type == NODE_BINOP && inner->d.binop.op == OP_MUL) {
            if (inner->d.binop.left->type == NODE_NUMBER &&
                is_var(inner->d.binop.right, var)) {
                *a_coeff = inner->d.binop.left->d.number;
                return true;
            }
            if (inner->d.binop.right->type == NODE_NUMBER &&
                is_var(inner->d.binop.left, var)) {
                *a_coeff = inner->d.binop.right->d.number;
                return true;
            }
        }
    }
    return false;
}

/* Check if expr matches sin(b*var) or cos(b*var). Returns true, sets *b_coeff and *is_sin. */
static bool match_trig_linear(const ast_node_t *expr, char var, ce_float_t *b_coeff, bool *is_sin)
{
    if (expr->type != NODE_FUNC) return false;
    if (expr->d.func.id != FN_SIN && expr->d.func.id != FN_COS) return false;
    *is_sin = (expr->d.func.id == FN_SIN);
    if (is_var(expr->d.func.arg, var)) {
        *b_coeff = 1.0f;
        return true;
    }
    if (expr->d.func.arg->type == NODE_BINOP &&
        expr->d.func.arg->d.binop.op == OP_MUL) {
        const ast_node_t *m = expr->d.func.arg;
        if (m->d.binop.left->type == NODE_NUMBER && is_var(m->d.binop.right, var)) {
            *b_coeff = m->d.binop.left->d.number;
            return true;
        }
        if (m->d.binop.right->type == NODE_NUMBER && is_var(m->d.binop.left, var)) {
            *b_coeff = m->d.binop.right->d.number;
            return true;
        }
    }
    return false;
}

#if CE_INTEG_ENABLE_CYCLIC_PARTS
static ast_node_t *try_cyclic_ibp(const ast_node_t *expr, char var)
{
    const ast_node_t *f, *g;
    ce_float_t a, b;
    bool is_sin;
    ast_node_t *exp_part, *trig_inner, *denom;

    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_MUL)
        return NULL;

    f = expr->d.binop.left;
    g = expr->d.binop.right;

    /* Try f=exp, g=trig */
    if (match_exp_linear(f, var, &a) && match_trig_linear(g, var, &b, &is_sin)) {
        /* matched */
    }
    /* Try f=trig, g=exp */
    else if (match_exp_linear(g, var, &a) && match_trig_linear(f, var, &b, &is_sin)) {
        /* matched (swapped) */
    }
    else return NULL;

    /* Build the result:
     * int(e^(ax)*sin(bx)) = e^(ax)*(a*sin(bx)-b*cos(bx))/(a^2+b^2)
     * int(e^(ax)*cos(bx)) = e^(ax)*(a*cos(bx)+b*sin(bx))/(a^2+b^2)
     */
    steps_addf("Cyclic IBP: int(e^(%.4g%c)*%s(%.4g%c))",
               (double)a, var, is_sin ? "sin" : "cos", (double)b, var);
    steps_addf("  = e^(a%c)*(a*%s(b%c) %c b*%s(b%c))/(a^2+b^2)",
               var, is_sin ? "sin" : "cos", var,
               is_sin ? '-' : '+',
               is_sin ? "cos" : "sin", var);

    /* Build e^(a*x) term */
    if (fabsf(a - 1.0f) < 1e-10f)
        exp_part = ast_binop(OP_POW, ast_constant(CONST_E), ast_variable(var));
    else
        exp_part = ast_binop(OP_POW, ast_constant(CONST_E),
                     ast_binop(OP_MUL, ast_number(a), ast_variable(var)));

    /* Build trig inner argument b*x */
    if (fabsf(b - 1.0f) < 1e-10f)
        trig_inner = ast_variable(var);
    else
        trig_inner = ast_binop(OP_MUL, ast_number(b), ast_variable(var));

    denom = ast_number(a * a + b * b);

    if (is_sin) {
        /* e^(ax)*(a*sin(bx) - b*cos(bx))/(a^2+b^2) */
        return ast_simplify(
            ast_binop(OP_DIV,
                ast_binop(OP_MUL, exp_part,
                    ast_binop(OP_SUB,
                        ast_binop(OP_MUL, ast_number(a),
                            ast_func(FN_SIN, ast_clone(trig_inner))),
                        ast_binop(OP_MUL, ast_number(b),
                            ast_func(FN_COS, trig_inner)))),
                denom));
    } else {
        /* e^(ax)*(a*cos(bx) + b*sin(bx))/(a^2+b^2) */
        return ast_simplify(
            ast_binop(OP_DIV,
                ast_binop(OP_MUL, exp_part,
                    ast_binop(OP_ADD,
                        ast_binop(OP_MUL, ast_number(a),
                            ast_func(FN_COS, ast_clone(trig_inner))),
                        ast_binop(OP_MUL, ast_number(b),
                            ast_func(FN_SIN, trig_inner)))),
                denom));
    }
}

/* ── Integration by parts: int(u*dv) = u*v - int(v*du) ──── */
/*
 * Recognizable patterns using LIATE priority:
 *   - x^n * e^x    → u=x^n, dv=e^x dx
 *   - x^n * sin(x) → u=x^n, dv=sin(x) dx
 *   - x^n * cos(x) → u=x^n, dv=cos(x) dx
 *   - x^n * ln(x)  → u=ln(x), dv=x^n dx
 *   - ln(x)        → u=ln(x), dv=dx
 */

#endif
#if CE_INTEG_ENABLE_PARTS
static ast_node_t *try_parts(const ast_node_t *expr, char var, int depth)
{
    const ast_node_t *f, *g;
    ast_node_t *u_part, *dv_int, *du, *v_du_product, *sub_integral;

    /* Prevent infinite recursion */
    if (depth > 4) return NULL;

    /* int(ln(x)) = x*ln(x) - x  (special case) */
    if (expr->type == NODE_FUNC && expr->d.func.id == FN_LN &&
        is_var(expr->d.func.arg, var)) {
        steps_addf("By parts: int(ln(%c)) = %c*ln(%c) - %c", var, var, var, var);
        return ast_binop(OP_SUB,
            ast_binop(OP_MUL, ast_variable(var),
                ast_func(FN_LN, ast_variable(var))),
            ast_variable(var));
    }

    /* Need a product */
    if (expr->type != NODE_BINOP || expr->d.binop.op != OP_MUL)
        return NULL;

    f = expr->d.binop.left;
    g = expr->d.binop.right;

    /*
     * Decide which is u and which is dv.
     * LIATE: Logarithmic > Inverse trig > Algebraic > Trig > Exponential
     * u should be the higher LIATE category.
     */

    /* Pattern: poly * func(var)  where func is trig or exp */
    /* Try: u = f (algebraic), dv = g (trig/exp) */
    if (ast_contains_var(f, var) && ast_contains_var(g, var)) {
        bool f_is_poly = false, g_is_poly = false;
        bool f_is_trig_exp = false, g_is_trig_exp = false;
        bool f_is_log = false, g_is_log = false;

        /* Classify f */
        if (is_var(f, var) ||
            (f->type == NODE_BINOP && f->d.binop.op == OP_POW &&
             is_var(f->d.binop.left, var) && is_const_wrt(f->d.binop.right, var)))
            f_is_poly = true;

        if (f->type == NODE_FUNC &&
            (f->d.func.id == FN_SIN || f->d.func.id == FN_COS ||
             f->d.func.id == FN_EXP) &&
            is_var(f->d.func.arg, var))
            f_is_trig_exp = true;

        if (f->type == NODE_FUNC && f->d.func.id == FN_LN &&
            is_var(f->d.func.arg, var))
            f_is_log = true;

        /* Also handle e^x form */
        if (f->type == NODE_BINOP && f->d.binop.op == OP_POW &&
            f->d.binop.left->type == NODE_CONSTANT &&
            f->d.binop.left->d.constant == CONST_E &&
            is_var(f->d.binop.right, var))
            f_is_trig_exp = true;

        /* Classify g */
        if (is_var(g, var) ||
            (g->type == NODE_BINOP && g->d.binop.op == OP_POW &&
             is_var(g->d.binop.left, var) && is_const_wrt(g->d.binop.right, var)))
            g_is_poly = true;

        if (g->type == NODE_FUNC &&
            (g->d.func.id == FN_SIN || g->d.func.id == FN_COS ||
             g->d.func.id == FN_EXP) &&
            is_var(g->d.func.arg, var))
            g_is_trig_exp = true;

        if (g->type == NODE_FUNC && g->d.func.id == FN_LN &&
            is_var(g->d.func.arg, var))
            g_is_log = true;

        /* Also handle e^x form */
        if (g->type == NODE_BINOP && g->d.binop.op == OP_POW &&
            g->d.binop.left->type == NODE_CONSTANT &&
            g->d.binop.left->d.constant == CONST_E &&
            is_var(g->d.binop.right, var))
            g_is_trig_exp = true;

        /* Cases 1 & 2: ln(x) * poly  or  poly * ln(x)
         * → u=ln(x), dv=poly dx
         * v*du = [int(poly)] * (1/x)
         * For poly=x^n: v*du = x^(n+1)/((n+1)*x) = x^n/(n+1)
         * Build v*du as x^n/(n+1) directly for reliable sub-integration.
         */
        if ((f_is_log && g_is_poly) || (g_is_log && f_is_poly)) {
            const ast_node_t *poly_part = f_is_poly ? f : g;
            const ast_node_t *log_part  = f_is_log  ? f : g;

            steps_addf("By parts on %s: u=ln(%c), dv=...",
                       integ_text(expr, 60), var);

            u_part = ast_clone(log_part);              /* ln(x) */
            dv_int = integ(poly_part, var, depth + 1); /* int(poly) */
            if (!dv_int) { ast_free(u_part); return NULL; }

            /* Build v*du directly as poly_part/(n+1) instead of
             * (dv_int)/x which doesn't simplify well.
             * For x^n: int(x^n)=x^(n+1)/(n+1), so v*du = x^n/(n+1)
             * For x: int(x)=x^2/2, so v*du = x/2
             * For constant k: int(k)=kx, so v*du = k (trivially) */
            if (is_var(poly_part, var)) {
                /* poly = x → v*du = x/2 */
                v_du_product = ast_binop(OP_DIV,
                    ast_variable(var), ast_number(2));
            } else if (poly_part->type == NODE_BINOP &&
                       poly_part->d.binop.op == OP_POW &&
                       is_var(poly_part->d.binop.left, var) &&
                       poly_part->d.binop.right->type == NODE_NUMBER) {
                /* poly = x^n → v*du = x^n/(n+1) */
                ce_float_t n_val = poly_part->d.binop.right->d.number;
                v_du_product = ast_binop(OP_DIV,
                    ast_clone(poly_part), ast_number(n_val + 1));
            } else if (poly_part->type == NODE_NUMBER) {
                /* poly = k → v*du = k */
                v_du_product = ast_number(poly_part->d.number);
            } else {
                /* Fallback: build (dv_int)/x and hope simplifier handles it */
                v_du_product = ast_simplify(
                    ast_binop(OP_DIV, ast_clone(dv_int), ast_variable(var)));
            }
            sub_integral = integ(v_du_product, var, depth + 1);
            ast_free(v_du_product);
            if (!sub_integral) {
                ast_free(u_part); ast_free(dv_int);
                return NULL;
            }

            return ast_simplify(
                ast_binop(OP_SUB,
                    ast_binop(OP_MUL, u_part, dv_int),
                    sub_integral));
        }

        /* Case 3: poly * trig/exp  → u=poly, dv=trig/exp */
        if (f_is_poly && g_is_trig_exp) {
            steps_addf("By parts on %s: u=poly, dv=...", integ_text(expr, 60));

            u_part = ast_clone(f);                    /* x^n */
            /* du = differentiate u */
            /* Use power rule manually for simple x^n */
            if (is_var(f, var)) {
                du = ast_number(1);
            } else if (f->type == NODE_BINOP && f->d.binop.op == OP_POW &&
                       f->d.binop.right->type == NODE_NUMBER) {
                /* x^n → n*x^(n-1) */
                ce_float_t n_val = f->d.binop.right->d.number;
                du = ast_simplify(
                    ast_binop(OP_MUL,
                        ast_number(n_val),
                        ast_binop(OP_POW, ast_variable(var),
                                  ast_number(n_val - 1))));
            } else {
                ast_free(u_part);
                return NULL;
            }

            dv_int = integ(g, var, depth + 1);        /* int(trig/exp) */
            if (!dv_int) { ast_free(u_part); ast_free(du); return NULL; }

            v_du_product = ast_binop(OP_MUL, ast_clone(dv_int), du);
            sub_integral = integ(v_du_product, var, depth + 1);
            ast_free(v_du_product);
            if (!sub_integral) {
                ast_free(u_part); ast_free(dv_int);
                return NULL;
            }

            return ast_simplify(
                ast_binop(OP_SUB,
                    ast_binop(OP_MUL, u_part, dv_int),
                    sub_integral));
        }

        /* Case 4: trig/exp * poly  → u=poly, dv=trig/exp */
        if (g_is_poly && f_is_trig_exp) {
            steps_addf("By parts on %s: u=poly, dv=...", integ_text(expr, 60));

            u_part = ast_clone(g);
            if (is_var(g, var)) {
                du = ast_number(1);
            } else if (g->type == NODE_BINOP && g->d.binop.op == OP_POW &&
                       g->d.binop.right->type == NODE_NUMBER) {
                ce_float_t n_val = g->d.binop.right->d.number;
                du = ast_simplify(
                    ast_binop(OP_MUL,
                        ast_number(n_val),
                        ast_binop(OP_POW, ast_variable(var),
                                  ast_number(n_val - 1))));
            } else {
                ast_free(u_part);
                return NULL;
            }

            dv_int = integ(f, var, depth + 1);
            if (!dv_int) { ast_free(u_part); ast_free(du); return NULL; }

            v_du_product = ast_binop(OP_MUL, ast_clone(dv_int), du);
            sub_integral = integ(v_du_product, var, depth + 1);
            ast_free(v_du_product);
            if (!sub_integral) {
                ast_free(u_part); ast_free(dv_int);
                return NULL;
            }

            return ast_simplify(
                ast_binop(OP_SUB,
                    ast_binop(OP_MUL, u_part, dv_int),
                    sub_integral));
        }
    }

    return NULL;
}
#endif

/* ── Core recursive integrator ───────────────────────────── */

static ast_node_t *integ_node(const ast_node_t *expr, char var, int depth);

static ast_node_t *integ(const ast_node_t *expr, char var, int depth)
{
    ast_node_t *result;

    if (g_integ_nest >= INTEG_MAX_DEPTH) {
        /* integrate(), integrate_with_strategy() and integrate_tabular()
         * turn this into the INTEG_TOO_DEEP_MSG failure reason. */
        g_integ_too_deep = true;
        return NULL;
    }
    g_integ_nest++;
    result = integ_node(expr, var, depth);
    g_integ_nest--;
    return result;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static ast_node_t *integ_rules(const ast_node_t *expr, char var, int depth)
{
    ast_node_t *result;

    (void)depth; /* used only by the strategy rules some workers compile in */

    switch (expr->type) {

    /* ── Variable: int(x) = x^2/2 ───────────────────────── */
    case NODE_VARIABLE:
        /* integ_node() took every expression without var, so this is var. */
        steps_addf("int(%c) = %c^2/2", var, var);
        return ast_binop(OP_DIV,
            ast_binop(OP_POW, ast_variable(var), ast_number(2)),
            ast_number(2));

    /* ── Unary negation: int(-f) = -int(f) ──────────────── */
    /* NODE_UNOP: negation is handled in integ_node(); others fail. */

    /* ── Binary operations ───────────────────────────────── */
    case NODE_BINOP:
        switch (expr->d.binop.op) {

        /* Sums and constant multiples are handled in integ_node(). */
        case OP_MUL: {
            /* Nonlinear u-substitution: f'(x)*g(f(x)) */
#if CE_INTEG_ENABLE_NONLINEAR_USUB
            result = try_nonlinear_usub(expr, var, depth);
            if (result) return result;
#endif

            /* Trig product: sec*tan, csc*cot */
            result = try_trig_product(expr, var);
            if (result) return result;

            /* Cyclic IBP: e^(ax)*sin(bx), e^(ax)*cos(bx) */
#if CE_INTEG_ENABLE_CYCLIC_PARTS
            result = try_cyclic_ibp(expr, var);
            if (result) return result;
#endif

            /* Both contain var — try integration by parts */
#if CE_INTEG_ENABLE_PARTS
            result = try_parts(expr, var, depth);
            if (result) return result;
#endif

            return NULL;
        }

        /* ── Division ────────────────────────────────────── */
        case OP_DIV: {
            const ast_node_t *num = expr->d.binop.left;
            const ast_node_t *den = expr->d.binop.right;

            /* Nonlinear u-substitution in quotient form f'(x)/g(f(x)) */
#if CE_INTEG_ENABLE_NONLINEAR_USUB
            result = try_nonlinear_usub(expr, var, depth);
            if (result) return result;
#endif

            /* 1/x or c/x → ln|x| */
            result = try_recip(expr, var);
            if (result) return result;

            /* c/x^n → c * x^(-n) → power rule */
            if (is_const_wrt(num, var) &&
                den->type == NODE_BINOP && den->d.binop.op == OP_POW &&
                is_var(den->d.binop.left, var) &&
                is_const_wrt(den->d.binop.right, var)) {
                /* Rewrite as c * x^(-n) */
                ast_node_t *neg_exp = ast_unop(UOP_NEG, ast_clone(den->d.binop.right));
                neg_exp = ast_simplify(neg_exp);
                ast_node_t *ni = integ_power_rule(den->d.binop.left, var, neg_exp);
                ast_free(neg_exp);
                if (ni) {
                    if (is_num(num, 1)) return ni;
                    return ast_binop(OP_MUL, ast_clone(num), ni);
                }
            }

            /* Inverse trig: 1/(1+x^2), 1/sqrt(1-x^2) */
            result = try_inverse_trig(expr, var);
            if (result) return result;

            /* Trig substitution: 1/sqrt(a^2-x^2), etc. */
#if CE_INTEG_ENABLE_TRIG_SUB
            result = try_trig_sub(expr, var);
            if (result) return result;
#endif

            /* Partial fractions: c/((x+a)(x+b)), c/(x^2-a^2), etc. */
#if CE_INTEG_ENABLE_PARTIAL_FRAC
            result = try_partial_fractions(expr, var);
            if (result) return result;
#endif

            return NULL;
        }

        /* ── Power: x^n, (ax+b)^n, a^x, e^(ax+b) ──────── */
        case OP_POW: {
            const ast_node_t *base = expr->d.binop.left;
            const ast_node_t *exp_node = expr->d.binop.right;

            /* x^n where n is constant */
            if (is_var(base, var) && is_const_wrt(exp_node, var))
                return integ_power_rule(base, var, exp_node);

            /* (ax+b)^n */
            result = try_linear_power(expr, var);
            if (result) return result;

            /* sec^2, csc^2 */
            result = try_sec2(expr, var);
            if (result) return result;
            result = try_csc2(expr, var);
            if (result) return result;

            /* sin^2, cos^2, tan^2 */
            result = try_trig_power(expr, var);
            if (result) return result;

            /* e^x, a^x, e^(ax+b) */
            result = try_exp_rule(expr, var);
            if (result) return result;

            return NULL;
        }

        default:
            return NULL;
        }
        break;

    /* ── Function calls ──────────────────────────────────── */
    case NODE_FUNC: {
        func_id_t fn = expr->d.func.id;
        const ast_node_t *arg = expr->d.func.arg;

        /* Direct: func(x) where arg is just the variable */
        if (is_var(arg, var))
            return integ_trig_basic(fn, var);

        /* u-substitution: func(ax+b) */
        {
            func_id_t fn_out;
            const ast_node_t *a_coeff, *inner;
            if (is_func_of_linear(expr, var, &fn_out, &a_coeff, &inner))
                return integ_func_linear(fn_out, inner, a_coeff);
        }

        /* Trig substitution: sqrt(a^2-x^2) */
#if CE_INTEG_ENABLE_TRIG_SUB
        result = try_trig_sub(expr, var);
        if (result) return result;
#endif

        return NULL;
    }

    default:
        return NULL;
    }

}

/* The recursive spine: the linear rules (negation, sums, constant
 * multiples and constant denominators) recurse through this small frame.
 * Every other rule is a leaf (or a strategy capped by depth > 8) and runs
 * in integ_rules(), whose frame is large because LTO inlines the rule
 * helpers into it; it is kept out of line so a chain such as x*2*2*...*2
 * or -(-(-x)) uses only the integ_node() frame at each level. */
static ast_node_t *integ_node(const ast_node_t *expr, char var, int depth)
{
    const ast_node_t *L, *R;
    ast_node_t *sub, *rsub;

    if (!expr) return NULL;
    if (depth > 8) return NULL; /* guard against deep recursion */

    /* ── Expression doesn't contain var → constant rule ── */
    if (!ast_contains_var(expr, var))
        return integ_const(expr, var);

    /* ── Unary negation: int(-f) = -int(f) ──────────────── */
    if (expr->type == NODE_UNOP && expr->d.unop.op == UOP_NEG) {
        sub = integ(expr->d.unop.operand, var, depth);
        if (!sub) return NULL;
        steps_addf("Negate: int(-f) = -int(f)");
        return ast_unop(UOP_NEG, sub);
    }

    if (expr->type == NODE_BINOP) {
        L = expr->d.binop.left;
        R = expr->d.binop.right;
        switch (expr->d.binop.op) {
        /* ── Sum / Difference rule ───────────────────────── */
        case OP_ADD:
        case OP_SUB:
            steps_addf("%s rule on %s",
                       expr->d.binop.op == OP_ADD ? "Sum" : "Difference",
                       integ_text(expr, 60));
            sub = integ(L, var, depth);
            rsub = integ(R, var, depth);
            if (!sub || !rsub) {
                ast_free(sub);
                ast_free(rsub);
                return NULL;
            }
            return ast_binop(expr->d.binop.op, sub, rsub);

        /* ── c*f(x), f(x)*c and f(x)/c ────────────────────── */
        case OP_MUL:
            if (is_const_wrt(L, var)) {
                const ast_node_t *t = L; L = R; R = t; /* L = f, R = c */
            } else if (!is_const_wrt(R, var)) {
                break;
            }
            goto const_factor;
        case OP_DIV:
            if (!is_const_wrt(R, var)) break;
        const_factor:
            if (expr->d.binop.op == OP_DIV)
                steps_addf("Const denom: int(...) / %s", integ_text(R, 60));
            else
                steps_addf("Factor: %s * int(...)", integ_text(R, 60));
            sub = integ(L, var, depth);
            if (!sub) return NULL;
            if (expr->d.binop.op == OP_DIV)
                return ast_binop(OP_DIV, sub, ast_clone(R));
            return ast_binop(OP_MUL, ast_clone(R), sub);

        default:
            break;
        }
    }

    return integ_rules(expr, var, depth);
}

/* ── Public API: Indefinite integral ─────────────────────── */

#if CE_INTEG_ENABLE_STRATEGY_REPORT
static const char *strategy_name(integ_strategy_t strategy)
{
    switch (strategy) {
    case INTEG_STRATEGY_AUTO:    return "auto";
    case INTEG_STRATEGY_PARTS:   return "parts";
    case INTEG_STRATEGY_TRIGSUB: return "trigsub";
    case INTEG_STRATEGY_USUB:    return "u-sub";
    case INTEG_STRATEGY_PARTIAL_FRAC: return "partial fractions";
    default:                     return "unknown";
    }
}

static bool has_trig_sub_pattern(const ast_node_t *expr, char var)
{
    ce_float_t a2 = 0.0f;
    if (!expr) return false;

    if (expr->type == NODE_FUNC && expr->d.func.id == FN_SQRT) {
        if (match_a2_minus_x2(expr->d.func.arg, var, &a2) ||
            match_x2_plus_a2(expr->d.func.arg, var, &a2) ||
            match_x2_minus_a2(expr->d.func.arg, var, &a2)) {
            return true;
        }
    }
    if (expr->type == NODE_BINOP) {
        return has_trig_sub_pattern(expr->d.binop.left, var) ||
               has_trig_sub_pattern(expr->d.binop.right, var);
    }
    if (expr->type == NODE_UNOP)
        return has_trig_sub_pattern(expr->d.unop.operand, var);
    if (expr->type == NODE_FUNC)
        return has_trig_sub_pattern(expr->d.func.arg, var);
    return false;
}
#endif

#if CE_INTEG_ENABLE_STRATEGY_API || CE_INTEG_ENABLE_STRATEGY_REPORT
static ast_node_t *probe_usub(const ast_node_t *expr, char var)
{
    ast_node_t *res = NULL;
    if (!expr) return NULL;

    if (expr->type == NODE_BINOP &&
        (expr->d.binop.op == OP_MUL || expr->d.binop.op == OP_DIV)) {
#if CE_INTEG_ENABLE_NONLINEAR_USUB
        res = try_nonlinear_usub(expr, var, 0);
        if (res) return res;
#endif
    }
    if (expr->type == NODE_FUNC) {
        func_id_t fn_out;
        const ast_node_t *a_coeff, *inner;
        if (is_func_of_linear(expr, var, &fn_out, &a_coeff, &inner)) {
            res = integ_func_linear(fn_out, inner, a_coeff);
            if (res) return res;
        }
    }
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_POW) {
        res = try_linear_power(expr, var);
        if (res) return res;
    }
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_DIV)
        return try_recip(expr, var);

    return NULL;
}
#endif

#if CE_INTEG_ENABLE_STRATEGY_API
ast_node_t *integrate_with_strategy(const ast_node_t *expr, char var,
                                    integ_strategy_t strategy,
                                    const ast_node_t *u_hint)
{
    ast_node_t *result = NULL;
    ast_node_t *simplified = NULL;

    integ_begin();
    if (!expr) {
        set_integrate_fail_reason("Integral input was null");
        return NULL;
    }

    set_integrate_fail_reason("No symbolic solution found");
    steps_addf("int(%s) d%c", integ_text(expr, 120), var);

    switch (strategy) {
    case INTEG_STRATEGY_AUTO:
        result = integ(expr, var, 0);
        if (!result)
            set_integrate_fail_reason(
                "No symbolic antiderivative found");
        break;

    case INTEG_STRATEGY_PARTS:
#if CE_INTEG_ENABLE_PARTS
        if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
            result = try_parts(expr, var, 0);
            if (!result) {
                set_integrate_fail_reason(INTEG_PARTS_NO_SPLIT_MSG);
            }
        } else {
            set_integrate_fail_reason(INTEG_PARTS_NOT_PRODUCT_MSG);
        }
#else
        set_integrate_fail_reason("Integration by parts is not enabled in this worker");
#endif
        break;

    case INTEG_STRATEGY_TRIGSUB:
#if CE_INTEG_ENABLE_TRIG_SUB
        result = try_trig_sub(expr, var);
        if (!result) {
            set_integrate_fail_reason(
                "Trig substitution pattern not detected (sqrt(a^2-x^2), "
                "sqrt(x^2+a^2), sqrt(x^2-a^2), or reciprocal forms)");
        }
#else
        set_integrate_fail_reason("Trig substitution is not enabled in this worker");
#endif
        break;

    case INTEG_STRATEGY_USUB:
#if CE_INTEG_ENABLE_NONLINEAR_USUB
        if (u_hint) {
            result = try_usub_with_inner(expr, var, u_hint, 0);
            if (!result) {
                steps_addf("u-sub with u_hint failed: derivative of u does not"
                           " appear as a factor in the integrand, so the"
                           " substitution cannot cancel d%c", var);
                set_integrate_fail_reason(
                    "Forced u-substitution failed: derivative of supplied u"
                    " is not a factor of the integrand");
            }
        } else {
            result = probe_usub(expr, var);
            if (!result) {
                steps_addf("Nonlinear u-substitution scan: no composite"
                           " sub-expression u found such that du/d%c appears"
                           " as a multiplicative factor - u-sub does not apply"
                           " to this integrand in standard form", var);
                set_integrate_fail_reason(
                    "u-substitution not applicable: no inner function u found"
                    " whose derivative is a factor of the integrand");
            }
        }
#else
        set_integrate_fail_reason("u-substitution is not enabled in this worker");
#endif
        break;

    case INTEG_STRATEGY_PARTIAL_FRAC:
#if CE_INTEG_ENABLE_PARTIAL_FRAC
        result = try_partial_fractions(expr, var);
        if (!result) {
            set_integrate_fail_reason(
                "Partial fractions requires a rational form that decomposes cleanly");
        }
#else
        set_integrate_fail_reason("Partial fractions are not enabled in this worker");
#endif
        break;

    default:
        set_integrate_fail_reason("Unknown integration strategy");
        return NULL;
    }

    if (!result) {
        /* An integral hit the nesting cap: say so rather than "pattern
         * not detected". */
        if (g_integ_too_deep) set_integrate_fail_reason(INTEG_TOO_DEEP_MSG);
        else if (mem_alloc_failures() != g_integ_alloc_start)
            set_integrate_fail_reason(INTEG_OOM_MSG);
        return NULL;
    }

    simplified = integ_finish(result);
    if (!simplified) return NULL;
    steps_add("+ C");
    set_integrate_fail_reason("No failure");
    return simplified;
}

#endif

const char *integrate_last_failure_reason(void)
{
    return g_integrate_fail_reason[0] ? g_integrate_fail_reason
                                      : CE_INTEG_DEFAULT_FAIL_REASON;
}

bool integrate_hit_depth_cap(void)
{
    return g_integ_too_deep;
}

#if CE_INTEG_ENABLE_STRATEGY_REPORT
void integrate_strategy_report(const ast_node_t *expr, char var,
                               char *buf, int buf_size)
{
    typedef struct {
        integ_strategy_t strategy;
        int score;
        const char *reason;
    } rank_item_t;
    rank_item_t ranks[5];
    int i, j;
    int pos = 0;

    if (!buf || buf_size <= 0)
        return;
    buf[0] = '\0';

    if (!expr) {
        snprintf(buf, buf_size, "No expression supplied");
        return;
    }

    ranks[0].strategy = INTEG_STRATEGY_AUTO;
    ranks[1].strategy = INTEG_STRATEGY_USUB;
    ranks[2].strategy = INTEG_STRATEGY_PARTS;
    ranks[3].strategy = INTEG_STRATEGY_TRIGSUB;
    ranks[4].strategy = INTEG_STRATEGY_PARTIAL_FRAC;
    ranks[0].score = 45; ranks[0].reason = "baseline general solver";
    ranks[1].score = 25; ranks[1].reason = "composition substitution candidate";
    ranks[2].score = 20; ranks[2].reason = "product reduction candidate";
    ranks[3].score = 15; ranks[3].reason = "radical trig-pattern candidate";
    ranks[4].score = 18; ranks[4].reason = "rational decomposition candidate";

    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        ranks[1].score += 18;
        ranks[2].score += 22;
    }
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_DIV) {
        ranks[1].score += 14;
        ranks[3].score += 10;
        ranks[4].score += 24;
    }
    if (expr->type == NODE_FUNC && expr->d.func.arg &&
        ast_contains_var(expr->d.func.arg, var) &&
        !is_var(expr->d.func.arg, var)) {
        ranks[1].score += 20;
    }
    if (has_trig_sub_pattern(expr, var))
        ranks[3].score += 40;

    {
        int saved = steps_count();
        ast_node_t *probe = integ(expr, var, 0);
        steps_restore(saved);
        if (probe) {
            ranks[0].score += 35;
            ast_free(probe);
        }
    }
    {
        int saved = steps_count();
        ast_node_t *probe = probe_usub(expr, var);
        steps_restore(saved);
        if (probe) {
            ranks[1].score += 36;
            ast_free(probe);
        }
    }
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        int saved = steps_count();
        ast_node_t *probe = try_parts(expr, var, 0);
        steps_restore(saved);
        if (probe) {
            ranks[2].score += 36;
            ast_free(probe);
        }
    }
    {
        int saved = steps_count();
        ast_node_t *probe = try_trig_sub(expr, var);
        steps_restore(saved);
        if (probe) {
            ranks[3].score += 36;
            ast_free(probe);
        }
    }
    {
        int saved = steps_count();
        ast_node_t *probe = try_partial_fractions(expr, var);
        steps_restore(saved);
        if (probe) {
            ranks[4].score += 36;
            ast_free(probe);
        }
    }

    for (i = 0; i < 5; i++) {
        if (ranks[i].score < 0) ranks[i].score = 0;
        if (ranks[i].score > 99) ranks[i].score = 99;
    }

    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 5; j++) {
            if (ranks[j].score > ranks[i].score) {
                rank_item_t tmp = ranks[i];
                ranks[i] = ranks[j];
                ranks[j] = tmp;
            }
        }
    }

    pos += snprintf(buf + pos, (pos < buf_size) ? buf_size - pos : 0,
                    "Strategy ranking for int(expr,d%c):", var);
    for (i = 0; i < 5 && pos < buf_size; i++) {
        pos += snprintf(buf + pos, (pos < buf_size) ? buf_size - pos : 0,
                        "\n%d) %s (score %d): %s",
                        i + 1,
                        strategy_name(ranks[i].strategy),
                        ranks[i].score,
                        ranks[i].reason);
    }
}
#endif

#if CE_INTEG_ENABLE_ODE
/* Reduce a left-associated sum of the independent variable before cloning.
 * This bounded walk avoids both the integration depth cap and pool pressure.
 * Mixed terms and other tree shapes keep the existing clone path. */
static ast_node_t *ode_clone_x_sum(const ast_node_t *rhs, char x_var)
{
    const ast_node_t *n = rhs;
    unsigned terms = 1;
    while (n && n->type == NODE_BINOP && n->d.binop.op == OP_ADD &&
           is_var(n->d.binop.right, x_var)) {
        ++terms;
        n = n->d.binop.left;
    }
    if (terms > 1 && is_var(n, x_var))
        return ast_binop(OP_MUL, ast_number(terms), ast_variable(x_var));
    return ast_clone(rhs);
}

static bool split_separable_rhs(const ast_node_t *rhs, char x_var, char y_var,
                                ast_node_t **lhs_y, ast_node_t **rhs_x)
{
    if (!rhs || !lhs_y || !rhs_x) return false;
    *lhs_y = NULL;
    *rhs_x = NULL;

    if (!ast_contains_var(rhs, y_var)) {
        *lhs_y = ast_number(1);
        *rhs_x = ode_clone_x_sum(rhs, x_var);
    } else if (!ast_contains_var(rhs, x_var)) {
        *lhs_y = ast_binop(OP_DIV, ast_number(1), ast_clone(rhs));
        *lhs_y = ast_simplify(*lhs_y);
        *rhs_x = ast_number(1);
    } else if (rhs->type == NODE_UNOP && rhs->d.unop.op == UOP_NEG) {
        ast_node_t *tmp_l = NULL;
        ast_node_t *tmp_r = NULL;
        if (!split_separable_rhs(rhs->d.unop.operand, x_var, y_var, &tmp_l, &tmp_r))
            return false;
        *lhs_y = tmp_l;
        *rhs_x = ast_simplify(ast_unop(UOP_NEG, tmp_r));
    } else if (rhs->type == NODE_BINOP && rhs->d.binop.op == OP_MUL) {
        const ast_node_t *L = rhs->d.binop.left;
        const ast_node_t *R = rhs->d.binop.right;
        if (ast_contains_var(L, x_var) && !ast_contains_var(L, y_var) &&
            ast_contains_var(R, y_var) && !ast_contains_var(R, x_var)) {
            *lhs_y = ast_binop(OP_DIV, ast_number(1), ast_clone(R));
            *lhs_y = ast_simplify(*lhs_y);
            *rhs_x = ast_clone(L);
        } else if (ast_contains_var(R, x_var) && !ast_contains_var(R, y_var) &&
                   ast_contains_var(L, y_var) && !ast_contains_var(L, x_var)) {
            *lhs_y = ast_binop(OP_DIV, ast_number(1), ast_clone(L));
            *lhs_y = ast_simplify(*lhs_y);
            *rhs_x = ast_clone(R);
        }
    } else if (rhs->type == NODE_BINOP && rhs->d.binop.op == OP_DIV) {
        const ast_node_t *N = rhs->d.binop.left;
        const ast_node_t *D = rhs->d.binop.right;
        if (ast_contains_var(N, x_var) && !ast_contains_var(N, y_var) &&
            ast_contains_var(D, y_var) && !ast_contains_var(D, x_var)) {
            *lhs_y = ast_clone(D);
            *rhs_x = ast_clone(N);
        } else if (ast_contains_var(N, y_var) && !ast_contains_var(N, x_var) &&
                   ast_contains_var(D, x_var) && !ast_contains_var(D, y_var)) {
            *lhs_y = ast_binop(OP_DIV, ast_number(1), ast_clone(N));
            *lhs_y = ast_simplify(*lhs_y);
            *rhs_x = ast_binop(OP_DIV, ast_number(1), ast_clone(D));
            *rhs_x = ast_simplify(*rhs_x);
        }
    }

    if (!*lhs_y || !*rhs_x) {
        ast_free(*lhs_y); *lhs_y = NULL;
        ast_free(*rhs_x); *rhs_x = NULL;
        return false;
    }
    if (ast_contains_var(*lhs_y, x_var) || ast_contains_var(*rhs_x, y_var)) {
        ast_free(*lhs_y); *lhs_y = NULL;
        ast_free(*rhs_x); *rhs_x = NULL;
        return false;
    }
    return true;
}

static bool split_linear_rhs(const ast_node_t *rhs, char y_var,
                             ast_node_t **p_out, ast_node_t **q_out)
{
    ast_node_t *a = NULL;
    ast_node_t *zero = NULL;
    ast_node_t *q = NULL;
    ast_node_t *p = NULL;

    if (!rhs || !p_out || !q_out) return false;
    *p_out = NULL;
    *q_out = NULL;

    a = deriv_diff_quiet(rhs, y_var);
    if (!a) return false;
    a = ast_simplify(a);
    if (!a) return false;

    if (ast_contains_var(a, y_var)) {
        ast_free(a);
        return false;
    }

    zero = ast_number(0);
    if (!zero) {
        ast_free(a);
        return false;
    }
    q = substitute_var(rhs, y_var, zero);
    ast_free(zero);
    q = ast_simplify(q);
    if (!q) {
        ast_free(a);
        return false;
    }
    if (ast_contains_var(q, y_var)) {
        ast_free(a);
        ast_free(q);
        return false;
    }

    p = ast_simplify(ast_unop(UOP_NEG, ast_clone(a)));
    ast_free(a);
    if (!p) {
        ast_free(q);
        return false;
    }
    if (ast_contains_var(p, y_var)) {
        ast_free(p);
        ast_free(q);
        return false;
    }

    *p_out = p;
    *q_out = q;
    return true;
}

bool ode_solve_separable(const ast_node_t *rhs, char x_var, char y_var,
                         char *result_buf, int buf_size)
{
    ast_node_t *lhs_y = NULL, *rhs_x = NULL;
    ast_node_t *lhs_int = NULL, *rhs_int = NULL;
    int pos;

    if (!result_buf || buf_size <= 0)
        return false;
    result_buf[0] = '\0';

    if (!rhs) {
        snprintf(result_buf, buf_size, "ODE input missing");
        return false;
    }
    if (x_var == y_var) {
        snprintf(result_buf, buf_size,
                 "ODE variable mismatch: independent and dependent variables must differ");
        return false;
    }

    if (!split_separable_rhs(rhs, x_var, y_var, &lhs_y, &rhs_x)) {
        snprintf(result_buf, buf_size,
                 "Not separable: cannot rewrite dy/d%c = f(%c)g(%c)",
                 x_var, x_var, y_var);
        return false;
    }

    lhs_int = integrate(lhs_y, y_var);
    if (!lhs_int) {
        snprintf(result_buf, buf_size,
                 "Separable ODE failed: could not integrate y-side (%s)",
                 integrate_last_failure_reason());
        ast_free(lhs_y);
        ast_free(rhs_x);
        return false;
    }
    rhs_int = integrate(rhs_x, x_var);
    if (!rhs_int) {
        snprintf(result_buf, buf_size,
                 "Separable ODE failed: could not integrate x-side (%s)",
                 integrate_last_failure_reason());
        ast_free(lhs_y);
        ast_free(rhs_x);
        ast_free(lhs_int);
        return false;
    }

    /* The answer is printed straight into result_buf: per-part 120 B
     * buffers here sat under the deepest integration frames on the CE
     * stack. ast_to_display_str() returns at most size-1. */
    pos = ast_to_display_str(lhs_int, result_buf, buf_size);
    pos += snprintf(result_buf + pos, buf_size - pos, " = ");
    if (pos < buf_size) {
        pos += ast_to_display_str(rhs_int, result_buf + pos, buf_size - pos);
        if (pos < buf_size)
            pos += snprintf(result_buf + pos, buf_size - pos, " + C");
    }

    /* A clipped AST fills its remaining space. The required suffix then
     * makes pos reach capacity, including when only a literal was cut.
     * Replace partial math with a refusal, never a balanced-looking prefix.
     * A cut marker from a printer guard that stops early also refuses. */
    if (pos < buf_size && strstr(result_buf, "...")) pos = buf_size;
    if (pos >= buf_size) snprintf(result_buf, buf_size, "%s", ODE_TOO_LONG_MSG);

#ifndef CE_DISABLE_STEP_RECORDING
    {
        char rhs_expr[120];
        ast_to_str(rhs, rhs_expr, sizeof(rhs_expr));
        steps_addf("ODE separable: dy/d%c = %s", x_var, rhs_expr);
    }
#endif
    steps_addf("Separate: [%c-side] d%c = [%c-side] d%c", y_var, y_var, x_var, x_var);
    steps_addf("Integrate both sides");
    if (pos < buf_size) steps_addf("Solution: %s", result_buf);

    ast_free(lhs_y);
    ast_free(rhs_x);
    ast_free(lhs_int);
    ast_free(rhs_int);
    return pos < buf_size;
}

/* For constant nonzero P and affine Q, one closed-form parts step gives
 * integral(exp(P*x)*Q) = exp(P*x)*(Q/P - Q'/P/P).
 * Reuse the existing affine matcher. Do not enable recursive parts or
 * forward ODE state through the standalone integration strategy workers. */
static ast_node_t *ode_integrate_affine(const ast_node_t *p,
                                       const ast_node_t *q,
                                       const ast_node_t *mu, char var)
{
    const ast_node_t *slope, *offset;
    ast_node_t *body;

    if (!p || p->type != NODE_NUMBER || p->d.number == 0 ||
        !ce_isfinite(p->d.number) ||
        !is_linear(q, var, &slope, &offset) ||
        slope->type != NODE_NUMBER || !ce_isfinite(slope->d.number))
        return NULL;

    body = ast_binop(OP_SUB,
        ast_binop(OP_DIV, ast_clone(q), ast_clone(p)),
        ast_binop(OP_DIV,
            ast_binop(OP_DIV, ast_clone(slope), ast_clone(p)),
            ast_clone(p)));
    /* Constructors can retain NULL children after pool exhaustion.
     * integ_finish checks completeness before simplifying. */
    return integ_finish(ast_binop(OP_MUL, ast_clone(mu), body));
}

bool ode_solve_linear(const ast_node_t *rhs, char x_var, char y_var,
                      char *result_buf, int buf_size)
{
    ast_node_t *p = NULL, *q = NULL;
    ast_node_t *int_p = NULL, *mu = NULL;
    ast_node_t *mu_q = NULL, *int_mu_q = NULL;
    int pos;

    if (!result_buf || buf_size <= 0)
        return false;
    result_buf[0] = '\0';

    if (!rhs) {
        snprintf(result_buf, buf_size, "ODE input missing");
        return false;
    }
    if (x_var == y_var) {
        snprintf(result_buf, buf_size,
                 "ODE variable mismatch: independent and dependent variables must differ");
        return false;
    }

    if (!split_linear_rhs(rhs, y_var, &p, &q)) {
        snprintf(result_buf, buf_size,
                 "Not linear in %c: expected d%c/d%c + P(%c)%c = Q(%c)",
                 y_var, y_var, x_var, x_var, y_var, x_var);
        return false;
    }

    int_p = integrate(p, x_var);
    if (!int_p) {
        snprintf(result_buf, buf_size,
                 "Linear ODE failed: could not integrate P(%c) (%s)",
                 x_var, integrate_last_failure_reason());
        ast_free(p);
        ast_free(q);
        return false;
    }

    mu = ast_func(FN_EXP, ast_clone(int_p));
    if (!mu) {
        snprintf(result_buf, buf_size, "Linear ODE failed: out of memory");
        ast_free(p);
        ast_free(q);
        ast_free(int_p);
        return false;
    }

    mu_q = ast_simplify(ast_binop(OP_MUL, ast_clone(mu), ast_clone(q)));
    if (!mu_q) {
        snprintf(result_buf, buf_size, "Linear ODE failed: out of memory");
        ast_free(p);
        ast_free(q);
        ast_free(int_p);
        ast_free(mu);
        return false;
    }

    int_mu_q = integrate(mu_q, x_var);
    if (!int_mu_q && !g_integ_too_deep &&
        mem_alloc_failures() == g_integ_alloc_start)
        int_mu_q = ode_integrate_affine(p, q, mu, x_var);
    if (!int_mu_q) {
        snprintf(result_buf, buf_size,
                 "Linear ODE failed: could not integrate mu*Q (%s)",
                 integrate_last_failure_reason());
        ast_free(p);
        ast_free(q);
        ast_free(int_p);
        ast_free(mu);
        ast_free(mu_q);
        return false;
    }

    /* Printed straight into result_buf, as in ode_solve_separable(). */
    pos = snprintf(result_buf, buf_size, "%c = (", y_var);
    if (pos < buf_size) {
        pos += ast_to_display_str(int_mu_q, result_buf + pos, buf_size - pos);
        if (pos < buf_size)
            pos += snprintf(result_buf + pos, buf_size - pos, " + C)/(");
        if (pos < buf_size) {
            pos += ast_to_display_str(mu, result_buf + pos, buf_size - pos);
            if (pos < buf_size)
                pos += snprintf(result_buf + pos, buf_size - pos, ")");
        }
    }

    if (pos < buf_size && strstr(result_buf, "...")) pos = buf_size;
    if (pos >= buf_size) snprintf(result_buf, buf_size, "%s", ODE_TOO_LONG_MSG);

    steps_addf("ODE linear: d%c/d%c + P(%c)%c = Q(%c)",
               y_var, x_var, x_var, y_var, x_var);
#ifndef CE_DISABLE_STEP_RECORDING
    {
        char sbuf[120];
        ast_to_str(p, sbuf, sizeof(sbuf));
        steps_addf("P(%c) = %s", x_var, sbuf);
        ast_to_str(q, sbuf, sizeof(sbuf));
        steps_addf("Q(%c) = %s", x_var, sbuf);
        ast_to_str(mu, sbuf, sizeof(sbuf));
        steps_addf("Integrating factor mu(%c) = %s", x_var, sbuf);
    }
#endif
    if (pos < buf_size) steps_addf("Solution: %s", result_buf);

    ast_free(p);
    ast_free(q);
    ast_free(int_p);
    ast_free(mu);
    ast_free(mu_q);
    ast_free(int_mu_q);
    return pos < buf_size;
}
#endif

ast_node_t *integrate(const ast_node_t *expr, char var)
{
#if CE_INTEG_ENABLE_STRATEGY_API
    return integrate_with_strategy(expr, var, INTEG_STRATEGY_AUTO, NULL);
#else
    ast_node_t *result;
    ast_node_t *simplified;

    integ_begin();
    if (!expr) {
        set_integrate_fail_reason("Integral input was null");
        return NULL;
    }

    steps_addf("int(%s) d%c", integ_text(expr, 120), var);

    result = integ(expr, var, 0);
    if (!result) {
        set_integrate_fail_reason(g_integ_too_deep
            ? INTEG_TOO_DEEP_MSG
            : mem_alloc_failures() != g_integ_alloc_start
            ? INTEG_OOM_MSG : "No symbolic antiderivative found");
        return NULL;
    }

    simplified = integ_finish(result);
    if (!simplified) return NULL;

    steps_add("+ C");
    set_integrate_fail_reason("No failure");
    return simplified;
#endif
}

/* ── Public API: Definite integral via FTC ───────────────── */

#if CE_INTEG_ENABLE_DEFINITE
/* An integer that float32 holds exactly (|v| < 2^24). */
static inline bool integ_exact_int(ce_float_t v)
{
    return fabsf(v) < 16777216.0f && v == (ce_float_t)(long)v;
}

static ce_float_t integ_err_scale(const ast_node_t *n, char var, ce_float_t x);

ce_float_t integrate_definite(const ast_node_t *expr, char var,
                              ce_float_t lo, ce_float_t hi,
                              char *antideriv_buf, int buf_size,
                              ce_error_t *err)
{
    ast_node_t *F;
    eval_ctx_t ctx;
    ce_float_t F_hi, F_lo, result;
    *err = CE_OK;

    /* Step 1: Find the antiderivative */
    F = integrate(expr, var);
    if (!F) {
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }

    /* Store antiderivative string if buffer provided */
#if CE_INTEG_DEFINITE_PRINT_F
    if (antideriv_buf && buf_size > 0)
        ast_to_display_str(F, antideriv_buf, buf_size);
#else
    (void)antideriv_buf;
    (void)buf_size;
#endif

    /* Step 2: Evaluate at upper bound */
    steps_addf("Evaluate at upper bound %c=%.4g:",
               var, (double)hi);

    eval_init(&ctx);
    eval_set_var(&ctx, var, hi);
    F_hi = eval_expr(&ctx, F);
    if (eval_error(&ctx) != CE_OK) {
        ce_error_t ev_err = eval_error(&ctx);
        if (ev_err == CE_ERR_DIV_ZERO || ev_err == CE_ERR_DOMAIN ||
            ev_err == CE_ERR_OVERFLOW) {
            steps_addf("  F(%.4g) = DIVERGES (singularity at upper bound)", (double)hi);
            steps_addf("Antiderivative is non-finite at a bound - integral DIVERGES");
            *err = CE_ERR_OVERFLOW;
        } else {
            *err = ev_err;
        }
        ast_free(F);
        return 0;
    }
    if (!ce_isfinite(F_hi))
        steps_addf("  F(%.4g) = DIVERGES (non-finite antiderivative at upper bound)",
                   (double)hi);
    else
        steps_addf("  F(%.4g) = %.6g", (double)hi, (double)F_hi);

    /* Step 3: Evaluate at lower bound */
    steps_addf("Evaluate at lower bound %c=%.4g:",
               var, (double)lo);

    eval_set_var(&ctx, var, lo);
    F_lo = eval_expr(&ctx, F);
    if (eval_error(&ctx) != CE_OK) {
        ce_error_t ev_err = eval_error(&ctx);
        if (ev_err == CE_ERR_DIV_ZERO || ev_err == CE_ERR_DOMAIN ||
            ev_err == CE_ERR_OVERFLOW) {
            steps_addf("  F(%.4g) = DIVERGES (singularity at lower bound)", (double)lo);
            steps_addf("Antiderivative is non-finite at a bound - integral DIVERGES");
            *err = CE_ERR_OVERFLOW;
        } else {
            *err = ev_err;
        }
        ast_free(F);
        return 0;
    }
    if (!ce_isfinite(F_lo))
        steps_addf("  F(%.4g) = DIVERGES (non-finite antiderivative at lower bound)",
                   (double)lo);
    else
        steps_addf("  F(%.4g) = %.6g", (double)lo, (double)F_lo);

    /* Guard: if either endpoint is non-finite, the definite integral diverges */
    if (!ce_isfinite(F_hi) || !ce_isfinite(F_lo)) {
        steps_addf("Antiderivative is non-finite at a bound - integral DIVERGES");
        *err = CE_ERR_OVERFLOW;
        ast_free(F);
        return 0;
    }

    /* Step 4: Subtract F(b) - F(a). Both are finite, but the difference
     * can overflow, and CEdev float subtraction is not relied on to produce
     * inf. Halves cannot overflow; the doubled result is kept only when it
     * still fits (int(1e38,x,2,3) = 1e38 evaluates, int(1e38,x,-2,2) does
     * not). */
    result = 0.5f * F_hi - 0.5f * F_lo;
    if (fabsf(result) >= 1.7e38f) {
        *err = CE_ERR_OVERFLOW;
        ast_free(F);
        return 0;
    }
    result += result;
    if (hi == lo) result = 0;  /* exact, whatever F's rounding */

    /* Step 5: refuse a difference that float32 rounding swamps
     * (review round 9: int(x-100000,x,100000,100001) gave -512, not 0.5).
     * u is the rounding-error estimate of F(b) - F(a) (an overestimate
     * that models CEdev's libm, not glibc's, typically 2-3x the real
     * error) and ub the part from bounds that float32 cannot hold exactly.
     * The value is kept when u is under 2^-13 of it and ub under 2^-12
     * (errors stay below the 4th significant digit; noise in the last
     * digits is the engine's float32 resolution). A result within 2(u + ub) of zero is reported as 0 when
     * that is also under 2^-17 of the integrand's own size |b-a| max|f|
     * (int(sin(x),x,0,2pi) = 0). Anything else is refused. */
    {
        ce_float_t sb = integ_err_scale(F, var, hi);
        ce_float_t sa = integ_err_scale(F, var, lo);
        ce_float_t fb, fa, u = -1, ub = 0;
        /* A bound that is not an integer is itself rounded to float32
         * (10.001 is off by 4e-7), which moves F(b) by about f(b) b 2^-24;
         * cancellation amplifies that too (int(e^x,x,10,10.001) printed
         * 22.0469, not 22.0375). Integers below 2^24 are exact. */
        if (integrate_eval_at(expr, var, hi, &fb) && integrate_eval_at(expr, var, lo, &fa)) {
            u = sb + sa;
            if (!integ_exact_int(hi)) ub += fabsf(fb) * 5.9604645e-8f * fabsf(hi);
            if (!integ_exact_int(lo)) ub += fabsf(fa) * 5.9604645e-8f * fabsf(lo);
        }
        if (sb < 0 || sa < 0 || !(u >= 0) || !ce_isfinite(u)) {
            u = -1;
        } else if (u * 8192.0f + ub * 4096.0f > fabsf(result) && hi != lo) {
            /* Rounding in F under 2^-13, bound representation under 2^-12. */
            ce_float_t fmax = fabsf(fa) > fabsf(fb) ? fabsf(fa) : fabsf(fb);
            ce_float_t fx, t = 0;
            int k;
            u += ub;
            /* Interior points at golden-ratio fractions, which no simple
             * period lands on (k/4 points all hit zeros of sin(2x) on
             * [0, 2pi], review round 10). */
            for (k = 0; k < 9; k++) {
                t += 0.618034f;
                if (t >= 1.0f) t -= 1.0f;
                if (integrate_eval_at(expr, var, lo + (hi - lo) * t, &fx) &&
                    ce_isfinite(fx) && fabsf(fx) > fmax)
                    fmax = fabsf(fx);
            }
            if (fabsf(result) <= 2.0f * u &&
                u * 131072.0f <= fabsf(hi - lo) * fmax) {  /* 2^17 */
                result = 0;
            } else {
                u = -1;
            }
        }
        if (u < 0) {
            *err = CE_ERR_PRECISION;
            ast_free(F);
            return 0;
        }
    }
    steps_addf("Subtract: F(%.4g)-F(%.4g)", (double)hi, (double)lo);
    steps_addf("= %.6g - %.6g = %.6g",
               (double)F_hi, (double)F_lo, (double)result);

    ast_free(F);
    return result;
}

/* ── Public API: Improper integral ───────────────────────── */

/*
 * Helper: evaluate limit of F(x) as x → ±infinity.
 * Numerically approaches ±inf and checks convergence.
 * Returns true if converges, sets *val.
 */
#endif

/* Evaluate F at var = x in a fresh context. False on any eval error. Shared by
 * the improper-integral limit probes so each probe costs one call on eZ80. */
bool integrate_eval_at(const ast_node_t *F, char var, ce_float_t x,
                       ce_float_t *out)
{
    eval_ctx_t ctx;
    eval_init(&ctx);
    eval_set_var(&ctx, var, x);
    *out = eval_expr(&ctx, F);
    return eval_error(&ctx) == CE_OK;
}

#if CE_INTEG_ENABLE_DEFINITE
/* Rounding-error estimate of evaluating n at var = x: 2^-24 (one float32
 * rounding) times the magnitudes summed over n's operations on var. The 2^-24 is
 * applied per node, so the estimate does not overflow for representable
 * values (int(1e38,x,2,3)). Added terms contribute their own
 * magnitudes, so cancellation inside n shows: x^2/2 - 100000*x at
 * x = 100001 is about -5e9 but its scale is about 1e11. Errors in
 * function and power arguments are carried through a finite-difference
 * slope. Returns a negative value when n cannot be evaluated or the
 * estimate overflows; callers then refuse.
 *
 * Stack use: the estimate used to recurse once per node, about
 * 125 B of CE stack per level, so the antiderivative of a 20-term sum hit
 * the stack-limit NMI (with #125's eval backstop, it refused instead).
 * integ_err_node() is that per-node rule, with the estimates of the node's
 * children passed in as sl (operand, argument or left operand) and sr
 * (right operand). integ_err_scale() walks n in post order over a static
 * stack, so its CE stack use no longer grows with n's depth; only the
 * evaluations of n's subtrees do. None of those is deeper in nodes than
 * the evaluation of the whole antiderivative that integrate_definite() has
 * already made, but the last one (n itself) runs about 250 B lower on the
 * stack, under this walk, integ_err_node() and integrate_eval_at()'s
 * context. eval_expr()'s eZ80 stack backstop (#125) covers that gap: a
 * refusal there fails the estimate, so the integral is refused (as
 * CE_ERR_PRECISION; the sticky g_eval_refused reports it as "Eval
 * recursion limit"), never answered. The result is the old one (see
 * INTEG_ERR_MAX_DEPTH). */
/* One entry per open node on a path from the root. The antiderivative has
 * already been evaluated by then, and eval_expr() refuses a path longer than
 * CE_EVAL_MAX_DEPTH nodes, so at most CE_EVAL_MAX_DEPTH - 1 nodes above the
 * leaf are ever open (one slot is spare; do not "tighten" the size by
 * that). The walk therefore never refuses an estimate the old recursion
 * could give. A deeper path (one that eval skipped) refuses. */
#define INTEG_ERR_MAX_DEPTH CE_EVAL_MAX_DEPTH

/* The walk takes a negation's operand and a function's argument as
 * d.binop.left. */
typedef char integ_err_child_layout[
    (offsetof(ast_node_t, d.binop.left) == offsetof(ast_node_t, d.unop.operand) &&
     offsetof(ast_node_t, d.binop.left) == offsetof(ast_node_t, d.func.arg)) ? 1 : -1];

/* True when integ_err_node() uses n's children: n has var and is a negation,
 * a function or a binop. Every other node's estimate is its own. */
static bool integ_err_inner(const ast_node_t *n, char var)
{
    return n && (n->type == NODE_BINOP || n->type == NODE_FUNC ||
                 (n->type == NODE_UNOP && n->d.unop.op == UOP_NEG)) &&
           ast_contains_var(n, var);
}

static CE_INTEG_NOINLINE ce_float_t integ_err_node(const ast_node_t *n, char var,
                                                   ce_float_t x, ce_float_t sl,
                                                   ce_float_t sr)
{
    const ce_float_t K = 5.9604645e-8f;  /* 2^-24: one float32 rounding */
    ce_float_t v, v0, l, r;

    if (!n || !integrate_eval_at(n, var, x, &v0) || !ce_isfinite(v0)) return -1;
    /* Inputs count as exact: the bound itself, numbers, named constants
     * and subexpressions without the variable ((2+1), 2/3, e). How float32
     * represents them is the input's own resolution, the same at both
     * bounds; only rounding in operations on the variable can cancel. */
    if (!ast_contains_var(n, var) || n->type == NODE_VARIABLE) return 0;
    v = fabsf(v0) * K;
    if (n->type == NODE_UNOP && n->d.unop.op == UOP_NEG)
        return sl;
    if (n->type == NODE_FUNC) {
        ast_node_t num, fn;
        ce_float_t a, a2, v2;
        if (sl < 0 || !integrate_eval_at(n->d.func.arg, var, x, &a)) return -1;
        /* abs only clears a sign bit. ln(1) is exactly zero when its
         * argument is exact; do not invent uncertainty for this identity.
         * This permits exact-zero log-tail integrals without relaxing the
         * relative rounding guard for near-cancellation. */
        if (n->d.func.id == FN_ABS) return sl;
        if (n->d.func.id == FN_LN && a == 1.0f && sl == 0 && v0 == 0) return 0;
        /* Slope of f at a. sin/cos use the bound 1 (a secant can alias
         * the period: a relative step at a = 6434 spans one, round 10).
         * Others use the secant |f(a + d) - f(a)| / d with
         * d = max(2^-10 min(|a|, 1), 2^-20 |a|), stepping the other way at
         * a domain edge (asin at 1); tan/sec/csc/cot past 1e5 refuse. */
        if (n->d.func.id == FN_SIN || n->d.func.id == FN_COS) {
            v2 = 1.0f;  /* |slope| <= 1; a secant can alias the period */
        } else {
            ce_float_t d = fabsf(a);
            /* Other trig functions are periodic too; past 1e5 the step
             * below could span a period, and CEdev's float argument
             * reduction is meaningless there anyway. */
            if (n->d.func.id <= FN_COT && d > 1e5f) return -1;
            d = (d > 0 && d < 1.0f ? d : 1.0f) * 0.0009765625f;
            /* Keep a + d distinct from a (atan at 20000, round 11). */
            if (d < fabsf(a) * 9.5367432e-7f) d = fabsf(a) * 9.5367432e-7f;  /* 2^-20 */
            memset(&num, 0, sizeof(num));
            num.type = NODE_NUMBER;
            fn = *n;
            fn.d.func.arg = &num;
            a2 = a + d;
            num.d.number = a2;
            if (!integrate_eval_at(&fn, var, x, &v2) || !ce_isfinite(v2)) {
                a2 = a - d;
                num.d.number = a2;
                if (!integrate_eval_at(&fn, var, x, &v2) || !ce_isfinite(v2)) return -1;
            }
            if (a2 == a) return -1;
            v2 = fabsf(v2 - v0) / fabsf(a2 - a);
        }
        /* Error in the argument, plus the library's own error modelled as
         * an argument off by one rounding: CEdev reduces trig arguments in
         * float, so cos(10001) is off by about 10001 * 2^-24. */
        v2 = v + v2 * (sl + fabsf(a) * K);
        return ce_isfinite(v2) ? v2 : -1;
    }
    if (n->type != NODE_BINOP) return v;
    if (sl < 0 || sr < 0 ||
        !integrate_eval_at(n->d.binop.left, var, x, &l) ||
        !integrate_eval_at(n->d.binop.right, var, x, &r))
        return -1;
    /* Integer arithmetic on exact integer operands with an integer result
     * below 2^24 rounds nothing (x^2 at x = 1001 is 1002001). The operands
     * must be integers too: 1.1 * 8000000 rounds to the integer 8800000
     * (round 11). Division also needs v0 * r == l; powers e >= 0. */
    if (sl == 0 && sr == 0 && integ_exact_int(l) && integ_exact_int(r) &&
        integ_exact_int(v0) &&
        (n->d.binop.op != OP_DIV || v0 * r == l) &&
        (n->d.binop.op != OP_POW || r >= 0))
        return 0;
    {
        ce_float_t e = r;   /* signed exponent for OP_POW */
        l = fabsf(l);
        r = fabsf(r);
        switch (n->d.binop.op) {
        case OP_ADD:
        case OP_SUB: v = v + sl + sr; break;
        case OP_MUL:
        case OP_DIV: {
            /* Multiplying or dividing by a constant power of two (x^2/2,
             * 4*x) is exact; 1/x is not, so for OP_DIV only the right
             * operand may be the constant. */
            int ex;
            bool lc = !ast_contains_var(n->d.binop.left, var);
            bool rc = !ast_contains_var(n->d.binop.right, var);
            ce_float_t c = rc ? r : (lc && n->d.binop.op == OP_MUL) ? l : 0;
            if (c != 0 && frexpf(c, &ex) == 0.5f)
                v = 0;
            if (n->d.binop.op == OP_MUL) {
                v = v + sl * r + l * sr;
            } else {
                if (r == 0) return -1;
                /* l sr / r^2 written as |l/r| sr / r: r*r underflows for
                 * x/1e-30 (review round 10). */
                v = v + sl / r + fabsf(v0) * (sr / r);
            }
            break;
        }
        default: /* OP_POW: d(b^e) = b^e (e/b db + ln|b| de) */
            if (l == 0) return v + sl;
            {
                ce_float_t lnb = fabsf(logf(l));
                /* Small integer powers are repeated squaring in eval.c (a
                 * few roundings); others are powf = exp(e ln b), whose own
                 * error grows with |e ln b|. */
                ce_float_t own = 1.0f + fabsf(e) * lnb;
                if (e >= 2.0f && e <= 16.0f && e == floorf(e)) {
                    /* Repeated squaring: each squaring doubles the error
                     * already there, so e - 1 roundings bound it (round
                     * 11: x^4 is (x^2(1+d))^2 (1+d) = x^4 (1+3d)). */
                    own = e - 1.0f;
                }
                v = v * own + fabsf(v0) * (fabsf(e) * sl / l + lnb * sr);
            }
            break;
        }
    }
    return ce_isfinite(v) ? v : -1;
}

static ce_float_t integ_err_scale(const ast_node_t *n, char var, ce_float_t x)
{
    /* The open inner nodes, each with its first child's estimate: -1 until
     * that is known, which for a binop means its right operand is still to
     * come. Not reentrant; nothing that integ_err_node() evaluates
     * integrates by FTC. */
    static struct integ_err_open { const ast_node_t *n; ce_float_t sl; } s_open[INTEG_ERR_MAX_DEPTH];
    struct integ_err_open *top = s_open;
    ce_float_t s;

    for (;;) {
        while (integ_err_inner(n, var)) {
            /* Deeper than eval_expr() goes: refuse. */
            if (top == s_open + INTEG_ERR_MAX_DEPTH) return -1;
            top->n = n;
            top->sl = -1;
            top++;
            n = n->d.binop.left;
        }
        s = integ_err_node(n, var, x, 0, 0);
        for (;;) {
            const ast_node_t *m;
            /* Every rule maps a negative child estimate to -1. */
            if (s < 0) return -1;
            if (top == s_open) return s;
            top--;
            m = top->n;
            if (m->type == NODE_BINOP && top->sl < 0) {
                top->sl = s;
                top++;
                n = m->d.binop.right;
                break;
            }
            s = integ_err_node(m, var, x, m->type == NODE_BINOP ? top->sl : s, s);
        }
    }
}

/* Exposes integ_err_scale() for the pole scan in processor.c and for
 * eval_limit_inf(). */
ce_float_t integrate_err_scale(const ast_node_t *n, char var, ce_float_t x)
{
    return integ_err_scale(n, var, x);
}
#endif /* CE_INTEG_ENABLE_DEFINITE */

/* lim F(x) as x -> sign*inf from F at sign*1e2, 1e3, ..., 1e6.
 * Returns 1 with *val (and *unc, its uncertainty) when F converges, 0 when
 * F visibly grows (the caller then tests divergence, as before), and -1
 * when the samples cannot support a six-digit value (refuse).
 *
 * F(1e4) alone is the limit only to about F's tail: -1/x is still -1e-4
 * there, so using that sample alone gave 0.9999 for int(1/x^2,x,1,inf).
 * A tail c x^-p shrinks by r = 10^-p per decade, so the rest after 1e6 is
 * d4 r/(1-r), where d4 = F(1e6) - F(1e5), once the tail is geometric.
 *
 * A sum of tails with different rates is never geometric, and five samples
 * cannot always tell (review round 2: 1/x^2 + 0.000001/x passed a ratio
 * test and printed "converges"). The integrator returns sums of terms, one
 * per rate (-1/x + 1e-6 ln|x|), so each additive term is extrapolated on
 * its own (limit_inf_term). Logarithms of polynomials, the terms that
 * grow, are not sampled: m ln|p(x)| = m deg(p) ln|x| + m ln|lead(p)| +
 * o(1), so their sum converges exactly when the m deg(p) cancel, to the sum
 * of the m ln|lead(p)| (review round 3: ln|x| - 0.99999 ln|x+100| diverges
 * though its samples look geometric). */
#define LIMIT_INF_TERMS 16

/* A double is finite: inf - inf and NaN - NaN are NaN. Not isfinite():
 * CEdev's float classification is wrong, so src/ never uses the raw
 * macros (tests/cemu/test_ce_float_class_guard.py). */
static inline bool integ_dfinite(double x)
{
    return x - x == 0;
}

#if CE_INTEG_IMPROPER_USE_LIMITS
/* Coefficients of n as a polynomial in var (c[0..*deg]), or false. */
#define POLY_MAX_DEG 32
/* Exactness certificates for polynomial expansion used as a zero-order
 * proof. TwoSum exposes addition's discarded bits; FMA exposes product's
 * residual without rounding it a second time. Reject extreme magnitudes
 * where residuals could underflow. This is limits-only host code.
 * More precision alone is not a certificate: 1e16*(x-1)^3+(x-1)
 * loses its linear term even in double expansion (review round 19). */
static bool poly_add_exact(double a, double b, double sum)
{
    double z = sum - a;
    return integ_dfinite(sum) && fabs(a) < 1e290 && fabs(b) < 1e290 &&
           ((a - (sum - z)) + (b - z)) == 0;
}

static bool poly_mul_exact(double a, double b, double product)
{
    if (a == 0 || b == 0) return product == 0;
    return integ_dfinite(product) && fabs(product) > 1e-290 &&
           fabs(product) < 1e290 && fma(a, b, -product) == 0;
}

/* c <- c * k, every product certified exact. */
static bool poly_scale_exact(double *c, int deg, double k)
{
    int i;
    for (i = 0; i <= deg; i++) {
        double t = c[i] * k;
        if (!poly_mul_exact(c[i], k, t)) return false;
        c[i] = t;
    }
    return true;
}

/* n == (c[0] + ... + c[*deg] var^*deg) / *den exactly, or false. A
 * constant divisor is kept in *den instead of being divided out, because
 * x/3 has no exact binary coefficient; zero orders and root bounds do not
 * depend on the nonzero scale (int((x^3/3-x/3)/(x^2-1),x,0,3), review
 * round 20). Every product and sum must be exact. */
static bool poly_expand(const ast_node_t *n, char var, double *c, int *deg, double *den, int depth)
{
    double l[POLY_MAX_DEG + 1], r[POLY_MAX_DEG + 1], dl, dr;
    int ol, odr, i, j;
    ce_float_t v;

    if (depth > 24) return false;
    memset(c, 0, sizeof(double) * (POLY_MAX_DEG + 1));
    *den = 1;
    if (!ast_contains_var(n, var)) {
        if (!integrate_eval_at(n, var, 0, &v) || !ce_isfinite(v)) return false;
        c[0] = v;
        *deg = 0;
        return true;
    }
    switch (n->type) {
    case NODE_VARIABLE:
        c[1] = 1;
        *deg = 1;
        return true;
    case NODE_UNOP:
        if (n->d.unop.op != UOP_NEG ||
            !poly_expand(n->d.unop.operand, var, c, deg, den, depth + 1))
            return false;
        for (i = 0; i <= *deg; i++) c[i] = -c[i];
        return true;
    case NODE_BINOP:
        if (!poly_expand(n->d.binop.left, var, l, &ol, &dl, depth + 1)) return false;
        switch (n->d.binop.op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
            if (!poly_expand(n->d.binop.right, var, r, &odr, &dr, depth + 1)) return false;
            if (n->d.binop.op == OP_MUL) {
                if (ol + odr > POLY_MAX_DEG) return false;
                for (i = 0; i <= ol; i++)
                    for (j = 0; j <= odr; j++) {
                        double t = l[i] * r[j], v0 = c[i + j];
                        if (!poly_mul_exact(l[i], r[j], t) ||
                            !poly_add_exact(v0, t, v0 + t)) return false;
                        c[i + j] = v0 + t;
                    }
                *deg = ol + odr;
                *den = dl * dr;
                return poly_mul_exact(dl, dr, *den);
            }
            /* l/dl +- r/dr == (l dr +- r dl) / (dl dr) */
            if (dl != dr) {
                *den = dl * dr;
                if (!poly_mul_exact(dl, dr, *den) || !poly_scale_exact(l, ol, dr) ||
                    !poly_scale_exact(r, odr, dl))
                    return false;
            } else {
                *den = dl;
            }
            *deg = ol > odr ? ol : odr;
            for (i = 0; i <= *deg; i++) {
                double t = n->d.binop.op == OP_SUB ? -r[i] : r[i];
                c[i] = l[i] + t;
                if (!poly_add_exact(l[i], t, c[i])) return false;
            }
            return true;
        case OP_DIV:
            if (ast_contains_var(n->d.binop.right, var) ||
                !integrate_eval_at(n->d.binop.right, var, 0, &v) || v == 0 ||
                !ce_isfinite(v))
                return false;
            memcpy(c, l, sizeof l);
            *deg = ol;
            *den = dl * (double)v;
            return poly_mul_exact(dl, (double)v, *den);
        case OP_POW: {
            int e;
            if (ast_contains_var(n->d.binop.right, var) ||
                !integrate_eval_at(n->d.binop.right, var, 0, &v) ||
                !(v >= 0 && v <= POLY_MAX_DEG && v == floorf(v)) ||
                ol * (int)v > POLY_MAX_DEG)
                return false;
            c[0] = 1;
            *deg = 0;
            for (e = 0; e < (int)v; e++) {
                double d0 = *den;
                memset(r, 0, sizeof r);
                for (i = 0; i <= *deg; i++)
                    for (j = 0; j <= ol; j++) {
                        double t = c[i] * l[j], v0 = r[i + j];
                        if (!poly_mul_exact(c[i], l[j], t) ||
                            !poly_add_exact(v0, t, v0 + t)) return false;
                        r[i + j] = v0 + t;
                    }
                *deg += ol;
                memcpy(c, r, sizeof r);
                *den = d0 * dl;
                if (!poly_mul_exact(d0, dl, *den)) return false;
            }
            return true;
        }
        default:
            return false;
        }
    default:
        return false;
    }
}

bool integrate_poly_root_bound(const ast_node_t *p, char var, ce_float_t *bound)
{
    double c[POLY_MAX_DEG + 1], m = 0, den;
    int deg, i;

    if (!poly_expand(p, var, c, &deg, &den, 0)) return false;
    while (deg > 0 && c[deg] == 0) deg--;
    if (c[deg] == 0) return false;  /* identically 0 */
    for (i = 0; i < deg; i++) {
        /* Fujiwara: every root has |r| <= 2 max |c_i / c_n|^(1/(n-i)),
         * tighter than Cauchy for x^2 + c (2 sqrt(c), not 1 + c). */
        double t = pow(fabs(c[i] / c[deg]), 1.0 / (deg - i));
        if (t > m) m = t;
    }
    *bound = deg ? (ce_float_t)(2 * m * (1 + 1e-6)) : 0;
    return integ_dfinite(*bound);
}

/* Multiplicity at x0, or -1 unknown / -2 not certifiable as an exact
 * polynomial. Both coefficient expansion and every synthetic-division
 * operation must be exact; otherwise zord keeps its structural lower bound.
 * For an exact denominator order the stopping remainder must also stand
 * clear of input rounding (round 16). */
static double zord_poly(const ast_node_t *n, char var, ce_float_t x0, bool up)
{
    double c[POLY_MAX_DEG + 1], r, sc, xp, den;
    int deg, i, m;

    if (!poly_expand(n, var, c, &deg, &den, 0)) return -2;
    while (deg > 0 && c[deg] == 0) deg--;
    if (deg == 0) return c[0] == 0 ? 1e9 : (up ? -1 : 0);
    for (m = 0;; m++, deg--) {
        sc = 0;
        xp = 1;
        for (i = 0; i <= deg; i++, xp *= fabs((double)x0)) sc += fabs(c[i]) * xp;
        if (deg == 0) {
            r = c[0];
        } else {
            /* c <- c / (t - x0); the quotient lands in c[0..deg-1] */
            r = c[deg];
            for (i = deg - 1; i >= 0; i--) {
                double t = c[i];
                c[i] = r;
                double prod = r * (double)x0;
                if (!poly_mul_exact(r, (double)x0, prod) ||
                    !poly_add_exact(prod, t, prod + t)) return -2;
                r = prod + t;
            }
        }
        if (r != 0 || deg == 0) break;
    }
    if (!up) return m;
    return m > 0 && fabs(r) > 1e-9 * sc ? m : -1;
}

/* Order of the zero of n at var = x0 for n analytic there: 0 where n is
 * nonzero, a large value where n is identically 0, -1 unknown. With up,
 * the exact order (unknown when it cannot be proved); without, a lower
 * bound (a sum is at least its smaller order). Products, positive integer
 * powers and odd analytic functions of a zero (sin, tan, sinh, tanh,
 * atan, asin, asinh) are counted structurally, before any expansion:
 * (x-0.3)^4 is order 4 although its expanded coefficients round (round
 * 16). Sums: 1 - cos u (2 ord u), e^u - 1 and b^u - 1 (ord u), then
 * polynomials by zord_poly, then the smaller order. abs, sqrt and
 * fractional powers are unknown: |x|/x jumps. */
static double zord(const ast_node_t *n, char var, ce_float_t x0, bool up, int depth)
{
    double a, b;
    ce_float_t v, w;
    bool ev;

    if (depth > 24) return -1;
    ev = integrate_eval_at(n, var, x0, &v) && ce_isfinite(v);
    if (ev && v != 0) return 0;
    if (!ast_contains_var(n, var)) return ev ? 1e9 : -1;
    if (!ev) return -1;
    switch (n->type) {
    case NODE_VARIABLE:
        return 1;
    case NODE_UNOP:
        return n->d.unop.op == UOP_NEG ? zord(n->d.unop.operand, var, x0, up, depth + 1) : -1;
    case NODE_BINOP: {
        const ast_node_t *l = n->d.binop.left, *r = n->d.binop.right;
        switch (n->d.binop.op) {
        case OP_MUL:
            a = zord(l, var, x0, up, depth + 1);
            b = zord(r, var, x0, up, depth + 1);
            return a < 0 || b < 0 ? -1 : a + b;
        case OP_DIV:
            return zord(r, var, x0, true, depth + 1) == 0 ? zord(l, var, x0, up, depth + 1) : -1;
        case OP_POW:
            /* Positive integer powers only: (x^2)^(1/2) is |x|. */
            if (ast_contains_var(r, var) || !integrate_eval_at(r, var, x0, &w) ||
                !(w >= 1 && w <= 64 && w == floorf(w)))
                return -1;
            a = zord(l, var, x0, up, depth + 1);
            return a < 0 ? -1 : a * w;
        case OP_ADD:
        case OP_SUB: {
            /* 1 - cos u, e^u - 1 and b^u - 1 (e^x parses as a power) at
             * u = 0: orders 2 ord u, ord u and ord u. */
            const ast_node_t *k = !ast_contains_var(l, var) ? l : !ast_contains_var(r, var) ? r : NULL;
            const ast_node_t *g = k == l ? r : l, *u = NULL;
            bool cosine = false;
            ce_float_t bb;
            if (k && integrate_eval_at(k, var, x0, &w)) {
                if (g->type == NODE_FUNC && g->d.func.id == FN_COS && fabsf(w) == 1) {
                    u = g->d.func.arg;
                    cosine = true;
                } else if (g->type == NODE_FUNC && g->d.func.id == FN_EXP && w == 1) {
                    u = g->d.func.arg;
                } else if (g->type == NODE_BINOP && g->d.binop.op == OP_POW && w == 1 &&
                           !ast_contains_var(g->d.binop.left, var) &&
                           integrate_eval_at(g->d.binop.left, var, x0, &bb) &&
                           bb > 0 && bb != 1) {
                    u = g->d.binop.right;
                }
            }
            if (u && integrate_eval_at(u, var, x0, &v) && v == 0) {
                a = zord(u, var, x0, up, depth + 1);
                return a < 0 ? -1 : cosine ? 2 * a : a;
            }
            a = zord_poly(n, var, x0, up);
            if (a != -2) return a;
            a = zord(l, var, x0, up, depth + 1);
            b = zord(r, var, x0, up, depth + 1);
            if (a < 0 || b < 0) return -1;
            if (a != b) return a < b ? a : b;
            return up ? -1 : a;
        }
        default:
            return -1;
        }
    }
    case NODE_FUNC:
        if (!integrate_eval_at(n->d.func.arg, var, x0, &w)) return -1;
        switch (n->d.func.id) {
        case FN_SIN: case FN_TAN: case FN_SINH: case FN_TANH:
        case FN_ATAN: case FN_ASIN: case FN_ASINH:
            if (w != 0) return -1;
            return zord(n->d.func.arg, var, x0, up, depth + 1);
        default:
            return -1;
        }
    default:
        return -1;
    }
}

bool integrate_quot_bounded(const ast_node_t *q, char var, ce_float_t x0)
{
    double od, on;
    if (q->type != NODE_BINOP || q->d.binop.op != OP_DIV) return false;
    od = zord(q->d.binop.right, var, x0, true, 0);
    on = zord(q->d.binop.left, var, x0, false, 0);
    return od > 0 && on >= od;
}
#endif

/* Parity of n in var: 1 even, -1 odd, 0 not proved either way. */
static int integ_parity(const ast_node_t *n, char var, int depth)
{
    int pl, pr;
    ce_float_t v;

    if (depth > 32) return 0;
    if (!ast_contains_var(n, var)) return 1;
    switch (n->type) {
    case NODE_VARIABLE:
        return -1;
    case NODE_UNOP:
        return n->d.unop.op == UOP_NEG ? integ_parity(n->d.unop.operand, var, depth + 1) : 0;
    case NODE_BINOP:
        pl = integ_parity(n->d.binop.left, var, depth + 1);
        if (!pl) return 0;
        switch (n->d.binop.op) {
        case OP_ADD:
        case OP_SUB:
            pr = integ_parity(n->d.binop.right, var, depth + 1);
            return pl == pr ? pl : 0;
        case OP_MUL:
        case OP_DIV:
            pr = integ_parity(n->d.binop.right, var, depth + 1);
            return pl * pr;
        case OP_POW:
            if (ast_contains_var(n->d.binop.right, var) ||
                !integrate_eval_at(n->d.binop.right, var, 0, &v))
                return 0;
            if (pl == 1) return 1;
            if (v != floorf(v)) return 0;
            return fmodf(v, 2.0f) == 0 ? 1 : -1;
        default:
            return 0;
        }
    case NODE_FUNC:
        pl = integ_parity(n->d.func.arg, var, depth + 1);
        if (pl == 1) return 1;  /* f(even) is even */
        if (pl == 0) return 0;
        switch (n->d.func.id) {
        case FN_SIN: case FN_TAN: case FN_CSC: case FN_COT:
        case FN_SINH: case FN_TANH: case FN_CSCH: case FN_COTH:
        case FN_ASIN: case FN_ATAN: case FN_ASINH: case FN_ATANH:
            return -1;
        case FN_COS: case FN_SEC: case FN_COSH: case FN_SECH: case FN_ABS:
            return 1;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/* Uncertainty of the last integrate_improper() result; -1 unknown. */
static ce_float_t s_improp_unc;

typedef struct {
    ce_float_t sum;      /* limits of the extrapolated terms */
    ce_float_t unc;      /* their rounding and extrapolation uncertainty */
    ce_float_t lsum;     /* sum of m ln|lead(p)| over the log terms */
    double ldeg;         /* sum of m deg(p), exact for float m */
    ce_float_t ldeg_max; /* largest |m deg(p)| */
    ce_float_t rv[5];    /* samples of the sum of the other moving terms */
    ce_float_t ra[5];    /* sums of their magnitudes (rounding scale) */
    const ast_node_t *ut[LIMIT_INF_TERMS];  /* the other moving terms */
    ce_float_t um[LIMIT_INF_TERMS];         /* and their multipliers */
    bool failed;         /* one of them did not evaluate at every sample */
    int8_t nu;           /* other moving terms */
    int8_t n;            /* terms seen */
    int8_t res;          /* -1 refuse, 0 F grows, 1 so far fine */
} limit_inf_acc_t;

/* Limit from five samples of one tail, or false (refuse). mag[3], mag[4]:
 * the size of what was added up for v[3], v[4], whose rounding is noise in
 * d3 and d4. *unc gets the extrapolation's uncertainty. */
static bool limit_inf_geometric(const ce_float_t v[5], const ce_float_t mag[5],
                                ce_float_t *val, ce_float_t *unc)
{
    ce_float_t d2 = v[2] - v[1], d3 = v[3] - v[2], d4 = v[4] - v[3], r3, r4;
    ce_float_t noise;

    *val = v[4];
    *unc = 0;
    if (d3 == 0 && d4 == 0) return true;  /* flat (e^(-x) underflows) */
    if (d2 == 0 || d3 == 0) return false;
    r3 = d3 / d2;
    r4 = d4 / d3;
    /* A tail shrinking by less than half per decade (p < 0.3) is too slow
     * for its remainder to be trusted; 1e-9 ln(x) (r = 1) diverges. Mixed
     * signs oscillate. */
    if (!(r3 > 0 && r4 > 0 && r3 <= 0.5f && r4 <= 0.5f)) return false;
    /* What rounding (about 2 ulps in each of d3 and d4) can do to r4. */
    noise = 4.0f * 1.1920929e-7f * fmaxf(mag[3], mag[4]) / fabsf(d3);
    if (r4 <= r3) {
        /* Falling toward its rate, as a shifted tail 1/(x+100)^2 does:
         * d4 r4/(1-r4) then overestimates a remainder that is already
         * small. */
        if (r3 - r4 > 0.1f * r3 + 0.01f + noise) return false;
    } else {
        /* Rising: a slower component is taking over, and the remainder
         * would be underestimated. Only rounding may raise r4. */
        if (r4 - r3 > 0.002f + noise) return false;
    }
    *val = v[4] + d4 * (r4 / (1 - r4));
    /* r/(1-r) moves by dr/(1-r)^2 when r is off by dr = |r4 - r3| + noise. */
    *unc = fabsf(d4) * (fabsf(r4 - r3) + noise) / ((1 - r4) * (1 - r4));
    return true;
}

/* Degree and leading coefficient of n as a polynomial in var, or false.
 * Equal-degree terms whose leading coefficients cancel fail. */
static bool limit_inf_poly(const ast_node_t *n, char var, int *deg, ce_float_t *lead)
{
    int d2;
    ce_float_t l2, c;

    if (!ast_contains_var(n, var)) {
        if (!integrate_eval_at(n, var, 0, lead) || !ce_isfinite(*lead) || *lead == 0)
            return false;
        *deg = 0;
        return true;
    }
    if (n->type == NODE_VARIABLE) {
        *deg = 1;
        *lead = 1;
        return true;
    }
    if (n->type == NODE_UNOP && n->d.unop.op == UOP_NEG) {
        if (!limit_inf_poly(n->d.unop.operand, var, deg, lead)) return false;
        *lead = -*lead;
        return true;
    }
    if (n->type != NODE_BINOP) return false;
    switch (n->d.binop.op) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
        if (!limit_inf_poly(n->d.binop.left, var, deg, lead) ||
            !limit_inf_poly(n->d.binop.right, var, &d2, &l2))
            return false;
        if (n->d.binop.op == OP_MUL) {
            *deg += d2;
            *lead *= l2;
        } else {
            if (n->d.binop.op == OP_SUB) l2 = -l2;
            if (d2 > *deg) {
                *deg = d2;
                *lead = l2;
            } else if (d2 == *deg) {
                c = *lead + l2;
                /* Cancelling leading coefficients: the degree is not
                 * known from float32 values. */
                if (fabsf(c) <= 1e-6f * (fabsf(*lead) + fabsf(l2))) return false;
                *lead = c;
            }
        }
        break;
    case OP_DIV:
        if (ast_contains_var(n->d.binop.right, var) ||
            !limit_inf_poly(n->d.binop.left, var, deg, lead) ||
            !integrate_eval_at(n->d.binop.right, var, 0, &c) || c == 0)
            return false;
        *lead /= c;
        break;
    case OP_POW:
        if (ast_contains_var(n->d.binop.right, var) ||
            !integrate_eval_at(n->d.binop.right, var, 0, &c) ||
            !(c >= 1 && c <= 16 && c == floorf(c)) ||
            !limit_inf_poly(n->d.binop.left, var, deg, lead))
            return false;
        *deg *= (int)c;
        *lead = powf(*lead, c);
        break;
    default:
        return false;
    }
    return ce_isfinite(*lead) && *lead != 0;
}

/* Asymptotic order of an expression as x = sign*t, t -> +inf, for deciding
 * from its structure, not from samples, that F is unbounded and what a
 * term's limit is (review rounds 6-8: an intermediate overflow, x^2 in
 * -1e12/(1e10+x^2), is not growth; equal samples, -1e14/(x+1e14) = -1 up to
 * 1e6, are not a limit). Kinds:
 *   ASYM_POW     c t^q (ln t)^k, c != 0
 *   ASYM_EXP     c t^q (ln t)^k e^g, g an unbounded ASYM_POW with gc > 0
 *   ASYM_LIMIT   tends to the finite c, order unknown
 *   ASYM_BOUNDED bounded, no limit known (sin, cos)
 *   ASYM_DECAY   tends to 0 faster than any power; c = 0 with ex is an
 *                exact zero
 * ex: the term equals its description exactly (no lower-order part was
 * dropped); gex: the same for an ASYM_EXP exponent. Sums whose leading
 * coefficients cancel (to float32 rounding, since they come from float
 * constants) are unknown, which errs toward refusing. */
enum { ASYM_UNKNOWN, ASYM_POW, ASYM_EXP, ASYM_BOUNDED, ASYM_DECAY, ASYM_LIMIT };

typedef struct {
    int8_t kind;
    bool ex;
    bool gex;
    double c, q, k;
    double gc, gq, gk;
} asym_t;

static asym_t asym_kind(int8_t kind)
{
    asym_t r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    return r;
}

/* c t^q (ln t)^k, exact; c = 0 is an exact zero. */
static asym_t asym_pow(double c, double q, double k)
{
    asym_t r = asym_kind(!integ_dfinite(c) ? ASYM_UNKNOWN : c == 0 ? ASYM_DECAY : ASYM_POW);
    r.ex = true;
    r.c = c;
    r.q = q;
    r.k = k;
    return r;
}

static asym_t asym_limitk(double L)
{
    asym_t r = asym_kind(integ_dfinite(L) ? ASYM_LIMIT : ASYM_UNKNOWN);
    r.c = L;
    return r;
}

static bool asym_zero(const asym_t *a)
{
    return a->kind == ASYM_DECAY && a->ex && a->c == 0;
}

/* ASYM_POW with t^q (ln t)^k -> +-inf. */
static bool asym_pow_unbounded(const asym_t *a)
{
    return a->kind == ASYM_POW && (a->q > 0 || (a->q == 0 && a->k > 0));
}

/* The limit of a, when a has one. */
static bool asym_lim(const asym_t *a, double *L)
{
    if (a->kind == ASYM_DECAY ||
        (a->kind == ASYM_POW && (a->q < 0 || (a->q == 0 && a->k < 0)))) {
        *L = 0;
        return true;
    }
    if ((a->kind == ASYM_POW && a->q == 0 && a->k == 0) || a->kind == ASYM_LIMIT) {
        *L = a->c;
        return true;
    }
    return false;
}

static bool asym_cancels(double a, double b)
{
    return fabs(a + b) <= 1e-6 * fmax(fabs(a), fabs(b));
}

/* Compare the growth of two power parts: >0 if a outgrows b. */
static int asym_cmp(double qa, double ka, double qb, double kb)
{
    if (qa != qb) return qa > qb ? 1 : -1;
    if (ka != kb) return ka > kb ? 1 : -1;
    return 0;
}

/* d dominates; the other part is dropped, so d is no longer exact. */
static asym_t asym_keep(asym_t d)
{
    d.ex = false;
    return d;
}

static asym_t asym_add(asym_t a, asym_t b)
{
    int c;
    double la, lb;
    if (a.kind == ASYM_UNKNOWN || b.kind == ASYM_UNKNOWN) return asym_kind(ASYM_UNKNOWN);
    if (asym_zero(&a)) return b;
    if (asym_zero(&b)) return a;
    if (a.kind == ASYM_EXP || b.kind == ASYM_EXP) {
        if (a.kind != ASYM_EXP) return asym_keep(b);
        if (b.kind != ASYM_EXP) return asym_keep(a);
        c = asym_cmp(a.gq, a.gk, b.gq, b.gk);
        if (c == 0 && !asym_cancels(a.gc, -b.gc)) c = a.gc > b.gc ? 1 : -1;
        if (c) return asym_keep(c > 0 ? a : b);
        /* Same leading exponent: only exact, equal exponents let the
         * prefactors decide (x e^x - e^x); e^(x+1) - e e^x kept only x of
         * x + 1 (review round 7). */
        if (!(a.gex && b.gex && a.gc == b.gc)) return asym_kind(ASYM_UNKNOWN);
        c = asym_cmp(a.q, a.k, b.q, b.k);
        if (c) return asym_keep(c > 0 ? a : b);
        if (asym_cancels(a.c, b.c)) return asym_kind(ASYM_UNKNOWN);
        a.c += b.c;
        a.ex = a.ex && b.ex;
        return a;
    }
    if (asym_pow_unbounded(&a) && b.kind != ASYM_POW) return asym_keep(a);
    if (asym_pow_unbounded(&b) && a.kind != ASYM_POW) return asym_keep(b);
    if (a.kind == ASYM_BOUNDED || b.kind == ASYM_BOUNDED) return asym_kind(ASYM_BOUNDED);
    if (a.kind == ASYM_DECAY) return asym_keep(b);
    if (b.kind == ASYM_DECAY) return asym_keep(a);
    if (a.kind == ASYM_LIMIT || b.kind == ASYM_LIMIT) {
        if (!asym_lim(&a, &la) || !asym_lim(&b, &lb)) return asym_kind(ASYM_UNKNOWN);
        /* Limits that cancel to float rounding have an unknown residual:
         * neither 0 (pi/2 - 1.570795 is 1.27e-6, review round 10) nor a
         * reliable nonzero (e^x times it is not known growth, round 9). */
        if (la + lb != 0 && asym_cancels(la, lb)) return asym_kind(ASYM_UNKNOWN);
        return asym_limitk(la + lb);
    }
    c = asym_cmp(a.q, a.k, b.q, b.k);
    if (c) return asym_keep(c > 0 ? a : b);
    if (asym_cancels(a.c, b.c)) return asym_kind(ASYM_UNKNOWN);
    a.c += b.c;
    a.ex = a.ex && b.ex;
    return a;
}

static asym_t asym_mul(asym_t a, asym_t b)
{
    double L;
    if (a.kind == ASYM_UNKNOWN || b.kind == ASYM_UNKNOWN) return asym_kind(ASYM_UNKNOWN);
    if (asym_zero(&a) || asym_zero(&b)) return asym_pow(0, 0, 0);
    if (a.kind == ASYM_EXP && b.kind == ASYM_EXP) return asym_kind(ASYM_UNKNOWN);
    if (b.kind == ASYM_EXP || (b.kind == ASYM_DECAY && a.kind != ASYM_EXP) ||
        (b.kind == ASYM_BOUNDED && a.kind != ASYM_EXP && a.kind != ASYM_DECAY) ||
        (b.kind == ASYM_LIMIT && a.kind == ASYM_POW)) {
        asym_t t = a;
        a = b;
        b = t;
    }
    switch (a.kind) {
    case ASYM_EXP:
        /* e^g wins against powers and nonzero limits only. */
        if (b.kind == ASYM_POW) {
            a.c *= b.c;
            a.q += b.q;
            a.k += b.k;
            a.ex = a.ex && b.ex;
            return a;
        }
        if (b.kind == ASYM_LIMIT && b.c != 0) {
            a.c *= b.c;
            a.ex = false;
            return a;
        }
        return asym_kind(ASYM_UNKNOWN);
    case ASYM_DECAY:
        /* e^(-x) times a power, a bound or a limit still decays. */
        return asym_kind(ASYM_DECAY);
    case ASYM_BOUNDED:
        if (b.kind == ASYM_BOUNDED || b.kind == ASYM_LIMIT ||
            (b.kind == ASYM_POW && !asym_pow_unbounded(&b)))
            return asym_kind(ASYM_BOUNDED);
        return asym_kind(ASYM_UNKNOWN);  /* x sin x oscillates unboundedly */
    case ASYM_LIMIT:
        if (b.kind == ASYM_LIMIT) return asym_limitk(a.c * b.c);
        /* b is ASYM_POW */
        if (a.c != 0) {
            b.c *= a.c;
            b.ex = false;
            return b;
        }
        if (asym_lim(&b, &L)) return asym_limitk(0);
        return asym_kind(ASYM_UNKNOWN);  /* 0 * infinity */
    default: {
        asym_t r = asym_pow(a.c * b.c, a.q + b.q, a.k + b.k);
        if (r.kind == ASYM_DECAY) return asym_kind(ASYM_UNKNOWN);  /* underflow */
        r.ex = a.ex && b.ex;
        return r;
    }
    }
}

/* pref e^g. */
static asym_t asym_exp(asym_t g, double pref)
{
    asym_t r;
    double L;
    if (asym_zero(&g)) return asym_pow(pref, 0, 0);
    if (g.kind == ASYM_BOUNDED) return asym_kind(ASYM_BOUNDED);
    if (g.kind == ASYM_POW && g.q == 0 && g.k == 0) {
        /* A constant exponent: exact only if g is. An underflow to 0 is
         * not an exact zero. */
        double v = pref * exp(g.c);
        if (v == 0 && pref != 0) return asym_kind(ASYM_UNKNOWN);
        return g.ex ? asym_pow(v, 0, 0) : asym_limitk(v);
    }
    if (asym_lim(&g, &L)) return asym_limitk(pref * exp(L));
    if (!asym_pow_unbounded(&g)) return asym_kind(ASYM_UNKNOWN);
    if (g.q == 0) {
        /* e^(c ln t) = t^c, but only for an exact exponent: ln(3x) kept
         * only ln t, and e^(ln 3x) is 3t, not t (review round 9). */
        if (g.k != 1 || !g.ex) return asym_kind(ASYM_UNKNOWN);
        return asym_pow(pref, g.c, 0);
    }
    if (g.c < 0) return asym_kind(ASYM_DECAY);
    r = asym_pow(pref, 0, 0);
    r.kind = ASYM_EXP;
    r.gex = g.ex;
    r.gc = g.c;
    r.gq = g.q;
    r.gk = g.k;
    return r;
}

/* f(L) for a function f continuous at L, or false. */
static bool asym_fn_at(int id, double L, double *out)
{
    switch (id) {
    /* Near a zero of sin, cos or tan the value is lost in the rounding of
     * L (sin(pi + u), review round 9) yet need not be 0 (sin(1e-6),
     * round 10): unknown. An exact sin 0 or tan 0 stays exact. */
    case FN_SIN:
    case FN_COS:
    case FN_TAN:
        if (fabs(cos(L)) < 1e-6 && id == FN_TAN) return false;
        *out = id == FN_SIN ? sin(L) : id == FN_COS ? cos(L) : tan(L);
        if (L == 0 && id != FN_COS) return true;
        return fabs(*out) > 1e-6 * fmax(1, fabs(L));
    case FN_EXP: *out = exp(L); return integ_dfinite(*out);
    case FN_LN: if (!(L > 0)) return false; *out = log(L); return true;
    case FN_SQRT: if (!(L > 0)) return false; *out = sqrt(L); return true;
    case FN_ABS: *out = fabs(L); return true;
    case FN_ATAN: *out = atan(L); return true;
    case FN_TANH: *out = tanh(L); return true;
    case FN_SINH: *out = sinh(L); return integ_dfinite(*out);
    case FN_COSH: *out = cosh(L); return integ_dfinite(*out);
    case FN_ASINH: *out = asinh(L); return true;
    case FN_ASIN: if (!(fabs(L) < 1)) return false; *out = asin(L); return true;
    case FN_ACOS: if (!(fabs(L) < 1)) return false; *out = acos(L); return true;
    default: return false;
    }
}

static asym_t asym_of(const ast_node_t *n, char var, int8_t sign, int depth)
{
    asym_t a, b;
    ce_float_t v;
    double L;

    if (depth > 24) return asym_kind(ASYM_UNKNOWN);
    if (!ast_contains_var(n, var)) {
        if (!integrate_eval_at(n, var, 0, &v) || !ce_isfinite(v))
            return asym_kind(ASYM_UNKNOWN);
        return asym_pow(v, 0, 0);
    }
    switch (n->type) {
    case NODE_VARIABLE:
        return asym_pow(sign, 1, 0);
    case NODE_UNOP:
        if (n->d.unop.op != UOP_NEG) return asym_kind(ASYM_UNKNOWN);
        a = asym_of(n->d.unop.operand, var, sign, depth + 1);
        a.c = -a.c;
        return a;
    case NODE_BINOP:
        a = asym_of(n->d.binop.left, var, sign, depth + 1);
        switch (n->d.binop.op) {
        case OP_ADD:
        case OP_SUB:
            b = asym_of(n->d.binop.right, var, sign, depth + 1);
            if (n->d.binop.op == OP_SUB) b.c = -b.c;
            return asym_add(a, b);
        case OP_MUL:
            return asym_mul(a, asym_of(n->d.binop.right, var, sign, depth + 1));
        case OP_DIV:
            b = asym_of(n->d.binop.right, var, sign, depth + 1);
            if (b.kind == ASYM_POW) {
                asym_t r = asym_pow(1.0 / b.c, -b.q, -b.k);
                r.ex = b.ex;
                return asym_mul(a, r);
            }
            if (b.kind == ASYM_LIMIT && b.c != 0) return asym_mul(a, asym_limitk(1.0 / b.c));
            if (b.kind == ASYM_EXP && a.kind != ASYM_EXP && a.kind != ASYM_UNKNOWN)
                return asym_kind(ASYM_DECAY);
            return asym_kind(ASYM_UNKNOWN);
        case OP_POW:
            if (!ast_contains_var(n->d.binop.right, var)) {
                double e;
                if (!integrate_eval_at(n->d.binop.right, var, 0, &v) || !ce_isfinite(v))
                    return asym_kind(ASYM_UNKNOWN);
                e = v;
                switch (a.kind) {
                case ASYM_POW:
                    if (a.c < 0 && e != floor(e)) return asym_kind(ASYM_UNKNOWN);
                    if (a.k != 0 && a.k * e != floor(a.k * e)) return asym_kind(ASYM_UNKNOWN);
                    b = asym_pow(pow(a.c, e), a.q * e, a.k * e);
                    if (b.kind == ASYM_DECAY) return asym_kind(ASYM_UNKNOWN);  /* underflow */
                    b.ex = a.ex;
                    return b;
                case ASYM_LIMIT:
                    if (a.c > 0 || (a.c < 0 && e == floor(e))) return asym_limitk(pow(a.c, e));
                    return a.c == 0 && e > 0 ? asym_limitk(0) : asym_kind(ASYM_UNKNOWN);
                case ASYM_DECAY:
                    return e > 0 ? asym_kind(ASYM_DECAY) : asym_kind(ASYM_UNKNOWN);
                case ASYM_BOUNDED:
                    return e > 0 ? asym_kind(ASYM_BOUNDED) : asym_kind(ASYM_UNKNOWN);
                default:
                    return asym_kind(ASYM_UNKNOWN);
                }
            }
            /* b^g with b > 0 constant: e^(g ln b) (e^x parses this way). */
            if (!ast_contains_var(n->d.binop.left, var) && a.kind == ASYM_POW &&
                a.c > 0 && a.c != 1) {
                double lnb = n->d.binop.left->type == NODE_CONSTANT &&
                             n->d.binop.left->d.constant == CONST_E ? 1.0 : log(a.c);
                b = asym_of(n->d.binop.right, var, sign, depth + 1);
                return asym_exp(asym_mul(b, asym_pow(lnb, 0, 0)), 1);
            }
            return asym_kind(ASYM_UNKNOWN);
        default:
            return asym_kind(ASYM_UNKNOWN);
        }
    case NODE_FUNC: {
        int id = n->d.func.id;
        bool has_abs = n->d.func.arg->type == NODE_FUNC &&
                       n->d.func.arg->d.func.id == FN_ABS;
        a = asym_of(n->d.func.arg, var, sign, depth + 1);
        if (a.kind == ASYM_UNKNOWN) return a;
        switch (id) {
        case FN_EXP:
            return asym_exp(a, 1);
        case FN_SINH:
        case FN_COSH:
            /* sinh/cosh u ~ +-e^|u| / 2 */
            if (asym_pow_unbounded(&a) && a.q > 0) {
                double s2 = a.c > 0 ? 0.5 : (id == FN_SINH ? -0.5 : 0.5);
                a.c = fabs(a.c);
                b = asym_exp(a, s2);
                b.ex = false;  /* e^(-u)/2 dropped */
                return b;
            }
            break;
        case FN_ABS:
            if (a.kind == ASYM_POW || a.kind == ASYM_EXP || a.kind == ASYM_LIMIT) {
                a.c = fabs(a.c);
                return a;
            }
            return a;  /* bounded or decaying stays so */
        case FN_SQRT:
            if (a.kind == ASYM_POW && a.c > 0) {
                b = asym_pow(sqrt(a.c), a.q / 2, a.k / 2);
                b.ex = a.ex;
                return b;
            }
            if (a.kind == ASYM_EXP && a.c > 0) {
                a.c = sqrt(a.c);
                a.q /= 2;
                a.k /= 2;
                a.gc /= 2;
                return a;
            }
            if (a.kind == ASYM_DECAY) return a;
            if (a.kind == ASYM_LIMIT && a.c == 0) return asym_limitk(0);
            break;
        case FN_LN:
            /* ln|c t^q (ln t)^k| ~ q ln t; ln|e^g| ~ g; both drop terms. */
            if (!has_abs && !((a.kind == ASYM_POW || a.kind == ASYM_EXP ||
                               a.kind == ASYM_LIMIT) && a.c > 0))
                return asym_kind(ASYM_UNKNOWN);
            if (a.kind == ASYM_EXP) {
                b = asym_pow(a.gc, a.gq, a.gk);
                b.ex = false;
                return b;
            }
            if (a.kind == ASYM_POW && a.q != 0) {
                b = asym_pow(a.q, 0, 1);
                b.ex = false;
                return b;
            }
            if (a.kind == ASYM_POW && a.k != 0) return asym_kind(ASYM_UNKNOWN);
            if (asym_lim(&a, &L) && L != 0) return asym_limitk(log(fabs(L)));
            return asym_kind(ASYM_UNKNOWN);
        case FN_ATAN:
        case FN_TANH:
            if (asym_pow_unbounded(&a) || a.kind == ASYM_EXP) {
                double lim = id == FN_ATAN ? 1.5707963267948966 : 1.0;
                return asym_limitk(a.c > 0 ? lim : -lim);
            }
            break;
        case FN_SIN:
        case FN_COS:
            if (!asym_lim(&a, &L)) return asym_kind(ASYM_BOUNDED);
            break;
        default:
            break;
        }
        /* A function continuous at its argument's limit. */
        if (asym_lim(&a, &L) && asym_fn_at(id, L, &L)) return asym_limitk(L);
        return asym_kind(ASYM_UNKNOWN);
    }
    default:
        return asym_kind(ASYM_UNKNOWN);
    }
}

/* lim T as x -> sign*inf when its structure gives one. */
static bool asym_limit(const ast_node_t *T, char var, int8_t sign, double *lim)
{
    asym_t a = asym_of(T, var, sign, 0);
    return asym_lim(&a, lim);
}

/* The limit of m T from its samples (lim, unc) and its structure. When the
 * structure gives a limit, the samples must agree with it and the
 * structural value is used: an extrapolation can be off in the 6th digit
 * (100(x+20)^-1.5 printed 44.7217, true 44.7214, review round 8), and
 * equal samples alone do not show a limit (-1e14/(x+1e14) is -1 at every
 * sample, round 7). Without a structural limit a term refuses: e^(1/x +
 * x/1e16) looks geometric up to 1e6 and then grows (round 11). */
static bool limit_inf_settle(const ast_node_t *T, char var, int8_t sign,
                             ce_float_t m, ce_float_t *lim, ce_float_t *unc,
                             ce_float_t mag4)
{
    double sl;
    if (!asym_limit(T, var, sign, &sl)) return false;
    sl *= m;
    if (!(fabs((double)*lim - sl) <= 1e-5 * (fabs(sl) + mag4) + 8.0 * *unc)) return false;
    *lim = (ce_float_t)sl;
    *unc = 2.4e-7f * (ce_float_t)fabs(sl);  /* one rounding of the value */
    return true;
}

/* m T is one additive term of F. */
static void limit_inf_term(const ast_node_t *T, char var, int8_t sign,
                           ce_float_t m, limit_inf_acc_t *a)
{
    ce_float_t v[5], mag[5], x = sign * 100.0f, lim, unc;
    int i, n = 5, deg;

    while (a->res > 0) {
        if (T->type == NODE_UNOP && T->d.unop.op == UOP_NEG) {
            m = -m;
            T = T->d.unop.operand;
        } else if (T->type == NODE_BINOP &&
                   (T->d.binop.op == OP_ADD || T->d.binop.op == OP_SUB)) {
            limit_inf_term(T->d.binop.left, var, sign, m, a);
            if (T->d.binop.op == OP_SUB) m = -m;
            T = T->d.binop.right;
        } else if (T->type == NODE_BINOP &&
                   (T->d.binop.op == OP_MUL || T->d.binop.op == OP_DIV) &&
                   !ast_contains_var(T->d.binop.right, var) &&
                   integrate_eval_at(T->d.binop.right, var, 0, &lim) &&
                   ce_isfinite(lim) && lim != 0) {
            m = T->d.binop.op == OP_MUL ? m * lim : m / lim;
            T = T->d.binop.left;
        } else if (T->type == NODE_BINOP && T->d.binop.op == OP_MUL &&
                   !ast_contains_var(T->d.binop.left, var) &&
                   integrate_eval_at(T->d.binop.left, var, 0, &lim) &&
                   ce_isfinite(lim) && lim != 0) {
            m *= lim;
            T = T->d.binop.right;
        } else {
            break;
        }
    }
    if (a->res <= 0) return;
    if (++a->n > LIMIT_INF_TERMS) {
        a->res = -1;
        return;
    }
    /* m ln|p(x)| (or m ln p(x)) with p a polynomial of degree >= 1. */
    if (T->type == NODE_FUNC && T->d.func.id == FN_LN) {
        const ast_node_t *p = T->d.func.arg;
        if (p->type == NODE_FUNC && p->d.func.id == FN_ABS) p = p->d.func.arg;
        if (limit_inf_poly(p, var, &deg, &lim) && deg >= 1) {
            a->lsum += m * logf(fabsf(lim));
            a->ldeg += (double)m * deg;
            a->ldeg_max = fmaxf(a->ldeg_max, fabsf(m * (ce_float_t)deg));
            a->unc += 2.4e-7f * fabsf(m * logf(fabsf(lim)));  /* logf */
            return;
        }
    }
    for (i = 0; i < 5; i++) {
        if (!integrate_eval_at(T, var, x, &v[i]) || !ce_isfinite(v[i] *= m)) {
            /* A term that stops evaluating (overflow: e^x past 88, x^7/7
             * past 1e5, or only an intermediate, x^2 in 1/(1e10+x^2))
             * is decided by its structure (asym_of) in eval_limit_inf. */
            a->ut[a->nu] = T;
            a->um[a->nu] = m;
            a->nu++;
            a->failed = true;
            return;
        }
        mag[i] = fabsf(v[i]);
        x *= 10.0f;
    }
    /* A geometric tail converges, whether or not it has settled by 1e4
     * (-2/sqrt(x) for int(1/x^1.5,x,1,inf)). */
    /* Rounding of the sample at 1e6, which every extrapolation starts
     * from (logarithms above are not sampled, so they add none). */
    {
#if CE_INTEG_ENABLE_DEFINITE
        ce_float_t e = integrate_err_scale(T, var, x / 10.0f);
#else
        ce_float_t e = -1;  /* no rounding estimate in this build: refuse */
#endif
        if (!(e >= 0)) {
            a->res = -1;
            return;
        }
        a->unc += fabsf(m) * e;
    }
    if (limit_inf_geometric(v, mag, &lim, &unc)) {
        if (!limit_inf_settle(T, var, sign, m, &lim, &unc, mag[4])) {
            a->res = -1;
            return;
        }
        a->sum += lim;
        a->unc += unc;
        return;
    }
    a->ut[a->nu] = T;
    a->um[a->nu] = m;
    a->nu++;
    for (i = 0; i < n; i++) {
        a->rv[i] += v[i];
        a->ra[i] += mag[i];
    }
}

static CE_INTEG_NOINLINE int8_t eval_limit_inf(const ast_node_t *F, char var,
                                               int8_t sign, ce_float_t *val,
                                               ce_float_t *unc)
{
    limit_inf_acc_t a;
    ce_float_t lim, u;
    bool logs_cancel;

    memset(&a, 0, sizeof a);
    a.res = 1;
    limit_inf_term(F, var, sign, 1.0f, &a);
    if (a.res <= 0) return a.res;
    /* The m deg(p) must cancel exactly (a double holds their sum exactly):
     * 1000000 ln|x| - 999999.875 ln|x+1| leaves 0.125 ln|x|, which
     * diverges (review rounds 4-5). Partial fractions give exact
     * negatives (0.05, -0.05). */
    logs_cancel = a.ldeg == 0;
    if (a.nu == 0) {
        /* An uncancelled m ln|x| grows: the caller tests divergence. */
        if (!logs_cancel) return 0;
        *val = a.sum + a.lsum;
        *unc = a.unc;
        return 1;
    }
    /* Moving terms that are not logarithms of polynomials, together: their
     * structure must give a limit that the samples agree with (as in
     * limit_inf_settle). */
    {
        asym_t g = asym_pow(0, 0, 0);
        int i;
        for (i = 0; i < a.nu; i++)
            g = asym_add(g, asym_mul(asym_of(a.ut[i], var, sign, 0),
                                     asym_pow(a.um[i], 0, 0)));
        if (logs_cancel && !a.failed && limit_inf_geometric(a.rv, a.ra, &lim, &u)) {
            double sl;
            if (!asym_lim(&g, &sl) ||
                !(fabs((double)lim - sl) <= 1e-5 * (fabs(sl) + a.ra[4]) + 8.0 * u))
                return -1;  /* see limit_inf_settle */
            lim = (ce_float_t)sl;
            u = 2.4e-7f * (ce_float_t)fabs(sl);
            *val = a.sum + a.lsum + lim;
            *unc = a.unc + u;
            return 1;
        }
    }
    /* Only an F whose moving part is unbounded by its structure reaches the
     * divergence test (x ln x - x, (x^4)/4, e^x, x + ln|x|); a slow tail
     * (1/x^1.001), a saturating one (-1e6 e^(-1e-6 x)), a bounded F whose
     * parts overflow (-1/cosh x) or cancelling growth (ln|1+e^x| - x)
     * refuses (review rounds 3-6). The settled terms are bounded. */
    {
        asym_t g = asym_pow(a.ldeg, 0, 1);
        int i;
        for (i = 0; i < a.nu; i++) {
            asym_t t = asym_of(a.ut[i], var, sign, 0);
            t = asym_mul(t, asym_pow(a.um[i], 0, 0));
            g = asym_add(g, t);
        }
        if (asym_pow_unbounded(&g) || g.kind == ASYM_EXP) return 0;
    }
    return -1;
}

/*
 * Helper: check if F(x) diverges as x → ±infinity.
 * Checks multiple scales to detect any kind of unbounded growth.
 */
static CE_INTEG_NOINLINE bool eval_diverges_inf(const ast_node_t *F, char var, int8_t sign)
{
    ce_float_t v[3];
    ce_float_t x = sign * 10.0f;
    int i;

    /* Probe x = sign*10, sign*1000, sign*100000. Overflow (an OVERFLOW
     * error or a non-finite value) counts as divergence. Any other error
     * (domain, pole, recursion limit) is no evidence either way: report
     * "not divergent", so the caller gives up instead of saying DIVERGES. */
    for (i = 0; i < 3; i++) {
        ce_error_t err;
        v[i] = eval_at(F, var, x, &err);
        if (err == CE_ERR_OVERFLOW || (err == CE_OK && !ce_isfinite(v[i]))) return true;
        if (err != CE_OK) return false;
        x *= 100.0f;
    }

    /* If values keep moving in the same direction (monotone unbounded), diverges.
     * This catches logarithmic, polynomial, and faster growth. */
    if ((v[2] > v[1] && v[1] > v[0] && fabsf(v[2] - v[0]) > 1.0f) ||
        (v[2] < v[1] && v[1] < v[0] && fabsf(v[2] - v[0]) > 1.0f))
        return true;

    return false;
}

#if CE_INTEG_IMPROPER_USE_LIMITS
/* One-sided limit of the antiderivative F at a finite endpoint where F
 * itself could not be evaluated, via symbolic compute_limit(). Builds
 * without limits.c (CECIDF) omit Case 4. Sampling F or f cannot decide a
 * singular endpoint because terms in a sum can cancel. Supporting Case 4
 * in those builds would require a reliable endpoint-limit method. */
static lim_status_t eval_endpoint_limit(const ast_node_t *F, char var,
                                        ce_float_t point, int8_t direction,
                                        ce_float_t *out)
{
    ast_node_t *pt = ast_number(point);
    lim_status_t ls;
    if (!pt) return LIM_FAILED;
    ls = compute_limit(F, var, pt, direction, out);
    ast_free(pt);
    if (ls == LIM_OK && !ce_isfinite(*out)) return LIM_FAILED;
    return ls;
}
#endif

static improp_status_t integrate_improper_raw(const ast_node_t *expr, char var,
                                              ce_float_t lo, bool inf_lo,
                                              ce_float_t hi, bool inf_hi,
                                              ce_float_t *result);

/* Float products can overflow to inf without an eval error, so a "converged"
 * improper value is only reported when it is finite (int(1e30*x,x,0,1e5)
 * used to print "= inf  (converges)"). */
#if CE_INTEG_IMPROPER_USE_LIMITS && CE_INTEG_ENABLE_DEFINITE
/* F's rounding scale at p, or (twice) at p + (toward - p)/1024 when F is
 * undefined at p; -1 when neither evaluates. Plus, as in
 * integrate_definite, f(p) times the rounding of a bound that is not an
 * exact integer (f(99.99) = -40000 and 99.99 off by 4e-6: review round
 * 13 printed 599.646 for 600). Where f fails, p is the split point, an
 * exact float the scan found, or a singular bound F's limit covers. */
static ce_float_t integ_end_unc(const ast_node_t *f, const ast_node_t *F, char var,
                                ce_float_t p, ce_float_t toward)
{
    ce_float_t fp, s = integ_err_scale(F, var, p);
    if (s < 0) {
        s = integ_err_scale(F, var, p + (toward - p) * 0.0009765625f);
        if (s < 0) return -1;
        s += s;
    }
    if (!integ_exact_int(p) && integrate_eval_at(f, var, p, &fp) && ce_isfinite(fp))
        s += fabsf(fp) * 5.9604645e-8f * fabsf(p);
    return ce_isfinite(s) ? s : -1;
}
#endif

ce_float_t integrate_improper_unc(void)
{
    return s_improp_unc;
}

improp_status_t integrate_improper(const ast_node_t *expr, char var,
                                    ce_float_t lo, bool inf_lo,
                                    ce_float_t hi, bool inf_hi,
                                    ce_float_t *result)
{
    improp_status_t st;
    s_improp_unc = -1;
    st = integrate_improper_raw(expr, var, lo, inf_lo, hi, inf_hi, result);
    if (st == IMPROP_OK && !ce_isfinite(*result)) {
        *result = 0;
        return IMPROP_FAILED;
    }
    return st;
}

static improp_status_t integrate_improper_raw(const ast_node_t *expr, char var,
                                              ce_float_t lo, bool inf_lo,
                                              ce_float_t hi, bool inf_hi,
                                              ce_float_t *result)
{
    char buf[80];
    ast_node_t *F;
    eval_ctx_t ctx;
    int sc;

#ifndef CE_DISABLE_STEP_RECORDING
    ast_to_str(expr, buf, sizeof(buf));
#else
    buf[0] = '\0';  /* only step text uses it */
#endif
    *result = 0;

    /* Strategy: get symbolic antiderivative F(x), then evaluate limits.
     * int(a, inf) f(x)dx = lim(x→inf) F(x) - F(a)
     * int(-inf, b) f(x)dx = F(b) - lim(x→-inf) F(x)
     * int(-inf, inf) = split at 0
     */

    /* Orientation. The cases below assume an infinite lo is -inf, an
     * infinite hi is +inf, and finite lo <= hi (Case 4 approaches hi from
     * below and lo from above). An infinite bound may carry its sign in lo/hi
     * (the processor passes the real values; older callers pass 0, which
     * reads as the usual orientation). Otherwise evaluate int(hi..lo) and
     * negate; equal infinite bounds give 0. */
    if (inf_lo && inf_hi && (lo > 0) == (hi > 0) && lo != 0 && hi != 0) {
        *result = 0;
        s_improp_unc = 0;
        return IMPROP_OK;
    }
    if ((!inf_lo && !inf_hi && lo > hi) || (inf_lo && lo > 0) || (inf_hi && hi < 0)) {
        improp_status_t rs = integrate_improper(expr, var, hi, inf_hi, lo, inf_lo, result);
        if (rs == IMPROP_OK) *result = -*result;
        return rs;
    }

    /* ── Case 1: Both bounds infinite ────────────────────── */
    if (inf_lo && inf_hi) {
        ce_float_t left_sum, right_sum;
        improp_status_t ls, rs;

        steps_addf("Improper: int(%s, -inf, inf)", buf);
        steps_addf("Split at 0: int(-inf,0) + int(0,inf)");

        ce_float_t lu, ru;

        s_improp_unc = 0;
        ls = integrate_improper(expr, var, 0, true, 0, false, &left_sum);
        lu = s_improp_unc;
        s_improp_unc = 0;
        rs = integrate_improper(expr, var, 0, false, 0, true, &right_sum);
        ru = s_improp_unc;

        /* The halves' uncertainties against their sum: halves near
         * +-1e8 that cancel leave only rounding (review round 4). */
        if (ls == IMPROP_OK && rs == IMPROP_OK &&
            (lu + ru) * 8192.0f > fabsf(left_sum + right_sum)) {
            /* Both halves converge; an odd integrand's sum is exactly 0
             * (int(2*x/(1+x^2)^2,x,-inf,inf), review round 5). Oddness is
             * proved from the expression: samples cannot see an even part
             * that float32 rounding hides (1e9*odd + 1/(1+x^2), round 6). */
            if (integ_parity(expr, var, 0) == -1) {
                steps_addf("Odd integrand: = 0");
                s_improp_unc = 0;
                *result = 0;
                return IMPROP_OK;
            }
            steps_addf("Halves cancel below rounding");
            return IMPROP_FAILED;
        }
        if (ls == IMPROP_OK && rs == IMPROP_OK) {
            s_improp_unc = lu + ru;
            *result = left_sum + right_sum;
            steps_addf("= %.6g + %.6g = %.6g",
                       (double)left_sum, (double)right_sum, (double)*result);
            return IMPROP_OK;
        }
        if (ls == IMPROP_DIVERGES || rs == IMPROP_DIVERGES) {
            steps_addf("DIVERGES");
            return IMPROP_DIVERGES;
        }
        return IMPROP_FAILED;
    }


    /* Try to get symbolic antiderivative */
    sc = steps_count();
    F = integrate(expr, var);
    steps_restore(sc);

    /* ── Cases 2 and 3: one infinite bound ──────────────────
     * int(a, inf)  = lim(x->+inf) F(x) - F(a)          (sgn = +1)
     * int(-inf, b) = F(b) - lim(x->-inf) F(x)
     *              = -(lim(x->-inf) F(x) - F(b))       (sgn = -1)
     * One shared body keeps CECIDF under 51,200 B. */
    if (inf_hi || inf_lo) {
        int8_t sgn = inf_hi ? 1 : -1;
        ce_float_t fin = inf_hi ? lo : hi;

        steps_addf("Improper: int(%s) with %s bound %.4g",
                   buf, inf_hi ? "lower" : "upper", (double)fin);

        if (F) {
            ce_float_t F_fin, F_inf, F_unc = 0;
            int8_t lr;
#ifndef CE_DISABLE_STEP_RECORDING
            char fbuf[60];
            ast_to_str(F, fbuf, sizeof(fbuf));
            steps_addf("F(%c) = %s", var, fbuf);
#endif

            /* Evaluate F at the finite bound */
            eval_init(&ctx);
            eval_set_var(&ctx, var, fin);
            F_fin = eval_expr(&ctx, F);
            if (eval_error(&ctx) != CE_OK) { ast_free(F); return IMPROP_FAILED; }

            steps_addf("F(%.4g) = %.6g", (double)fin, (double)F_fin);

            /* Evaluate lim(x->sgn*inf) F(x) */
            lr = eval_limit_inf(F, var, sgn, &F_inf, &F_unc);
            if (lr > 0) {
                /* Refuse what float32 rounding swamps, with the finite
                 * case's 2^-13 budget: F(a)'s rounding, F's at 1e6 and the
                 * limit's own uncertainty (review round 3:
                 * int(10000/(1+x^2),x,10000,inf) printed 0.999023, and
                 * with 1e7 printed 0). Below that budget the last shown
                 * digit can be off as in the finite case: no tighter one
                 * separates 1000 e^(atan x - 1.570795) on [100,inf)
                 * (9.94983, true 9.94985) from ln|x| - ln|x+1| on [50,inf)
                 * (right), whose estimates are 4.5e-5 and 4.2e-5 of the
                 * result (review round 10). */
#if CE_INTEG_ENABLE_DEFINITE
                ce_float_t sa = integrate_err_scale(F, var, fin);
                ce_float_t fa, u = sa + F_unc;
                if (!integ_exact_int(fin) && integrate_eval_at(expr, var, fin, &fa))
                    u += fabsf(fa) * 5.9604645e-8f * fabsf(fin);
#else
                /* Unreachable: eval_limit_inf refuses without the rounding
                 * estimate (limit_inf_term). */
                ce_float_t sa = -1, u = 0;
#endif
                if (sa < 0 || !ce_isfinite(u) ||
                    u * 8192.0f > fabsf(F_inf - F_fin)) {
                    steps_addf("F(inf) - F(%.4g): rounding too large", (double)fin);
                    ast_free(F);
                    return IMPROP_FAILED;
                }
                s_improp_unc = u;
                *result = (ce_float_t)sgn * (F_inf - F_fin);
                steps_addf("lim(x->%sinf) F(x) = %.6g",
                           sgn > 0 ? "" : "-", (double)F_inf);
                steps_addf("= %.6g", (double)*result);
                ast_free(F);
                return IMPROP_OK;
            }

            /* Limit didn't converge → diverges */
            if (lr == 0 && eval_diverges_inf(F, var, sgn)) {
                steps_addf("F(x) diverges as x->%sinf: DIVERGES",
                           sgn > 0 ? "" : "-");
                ast_free(F);
                return IMPROP_DIVERGES;
            }

            ast_free(F);
        }
        return IMPROP_FAILED;
    }

#if CE_INTEG_IMPROPER_USE_LIMITS
    /* ── Case 4: Finite bounds (possible discontinuity) ──── */
    if (F) {
        ce_float_t F_hi = 0, F_lo_val = 0;
        bool hi_ok = false, lo_ok = false;
        lim_status_t ls;

        eval_init(&ctx);
        eval_set_var(&ctx, var, hi);
        F_hi = eval_expr(&ctx, F);
        /* A finite float can overflow to inf without an eval error. */
        if (eval_error(&ctx) == CE_OK && ce_isfinite(F_hi)) {
            hi_ok = true;
        } else {
            ls = eval_endpoint_limit(F, var, hi, -1, &F_hi);
            if (ls == LIM_OK) {
                hi_ok = true;
            } else if (ls == LIM_POS_INF || ls == LIM_NEG_INF) {
                ast_free(F);
                return IMPROP_DIVERGES;
            } else {
                ast_free(F);
                return IMPROP_FAILED;
            }
        }

        eval_init(&ctx);
        eval_set_var(&ctx, var, lo);
        F_lo_val = eval_expr(&ctx, F);
        if (eval_error(&ctx) == CE_OK && ce_isfinite(F_lo_val)) {
            lo_ok = true;
        } else {
            ls = eval_endpoint_limit(F, var, lo, 1, &F_lo_val);
            if (ls == LIM_OK) {
                lo_ok = true;
            } else if (ls == LIM_POS_INF || ls == LIM_NEG_INF) {
                ast_free(F);
                return IMPROP_DIVERGES;
            } else {
                ast_free(F);
                return IMPROP_FAILED;
            }
        }

        if (!hi_ok || !lo_ok) { ast_free(F); return IMPROP_FAILED; }
        *result = F_hi - F_lo_val;
#if CE_INTEG_ENABLE_DEFINITE
        /* F's rounding at each bound, or just inside it when the value is
         * an endpoint limit. Callers check it against what they return:
         * halves split at a removable point can cancel (review round 12,
         * 10000(x-100) + ... on [99,101] printed 8, true 7). */
        {
            ce_float_t uh = integ_end_unc(expr, F, var, hi, lo);
            ce_float_t ul = integ_end_unc(expr, F, var, lo, hi);
            s_improp_unc = uh < 0 || ul < 0 ? -1 : uh + ul;
        }
#endif
        ast_free(F);
        return IMPROP_OK;
    }

#else
    /* Finite bounds need endpoint limits, which this build lacks
     * (CE_INTEG_IMPROPER_USE_LIMITS=0): report that it cannot decide. */
    if (F) ast_free(F);
#endif
    return IMPROP_FAILED;
}

/* ═══════════════════════════════════════════════════════════ */
/*         NUMERICAL INTEGRATION WITH ERROR BOUNDS             */
/* ═══════════════════════════════════════════════════════════ */

/* Helper: evaluate f(x) at a point */
static ce_float_t numint_eval(const ast_node_t *expr, char var,
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

/*
 * Estimate max|f^(k)(x)| on [lo,hi] via central differences.
 * For k=2: f''(x) ~ (f(x+h) - 2f(x) + f(x-h)) / h^2
 * For k=4: f''''(x) ~ (f(x+2h) - 4f(x+h) + 6f(x) - 4f(x-h) + f(x-2h)) / h^4
 */
static ce_float_t estimate_max_deriv(const ast_node_t *expr, char var,
                                      ce_float_t lo, ce_float_t hi,
                                      int order)
{
    ce_float_t M = 0;
    ce_float_t range = hi - lo;
    ce_float_t h = range / 100.0f;
    int samples = 20;
    ce_float_t step = range / samples;
    int i;
    ce_error_t err;

    if (h < 1e-4f) h = 1e-4f;

    for (i = 0; i <= samples; i++) {
        ce_float_t x = lo + i * step;
        ce_float_t val = 0;

        if (order == 2) {
            ce_float_t fp = numint_eval(expr, var, x + h, &err);
            if (err != CE_OK) continue;
            ce_float_t f0 = numint_eval(expr, var, x, &err);
            if (err != CE_OK) continue;
            ce_float_t fm = numint_eval(expr, var, x - h, &err);
            if (err != CE_OK) continue;
            val = (fp - 2 * f0 + fm) / (h * h);
        } else if (order == 4) {
            ce_float_t fp2 = numint_eval(expr, var, x + 2*h, &err);
            if (err != CE_OK) continue;
            ce_float_t fp1 = numint_eval(expr, var, x + h, &err);
            if (err != CE_OK) continue;
            ce_float_t f0  = numint_eval(expr, var, x, &err);
            if (err != CE_OK) continue;
            ce_float_t fm1 = numint_eval(expr, var, x - h, &err);
            if (err != CE_OK) continue;
            ce_float_t fm2 = numint_eval(expr, var, x - 2*h, &err);
            if (err != CE_OK) continue;
            val = (fp2 - 4*fp1 + 6*f0 - 4*fm1 + fm2) / (h*h*h*h);
        }

        if (ce_isfinite(val) && fabsf(val) > M)
            M = fabsf(val);
    }

    return M;
}

/* ── Trapezoidal Rule ────────────────────────────────────── */

numint_result_t integrate_trapezoidal(const ast_node_t *expr, char var,
                                       ce_float_t lo, ce_float_t hi,
                                       int n_steps)
{
    numint_result_t res = { 0, 0, 0, CE_OK };
    ce_float_t h, sum, M;
    ce_float_t range;
    ce_error_t err;
    char buf[60];
    int i;

    if (n_steps < 2) n_steps = 2;
    if (n_steps > 10000) n_steps = 10000;
    res.n_steps = n_steps;

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Trapezoidal rule: int(%s, %.4g, %.4g)",
               buf, (double)lo, (double)hi);
    steps_addf("n = %d subintervals", n_steps);

    h = (hi - lo) / n_steps;
    range = hi - lo;
    steps_addf("h = (%.4g - %.4g)/%d = %.6g",
               (double)hi, (double)lo, n_steps, (double)h);

    /* T_n = h/2 * [f(x0) + 2*f(x1) + ... + 2*f(x_{n-1}) + f(xn)] */
    sum = 0;
    for (i = 0; i <= n_steps; i++) {
        ce_float_t xi = lo + i * h;
        ce_float_t fi;
        if ((i & 0xFF) == 0 && check_cancel()) {
            res.error = CE_ERR_CANCELLED;
            return res;
        }
        if ((i & 0xFF) == 0) progress_check();
        fi = numint_eval(expr, var, xi, &err);
        if (err != CE_OK) {
            res.error = err;
            return res;
        }
        if (i == 0 || i == n_steps)
            sum += fi;
        else
            sum += 2.0f * fi;
    }
    res.value = sum * h / 2.0f;

    steps_addf("T_%d = %.6g", n_steps, (double)res.value);

    /* Error bound: |E_T| <= (b-a)^3 / (12*n^2) * max|f''(x)| */
    M = estimate_max_deriv(expr, var, lo, hi, 2);
    res.error_bound = (range * range * range) /
                      (12.0f * (ce_float_t)n_steps * (ce_float_t)n_steps) * M;

    steps_addf("max|f''(x)| ~ %.4g", (double)M);
    steps_addf("|E_T| <= %.6g", (double)res.error_bound);

    return res;
}

/* ── Simpson's Rule ──────────────────────────────────────── */

numint_result_t integrate_simpson(const ast_node_t *expr, char var,
                                   ce_float_t lo, ce_float_t hi,
                                   int n_steps)
{
    numint_result_t res = { 0, 0, 0, CE_OK };
    ce_float_t h, sum, M;
    ce_float_t range;
    ce_error_t err;
    char buf[60];
    int i;

    /* Simpson's requires even n */
    if (n_steps < 2) n_steps = 2;
    if (n_steps % 2 != 0) n_steps++;
    if (n_steps > 10000) n_steps = 10000;
    res.n_steps = n_steps;

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Simpson's rule: int(%s, %.4g, %.4g)",
               buf, (double)lo, (double)hi);
    steps_addf("n = %d subintervals", n_steps);

    h = (hi - lo) / n_steps;
    range = hi - lo;
    steps_addf("h = (%.4g - %.4g)/%d = %.6g",
               (double)hi, (double)lo, n_steps, (double)h);

    /* S_n = h/3 * [f(x0) + 4*f(x1) + 2*f(x2) + 4*f(x3) + ... + f(xn)] */
    sum = 0;
    for (i = 0; i <= n_steps; i++) {
        ce_float_t xi = lo + i * h;
        ce_float_t fi;
        if ((i & 0xFF) == 0 && check_cancel()) {
            res.error = CE_ERR_CANCELLED;
            return res;
        }
        if ((i & 0xFF) == 0) progress_check();
        fi = numint_eval(expr, var, xi, &err);
        if (err != CE_OK) {
            res.error = err;
            return res;
        }
        if (i == 0 || i == n_steps)
            sum += fi;
        else if (i % 2 == 1)
            sum += 4.0f * fi;
        else
            sum += 2.0f * fi;
    }
    res.value = sum * h / 3.0f;

    steps_addf("S_%d = %.6g", n_steps, (double)res.value);

    /* Error bound: |E_S| <= (b-a)^5 / (180*n^4) * max|f''''(x)| */
    M = estimate_max_deriv(expr, var, lo, hi, 4);
    {
        ce_float_t n2 = (ce_float_t)n_steps * (ce_float_t)n_steps;
        res.error_bound = (range * range * range * range * range) /
                          (180.0f * n2 * n2) * M;
    }

    steps_addf("max|f''''(x)| ~ %.4g", (double)M);
    steps_addf("|E_S| <= %.6g", (double)res.error_bound);

    return res;
}

/* ═══════════════════════════════════════════════════════════ */
/*             ADDITIONAL CALCULUS FEATURES                     */
/* ═══════════════════════════════════════════════════════════ */

/* ── IVP: Antiderivative with initial condition ──────────── */
/*
 * Given f'(x) = expr and f(x0) = y0, find f(x).
 * Steps: F(x) = ∫expr dx + C  →  C = y0 - F(x0)  →  return F(x) + C
 */

ast_node_t *integrate_ivp(const ast_node_t *expr, char var,
                           ce_float_t x0, ce_float_t y0,
                           ce_float_t *c_out, ce_error_t *err)
{
    ast_node_t *antideriv, *result;
    eval_ctx_t ctx;
    ce_float_t F_x0, C;
    char buf[80], ad_buf[80];

    *err = CE_OK;

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("IVP: f'(%c) = %s, f(%.4g) = %.4g", var, buf,
               (double)x0, (double)y0);

    /* Find antiderivative F(x) */
    antideriv = integrate(expr, var);
    if (!antideriv) {
        *err = CE_ERR_TOO_COMPLEX;
        return NULL;
    }

    ast_to_str(antideriv, ad_buf, sizeof(ad_buf));
    steps_addf("F(%c) = %s + C", var, ad_buf);

    /* Evaluate F(x0) */
    eval_init(&ctx);
    eval_set_var(&ctx, var, x0);
    F_x0 = eval_expr(&ctx, antideriv);
    if (eval_error(&ctx) != CE_OK) {
        *err = eval_error(&ctx);
        ast_free(antideriv);
        return NULL;
    }

    /* Solve for C: y0 = F(x0) + C  →  C = y0 - F(x0) */
    C = y0 - F_x0;

    steps_addf("F(%.4g) = %.6g", (double)x0, (double)F_x0);
    steps_addf("C = %.4g - %.6g = %.6g", (double)y0, (double)F_x0, (double)C);

    if (c_out) *c_out = C;

    /* Build F(x) + C */
    if (fabsf(C) < 1e-10f) {
        result = antideriv;
    } else {
        result = ast_binop(OP_ADD, antideriv, ast_number(C));
    }

    result = ast_simplify(result);

    ast_to_str(result, buf, sizeof(buf));
    steps_addf("f(%c) = %s", var, buf);

    return result;
}

/* ── Tabular integration (repeated IBP as table) ─────────── */
/*
 * Display repeated integration by parts as a table.
 * D column: successive derivatives of u
 * I column: successive integrals of dv
 * Alternating +/- signs
 * Stops when derivative reaches 0 or max 8 rows.
 */

ast_node_t *integrate_tabular(const ast_node_t *u_part,
                               const ast_node_t *dv_part,
                               char var)
{
    ast_node_t *d_col[9], *i_col[9];
    ast_node_t *result = NULL, *term;
    int rows = 0, i;
    char buf[60];

    integ_begin();
    steps_addf("Tabular integration (repeated IBP)");

    /* Initialize: D column starts with u, I column starts with dv */
    d_col[0] = ast_clone(u_part);
    i_col[0] = integ(dv_part, var, 0);

    if (!d_col[0] || !i_col[0]) {
        if (g_integ_too_deep) set_integrate_fail_reason(INTEG_TOO_DEEP_MSG);
        if (d_col[0]) ast_free(d_col[0]);
        if (i_col[0]) ast_free(i_col[0]);
        return NULL;
    }

    rows = 1;

    ast_to_str(u_part, buf, sizeof(buf));
    steps_addf("u = %s", buf);
    ast_to_str(dv_part, buf, sizeof(buf));
    steps_addf("dv = %s", buf);
    steps_addf("D col  |  I col  | sign");

    /* Build table rows */
    for (i = 1; i < 8; i++) {
        ast_node_t *next_d, *next_i;

        /* Next derivative of D column */
        next_d = deriv_diff_quiet(d_col[i-1], var);
        if (!next_d) break;
        next_d = ast_simplify(next_d);

        /* Next integral of I column */
        next_i = integ(i_col[i-1], var, 0);
        if (!next_i) {
            ast_free(next_d);
            break;
        }
        next_i = ast_simplify(next_i);

        d_col[i] = next_d;
        i_col[i] = next_i;
        rows++;

        /* Check if derivative reached 0 */
        if (next_d->type == NODE_NUMBER && fabsf(next_d->d.number) < 1e-10f) {
            break;
        }
    }

    /* Log the table */
    for (i = 0; i < rows; i++) {
        char d_buf[40], i_buf[40];
        ast_to_str(d_col[i], d_buf, sizeof(d_buf));
        ast_to_str(i_col[i], i_buf, sizeof(i_buf));
        steps_addf("  %s  |  %s  |  %c", d_buf, i_buf, (i % 2 == 0) ? '+' : '-');
    }

    /* Build result: sum of d_col[i] * i_col[i] * sign */
    for (i = 0; i < rows; i++) {
        term = ast_binop(OP_MUL, ast_clone(d_col[i]), ast_clone(i_col[i]));

        if (i % 2 == 1) {
            /* Negative sign */
            term = ast_unop(UOP_NEG, term);
        }

        if (!result) {
            result = term;
        } else {
            result = ast_binop(OP_ADD, result, term);
        }
    }

    /* Cleanup */
    for (i = 0; i < rows; i++) {
        ast_free(d_col[i]);
        ast_free(i_col[i]);
    }

    if (result) {
        result = ast_simplify(result);
        ast_to_str(result, buf, sizeof(buf));
        steps_addf("Result = %s", buf);
    }

    return result;
}

/* ── Helper: evaluate expression at a point ──────────────── */

static ce_float_t integ_eval_at(const ast_node_t *expr, char var,
                                 ce_float_t x, ce_error_t *err)
{
    eval_ctx_t ctx;
    eval_init(&ctx);
    eval_set_var(&ctx, var, x);
    {
        ce_float_t val = eval_expr(&ctx, expr);
        *err = eval_error(&ctx);
        return val;
    }
}

/* ── Symbolic arc length ─────────────────────────────────── */
/*
 * Arc length = ∫_a^b sqrt(1 + [f'(x)]^2) dx
 * Try symbolic integration first, fall back to polar_arclen() numerical.
 */

ce_float_t integrate_arclength(const ast_node_t *f, char var,
                                ce_float_t a, ce_float_t b,
                                bool *symbolic_ok, ce_error_t *err)
{
    ast_node_t *fp, *fp2, *one_plus, *integrand, *antideriv;
    ce_float_t result;
    char buf[80];

    *err = CE_OK;
    *symbolic_ok = false;

    ast_to_str(f, buf, sizeof(buf));
    steps_addf("Arc length of %s from %.4g to %.4g", buf, (double)a, (double)b);
    steps_addf("L = int(sqrt(1 + [f'(%c)]^2), %c, %.4g, %.4g)", var, var,
               (double)a, (double)b);

    /* Compute f'(x) */
    fp = deriv_diff_quiet(f, var);
    if (!fp) {
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }

    /* Build integrand: sqrt(1 + [f'(x)]^2) */
    fp2 = ast_binop(OP_POW, ast_clone(fp), ast_number(2));
    one_plus = ast_binop(OP_ADD, ast_number(1), fp2);
    integrand = ast_func(FN_SQRT, one_plus);
    integrand = ast_simplify(integrand);

    /* Try symbolic integration */
    antideriv = integrate(integrand, var);

    if (antideriv) {
        /* Evaluate F(b) - F(a) */
        ce_float_t Fb, Fa;
        ce_error_t e1, e2;

        Fb = integ_eval_at(antideriv, var, b, &e1);
        Fa = integ_eval_at(antideriv, var, a, &e2);

        if (e1 == CE_OK && e2 == CE_OK && ce_isfinite(Fb) && ce_isfinite(Fa)) {
            result = Fb - Fa;
            *symbolic_ok = true;
            steps_addf("Symbolic: L = %.6g", (double)result);
            ast_free(antideriv);
            ast_free(integrand);
            ast_free(fp);
            return result;
        }
        ast_free(antideriv);
    }

    ast_free(integrand);
    ast_free(fp);

    /* Fall back to numerical (polar_arclen) */
    steps_addf("Falling back to numerical integration");
    result = polar_arclen(f, var, a, b, err);
    steps_addf("Numerical: L = %.6g", (double)result);

    return result;
}

/* ── Symbolic surface area of revolution ─────────────────── */
/*
 * SA = 2π * ∫_a^b |f(x)| * sqrt(1 + [f'(x)]^2) dx
 * Falls back to polar_surfrev() numerical.
 */

ce_float_t integrate_surfarea(const ast_node_t *f, char var,
                               ce_float_t a, ce_float_t b,
                               bool *symbolic_ok, ce_error_t *err)
{
    ast_node_t *fp, *fp2, *one_plus, *sqrt_part, *abs_f, *integrand, *antideriv;
    ce_float_t result;
    char buf[80];

    *err = CE_OK;
    *symbolic_ok = false;

    ast_to_str(f, buf, sizeof(buf));
    steps_addf("Surface area of revolution of %s from %.4g to %.4g",
               buf, (double)a, (double)b);
    steps_addf("SA = 2*pi * int(|f(%c)| * sqrt(1 + [f'(%c)]^2), %c, %.4g, %.4g)",
               var, var, var, (double)a, (double)b);

    /* Compute f'(x) */
    fp = deriv_diff_quiet(f, var);
    if (!fp) {
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }

    /* Build integrand: |f(x)| * sqrt(1 + [f'(x)]^2) */
    fp2 = ast_binop(OP_POW, ast_clone(fp), ast_number(2));
    one_plus = ast_binop(OP_ADD, ast_number(1), fp2);
    sqrt_part = ast_func(FN_SQRT, one_plus);
    abs_f = ast_func(FN_ABS, ast_clone(f));
    integrand = ast_binop(OP_MUL, abs_f, sqrt_part);
    integrand = ast_simplify(integrand);

    /* Try symbolic integration */
    antideriv = integrate(integrand, var);

    if (antideriv) {
        ce_float_t Fb, Fa;
        ce_error_t e1, e2;

        Fb = integ_eval_at(antideriv, var, b, &e1);
        Fa = integ_eval_at(antideriv, var, a, &e2);

        if (e1 == CE_OK && e2 == CE_OK && ce_isfinite(Fb) && ce_isfinite(Fa)) {
            result = 2.0f * CE_PI * (Fb - Fa);
            *symbolic_ok = true;
            steps_addf("Symbolic: SA = %.6g", (double)result);
            ast_free(antideriv);
            ast_free(integrand);
            ast_free(fp);
            return result;
        }
        ast_free(antideriv);
    }

    ast_free(integrand);
    ast_free(fp);

    /* Fall back to numerical */
    steps_addf("Falling back to numerical integration");
    result = polar_surfrev(f, var, a, b, err);
    steps_addf("Numerical: SA = %.6g", (double)result);

    return result;
}

/* ── Symbolic volume: disk method ────────────────────────── */
/*
 * V = π * ∫_a^b [f(x)]^2 dx
 * Falls back to polar_disk() numerical.
 */

ce_float_t integrate_disk(const ast_node_t *f, char var,
                           ce_float_t a, ce_float_t b,
                           bool *symbolic_ok, ce_error_t *err)
{
    ast_node_t *f2, *antideriv;
    ce_float_t result;
    char buf[80];

    *err = CE_OK;
    *symbolic_ok = false;

    ast_to_str(f, buf, sizeof(buf));
    steps_addf("Disk method: V = pi * int([%s]^2, %c, %.4g, %.4g)",
               buf, var, (double)a, (double)b);

    /* Build integrand: [f(x)]^2 */
    f2 = ast_binop(OP_POW, ast_clone(f), ast_number(2));
    f2 = ast_simplify(f2);

    /* Try symbolic integration */
    antideriv = integrate(f2, var);

    if (antideriv) {
        ce_float_t Fb, Fa;
        ce_error_t e1, e2;

        Fb = integ_eval_at(antideriv, var, b, &e1);
        Fa = integ_eval_at(antideriv, var, a, &e2);

        if (e1 == CE_OK && e2 == CE_OK && ce_isfinite(Fb) && ce_isfinite(Fa)) {
            result = CE_PI * (Fb - Fa);
            *symbolic_ok = true;
            steps_addf("Symbolic: V = pi * %.6g = %.6g",
                       (double)(Fb - Fa), (double)result);
            ast_free(antideriv);
            ast_free(f2);
            return result;
        }
        ast_free(antideriv);
    }

    ast_free(f2);

    /* Fall back to numerical */
    steps_addf("Falling back to numerical integration");
    result = polar_disk(f, var, a, b, err);
    steps_addf("Numerical: V = %.6g", (double)result);

    return result;
}

/* ── Symbolic volume: washer method ──────────────────────── */
/*
 * V = π * ∫_a^b ([R(x)]^2 - [r(x)]^2) dx
 * Falls back to polar_washer() numerical.
 */

ce_float_t integrate_washer(const ast_node_t *R, const ast_node_t *r,
                             char var, ce_float_t a, ce_float_t b,
                             bool *symbolic_ok, ce_error_t *err)
{
    ast_node_t *R2, *r2, *diff_expr, *antideriv;
    ce_float_t result;
    char rb[40], sb[40];

    *err = CE_OK;
    *symbolic_ok = false;

    ast_to_str(R, rb, sizeof(rb));
    ast_to_str(r, sb, sizeof(sb));
    steps_addf("Washer method: V = pi * int([%s]^2 - [%s]^2, %c, %.4g, %.4g)",
               rb, sb, var, (double)a, (double)b);

    /* Build integrand: [R(x)]^2 - [r(x)]^2 */
    R2 = ast_binop(OP_POW, ast_clone(R), ast_number(2));
    r2 = ast_binop(OP_POW, ast_clone(r), ast_number(2));
    diff_expr = ast_binop(OP_SUB, R2, r2);
    diff_expr = ast_simplify(diff_expr);

    /* Try symbolic integration */
    antideriv = integrate(diff_expr, var);

    if (antideriv) {
        ce_float_t Fb, Fa;
        ce_error_t e1, e2;

        Fb = integ_eval_at(antideriv, var, b, &e1);
        Fa = integ_eval_at(antideriv, var, a, &e2);

        if (e1 == CE_OK && e2 == CE_OK && ce_isfinite(Fb) && ce_isfinite(Fa)) {
            result = CE_PI * (Fb - Fa);
            *symbolic_ok = true;
            steps_addf("Symbolic: V = pi * %.6g = %.6g",
                       (double)(Fb - Fa), (double)result);
            ast_free(antideriv);
            ast_free(diff_expr);
            return result;
        }
        ast_free(antideriv);
    }

    ast_free(diff_expr);

    /* Fall back to numerical */
    steps_addf("Falling back to numerical integration");
    result = polar_washer(R, r, var, a, b, err);
    steps_addf("Numerical: V = %.6g", (double)result);

    return result;
}

/* ── Symbolic volume: shell method ───────────────────────── */
/*
 * V = 2π * ∫_a^b x * f(x) dx
 * Falls back to polar_shell() numerical.
 */

ce_float_t integrate_shell(const ast_node_t *f, char var,
                            ce_float_t a, ce_float_t b,
                            bool *symbolic_ok, ce_error_t *err)
{
    ast_node_t *xf, *antideriv;
    ce_float_t result;
    char buf[80];

    *err = CE_OK;
    *symbolic_ok = false;

    ast_to_str(f, buf, sizeof(buf));
    steps_addf("Shell method: V = 2*pi * int(%c * %s, %c, %.4g, %.4g)",
               var, buf, var, (double)a, (double)b);

    /* Build integrand: x * f(x) */
    xf = ast_binop(OP_MUL, ast_variable(var), ast_clone(f));
    xf = ast_simplify(xf);  /* e.g. x*x → x^2 */

    /* Try symbolic integration */
    antideriv = integrate(xf, var);

    if (antideriv) {
        ce_float_t Fb, Fa;
        ce_error_t e1, e2;

        Fb = integ_eval_at(antideriv, var, b, &e1);
        Fa = integ_eval_at(antideriv, var, a, &e2);

        if (e1 == CE_OK && e2 == CE_OK && ce_isfinite(Fb) && ce_isfinite(Fa)) {
            result = 2.0f * CE_PI * (Fb - Fa);
            *symbolic_ok = true;
            steps_addf("Symbolic: V = 2*pi * %.6g = %.6g",
                       (double)(Fb - Fa), (double)result);
            ast_free(antideriv);
            ast_free(xf);
            return result;
        }
        ast_free(antideriv);
    }

    ast_free(xf);

    /* Fall back to numerical */
    steps_addf("Falling back to numerical integration");
    result = polar_shell(f, var, a, b, err);
    steps_addf("Numerical: V = %.6g", (double)result);

    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*              AREA BETWEEN CURVES                            */
/* ═══════════════════════════════════════════════════════════ */

/* Maximum number of intersection points we track on [a,b]. */
#define AREA_MAX_CROSSINGS 16

/* Sample resolution for intersection detection. */
#define AREA_SAMPLE_N 64

/*
 * Helper: compute f(x) - g(x) at a single x value.
 * Returns (ce_float_t)NAN on error.
 */
static ce_float_t area_diff_at(const ast_node_t *f, const ast_node_t *g,
                                char var, ce_float_t x)
{
    eval_ctx_t ctx;
    ce_float_t fv, gv;

    eval_init(&ctx);
    eval_set_var(&ctx, var, x);
    fv = eval_expr(&ctx, f);
    if (eval_error(&ctx) != CE_OK || !ce_isfinite(fv)) return (ce_float_t)NAN;

    eval_init(&ctx);
    eval_set_var(&ctx, var, x);
    gv = eval_expr(&ctx, g);
    if (eval_error(&ctx) != CE_OK || !ce_isfinite(gv)) return (ce_float_t)NAN;

    return fv - gv;
}

/*
 * Bisect: find root of f(x)-g(x) on (lo, hi) where signs differ.
 * 40 iterations of bisection are more than enough for float32.
 */
static ce_float_t area_bisect(const ast_node_t *f, const ast_node_t *g,
                               char var, ce_float_t lo, ce_float_t hi)
{
    int iter;
    ce_float_t mid, v;
    ce_float_t vlo = area_diff_at(f, g, var, lo);

    for (iter = 0; iter < 40; iter++) {
        mid = 0.5f * (lo + hi);
        v   = area_diff_at(f, g, var, mid);
        if (!ce_isfinite(v) || fabsf(v) < 1e-7f || (hi - lo) < 1e-8f)
            return mid;
        if (vlo * v < 0) {
            hi = mid;
        } else {
            lo  = mid;
            vlo = v;
        }
    }
    return 0.5f * (lo + hi);
}

/*
 * Numerical integral of |f-g| on [x0, x1] using composite Simpson's rule.
 * n must be even; we use 32 sub-intervals.
 */
static ce_float_t area_numpiece(const ast_node_t *f, const ast_node_t *g,
                                 char var, ce_float_t x0, ce_float_t x1)
{
    const int n = 32;
    ce_float_t h = (x1 - x0) / (ce_float_t)n;
    ce_float_t sum = 0;
    int k;

    for (k = 0; k <= n; k++) {
        ce_float_t xk = x0 + (ce_float_t)k * h;
        ce_float_t d  = area_diff_at(f, g, var, xk);
        ce_float_t w;
        if (!ce_isfinite(d)) continue;
        if (d < 0) d = -d;
        if (k == 0 || k == n) w = 1.0f;
        else if (k % 2 == 1)  w = 4.0f;
        else                   w = 2.0f;
        sum += w * d;
    }
    return (h / 3.0f) * sum;
}

/*
 * Symbolic integral of (top - bottom) on [x0, x1].
 * top = f when f_is_top, else g.  Sets *ok=true on success.
 */
static ce_float_t area_sympiece(const ast_node_t *f, const ast_node_t *g,
                                 char var, ce_float_t x0, ce_float_t x1,
                                 bool f_is_top, bool *ok)
{
    ast_node_t *fc, *gc, *diff;
    ce_error_t  serr;
    ce_float_t  result;
    int saved_steps;

    *ok = false;
    fc = ast_clone(f);
    gc = ast_clone(g);
    if (!fc || !gc) { ast_free(fc); ast_free(gc); return (ce_float_t)NAN; }

    diff = f_is_top ? ast_binop(OP_SUB, fc, gc)
                    : ast_binop(OP_SUB, gc, fc);
    if (!diff) { ast_free(fc); ast_free(gc); return (ce_float_t)NAN; }

    saved_steps = steps_count();
    result = integrate_definite(diff, var, x0, x1, NULL, 0, &serr);
    steps_restore(saved_steps);

    ast_free(diff);

    if (serr != CE_OK || !ce_isfinite(result) || result < 0) {
        *ok = false;
        return (ce_float_t)NAN;
    }
    *ok = true;
    return result;
}

ce_float_t area_between_curves(const ast_node_t *f, const ast_node_t *g,
                                char var, ce_float_t a, ce_float_t b,
                                ce_error_t *err)
{
    char fbuf[80], gbuf[80];
    ce_float_t crossings[AREA_MAX_CROSSINGS];
    int n_cross = 0;
    ce_float_t bounds[AREA_MAX_CROSSINGS + 2];
    int n_bounds;
    ce_float_t total_area = 0;
    ce_float_t step_size;
    ce_float_t prev_x, prev_d;
    int i;

    *err = CE_OK;
    if (!f || !g || a >= b) { *err = CE_ERR_SYNTAX; return 0; }

    ast_to_str(f, fbuf, sizeof(fbuf));
    ast_to_str(g, gbuf, sizeof(gbuf));

    steps_addf("Area between curves: A = int|(%s)-(%s)| d%c on [%.4g,%.4g]",
               fbuf, gbuf, var, (double)a, (double)b);

    /* ── Step 1: Find intersections ─────────────────────────── */
    steps_addf("Step 1: Finding intersections on [%.4g,%.4g]", (double)a, (double)b);

    step_size = (b - a) / (ce_float_t)AREA_SAMPLE_N;
    prev_x    = a;
    prev_d    = area_diff_at(f, g, var, a);

    for (i = 1; i <= AREA_SAMPLE_N && n_cross < AREA_MAX_CROSSINGS; i++) {
        ce_float_t x = a + (ce_float_t)i * step_size;
        ce_float_t d = area_diff_at(f, g, var, x);

        if (ce_isfinite(prev_d) && ce_isfinite(d) && prev_d * d < 0) {
            ce_float_t root = area_bisect(f, g, var, prev_x, x);
            crossings[n_cross++] = root;
            steps_addf("  Intersection: %c = %.6g", var, (double)root);
        }
        prev_x = x;
        prev_d = d;
    }

    if (n_cross == 0)
        steps_addf("  No intersections found - single region");
    else
        steps_addf("  Found %d intersection(s)", n_cross);

    /* ── Step 2: Build boundary list ───────────────────────── */
    bounds[0] = a;
    for (i = 0; i < n_cross; i++) bounds[i + 1] = crossings[i];
    bounds[n_cross + 1] = b;
    n_bounds = n_cross + 2;

    /* ── Step 3: Integrate each sub-interval ──────────────── */
    steps_addf("Step 2: Integrating %d sub-interval(s)", n_bounds - 1);

    for (i = 0; i + 1 < n_bounds; i++) {
        ce_float_t x0    = bounds[i];
        ce_float_t x1    = bounds[i + 1];
        ce_float_t mid   = 0.5f * (x0 + x1);
        ce_float_t d_mid;
        bool f_is_top;
        ce_float_t piece;
        bool sym_ok;

        if (fabsf(x1 - x0) < 1e-10f) continue;
        d_mid = area_diff_at(f, g, var, mid);
        if (!ce_isfinite(d_mid)) continue;

        f_is_top = (d_mid >= 0);

        if (f_is_top)
            steps_addf("  [%.4g,%.4g]: f > g  top=%s", (double)x0, (double)x1, fbuf);
        else
            steps_addf("  [%.4g,%.4g]: g > f  top=%s", (double)x0, (double)x1, gbuf);

        piece = area_sympiece(f, g, var, x0, x1, f_is_top, &sym_ok);
        if (!sym_ok) {
            piece = area_numpiece(f, g, var, x0, x1);
            steps_addf("  Piece area (numerical) = %.6g", (double)piece);
        } else {
            steps_addf("  Piece area (symbolic)  = %.6g", (double)piece);
        }

        if (!ce_isfinite(piece)) {
            *err = CE_ERR_OVERFLOW;
            steps_addf("  Non-finite piece - aborting");
            return 0;
        }
        total_area += piece;
    }

    steps_addf("Total area = %.6g", (double)total_area);
    return total_area;
}
