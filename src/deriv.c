/*
 * deriv.c — Symbolic differentiation engine
 *
 * Implements:  power rule, sum/diff rule, product rule, quotient rule,
 *              chain rule, trig (sin/cos/tan + inverse), exp, ln, log,
 *              sqrt, abs.
 *
 * Each rule records a step via steps_addf() for the step-by-step UI.
 * The output AST is simplified via ast_simplify() before returning.
 */
#include "deriv.h"
#include "steps.h"
#include "memory.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef CE_DERIV_USE_EVAL
#define CE_DERIV_USE_EVAL 1
#endif

#if CE_DERIV_USE_EVAL
#include "eval.h"
#endif

#ifndef CE_DERIV_USE_SIMPLIFY
#define CE_DERIV_USE_SIMPLIFY 1
#endif

#if CE_DERIV_USE_SIMPLIFY
#include "simplify.h"
#else
static ast_node_t *ast_simplify(ast_node_t *expr)
{
    return expr;
}
#endif

#ifndef CE_DERIV_COMPACT_STEPS
#define CE_DERIV_COMPACT_STEPS 0
#endif
static int g_diff_depth = 0;
#if CE_DERIV_COMPACT_STEPS
static bool g_step_rules_omitted;
#endif

#if CE_DERIV_COMPACT_STEPS && !defined(CE_DISABLE_STEP_RECORDING)
/* Bound only explanatory printing, not the differentiation or simplification
 * limits. Even the first heading can otherwise traverse a 61-level input. */
static bool deriv_step_shallow(const ast_node_t *e, unsigned remaining)
{
    if (!e) return true;
    if (!remaining) return false;
    switch (e->type) {
    case NODE_NUMBER: case NODE_VARIABLE: case NODE_CONSTANT: return true;
    case NODE_BINOP:
        return deriv_step_shallow(e->d.binop.left, remaining - 1) &&
               deriv_step_shallow(e->d.binop.right, remaining - 1);
    case NODE_UNOP: return deriv_step_shallow(e->d.unop.operand, remaining - 1);
    case NODE_FUNC: return deriv_step_shallow(e->d.func.arg, remaining - 1);
    default: return false;
    }
}
static void deriv_step_str(const ast_node_t *e, char *buf, int size)
{
    if (deriv_step_shallow(e, 12)) ast_to_str(e, buf, size);
    else snprintf(buf, size, "(... deep expression)");
}
#else
#define deriv_step_str(e, buf, size) ast_to_str(e, buf, size)
#endif

/* Step text is scratch storage, never live across a recursive diff().
 * Keep both public-operation and per-rule buffers off the 4 KB OS stack.
 * The recorder is deliberately out of line: one shared formatter is smaller
 * and avoids multiplying its frame by the differentiation depth. */
#ifdef CE_DISABLE_STEP_RECORDING
#define DERIV_STEP_BUF(name, n)   char name[1]
#define DERIV_STEP_STR(e, name)   ((void)(e), (name)[0] = '\0')
#define deriv_record_rule(e, rule) ((void)0)
#else
#define DERIV_STEP_BUF(name, n)   static char name[n]
#define DERIV_STEP_STR(e, name)   deriv_step_str((e), (name), sizeof(name))
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void deriv_record_rule(const ast_node_t *e, const char *rule)
{
    static char buf[60];
#if CE_DERIV_COMPACT_STEPS
    /* Printing a deep subtree while diff already owns recursive frames can
     * exceed the OS stack even with static buffers. Keep the full result,
     * but explicitly abbreviate deeply nested explanatory rule text. */
    if (g_diff_depth > 8) {
        if (!g_step_rules_omitted) {
            steps_add("... nested rules omitted");
            g_step_rules_omitted = true;
        }
        return;
    }
#endif
    deriv_step_str(e, buf, sizeof(buf));
    steps_addf("%s rule on %s", rule, buf);
}
#endif

/* ── Forward ─────────────────────────────────────────────── */
static ast_node_t *diff(const ast_node_t *expr, char var);

/* Recursion depth guard — mirrors PARSER_MAX_DEPTH / SIMPLIFY_MAX_DEPTH */
/* Overridable per CE module: each level costs about 54 B of the 4 KB OS
 * stack in CECALC (measured with a stack high-water probe on a real OS 5.8
 * ROM in CEmu), so split/Makefile.calculus lowers it. */
#ifndef DIFF_MAX_DEPTH
#define DIFF_MAX_DEPTH 50
#endif

/* ── Sum / Difference ────────────────────────────────────── */

static ast_node_t *diff_add(const ast_node_t *e, char v, binop_t op)
{
    deriv_record_rule(e, op == OP_ADD ? "Sum" : "Difference");

    return ast_binop(op,
                     diff(e->d.binop.left,  v),
                     diff(e->d.binop.right, v));
}

/* ── Product ─────────────────────────────────────────────── */

static ast_node_t *diff_mul(const ast_node_t *e, char v)
{
    const ast_node_t *f = e->d.binop.left;
    const ast_node_t *g = e->d.binop.right;
    bool fv = ast_contains_var(f, v);
    bool gv = ast_contains_var(g, v);

    if (!fv && !gv) return ast_number(0);

    if (!fv) {
        deriv_record_rule(e, "Const-mult");
        return ast_binop(OP_MUL, ast_clone(f), diff(g, v));
    }
    if (!gv) {
        deriv_record_rule(e, "Const-mult");
        return ast_binop(OP_MUL, diff(f, v), ast_clone(g));
    }

    deriv_record_rule(e, "Product");
    return ast_binop(OP_ADD,
        ast_binop(OP_MUL, diff(f, v), ast_clone(g)),
        ast_binop(OP_MUL, ast_clone(f), diff(g, v)));
}

/* ── Quotient ────────────────────────────────────────────── */

static ast_node_t *diff_div(const ast_node_t *e, char v)
{
    const ast_node_t *f = e->d.binop.left;
    const ast_node_t *g = e->d.binop.right;
    bool fv = ast_contains_var(f, v);
    bool gv = ast_contains_var(g, v);

    if (!fv && !gv) return ast_number(0);

    if (!gv) {
        deriv_record_rule(e, "Const-denom");
        return ast_binop(OP_DIV, diff(f, v), ast_clone(g));
    }

    deriv_record_rule(e, "Quotient");
    return ast_binop(OP_DIV,
        ast_binop(OP_SUB,
            ast_binop(OP_MUL, diff(f, v), ast_clone(g)),
            ast_binop(OP_MUL, ast_clone(f), diff(g, v))),
        ast_binop(OP_POW, ast_clone(g), ast_number(2)));
}

/* ── Power ───────────────────────────────────────────────── */

static ast_node_t *diff_pow(const ast_node_t *e, char v)
{
    const ast_node_t *base = e->d.binop.left;
    const ast_node_t *exp  = e->d.binop.right;
    bool bv = ast_contains_var(base, v);
    bool ev = ast_contains_var(exp,  v);

    if (!bv && !ev) return ast_number(0);

    /* ── Power rule: d/dx(f^n) = n*f^(n-1)*f' ──────── */
    if (bv && !ev) {
        ast_node_t *coeff = ast_binop(OP_MUL,
            ast_clone(exp),
            ast_binop(OP_POW,
                ast_clone(base),
                ast_binop(OP_SUB, ast_clone(exp), ast_number(1))));

        deriv_record_rule(e, "Power");

        /* If base IS the variable, f'=1, skip chain mult */
        if (base->type == NODE_VARIABLE && base->d.variable == v)
            return coeff;

        steps_addf("  Chain rule: * d/d%c(inner)", v);
        return ast_binop(OP_MUL, coeff, diff(base, v));
    }

    /* ── Exponential rule: d/dx(a^g) = a^g * ln(a) * g' ─ */
    if (!bv && ev) {
        ast_node_t *ag = ast_binop(OP_POW, ast_clone(base), ast_clone(exp));
        ast_node_t *gp;
        bool is_e = base->type == NODE_CONSTANT && base->d.constant == CONST_E;

        /* Record before descending, so parent and child steps stay in order. */
        deriv_record_rule(e, "Exp");
        gp = diff(exp, v);

        /* Special case: e^g → e^g * g'  (ln(e)=1) */
        if (is_e) {
            return ast_binop(OP_MUL, ag, gp);
        }

        return ast_binop(OP_MUL,
            ast_binop(OP_MUL, ag, ast_func(FN_LN, ast_clone(base))),
            gp);
    }

    /* ── General: f^g, both depend on var ────────────── */
    deriv_record_rule(e, "General power");
    {
        ast_node_t *fp = diff(base, v);
        ast_node_t *gp = diff(exp,  v);
        /* f^g * (g'*ln(f) + g*f'/f) */
        return ast_binop(OP_MUL,
            ast_binop(OP_POW, ast_clone(base), ast_clone(exp)),
            ast_binop(OP_ADD,
                ast_binop(OP_MUL, gp, ast_func(FN_LN, ast_clone(base))),
                ast_binop(OP_DIV,
                    ast_binop(OP_MUL, ast_clone(exp), fp),
                    ast_clone(base))));
    }
}

/* ── Functions (includes chain rule) ─────────────────────── */

/* The tight CECALC worker trades redundant per-function formulas for the
 * same concise rule/input labels used by binary rules. Other modules and
 * desktop builds retain the full textbook formulas by default. */
#if CE_DERIV_COMPACT_STEPS
#define deriv_function_step(...) ((void)0)
#define deriv_compact_rule(e, rule) deriv_record_rule(e, rule)
#else
#define deriv_function_step steps_addf
#define deriv_compact_rule(e, rule) ((void)0)
#endif

static ast_node_t *diff_func(const ast_node_t *e, char v)
{
    func_id_t          fn = e->d.func.id;
    const ast_node_t  *u  = e->d.func.arg;
    ast_node_t        *up = diff(u, v);         /* u' — always needed */

#if !CE_DERIV_COMPACT_STEPS
    DERIV_STEP_BUF(buf, 60);
    DERIV_STEP_STR(e, buf);
#else
    /* Like the sum/product/power steps, identify the rule and its input;
     * the simplified derivative is recorded by deriv_diff/deriv_nth. One
     * shared call replaces the function-specific format strings.
     * Record only after the recursive diff(u) above. */
    if ((unsigned)fn < FN_FLOOR) {
        deriv_record_rule(e, "Chain");
    }
#endif

    switch (fn) {

    case FN_SIN:
        deriv_function_step("d(sin) on %s: cos(u)*u'", buf);
        return ast_binop(OP_MUL, ast_func(FN_COS, ast_clone(u)), up);

    case FN_COS:
        deriv_function_step("d(cos) on %s: -sin(u)*u'", buf);
        return ast_binop(OP_MUL,
            ast_unop(UOP_NEG, ast_func(FN_SIN, ast_clone(u))), up);

    case FN_TAN:
        deriv_function_step("d(tan) on %s: sec^2(u)*u'", buf);
        return ast_binop(OP_MUL,
            ast_binop(OP_POW, ast_func(FN_SEC, ast_clone(u)),
                      ast_number(2)),
            up);

    case FN_SEC:
        deriv_function_step("d(sec) on %s: sec(u)*tan(u)*u'", buf);
        return ast_binop(OP_MUL,
            ast_binop(OP_MUL,
                ast_func(FN_SEC, ast_clone(u)),
                ast_func(FN_TAN, ast_clone(u))),
            up);

    case FN_CSC:
        deriv_function_step("d(csc) on %s: -csc(u)*cot(u)*u'", buf);
        return ast_binop(OP_MUL,
            ast_unop(UOP_NEG,
                ast_binop(OP_MUL,
                    ast_func(FN_CSC, ast_clone(u)),
                    ast_func(FN_COT, ast_clone(u)))),
            up);

    case FN_COT:
        deriv_function_step("d(cot) on %s: -csc^2(u)*u'", buf);
        return ast_binop(OP_MUL,
            ast_unop(UOP_NEG,
                ast_binop(OP_POW, ast_func(FN_CSC, ast_clone(u)),
                          ast_number(2))),
            up);

    case FN_LN:
        deriv_function_step("d(ln) on %s: u'/u", buf);
        return ast_binop(OP_DIV, up, ast_clone(u));

    case FN_LOG:
        deriv_function_step("d(log) on %s: u'/(u*ln10)", buf);
        return ast_binop(OP_DIV, up,
            ast_binop(OP_MUL, ast_clone(u),
                      ast_func(FN_LN, ast_number(10))));

    case FN_EXP:
        deriv_function_step("d(exp) on %s: exp(u)*u'", buf);
        return ast_binop(OP_MUL, ast_func(FN_EXP, ast_clone(u)), up);

    case FN_SQRT:
        deriv_function_step("d(sqrt) on %s: u'/(2*sqrt(u))", buf);
        return ast_binop(OP_DIV, up,
            ast_binop(OP_MUL, ast_number(2),
                      ast_func(FN_SQRT, ast_clone(u))));

    case FN_ASIN:
        deriv_function_step("d(asin) on %s: u'/sqrt(1-u^2)", buf);
        return ast_binop(OP_DIV, up,
            ast_func(FN_SQRT,
                ast_binop(OP_SUB, ast_number(1),
                    ast_binop(OP_POW, ast_clone(u), ast_number(2)))));

    case FN_ACOS:
        deriv_function_step("d(acos) on %s: -u'/sqrt(1-u^2)", buf);
        return ast_binop(OP_DIV, ast_unop(UOP_NEG, up),
            ast_func(FN_SQRT,
                ast_binop(OP_SUB, ast_number(1),
                    ast_binop(OP_POW, ast_clone(u), ast_number(2)))));

    case FN_ATAN:
        deriv_function_step("d(atan) on %s: u'/(1+u^2)", buf);
        return ast_binop(OP_DIV, up,
            ast_binop(OP_ADD, ast_number(1),
                ast_binop(OP_POW, ast_clone(u), ast_number(2))));

    case FN_ASEC:
        deriv_function_step("d(asec) on %s: u'/(|u|*sqrt(u^2-1))", buf);
        /* d/dx asec(u) = u' / (|u| * sqrt(u^2 - 1)) */
        return ast_binop(OP_DIV, up,
            ast_binop(OP_MUL,
                ast_func(FN_ABS, ast_clone(u)),
                ast_func(FN_SQRT,
                    ast_binop(OP_SUB,
                        ast_binop(OP_POW, ast_clone(u), ast_number(2)),
                        ast_number(1)))));

    case FN_ACSC:
        deriv_function_step("d(acsc) on %s: -u'/(|u|*sqrt(u^2-1))", buf);
        /* d/dx acsc(u) = -u' / (|u| * sqrt(u^2 - 1)) */
        return ast_binop(OP_DIV, ast_unop(UOP_NEG, up),
            ast_binop(OP_MUL,
                ast_func(FN_ABS, ast_clone(u)),
                ast_func(FN_SQRT,
                    ast_binop(OP_SUB,
                        ast_binop(OP_POW, ast_clone(u), ast_number(2)),
                        ast_number(1)))));

    case FN_ACOT:
        deriv_function_step("d(acot) on %s: -u'/(1+u^2)", buf);
        /* d/dx acot(u) = -u' / (1 + u^2) */
        return ast_binop(OP_DIV, ast_unop(UOP_NEG, up),
            ast_binop(OP_ADD, ast_number(1),
                ast_binop(OP_POW, ast_clone(u), ast_number(2))));

    case FN_ABS:
        deriv_function_step("d(|u|) on %s: u*u'/|u|", buf);
        return ast_binop(OP_DIV,
            ast_binop(OP_MUL, ast_clone(u), up),
            ast_func(FN_ABS, ast_clone(u)));

    /* ── Hyperbolic functions ──────────────────────────── */
    case FN_SINH:
        deriv_function_step("d(sinh) on %s: cosh(u)*u'", buf);
        return ast_binop(OP_MUL, ast_func(FN_COSH, ast_clone(u)), up);

    case FN_COSH:
        deriv_function_step("d(cosh) on %s: sinh(u)*u'", buf);
        return ast_binop(OP_MUL, ast_func(FN_SINH, ast_clone(u)), up);

    case FN_TANH:
        deriv_function_step("d(tanh) on %s: (1-tanh^2(u))*u'", buf);
        /* d/dx tanh(u) = (1 - tanh^2(u)) * u' = sech^2(u) * u' */
        return ast_binop(OP_MUL,
            ast_binop(OP_SUB, ast_number(1),
                ast_binop(OP_POW, ast_func(FN_TANH, ast_clone(u)),
                          ast_number(2))),
            up);

    case FN_ASINH:
        deriv_function_step("d(asinh) on %s: u'/sqrt(u^2+1)", buf);
        /* d/dx asinh(u) = u'/sqrt(u^2+1) */
        return ast_binop(OP_DIV, up,
            ast_func(FN_SQRT,
                ast_binop(OP_ADD,
                    ast_binop(OP_POW, ast_clone(u), ast_number(2)),
                    ast_number(1))));

    case FN_ACOSH:
        deriv_function_step("d(acosh) on %s: u'/sqrt(u^2-1)", buf);
        /* d/dx acosh(u) = u'/sqrt(u^2-1) */
        return ast_binop(OP_DIV, up,
            ast_func(FN_SQRT,
                ast_binop(OP_SUB,
                    ast_binop(OP_POW, ast_clone(u), ast_number(2)),
                    ast_number(1))));

    case FN_ATANH:
        deriv_function_step("d(atanh) on %s: u'/(1-u^2)", buf);
        /* d/dx atanh(u) = u'/(1-u^2) */
        return ast_binop(OP_DIV, up,
            ast_binop(OP_SUB, ast_number(1),
                ast_binop(OP_POW, ast_clone(u), ast_number(2))));

    case FN_SECH:
        deriv_function_step("d(sech) on %s: -sech(u)*tanh(u)*u'", buf);
        /* d/dx sech(u) = -sech(u)*tanh(u)*u' */
        return ast_binop(OP_MUL,
            ast_unop(UOP_NEG,
                ast_binop(OP_MUL,
                    ast_func(FN_SECH, ast_clone(u)),
                    ast_func(FN_TANH, ast_clone(u)))),
            up);

    case FN_CSCH:
        deriv_function_step("d(csch) on %s: -csch(u)*coth(u)*u'", buf);
        /* d/dx csch(u) = -csch(u)*coth(u)*u' */
        return ast_binop(OP_MUL,
            ast_unop(UOP_NEG,
                ast_binop(OP_MUL,
                    ast_func(FN_CSCH, ast_clone(u)),
                    ast_func(FN_COTH, ast_clone(u)))),
            up);

    case FN_COTH:
        deriv_function_step("d(coth) on %s: -csch^2(u)*u'", buf);
        /* d/dx coth(u) = -csch^2(u)*u' */
        return ast_binop(OP_MUL,
            ast_unop(UOP_NEG,
                ast_binop(OP_POW, ast_func(FN_CSCH, ast_clone(u)),
                          ast_number(2))),
            up);

    case FN_FLOOR: case FN_CEIL: case FN_ROUND:
    case FN_IPART: case FN_FPART:
    case FN_DEC: case FN_HEX: case FN_BIN: case FN_OCT:
        ast_free(up);
        return ast_number(0);

    /* For real-valued arguments, real(x)=x, conj(x)=x, imag(x)=0 */
    case FN_REAL:
    case FN_CONJ:
        deriv_compact_rule(e, "Real-valued identity");
        deriv_function_step("d(%s) on %s: u' (real-valued)",
                   fn == FN_REAL ? "real" : "conj", buf);
        return up;
    case FN_IMAG:
        deriv_compact_rule(e, "Real-valued zero");
        deriv_function_step("d(imag) on %s: 0 (real-valued)", buf);
        ast_free(up);
        return ast_number(0);
    case FN_ANGLE:
        steps_addf("Cannot differentiate angle() symbolically");
        ast_free(up);
        return NULL;
    case FN_SIGN:
        deriv_compact_rule(e, "Piecewise zero");
        deriv_function_step("d(sign) on %s: 0", buf);
        /* d(sign(u))/dx = 0 (sign is piecewise constant, derivative is 0 a.e.) */
        ast_free(up);
        return ast_number(0);
    case FN_HEAVISIDE:
        deriv_compact_rule(e, "Classical zero");
        deriv_function_step("d(heaviside) on %s: 0 (classical)", buf);
        /* d(H(u))/dx = 0 (classical; distributional derivative is delta) */
        ast_free(up);
        return ast_number(0);

    default:
        ast_free(up);
        return NULL;
    }
}

/* ── Core recursive differentiator ───────────────────────── */

static ast_node_t *diff(const ast_node_t *e, char v)
{
    ast_node_t *result;

    if (!e) return ast_number(0);

#if CE_DERIV_COMPACT_STEPS
    /* Each root (including each order of nderiv) gets one honest notice,
     * even when unary nodes skip recording at the depth boundary. */
    if (g_diff_depth == 0) g_step_rules_omitted = false;
#endif
    /* Recursion depth guard — protects eZ80 4KB stack */
    if (g_diff_depth >= DIFF_MAX_DEPTH) {
        steps_addf("Expression too deeply nested for differentiation");
        return NULL;
    }
    g_diff_depth++;

    /* Quick exit: expression doesn't contain the variable → 0 */
    if (!ast_contains_var(e, v)) {
        g_diff_depth--;
        return ast_number(0);
    }

    result = NULL;
    switch (e->type) {

    case NODE_NUMBER:
    case NODE_CONSTANT:
        result = ast_number(0);
        break;

    case NODE_VARIABLE:
        result = (e->d.variable == v) ? ast_number(1) : ast_number(0);
        break;

    case NODE_BINOP:
        switch (e->d.binop.op) {
        case OP_ADD: result = diff_add(e, v, OP_ADD); break;
        case OP_SUB: result = diff_add(e, v, OP_SUB); break;
        case OP_MUL: result = diff_mul(e, v);          break;
        case OP_DIV: result = diff_div(e, v);          break;
        case OP_POW: result = diff_pow(e, v);          break;
        default:                                        break;
        }
        break;

    case NODE_UNOP:
        if (e->d.unop.op == UOP_NEG) {
            steps_addf("Negate: d(-u) = -u'");
            result = ast_unop(UOP_NEG, diff(e->d.unop.operand, v));
        } else if (e->d.unop.op == UOP_FACT) {
            steps_addf("Cannot differentiate factorial (digamma not implemented)");
            result = NULL;
        }
        break;

    case NODE_FUNC:
        result = diff_func(e, v);
        break;

    default:
        break;
    }

    if (result == NULL && e->type != NODE_UNOP) {
        steps_addf("Cannot differentiate node type %d", e->type);
    }

    g_diff_depth--;
    return result;
}

/* Simplify a fresh diff() result; NULL if the tree is incomplete before or
 * after simplification (pool exhausted).  Takes ownership of `result`. */
static ast_node_t *deriv_finish(ast_node_t *result)
{
    if (!result) return NULL;
    if (ast_tree_complete(result)) {
        result = ast_simplify(result);
        if (ast_tree_complete(result)) return result;
    }
    ast_free(result);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────── */

ast_node_t *deriv_diff(const ast_node_t *expr, char var)
{
    ast_node_t *result;
    DERIV_STEP_BUF(buf, 80);

    /* Detect nested NODE_DERIV (from nderiv parser) → nth derivative */
    if (expr && expr->type == NODE_DERIV) {
        int order = 2;  /* 1 from outer call in main.c + 1 for this node */
        const ast_node_t *innermost = expr->d.deriv.expr;
        while (innermost && innermost->type == NODE_DERIV) {
            order++;
            innermost = innermost->d.deriv.expr;
        }
        return deriv_nth(innermost, var, order);
    }

    DERIV_STEP_STR(expr, buf);
    steps_addf("d/d%c( %s )", var, buf);

    result = deriv_finish(diff(expr, var));
    if (!result) return NULL;

    DERIV_STEP_STR(result, buf);
    steps_addf("= %s", buf);

    return result;
}

ast_node_t *deriv_diff_quiet(const ast_node_t *expr, char var)
{
    int sc = steps_count();
    ast_node_t *result = diff(expr, var);
    steps_restore(sc);
    return deriv_finish(result);
}

/* ── Higher-order derivative: d^n/dx^n ───────────────────── */

ast_node_t *deriv_nth(const ast_node_t *expr, char var, int n)
{
    ast_node_t *current, *next;
    DERIV_STEP_BUF(buf, 80);
    int i;

    if (n <= 0) return ast_clone(expr);

    DERIV_STEP_STR(expr, buf);
    steps_addf("d^%d/d%c^%d( %s )", n, var, n, buf);

    current = ast_clone(expr);

    for (i = 0; i < n; i++) {
        if (i > 0) {
            DERIV_STEP_STR(current, buf);
            steps_addf("Derivative #%d of %s", i + 1, buf);
        }

        /* Free the previous order before simplifying the next one, so
         * simplify has the whole pool minus one tree to work with. */
        next = diff(current, var);
        ast_free(current);
        next = deriv_finish(next);

        if (!next) {
            steps_addf("Cannot differentiate past order %d", i + 1);
            return NULL;
        }

        current = next;
    }

    DERIV_STEP_STR(current, buf);
    steps_addf("= %s", buf);

    return current;
}

/* ── Implicit differentiation: dy/dx from f(x,y)=0 ──────── */

ast_node_t *deriv_implicit(const ast_node_t *expr, char var_x, char var_y)
{
    ast_node_t *f_x, *f_y, *result;
    char buf[80], fx_buf[60], fy_buf[60];

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Implicit diff: d/d%c( %s ) = 0", var_x, buf);

    /* Compute partial derivative w.r.t. x (treat y as constant) */
    f_x = deriv_diff_quiet(expr, var_x);
    if (!f_x) return NULL;

    /* Compute partial derivative w.r.t. y (treat x as constant) */
    f_y = deriv_diff_quiet(expr, var_y);
    if (!f_y) { ast_free(f_x); return NULL; }

    /* Check if f_y is zero (would cause division by zero) */
    if (f_y->type == NODE_NUMBER && fabsf(f_y->d.number) < 1e-10f) {
        ast_free(f_x);
        ast_free(f_y);
        return NULL;
    }

    ast_to_str(f_x, fx_buf, sizeof(fx_buf));
    ast_to_str(f_y, fy_buf, sizeof(fy_buf));
    steps_addf("f_%c = %s", var_x, fx_buf);
    steps_addf("f_%c = %s", var_y, fy_buf);
    steps_addf("d%c/d%c = -f_%c / f_%c = -(%s) / (%s)",
               var_y, var_x, var_x, var_y, fx_buf, fy_buf);

    /* dy/dx = -f_x / f_y */
    result = ast_simplify(
        ast_binop(OP_DIV,
            ast_unop(UOP_NEG, f_x),
            f_y));

    ast_to_str(result, buf, sizeof(buf));
    steps_addf("= %s", buf);

    return result;
}

/* ── Substitute: replace every occurrence of var with sub_expr ── */

static ast_node_t *substitute(const ast_node_t *expr, char var,
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
            substitute(expr->d.binop.left,  var, sub_expr),
            substitute(expr->d.binop.right, var, sub_expr));

    case NODE_UNOP:
        return ast_unop(expr->d.unop.op,
            substitute(expr->d.unop.operand, var, sub_expr));

    case NODE_FUNC:
        return ast_func(expr->d.func.id,
            substitute(expr->d.func.arg, var, sub_expr));

    default:
        return ast_clone(expr);
    }
}

/* ── Logarithmic differentiation ─────────────────────────── */
/*
 * For f(x)^g(x): show step-by-step via logarithmic differentiation.
 *   y = f^g  →  ln(y) = g*ln(f)
 *   y'/y = g'*ln(f) + g*f'/f
 *   y' = f^g * (g'*ln(f) + g*f'/f)
 */

ast_node_t *deriv_log_diff(const ast_node_t *expr, char var)
{
    const ast_node_t *base, *exp_node;
    ast_node_t *fp, *gp, *result;
    char buf[80], fb[80], gb[80];

    if (!expr || expr->type != NODE_BINOP || expr->d.binop.op != OP_POW)
        return NULL;

    base     = expr->d.binop.left;
    exp_node = expr->d.binop.right;

    /* Both must depend on var for log diff to be meaningful */
    if (!ast_contains_var(base, var) || !ast_contains_var(exp_node, var)) {
        /* Fall back to standard diff */
        return deriv_diff(expr, var);
    }

    ast_to_str(expr, buf, sizeof(buf));
    ast_to_str(base, fb, sizeof(fb));
    ast_to_str(exp_node, gb, sizeof(gb));

    steps_addf("Logarithmic differentiation of %s", buf);
    steps_addf("Let y = %s", buf);
    steps_addf("ln(y) = (%s)*ln(%s)", gb, fb);
    steps_addf("y'/y = (%s)'*ln(%s) + (%s)*(%s)'/(%s)", gb, fb, gb, fb, fb);

    fp = deriv_diff_quiet(base, var);
    gp = deriv_diff_quiet(exp_node, var);

    if (!fp || !gp) {
        if (fp) ast_free(fp);
        if (gp) ast_free(gp);
        return NULL;
    }

    /* y' = f^g * (g'*ln(f) + g*f'/f) */
    result = ast_simplify(
        ast_binop(OP_MUL,
            ast_binop(OP_POW, ast_clone(base), ast_clone(exp_node)),
            ast_binop(OP_ADD,
                ast_binop(OP_MUL, gp, ast_func(FN_LN, ast_clone(base))),
                ast_binop(OP_DIV,
                    ast_binop(OP_MUL, ast_clone(exp_node), fp),
                    ast_clone(base)))));

    if (result) {
        ast_to_str(result, buf, sizeof(buf));
        steps_addf("y' = %s", buf);
    }

    return result;
}

/* ── Derivative from limit definition ────────────────────── */
/*
 * Show: f'(x) = lim[h→0] (f(x+h) - f(x)) / h
 * For polynomials, algebraically simplify to cancel h.
 * For complex functions, compute standard derivative and show pedagogical steps.
 */

ast_node_t *deriv_from_definition(const ast_node_t *expr, char var)
{
    ast_node_t *x_plus_h, *f_xh, *f_x, *numer, *quotient;
    ast_node_t *simplified, *standard;
    char buf[80], res_buf[80];

    if (!expr) return NULL;

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Derivative from definition: f(%c) = %s", var, buf);
    steps_addf("f'(%c) = lim[h->0] (f(%c+h) - f(%c)) / h", var, var, var);

    /* Build f(x+h): substitute var with (var + h) */
    x_plus_h = ast_binop(OP_ADD, ast_variable(var), ast_variable('h'));
    f_xh = substitute(expr, var, x_plus_h);
    ast_free(x_plus_h);

    if (!f_xh) return NULL;

    /* Build f(x) */
    f_x = ast_clone(expr);

    /* Build (f(x+h) - f(x)) / h */
    numer = ast_binop(OP_SUB, f_xh, f_x);
    quotient = ast_binop(OP_DIV, numer, ast_variable('h'));

    /* Try to simplify algebraically (works for polynomials) */
    simplified = ast_simplify(ast_clone(quotient));

    if (simplified) {
        char simp_buf[80];
        ast_to_str(simplified, simp_buf, sizeof(simp_buf));
        steps_addf("(f(%c+h) - f(%c)) / h simplifies to: %s", var, var, simp_buf);
    }

    ast_free(quotient);

    /* Compute standard derivative for verification/fallback */
    standard = deriv_diff_quiet(expr, var);

    if (simplified && standard) {
#if CE_DERIV_USE_EVAL
        /* Try evaluating the simplified limit at h=0 */
        eval_ctx_t ctx;
        ce_float_t lim_val, std_val;
        ce_error_t e1, e2;

        eval_init(&ctx);
        eval_set_var(&ctx, var, 1.0f);
        eval_set_var(&ctx, 'h', 0.0f);
        lim_val = eval_expr(&ctx, simplified);
        e1 = eval_error(&ctx);

        eval_init(&ctx);
        eval_set_var(&ctx, var, 1.0f);
        std_val = eval_expr(&ctx, standard);
        e2 = eval_error(&ctx);

        if (e1 == CE_OK && e2 == CE_OK && fabsf(lim_val - std_val) < 0.1f) {
            steps_addf("Taking lim[h->0]: h terms vanish");
        } else {
            steps_addf("Direct limit requires cancellation of h");
        }
#else
        steps_addf("Taking lim[h->0]: h terms vanish");
#endif
    }

    ast_free(simplified);

    if (!standard) {
        steps_addf("Cannot compute derivative");
        return NULL;
    }

    ast_to_str(standard, res_buf, sizeof(res_buf));
    steps_addf("f'(%c) = %s", var, res_buf);

    return standard;
}
