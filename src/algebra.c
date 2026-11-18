/*
 * algebra.c -- Algebra & trigonometry engine
 *
 * Implements:
 *   - Polynomial representation (coefficient array)
 *   - Expand: polynomial conversion + AST-level distribution
 *   - Factor: quadratic formula, rational root theorem
 *   - Solve: linear, quadratic (complex), trig, Newton fallback
 *   - Polynomial long division / synthetic division
 *   - Trig identity simplification (Pythagorean)
 *   - Systems of equations (Cramer's Rule, 2x2 and 3x3)
 *   - Triangle solver (Law of Sines / Law of Cosines)
 *   - Step-by-step recording via steps module
 */
#include "algebra.h"
#include "eval.h"
#include "steps.h"
#include "memory.h"
#include "matrix.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef CE_ALG_USE_SIMPLIFY
#define CE_ALG_USE_SIMPLIFY 1
#endif

#ifndef CE_ALG_SOLVE_BASIC_ONLY
#define CE_ALG_SOLVE_BASIC_ONLY 0
#endif

#ifndef CE_ALG_SOLVE_SKIP_BASIC_POLY
#define CE_ALG_SOLVE_SKIP_BASIC_POLY 0
#endif

#ifndef CE_ALG_SOLVE_ENABLE_ABS
#define CE_ALG_SOLVE_ENABLE_ABS (!CE_ALG_SOLVE_BASIC_ONLY)
#endif

#ifndef CE_ALG_SOLVE_ENABLE_LOG_EXP
#define CE_ALG_SOLVE_ENABLE_LOG_EXP (!CE_ALG_SOLVE_BASIC_ONLY)
#endif

#ifndef CE_ALG_SOLVE_ENABLE_TRIG
#define CE_ALG_SOLVE_ENABLE_TRIG (!CE_ALG_SOLVE_BASIC_ONLY)
#endif

#if CE_ALG_USE_SIMPLIFY
#include "simplify.h"
#else
static ast_node_t *ast_simplify(ast_node_t *expr)
{
    return expr;
}
#endif

/* ═══════════════════════════════════════════════════════════ */
/*                    HELPERS                                   */
/* ═══════════════════════════════════════════════════════════ */

static void poly_init(poly_t *p)
{
    memset(p, 0, sizeof(*p));
}

static void poly_trim(poly_t *p)
{
    while (p->degree > 0 && fabsf(p->coeff[p->degree]) < 1e-8f)
        p->degree--;
}

static ce_float_t poly_eval(const poly_t *p, ce_float_t x)
{
    /* Horner's method */
    ce_float_t r = p->coeff[p->degree];
    int i;
    for (i = p->degree - 1; i >= 0; i--)
        r = r * x + p->coeff[i];
    return r;
}

/* Find first variable in expression */
static char find_var(const ast_node_t *e)
{
    char v;
    if (!e) return 0;
    switch (e->type) {
    case NODE_VARIABLE: return e->d.variable;
    case NODE_BINOP:
        v = find_var(e->d.binop.left);
        return v ? v : find_var(e->d.binop.right);
    case NODE_UNOP: return find_var(e->d.unop.operand);
    case NODE_FUNC: return find_var(e->d.func.arg);
    default: return 0;
    }
}

/* ast_equal() now lives in ast.c as public API */

/* ═══════════════════════════════════════════════════════════ */
/*               AST → POLY CONVERSION                         */
/* ═══════════════════════════════════════════════════════════ */

/* Keep the extra polynomial off every recursive conversion frame. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static bool poly_power(const poly_t *a, int n, poly_t *p)
{
    poly_t tmp;
    int i, j, k;
    if (n == 0) { p->coeff[0] = 1; return true; }
    *p = *a;
    for (k = 1; k < n; k++) {
        poly_init(&tmp);
        if (p->degree + a->degree > MAX_POLY_DEG) return false;
        tmp.degree = p->degree + a->degree;
        for (i = 0; i <= p->degree; i++)
            for (j = 0; j <= a->degree; j++)
                tmp.coeff[i + j] += p->coeff[i] * a->coeff[j];
        poly_trim(&tmp);
        *p = tmp;
    }
    return true;
}

static bool poly_too_deep;

static bool ast_to_poly_inner(const ast_node_t *e, char var, poly_t *p, int depth)
{
    poly_init(p);
    if (!e) return false;
    if (depth >= CE_POLY_MAX_DEPTH) {
        poly_too_deep = true;
        return false;
    }

    switch (e->type) {

    case NODE_NUMBER:
        p->coeff[0] = e->d.number;
        return true;

    case NODE_VARIABLE:
        if (e->d.variable == var) {
            p->coeff[1] = 1;
            p->degree = 1;
            return true;
        }
        return false;

    case NODE_CONSTANT:
        if (e->d.constant == CONST_PI)      { p->coeff[0] = CE_PI; return true; }
        else if (e->d.constant == CONST_E)  { p->coeff[0] = CE_E;  return true; }
        return false;  /* CONST_INF, CONST_I cannot be poly coefficients */

    case NODE_UNOP:
        if (e->d.unop.op == UOP_NEG) {
            int i;
            if (!ast_to_poly_inner(e->d.unop.operand, var, p, depth + 1)) return false;
            for (i = 0; i <= p->degree; i++)
                p->coeff[i] = -p->coeff[i];
            return true;
        }
        return false;

    case NODE_BINOP: {
        poly_t a, b;
        int i, j;

        switch (e->d.binop.op) {

        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV: {
            const ast_node_t *spine = e, *term;
            int count = 0, at, skip;
            /* Walk arithmetic left spines without recursive polynomial
             * frames. Revisit oldest to newest (O(n^2), node-pool bounded)
             * to preserve floating-point order. Right subtrees recurse.
             * Each result is freshly zeroed just like the recursive version:
             * coefficients above a trimmed degree must not be revived. */
            while (e && e->type == NODE_BINOP &&
                   (e->d.binop.op == OP_ADD || e->d.binop.op == OP_SUB ||
                    e->d.binop.op == OP_MUL || e->d.binop.op == OP_DIV)) {
                count++;
                e = e->d.binop.left;
            }
            if (!ast_to_poly_inner(e, var, p, depth + 1)) return false;
            for (at = count - 1; at >= 0; at--) {
                term = spine;
                for (skip = 0; skip < at; skip++) term = term->d.binop.left;
                if (!ast_to_poly_inner(term->d.binop.right, var, &b, depth + 1))
                    return false;
                poly_init(&a);
                switch (term->d.binop.op) {
                case OP_ADD:
                case OP_SUB:
                    a.degree = p->degree > b.degree ? p->degree : b.degree;
                    for (i = 0; i <= a.degree; i++)
                        a.coeff[i] = term->d.binop.op == OP_ADD ?
                                     p->coeff[i] + b.coeff[i] : p->coeff[i] - b.coeff[i];
                    break;
                case OP_MUL:
                    if (p->degree + b.degree > MAX_POLY_DEG) return false;
                    a.degree = p->degree + b.degree;
                    for (i = 0; i <= p->degree; i++)
                        for (j = 0; j <= b.degree; j++)
                            a.coeff[i + j] += p->coeff[i] * b.coeff[j];
                    break;
                default: /* OP_DIV */
                    if (b.degree != 0 || fabsf(b.coeff[0]) < 1e-20f) return false;
                    a = *p;
                    for (i = 0; i <= a.degree; i++) a.coeff[i] /= b.coeff[0];
                    break;
                }
                if (term->d.binop.op != OP_DIV) poly_trim(&a);
                *p = a;
            }
            return true;
        }

        case OP_POW: {
            int n;
            if (e->d.binop.right->type != NODE_NUMBER) return false;
            n = (int)e->d.binop.right->d.number;
            if (n < 0 || n > MAX_POLY_DEG) return false;
            /* x^1.5 is not a polynomial term; (int) would truncate it to
             * x^1 and polydiv(x^1.5,x,x) answered Q = 1 (internal review). */
            if ((ce_float_t)n != e->d.binop.right->d.number) return false;
            if (!ast_to_poly_inner(e->d.binop.left, var, &a, depth + 1)) return false;
            return poly_power(&a, n, p);
        }


        }
        break;
    }

    default:
        return false;
    }
    return false;
}

static bool ast_to_poly(const ast_node_t *e, char var, poly_t *p)
{
    return ast_to_poly_inner(e, var, p, 0);
}

/* ═══════════════════════════════════════════════════════════ */
/*               POLY → AST CONVERSION                         */
/* ═══════════════════════════════════════════════════════════ */

static ast_node_t *poly_to_ast(const poly_t *p, char var)
{
    ast_node_t *result = NULL;
    int i;

    for (i = p->degree; i >= 0; i--) {
        ast_node_t *term;
        ce_float_t c  = p->coeff[i];
        ce_float_t ac = fabsf(c);
        bool neg;

        if (ac < 1e-10f) continue;
        neg = (c < 0);

        /* Build |c| * x^i */
        if (i == 0) {
            term = ast_number(ac);
        } else {
            ast_node_t *xterm;
            if (i == 1)
                xterm = ast_variable(var);
            else
                xterm = ast_binop(OP_POW, ast_variable(var),
                                  ast_number((ce_float_t)i));
            if (fabsf(ac - 1.0f) < 1e-6f)
                term = xterm;
            else
                term = ast_binop(OP_MUL, ast_number(ac), xterm);
        }

        if (!result) {
            result = neg ? ast_unop(UOP_NEG, term) : term;
        } else {
            result = neg ? ast_binop(OP_SUB, result, term)
                         : ast_binop(OP_ADD, result, term);
        }
    }

    return result ? result : ast_number(0);
}

/* ═══════════════════════════════════════════════════════════ */
/*                POLYNOMIAL DIVISION                          */
/* ═══════════════════════════════════════════════════════════ */

static bool poly_divide(const poly_t *a, const poly_t *b,
                         poly_t *q, poly_t *r)
{
    int i, j;
    ce_float_t c;

    if (!a || !b || !q || !r)
        return false;

    if (a->degree < 0 || a->degree > MAX_POLY_DEG ||
        b->degree < 0 || b->degree > MAX_POLY_DEG)
        return false;

    if (fabsf(b->coeff[b->degree]) < 1e-20f)
        return false;

    poly_init(q);
    *r = *a;

    if (a->degree < b->degree) return true; /* q=0, r=a */

    q->degree = a->degree - b->degree;

    for (i = a->degree; i >= b->degree; i--) {
        c = r->coeff[i] / b->coeff[b->degree];
        q->coeff[i - b->degree] = c;
        for (j = 0; j <= b->degree; j++)
            r->coeff[i - b->degree + j] -= c * b->coeff[j];
    }

    r->degree = (b->degree > 0) ? b->degree - 1 : 0;
    poly_trim(r);
    poly_trim(q);
    return true;
}

/* ═══════════════════════════════════════════════════════════ */
/*             AST-LEVEL DISTRIBUTION (non-poly expand)        */
/* ═══════════════════════════════════════════════════════════ */

/* Distribute L * R where one might be a sum.
 * Returns NEW AST.  Does NOT own/free inputs. */
static ast_node_t *distribute(const ast_node_t *L, const ast_node_t *R)
{
    /* L * (Ra + Rb) = L*Ra + L*Rb */
    if (R->type == NODE_BINOP && R->d.binop.op == OP_ADD)
        return ast_binop(OP_ADD,
                distribute(L, R->d.binop.left),
                distribute(L, R->d.binop.right));

    if (R->type == NODE_BINOP && R->d.binop.op == OP_SUB)
        return ast_binop(OP_SUB,
                distribute(L, R->d.binop.left),
                distribute(L, R->d.binop.right));

    /* (La + Lb) * R = La*R + Lb*R */
    if (L->type == NODE_BINOP && L->d.binop.op == OP_ADD)
        return ast_binop(OP_ADD,
                distribute(L->d.binop.left, R),
                distribute(L->d.binop.right, R));

    if (L->type == NODE_BINOP && L->d.binop.op == OP_SUB)
        return ast_binop(OP_SUB,
                distribute(L->d.binop.left, R),
                distribute(L->d.binop.right, R));

    /* No distribution — leaf product */
    return ast_binop(OP_MUL, ast_clone(L), ast_clone(R));
}

/* Recursive AST-level expand.  Returns NEW AST. */
static ast_node_t *do_expand(const ast_node_t *e)
{
    if (!e) return NULL;

    switch (e->type) {
    case NODE_BINOP:
        switch (e->d.binop.op) {
        case OP_MUL: {
            ast_node_t *L = do_expand(e->d.binop.left);
            ast_node_t *R = do_expand(e->d.binop.right);
            ast_node_t *res = distribute(L, R);
            ast_free(L);
            ast_free(R);
            return res;
        }
        case OP_ADD:
        case OP_SUB:
            return ast_binop(e->d.binop.op,
                             do_expand(e->d.binop.left),
                             do_expand(e->d.binop.right));
        case OP_POW: {
            /* Expand (base)^n for small integer n */
            if (e->d.binop.right->type == NODE_NUMBER) {
                int n = (int)e->d.binop.right->d.number;
                if (n >= 2 && n <= 6) {
                    ast_node_t *base = do_expand(e->d.binop.left);
                    ast_node_t *res  = ast_clone(base);
                    ast_node_t *prev;
                    int k;
                    for (k = 1; k < n; k++) {
                        prev = res;
                        res  = distribute(prev, base);
                        ast_free(prev);
                    }
                    ast_free(base);
                    return res;
                }
            }
            return ast_binop(OP_POW,
                             do_expand(e->d.binop.left),
                             do_expand(e->d.binop.right));
        }
        default:
            return ast_binop(e->d.binop.op,
                             do_expand(e->d.binop.left),
                             do_expand(e->d.binop.right));
        }

    case NODE_UNOP:
        return ast_unop(e->d.unop.op, do_expand(e->d.unop.operand));

    case NODE_FUNC:
        return ast_func(e->d.func.id, do_expand(e->d.func.arg));

    default:
        return ast_clone(e);
    }
}

/* ═══════════════════════════════════════════════════════════ */
/*                      EXPAND                                  */
/* ═══════════════════════════════════════════════════════════ */

ast_node_t *algebra_expand(const ast_node_t *expr, char var)
{
    poly_t p;
    ast_node_t *result;
    char buf[60];

    if (!var) var = find_var(expr);

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Expand: %s", buf);

    /* Try polynomial form first (exact, combines like terms) */
    if (var && ast_to_poly(expr, var, &p)) {
        result = poly_to_ast(&p, var);
        result = ast_simplify(result);
        ast_to_str(result, buf, sizeof(buf));
        steps_addf("Polynomial form, deg %d", p.degree);
        steps_addf("= %s", buf);
        return result;
    }

    /* AST-level distribution fallback */
    result = do_expand(expr);
    result = ast_simplify(result);
    ast_to_str(result, buf, sizeof(buf));
    steps_addf("Distributed products");
    steps_addf("= %s", buf);
    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*                    ROOT FINDING                              */
/* ═══════════════════════════════════════════════════════════ */

/* Find positive integer divisors of |n|. Returns count written to divs[]. */
static int find_divisors(int n, int divs[], int max_divs)
{
    int count = 0, i;
    if (n < 0) n = -n;
    if (n == 0) { divs[0] = 1; return 1; }
    for (i = 1; i <= n && count < max_divs; i++) {
        if (n % i == 0)
            divs[count++] = i;
    }
    return count;
}

/* Build rational root candidates from factors of constant and leading coeff.
 * Returns count of candidates written to cands[]. */
static int build_rational_candidates(const poly_t *p, ce_float_t cands[], int max_cands)
{
    int a0 = (int)(p->coeff[0] + (p->coeff[0] >= 0 ? 0.5f : -0.5f));
    int an = (int)(p->coeff[p->degree] + (p->coeff[p->degree] >= 0 ? 0.5f : -0.5f));
    int p_divs[32], q_divs[32];
    int np, nq, i, j, count = 0, k;
    bool dup;

    /* If constant term is 0, x=0 is a root — add it first */
    if (fabsf(p->coeff[0]) < 1e-8f) {
        cands[count++] = 0.0f;
    }

    np = find_divisors(a0, p_divs, 32);
    nq = find_divisors(an, q_divs, 32);

    for (i = 0; i < np && count < max_cands - 1; i++) {
        for (j = 0; j < nq && count < max_cands - 1; j++) {
            ce_float_t c = (ce_float_t)p_divs[i] / (ce_float_t)q_divs[j];
            /* Check for duplicate */
            dup = false;
            for (k = 0; k < count; k++) {
                if (fabsf(cands[k] - c) < 1e-8f || fabsf(cands[k] + c) < 1e-8f) {
                    if (fabsf(cands[k] - c) < 1e-8f) { dup = true; break; }
                }
            }
            if (!dup && count < max_cands - 1) {
                cands[count++] = c;
                if (c != 0.0f && count < max_cands)
                    cands[count++] = -c;
            }
        }
    }
    return count;
}

/* Newton's method on polynomial */
static ce_float_t newton_poly(const poly_t *p, ce_float_t x0)
{
    ce_float_t x = x0;
    int iter, j;

    for (iter = 0; iter < 50; iter++) {
        ce_float_t fx = poly_eval(p, x);
        ce_float_t fpx = 0;
        ce_float_t xpow = 1;

        if (fabsf(fx) < 1e-8f) return x;

        for (j = 1; j <= p->degree; j++) {
            fpx += j * p->coeff[j] * xpow;
            xpow *= x;
        }
        if (fabsf(fpx) < 1e-20f) break;
        x -= fx / fpx;
    }
    return x;
}

/* Cauchy bound: every real root satisfies |x| <= 1 + max_i |a_i/an| */
static ce_float_t poly_root_bound(const poly_t *p)
{
    ce_float_t an, max_ratio = 0.0f;
    int i;

    if (!p || p->degree <= 0) return 10.0f;

    an = fabsf(p->coeff[p->degree]);
    if (an < 1e-20f) return 10.0f;

    for (i = 0; i < p->degree; i++) {
        ce_float_t ratio = fabsf(p->coeff[i]) / an;
        if (ratio > max_ratio) max_ratio = ratio;
    }

    if (!ce_isfinite(max_ratio)) return 10000.0f;
    if (max_ratio > 9999.0f) return 10000.0f;
    return 1.0f + max_ratio;
}

static ce_float_t poly_eval_scale(const poly_t *p, ce_float_t x)
{
    ce_float_t abs_x;
    ce_float_t xpow = 1.0f;
    ce_float_t scale = 1.0f;
    int i;

    if (!p) return 1.0f;

    abs_x = fabsf(x);
    for (i = 0; i <= p->degree; i++) {
        scale += fabsf(p->coeff[i]) * xpow;
        if (!ce_isfinite(scale) || scale > 1e30f)
            return 1e30f;

        if (i < p->degree) {
            if (abs_x < 1e-20f) {
                xpow = 0.0f;
            } else if (xpow > 1e30f / abs_x) {
                xpow = 1e30f;
            } else {
                xpow *= abs_x;
            }
        }
    }

    return scale;
}

static void add_start_unique(ce_float_t starts[], int *count, int max_count, ce_float_t x)
{
    int i;
    ce_float_t tol;

    if (!starts || !count || *count >= max_count || !ce_isfinite(x))
        return;

    tol = 1e-4f * (1.0f + fabsf(x));
    for (i = 0; i < *count; i++) {
        if (fabsf(starts[i] - x) <= tol)
            return;
    }
    starts[(*count)++] = x;
}

static int build_newton_starts(const poly_t *p, ce_float_t starts[], int max_count)
{
    ce_float_t bound = poly_root_bound(p);
    const ce_float_t base[] = {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 5.0f, -5.0f, 10.0f, -10.0f};
    int i, count = 0;
    ce_float_t mag;

    for (i = 0; i < (int)(sizeof(base) / sizeof(base[0])); i++)
        add_start_unique(starts, &count, max_count, base[i]);

    for (i = 1; i <= 8; i++) {
        ce_float_t x = bound * (ce_float_t)i / 8.0f;
        add_start_unique(starts, &count, max_count, x);
        add_start_unique(starts, &count, max_count, -x);
    }

    for (mag = 1.0f; mag < bound; mag *= 2.0f) {
        add_start_unique(starts, &count, max_count, mag);
        add_start_unique(starts, &count, max_count, -mag);
        if (count >= max_count) break;
    }

    add_start_unique(starts, &count, max_count, bound);
    add_start_unique(starts, &count, max_count, -bound);
    return count;
}

static bool deflate_linear_root(poly_t *work, ce_float_t root, ce_float_t *used_root)
{
    poly_t divisor, q, r;
    ce_float_t root_eval, rounded, rounded_eval;
    ce_float_t scale, tol;

    if (!work || !ce_isfinite(root)) return false;

    root_eval = poly_eval(work, root);
    scale = poly_eval_scale(work, root);
    tol = fmaxf(0.01f, 1e-6f * scale);
    if (fabsf(root_eval) > tol) return false;

    rounded = roundf(root * 1000.0f) / 1000.0f;
    rounded_eval = poly_eval(work, rounded);
    if (fabsf(rounded_eval) <= fabsf(root_eval)) {
        root = rounded;
        root_eval = rounded_eval;
    }

    rounded = roundf(root * 10.0f) / 10.0f;
    rounded_eval = poly_eval(work, rounded);
    if (fabsf(rounded_eval) <= fabsf(root_eval)) {
        root = rounded;
        root_eval = rounded_eval;
    }

    poly_init(&divisor);
    divisor.degree = 1;
    divisor.coeff[0] = -root;
    divisor.coeff[1] = 1;
    if (!poly_divide(work, &divisor, &q, &r))
        return false;

    scale = poly_eval_scale(work, root);
    tol = fmaxf(0.05f, 1e-6f * scale);
    if (fabsf(r.coeff[0]) > tol)
        return false;

    *work = q;
    if (used_root) *used_root = root;
    return true;
}

/* Find real roots of a polynomial.  Modifies *work in place
 * (divides out found roots).  Returns count. */
static int find_roots(poly_t *work, ce_float_t roots[], int max_roots)
{
    int count = 0;
    int i;

    while (work->degree > 0 && count < max_roots) {
        bool found = false;

        /* Degree 1: direct */
        if (work->degree == 1) {
            if (fabsf(work->coeff[1]) > 1e-20f)
                roots[count++] = -work->coeff[0] / work->coeff[1];
            break;
        }

        /* Degree 2: quadratic formula */
        if (work->degree == 2) {
            ce_float_t a = work->coeff[2];
            ce_float_t b = work->coeff[1];
            ce_float_t c = work->coeff[0];
            ce_float_t disc = b * b - 4 * a * c;

            if (disc >= 0) {
                ce_float_t sd = sqrtf(disc);
                roots[count++] = (-b + sd) / (2 * a);
                if (count < max_roots && disc > 0.001f)
                    roots[count++] = (-b - sd) / (2 * a);
            }
            break;
        }

        /* Higher degree: try rational root candidates (rational root theorem) */
        {
            /* No recursion or callbacks enter find_roots. Candidates are
             * overwritten before use, including across sequential solves. */
            static ce_float_t cands[128];
            int n_cands = build_rational_candidates(work, cands, 128);
            for (i = 0; i < n_cands; i++) {
                ce_float_t root = cands[i];
                if (deflate_linear_root(work, root, &root)) {
                    roots[count++] = root;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            /* Try Newton from adaptive starting points */
            static ce_float_t starts[64];
            int ns = build_newton_starts(work, starts, 64);
            for (i = 0; i < ns; i++) {
                ce_float_t root = newton_poly(work, starts[i]);
                if (deflate_linear_root(work, root, &root)) {
                    roots[count++] = root;
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════ */
/*                      FACTOR                                  */
/* ═══════════════════════════════════════════════════════════ */

ast_node_t *algebra_factor(const ast_node_t *expr, char var)
{
    poly_t p, work;
    ce_float_t roots[MAX_POLY_DEG];
    int n_roots, i;
    ast_node_t *result = NULL;
    char buf[60];

    if (!var) var = find_var(expr);

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Factor: %s", buf);

    if (!var || !ast_to_poly(expr, var, &p)) {
        steps_addf("Cannot convert to polynomial (degree limit: %d)", MAX_POLY_DEG);
        return ast_clone(expr);
    }

    if (p.degree <= 0) {
        steps_addf("Constant - nothing to factor");
        return ast_number(p.coeff[0]);
    }

    steps_addf("Degree %d polynomial", p.degree);

    work = p;
    n_roots = find_roots(&work, roots, MAX_POLY_DEG);

    if (n_roots == 0) {
        steps_addf("No rational roots found");
        return ast_clone(expr);
    }

    work = p;

    /* Build factored form: (x - r1)(x - r2) ... * remaining */
    for (i = 0; i < n_roots; i++) {
        ast_node_t *factor;
        ce_float_t r = roots[i];

        (void)deflate_linear_root(&work, r, NULL);

        steps_addf("Root: %c = %.6g", var, (double)r);

        if (fabsf(r) < 1e-10f)
            factor = ast_variable(var);
        else if (r > 0)
            factor = ast_binop(OP_SUB, ast_variable(var),
                               ast_number(r));
        else
            factor = ast_binop(OP_ADD, ast_variable(var),
                               ast_number(-r));

        result = result ? ast_binop(OP_MUL, result, factor) : factor;
    }

    /* Remaining quotient polynomial (after dividing out all roots) */
    if (work.degree > 0) {
        ast_node_t *rem = poly_to_ast(&work, var);
        result = ast_binop(OP_MUL, rem, result);
    } else if (fabsf(work.coeff[0] - 1.0f) > 1e-6f &&
               fabsf(work.coeff[0]) > 1e-10f) {
        result = ast_binop(OP_MUL, ast_number(work.coeff[0]), result);
    }

    if (!result) return ast_clone(expr);

    result = ast_simplify(result);
    ast_to_str(result, buf, sizeof(buf));
    steps_addf("= %s", buf);
    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*                       SOLVE                                  */
/* ═══════════════════════════════════════════════════════════ */

/* Try to solve trig(var) = c */
static int solve_trig(const ast_node_t *expr, char var,
                      char *buf, int size)
{
    const ast_node_t *fn_node = NULL;
    ce_float_t c_val = 0;

    /* Pattern: func(var) - number */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_SUB &&
        expr->d.binop.left->type == NODE_FUNC &&
        expr->d.binop.right->type == NODE_NUMBER) {
        fn_node = expr->d.binop.left;
        c_val   = expr->d.binop.right->d.number;
    }
    /* Pattern: func(var) */
    else if (expr->type == NODE_FUNC) {
        fn_node = expr;
        c_val   = 0;
    }
    /* Pattern: func(var) + number → solve func(var) = -number */
    else if (expr->type == NODE_BINOP && expr->d.binop.op == OP_ADD &&
             expr->d.binop.left->type == NODE_FUNC &&
             expr->d.binop.right->type == NODE_NUMBER) {
        fn_node = expr->d.binop.left;
        c_val   = -expr->d.binop.right->d.number;
    }

    if (!fn_node) return 0;
    if (fn_node->d.func.arg->type != NODE_VARIABLE ||
        fn_node->d.func.arg->d.variable != var)
        return 0;

    switch (fn_node->d.func.id) {
    case FN_SIN: {
        ce_float_t a, b;
        if (c_val < -1 || c_val > 1) {
            snprintf(buf, size, "No real solution (|%.4g|>1)",
                     (double)c_val);
            return 0;
        }
        a = asinf(c_val);
        b = CE_PI - a;
        steps_addf("sin(%c)=%.4g", var, (double)c_val);
        steps_addf("%c=asin(%.4g)=%.6g", var, (double)c_val, (double)a);
        snprintf(buf, size, "%c = %.6g+2npi or %c = %.6g+2npi",
                 var, (double)a, var, (double)b);
        return 2;
    }
    case FN_COS: {
        ce_float_t a;
        if (c_val < -1 || c_val > 1) {
            snprintf(buf, size, "No real solution (|%.4g|>1)",
                     (double)c_val);
            return 0;
        }
        a = acosf(c_val);
        steps_addf("cos(%c)=%.4g", var, (double)c_val);
        steps_addf("%c=acos(%.4g)=%.6g", var, (double)c_val, (double)a);
        snprintf(buf, size, "%c = %.6g+2npi or %c = %.6g+2npi",
                 var, (double)a, var, (double)(-a));
        return 2;
    }
    case FN_TAN: {
        ce_float_t a = atanf(c_val);
        steps_addf("tan(%c)=%.4g", var, (double)c_val);
        snprintf(buf, size, "%c = %.6g + npi", var, (double)a);
        return 1;
    }
    default:
        return 0;
    }
}

/* Newton's method using evaluator (for non-polynomial expressions) */
static ce_float_t newton_eval(const ast_node_t *expr, char var,
                               ce_float_t x0)
{
    ce_float_t x = x0, h = 1e-5f;
    int iter;

    for (iter = 0; iter < 50; iter++) {
        eval_ctx_t ctx;
        ce_float_t fx, f_plus, f_minus, fpx;

        eval_init(&ctx);
        eval_set_var(&ctx, var, x);
        fx = eval_expr(&ctx, expr);
        if (eval_error(&ctx) != CE_OK || !ce_isfinite(fx)) break;
        if (fabsf(fx) < 1e-8f) return x;

        eval_init(&ctx);
        eval_set_var(&ctx, var, x + h);
        f_plus = eval_expr(&ctx, expr);
        if (eval_error(&ctx) != CE_OK || !ce_isfinite(f_plus)) break;

        eval_init(&ctx);
        eval_set_var(&ctx, var, x - h);
        f_minus = eval_expr(&ctx, expr);
        if (eval_error(&ctx) != CE_OK || !ce_isfinite(f_minus)) break;

        fpx = (f_plus - f_minus) / (2 * h);
        if (fabsf(fpx) < 1e-20f) break;
        x -= fx / fpx;
    }
    return x;
}

/* Try to solve log/exp equation: ln(f(x)) - c = 0, e^(f(x)) - c = 0, etc. */
static int solve_log_exp(const ast_node_t *expr, char var,
                          char *buf, int size)
{
    const ast_node_t *fn_node = NULL;
    ce_float_t c_val = 0;
    bool negated = false;

    /* Pattern: func(something) - number = 0  →  func(something) = number */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_SUB) {
        if (expr->d.binop.right->type == NODE_NUMBER) {
            fn_node = expr->d.binop.left;
            c_val   = expr->d.binop.right->d.number;
        }
    }
    /* Pattern: func(something) + number = 0  →  func(something) = -number */
    else if (expr->type == NODE_BINOP && expr->d.binop.op == OP_ADD) {
        if (expr->d.binop.right->type == NODE_NUMBER) {
            fn_node = expr->d.binop.left;
            c_val   = -expr->d.binop.right->d.number;
        }
    }
    /* Pattern: -(func(something)) + number */
    else if (expr->type == NODE_BINOP && expr->d.binop.op == OP_ADD &&
             expr->d.binop.left->type == NODE_UNOP &&
             expr->d.binop.left->d.unop.op == UOP_NEG &&
             expr->d.binop.right->type == NODE_NUMBER) {
        fn_node = expr->d.binop.left->d.unop.operand;
        c_val = expr->d.binop.right->d.number;
        negated = true;
    }
    /* Pattern: number - func(something) */
    else if (expr->type == NODE_BINOP && expr->d.binop.op == OP_SUB &&
             expr->d.binop.left->type == NODE_NUMBER) {
        fn_node = expr->d.binop.right;
        c_val = expr->d.binop.left->d.number;
        negated = true;
    }

    if (!fn_node) return 0;

    /* ── ln(f(x)) = c  →  f(x) = e^c  →  solve f(x) - e^c = 0 ── */
    if (fn_node->type == NODE_FUNC && fn_node->d.func.id == FN_LN) {
        const ast_node_t *inner = fn_node->d.func.arg;
        ce_float_t rhs = negated ? c_val : c_val;
        ce_float_t target = expf(rhs);

        steps_addf("ln(expr) = %.4g", (double)rhs);
        steps_addf("expr = e^%.4g = %.6g", (double)rhs, (double)target);

        /* Check if inner is just var */
        if (inner->type == NODE_VARIABLE && inner->d.variable == var) {
            if (target <= 0) {
                snprintf(buf, size, "No solution (e^c <= 0 impossible for real)");
                return 0;
            }
            snprintf(buf, size, "%c = %.6g", var, (double)target);
            steps_addf("%c = %.6g", var, (double)target);
            return 1;
        }

        /* inner is (var + a) or (a*var + b) — build inner - target and solve linearly */
        {
            poly_t ip;
            if (ast_to_poly(inner, var, &ip) && ip.degree == 1) {
                ce_float_t sol = (target - ip.coeff[0]) / ip.coeff[1];
                steps_addf("%.4g*%c + %.4g = %.6g",
                           (double)ip.coeff[1], var, (double)ip.coeff[0], (double)target);
                steps_addf("%c = %.6g", var, (double)sol);
                snprintf(buf, size, "%c = %.6g", var, (double)sol);
                return 1;
            }
        }
        return 0;
    }

    /* ── log(f(x)) = c  →  f(x) = 10^c ── */
    if (fn_node->type == NODE_FUNC && fn_node->d.func.id == FN_LOG) {
        const ast_node_t *inner = fn_node->d.func.arg;
        ce_float_t rhs = negated ? c_val : c_val;
        ce_float_t target = powf(10.0f, rhs);

        steps_addf("log(expr) = %.4g", (double)rhs);
        steps_addf("expr = 10^%.4g = %.6g", (double)rhs, (double)target);

        if (inner->type == NODE_VARIABLE && inner->d.variable == var) {
            snprintf(buf, size, "%c = %.6g", var, (double)target);
            steps_addf("%c = %.6g", var, (double)target);
            return 1;
        }

        {
            poly_t ip;
            if (ast_to_poly(inner, var, &ip) && ip.degree == 1) {
                ce_float_t sol = (target - ip.coeff[0]) / ip.coeff[1];
                steps_addf("%c = %.6g", var, (double)sol);
                snprintf(buf, size, "%c = %.6g", var, (double)sol);
                return 1;
            }
        }
        return 0;
    }

    /* ── e^(f(x)) = c  →  f(x) = ln(c) ── */
    if (fn_node->type == NODE_FUNC && fn_node->d.func.id == FN_EXP) {
        const ast_node_t *inner = fn_node->d.func.arg;
        ce_float_t rhs = negated ? c_val : c_val;

        if (rhs <= 0) {
            snprintf(buf, size, "No solution (e^x > 0 always)");
            return 0;
        }

        {
            ce_float_t target = logf(rhs);
            steps_addf("e^(expr) = %.4g", (double)rhs);
            steps_addf("expr = ln(%.4g) = %.6g", (double)rhs, (double)target);

            if (inner->type == NODE_VARIABLE && inner->d.variable == var) {
                snprintf(buf, size, "%c = %.6g", var, (double)target);
                steps_addf("%c = %.6g", var, (double)target);
                return 1;
            }

            {
                poly_t ip;
                if (ast_to_poly(inner, var, &ip) && ip.degree == 1) {
                    ce_float_t sol = (target - ip.coeff[0]) / ip.coeff[1];
                    steps_addf("%.4g*%c + %.4g = %.6g",
                               (double)ip.coeff[1], var, (double)ip.coeff[0], (double)target);
                    steps_addf("%c = %.6g", var, (double)sol);
                    snprintf(buf, size, "%c = %.6g", var, (double)sol);
                    return 1;
                }
            }
        }
        return 0;
    }

    /* ── e^var as CONST_E^(f(x)): detect POW with base=e ── */
    if (fn_node->type == NODE_BINOP && fn_node->d.binop.op == OP_POW) {
        const ast_node_t *base = fn_node->d.binop.left;
        const ast_node_t *expo = fn_node->d.binop.right;

        /* e^(f(x)) = c */
        if (base->type == NODE_CONSTANT && base->d.constant == CONST_E &&
            ast_contains_var(expo, var)) {
            ce_float_t rhs = negated ? c_val : c_val;
            if (rhs <= 0) {
                snprintf(buf, size, "No solution (e^x > 0 always)");
                return 0;
            }
            {
                ce_float_t target = logf(rhs);
                steps_addf("e^(expr) = %.4g", (double)rhs);
                steps_addf("expr = ln(%.4g) = %.6g", (double)rhs, (double)target);

                if (expo->type == NODE_VARIABLE && expo->d.variable == var) {
                    snprintf(buf, size, "%c = %.6g", var, (double)target);
                    return 1;
                }
                {
                    poly_t ip;
                    if (ast_to_poly(expo, var, &ip) && ip.degree == 1) {
                        ce_float_t sol = (target - ip.coeff[0]) / ip.coeff[1];
                        steps_addf("%c = %.6g", var, (double)sol);
                        snprintf(buf, size, "%c = %.6g", var, (double)sol);
                        return 1;
                    }
                }
            }
            return 0;
        }

        /* a^x = c  →  x = ln(c)/ln(a) */
        if (!ast_contains_var(base, var) && ast_contains_var(expo, var)) {
            ce_float_t rhs = negated ? c_val : c_val;
            eval_ctx_t ctx;
            ce_float_t a_val;
            eval_init(&ctx);
            a_val = eval_expr(&ctx, base);
            if (eval_error(&ctx) != CE_OK || a_val <= 0 || fabsf(a_val - 1.0f) < 1e-10f)
                return 0;

            if (rhs <= 0) {
                snprintf(buf, size, "No solution (a^x > 0 for a > 0)");
                return 0;
            }

            {
                ce_float_t target = logf(rhs) / logf(a_val);
                steps_addf("%.4g^(expr) = %.4g", (double)a_val, (double)rhs);
                steps_addf("expr = ln(%.4g)/ln(%.4g) = %.6g",
                           (double)rhs, (double)a_val, (double)target);

                if (expo->type == NODE_VARIABLE && expo->d.variable == var) {
                    snprintf(buf, size, "%c = %.6g", var, (double)target);
                    return 1;
                }
                {
                    poly_t ip;
                    if (ast_to_poly(expo, var, &ip) && ip.degree == 1) {
                        ce_float_t sol = (target - ip.coeff[0]) / ip.coeff[1];
                        steps_addf("%c = %.6g", var, (double)sol);
                        snprintf(buf, size, "%c = %.6g", var, (double)sol);
                        return 1;
                    }
                }
            }
            return 0;
        }
    }

    return 0;
}

int algebra_solve(const ast_node_t *expr, char var,
                  char *result_buf, int buf_size)
{
    poly_t p;
    int pos;
    char buf[60];

    poly_too_deep = false;

    if (!var) var = find_var(expr);

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Solve: %s = 0 for %c", buf, var);

#if CE_ALG_SOLVE_ENABLE_ABS
    /* Try absolute value equation first */
    {
        int n = algebra_solve_abs(expr, var, result_buf, buf_size);
        if (n > 0) return n;
    }
#endif

#if CE_ALG_SOLVE_ENABLE_LOG_EXP
    /* Try log/exp equations first */
    {
        int n = solve_log_exp(expr, var, result_buf, buf_size);
        if (n > 0) return n;
    }
#endif

#if CE_ALG_SOLVE_ENABLE_TRIG
    /* Try trig equation */
    {
        int n = solve_trig(expr, var, result_buf, buf_size);
        if (n > 0) return n;
    }
#endif

    /* Try polynomial form */
    if (var && ast_to_poly(expr, var, &p)) {
        /* Degree 0: constant */
#if !CE_ALG_SOLVE_SKIP_BASIC_POLY
        if (p.degree == 0) {
            if (fabsf(p.coeff[0]) < 1e-6f)
                snprintf(result_buf, buf_size, "All %c (identity)", var);
            else
                snprintf(result_buf, buf_size, "No solution");
            return 0;
        }

        /* Degree 1: linear */
        if (p.degree == 1) {
            ce_float_t sol = -p.coeff[0] / p.coeff[1];
            steps_addf("Linear: %.4g*%c + %.4g = 0",
                       (double)p.coeff[1], var, (double)p.coeff[0]);
            steps_addf("%c = %.6g", var, (double)sol);
            snprintf(result_buf, buf_size, "%c = %.6g",
                     var, (double)sol);
            return 1;
        }

        /* Degree 2: quadratic formula */
        if (p.degree == 2) {
            ce_float_t a = p.coeff[2], b = p.coeff[1], c = p.coeff[0];
            ce_float_t disc = b * b - 4 * a * c;

            steps_addf("Quadratic: %.4g%c^2 + %.4g%c + %.4g = 0",
                       (double)a, var, (double)b, var, (double)c);
            steps_addf("disc = b^2-4ac = %.6g", (double)disc);

            if (disc >= 0) {
                ce_float_t sd = sqrtf(disc);
                ce_float_t r1 = (-b + sd) / (2 * a);
                ce_float_t r2 = (-b - sd) / (2 * a);

                steps_addf("%c = (%.4g +/- %.4g) / %.4g",
                           var, (double)(-b), (double)sd, (double)(2*a));

                if (fabsf(r1 - r2) < 1e-6f) {
                    steps_addf("%c = %.6g (double root)", var, (double)r1);
                    snprintf(result_buf, buf_size,
                             "%c = %.6g", var, (double)r1);
                    return 1;
                }
                steps_addf("%c = %.6g or %c = %.6g",
                           var, (double)r1, var, (double)r2);
                snprintf(result_buf, buf_size,
                         "%c = %.6g or %c = %.6g",
                         var, (double)r1, var, (double)r2);
                return 2;
            } else {
                /* Complex roots */
                ce_float_t re = -b / (2 * a);
                ce_float_t im = sqrtf(-disc) / (2 * a);
                ce_float_t abs_im = fabsf(im);
                char s1 = (im >= 0) ? '+' : '-';
                char s2 = (im >= 0) ? '-' : '+';
                steps_addf("Complex: %.4g +/- %.4gi",
                           (double)re, (double)abs_im);
                snprintf(result_buf, buf_size,
                         "%c = %.4g%c%.4gi or %c = %.4g%c%.4gi",
                         var, (double)re, s1, (double)abs_im,
                         var, (double)re, s2, (double)abs_im);
                return 2;
            }
        }
#endif

#if CE_ALG_SOLVE_BASIC_ONLY
        if (buf_size > 0)
            snprintf(result_buf, buf_size, "CEAADVREQ");
        return -1;
#else
        /* Higher degree: find roots */
        {
            poly_t work = p;
            ce_float_t roots[MAX_POLY_DEG];
            int n, i;

            n = find_roots(&work, roots, MAX_POLY_DEG);
            if (n > 0) {
                pos = 0;
                for (i = 0; i < n; i++) {
                    steps_addf("Root: %c = %.6g",
                               var, (double)roots[i]);
                    if (i > 0)
                        pos += snprintf(result_buf + pos,
                                        buf_size - pos, " or ");
                    pos += snprintf(result_buf + pos,
                                    buf_size - pos, "%c = %.6g",
                                    var, (double)roots[i]);
                }
                return n;
            }

            steps_addf("No rational roots found");
        }
#endif
    }

    /* A bounded conversion refusal is not proof of no solution. Do not
     * route it into heuristic Newton solving: a scaled residual or a pole
     * can turn an unsupported polynomial/rational equation into false roots.
     * The caller may retry a simpler expression instead. */
    if (poly_too_deep) {
        snprintf(result_buf, buf_size, "Expression too deep for solve");
        return -1;
    }
#if CE_ALG_SOLVE_BASIC_ONLY
    if (buf_size > 0)
        snprintf(result_buf, buf_size, "CEAADVREQ");
    return -1;
#else
    /* Newton's method fallback for any expression */
    {
        ce_float_t starts[] = {0, 1, -1, 2, -2, 0.5f, -0.5f,
                               CE_PI, -CE_PI, 5, -5, 10, -10};
        int ns = 13, count = 0, i, j;
        ce_float_t found_roots[8];
        bool duplicate;

        steps_addf("Trying numerical methods...");

        for (i = 0; i < ns && count < 8; i++) {
            ce_float_t root = newton_eval(expr, var, starts[i]);
            eval_ctx_t ctx;
            ce_float_t val;

            if (!ce_isfinite(root)) continue;
            eval_init(&ctx);
            eval_set_var(&ctx, var, root);
            val = eval_expr(&ctx, expr);
            if (eval_error(&ctx) == CE_ERR_DEPTH) {
                snprintf(result_buf, buf_size, "Expression too deep for solve");
                return -1;
            }
            if (eval_error(&ctx) != CE_OK || !ce_isfinite(val)) continue;
            if (fabsf(val) > 0.01f) continue;

            /* Check for duplicate */
            duplicate = false;
            for (j = 0; j < count; j++) {
                if (fabsf(found_roots[j] - root) < 0.01f) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                found_roots[count++] = root;
            }
        }

        if (count > 0) {
            pos = 0;
            for (i = 0; i < count; i++) {
                steps_addf("Root: %c ~ %.6g (numeric)",
                           var, (double)found_roots[i]);
                if (i > 0)
                    pos += snprintf(result_buf + pos,
                                    buf_size - pos, " or ");
                pos += snprintf(result_buf + pos,
                                buf_size - pos, "%c ~ %.6g",
                                var, (double)found_roots[i]);
            }
            return count;
        }
    }

    snprintf(result_buf, buf_size, "No solution found");
    return 0;
#endif
}

/* ═══════════════════════════════════════════════════════════ */
/*                 POLYNOMIAL LONG DIVISION                     */
/* ═══════════════════════════════════════════════════════════ */

ast_node_t *algebra_poly_div(const ast_node_t *dividend,
                              const ast_node_t *divisor,
                              char var,
                              char *rem_buf, int rem_size)
{
    poly_t a, b, q, r;
    ast_node_t *result;
    int i;
    char buf[60];

    if (!var) var = find_var(dividend);

    ast_to_str(dividend, buf, sizeof(buf));
    steps_addf("Divide: %s", buf);
    ast_to_str(divisor, buf, sizeof(buf));
    steps_addf("By: %s", buf);

    if (!ast_to_poly(dividend, var, &a) ||
        !ast_to_poly(divisor, var, &b)) {
        steps_addf("Cannot convert to polynomials");
        return NULL;
    }

    if (!poly_divide(&a, &b, &q, &r)) {
        steps_addf("Division failed (zero divisor?)");
        return NULL;
    }

    /* Log synthetic division steps */
    steps_addf("Deg(dividend)=%d, Deg(divisor)=%d", a.degree, b.degree);
    for (i = q.degree; i >= 0; i--) {
        if (fabsf(q.coeff[i]) > 1e-10f)
            steps_addf("q[%d] = %.6g", i, (double)q.coeff[i]);
    }

    result = poly_to_ast(&q, var);
    result = ast_simplify(result);

    ast_to_str(result, buf, sizeof(buf));
    steps_addf("Quotient: %s", buf);

    /* Remainder */
    {
        bool has_rem = false;
        for (i = 0; i <= r.degree; i++) {
            if (fabsf(r.coeff[i]) > 1e-10f) { has_rem = true; break; }
        }
        if (has_rem) {
            ast_node_t *rem_ast = poly_to_ast(&r, var);
            rem_ast = ast_simplify(rem_ast);
            ast_to_display_str(rem_ast, rem_buf, rem_size);
            steps_addf("Remainder: %s", rem_buf);
            ast_free(rem_ast);
        } else {
            snprintf(rem_buf, rem_size, "0");
            steps_addf("Remainder: 0 (divides evenly)");
        }
    }

    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*           PUBLIC POLY WRAPPERS (for limits.c etc.)           */
/* ═══════════════════════════════════════════════════════════ */

bool algebra_ast_to_poly(const ast_node_t *expr, char var, poly_t *p)
{
    return ast_to_poly(expr, var, p);
}

ce_float_t algebra_poly_eval(const poly_t *p, ce_float_t x)
{
    return poly_eval(p, x);
}

ast_node_t *algebra_poly_to_ast(const poly_t *p, char var)
{
    return poly_to_ast(p, var);
}

bool algebra_poly_divide(const poly_t *a, const poly_t *b,
                          poly_t *q, poly_t *r)
{
    return poly_divide(a, b, q, r);
}

/* ═══════════════════════════════════════════════════════════ */
/*                TRIG SIMPLIFICATION                           */
/* ═══════════════════════════════════════════════════════════ */

/* Check if node is func(arg)^2 for any trig function */
static bool is_trig_sq(const ast_node_t *n, func_id_t *fn,
                        const ast_node_t **arg)
{
    if (!n) return false;
    if (n->type == NODE_BINOP && n->d.binop.op == OP_POW &&
        n->d.binop.right->type == NODE_NUMBER &&
        fabsf(n->d.binop.right->d.number - 2.0f) < 1e-6f &&
        n->d.binop.left->type == NODE_FUNC) {
        func_id_t fid = n->d.binop.left->d.func.id;
        if (fid == FN_SIN || fid == FN_COS || fid == FN_TAN ||
            fid == FN_COT || fid == FN_SEC || fid == FN_CSC) {
            *fn  = fid;
            *arg = n->d.binop.left->d.func.arg;
            return true;
        }
    }
    return false;
}

/* Check if node is a trig function call */
static bool is_trig_call(const ast_node_t *n, func_id_t *fn,
                          const ast_node_t **arg)
{
    if (!n || n->type != NODE_FUNC) return false;
    if (n->d.func.id == FN_SIN || n->d.func.id == FN_COS ||
        n->d.func.id == FN_TAN) {
        *fn  = n->d.func.id;
        *arg = n->d.func.arg;
        return true;
    }
    return false;
}

/* Check if an expression is 2*inner (return inner) */
static bool is_double_arg(const ast_node_t *e, const ast_node_t **inner)
{
    if (!e) return false;
    if (e->type == NODE_BINOP && e->d.binop.op == OP_MUL) {
        if (e->d.binop.left->type == NODE_NUMBER &&
            fabsf(e->d.binop.left->d.number - 2.0f) < 1e-6f) {
            *inner = e->d.binop.right;
            return true;
        }
        if (e->d.binop.right->type == NODE_NUMBER &&
            fabsf(e->d.binop.right->d.number - 2.0f) < 1e-6f) {
            *inner = e->d.binop.left;
            return true;
        }
    }
    return false;
}

ast_node_t *algebra_trig_simplify(ast_node_t *expr)
{
    func_id_t fn_l, fn_r;
    const ast_node_t *arg_l, *arg_r;

    if (!expr) return NULL;

    /* Recurse into children first */
    switch (expr->type) {
    case NODE_BINOP:
        expr->d.binop.left  = algebra_trig_simplify(expr->d.binop.left);
        expr->d.binop.right = algebra_trig_simplify(expr->d.binop.right);
        break;
    case NODE_UNOP:
        expr->d.unop.operand = algebra_trig_simplify(expr->d.unop.operand);
        break;
    case NODE_FUNC:
        expr->d.func.arg = algebra_trig_simplify(expr->d.func.arg);
        break;
    default:
        return expr;
    }

    /* sin^2(x) + cos^2(x) = 1 */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_ADD) {
        if (is_trig_sq(expr->d.binop.left,  &fn_l, &arg_l) &&
            is_trig_sq(expr->d.binop.right, &fn_r, &arg_r) &&
            fn_l != fn_r && ast_equal(arg_l, arg_r)) {
            ast_free(expr);
            steps_addf("sin^2+cos^2 = 1");
            return ast_number(1);
        }
    }

    /* 1 + tan^2(x) = sec^2(x) */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_ADD) {
        const ast_node_t *other = NULL;
        const ast_node_t *tan_sq = NULL;

        if (expr->d.binop.left->type == NODE_NUMBER &&
            fabsf(expr->d.binop.left->d.number - 1.0f) < 1e-6f)
            other = expr->d.binop.left, tan_sq = expr->d.binop.right;
        else if (expr->d.binop.right->type == NODE_NUMBER &&
                 fabsf(expr->d.binop.right->d.number - 1.0f) < 1e-6f)
            other = expr->d.binop.right, tan_sq = expr->d.binop.left;

        if (tan_sq && is_trig_sq(tan_sq, &fn_l, &arg_l) &&
            fn_l == FN_TAN) {
            ast_node_t *arg_clone = ast_clone(arg_l);
            ast_free(expr);
            steps_addf("1+tan^2 = sec^2");
            (void)other;
            return ast_binop(OP_POW,
                             ast_func(FN_SEC, arg_clone),
                             ast_number(2));
        }
    }

    /* 1 + cot^2(x) = csc^2(x) */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_ADD) {
        const ast_node_t *other = NULL;
        const ast_node_t *cot_sq = NULL;

        if (expr->d.binop.left->type == NODE_NUMBER &&
            fabsf(expr->d.binop.left->d.number - 1.0f) < 1e-6f)
            other = expr->d.binop.left, cot_sq = expr->d.binop.right;
        else if (expr->d.binop.right->type == NODE_NUMBER &&
                 fabsf(expr->d.binop.right->d.number - 1.0f) < 1e-6f)
            other = expr->d.binop.right, cot_sq = expr->d.binop.left;

        if (cot_sq && is_trig_sq(cot_sq, &fn_l, &arg_l) &&
            fn_l == FN_COT) {
            ast_node_t *arg_clone = ast_clone(arg_l);
            ast_free(expr);
            steps_addf("1+cot^2 = csc^2");
            (void)other;
            return ast_binop(OP_POW,
                             ast_func(FN_CSC, arg_clone),
                             ast_number(2));
        }
    }

    /* 2*sin(x)*cos(x) → sin(2x)  (reverse double-angle) */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_MUL) {
        /* Look for 2 * sin(u) * cos(u) in any grouping:
         * Could be (2*sin(u))*cos(u) or 2*(sin(u)*cos(u)) or sin(u)*(2*cos(u)) */
        const ast_node_t *L = expr->d.binop.left;
        const ast_node_t *R = expr->d.binop.right;
        func_id_t fn_a, fn_b;
        const ast_node_t *arg_a, *arg_b;
        const ast_node_t *candidate_a = NULL, *candidate_b = NULL;
        ce_float_t coeff = 1.0f;

        /* Pattern: (2*A)*B or (A*2)*B */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_MUL) {
            if (L->d.binop.left->type == NODE_NUMBER &&
                fabsf(L->d.binop.left->d.number - 2.0f) < 1e-6f) {
                candidate_a = L->d.binop.right; candidate_b = R; coeff = 2.0f;
            } else if (L->d.binop.right->type == NODE_NUMBER &&
                       fabsf(L->d.binop.right->d.number - 2.0f) < 1e-6f) {
                candidate_a = L->d.binop.left; candidate_b = R; coeff = 2.0f;
            }
        }
        /* Pattern: A*(2*B) or A*(B*2) */
        if (!candidate_a && R->type == NODE_BINOP && R->d.binop.op == OP_MUL) {
            if (R->d.binop.left->type == NODE_NUMBER &&
                fabsf(R->d.binop.left->d.number - 2.0f) < 1e-6f) {
                candidate_a = L; candidate_b = R->d.binop.right; coeff = 2.0f;
            } else if (R->d.binop.right->type == NODE_NUMBER &&
                       fabsf(R->d.binop.right->d.number - 2.0f) < 1e-6f) {
                candidate_a = L; candidate_b = R->d.binop.left; coeff = 2.0f;
            }
        }
        /* Pattern: 2 * (sin*cos) */
        if (!candidate_a && L->type == NODE_NUMBER &&
            fabsf(L->d.number - 2.0f) < 1e-6f &&
            R->type == NODE_BINOP && R->d.binop.op == OP_MUL) {
            candidate_a = R->d.binop.left; candidate_b = R->d.binop.right; coeff = 2.0f;
        }
        /* Pattern: (sin*cos) * 2 */
        if (!candidate_a && R->type == NODE_NUMBER &&
            fabsf(R->d.number - 2.0f) < 1e-6f &&
            L->type == NODE_BINOP && L->d.binop.op == OP_MUL) {
            candidate_a = L->d.binop.left; candidate_b = L->d.binop.right; coeff = 2.0f;
        }

        if (candidate_a && candidate_b && fabsf(coeff - 2.0f) < 1e-6f &&
            is_trig_call(candidate_a, &fn_a, &arg_a) &&
            is_trig_call(candidate_b, &fn_b, &arg_b) &&
            fn_a != fn_b && ast_equal(arg_a, arg_b) &&
            ((fn_a == FN_SIN && fn_b == FN_COS) ||
             (fn_a == FN_COS && fn_b == FN_SIN))) {
            ast_node_t *double_arg = ast_binop(OP_MUL, ast_number(2), ast_clone(arg_a));
            ast_free(expr);
            steps_addf("2sin(u)cos(u) = sin(2u)");
            return ast_func(FN_SIN, double_arg);
        }
    }

    /* cos^2(x) - sin^2(x) → cos(2x)  (reverse double-angle) */
    if (expr->type == NODE_BINOP && expr->d.binop.op == OP_SUB) {
        func_id_t fn_ls, fn_rs;
        const ast_node_t *arg_ls, *arg_rs;
        if (is_trig_sq(expr->d.binop.left, &fn_ls, &arg_ls) &&
            is_trig_sq(expr->d.binop.right, &fn_rs, &arg_rs) &&
            fn_ls == FN_COS && fn_rs == FN_SIN &&
            ast_equal(arg_ls, arg_rs)) {
            ast_node_t *double_arg = ast_binop(OP_MUL, ast_number(2), ast_clone(arg_ls));
            ast_free(expr);
            steps_addf("cos^2(u)-sin^2(u) = cos(2u)");
            return ast_func(FN_COS, double_arg);
        }
    }

    /* sin(2x) = 2*sin(x)*cos(x) */
    if (expr->type == NODE_FUNC && expr->d.func.id == FN_SIN) {
        const ast_node_t *inner;
        if (is_double_arg(expr->d.func.arg, &inner)) {
            ast_node_t *ic = ast_clone(inner);
            ast_node_t *ic2 = ast_clone(inner);
            ast_free(expr);
            steps_addf("sin(2u) = 2sin(u)cos(u)");
            return ast_binop(OP_MUL, ast_number(2),
                     ast_binop(OP_MUL,
                               ast_func(FN_SIN, ic),
                               ast_func(FN_COS, ic2)));
        }
    }

    /* cos(2x) = cos^2(x) - sin^2(x) */
    if (expr->type == NODE_FUNC && expr->d.func.id == FN_COS) {
        const ast_node_t *inner;
        if (is_double_arg(expr->d.func.arg, &inner)) {
            ast_node_t *ic = ast_clone(inner);
            ast_node_t *ic2 = ast_clone(inner);
            ast_free(expr);
            steps_addf("cos(2u) = cos^2(u) - sin^2(u)");
            return ast_binop(OP_SUB,
                     ast_binop(OP_POW, ast_func(FN_COS, ic), ast_number(2)),
                     ast_binop(OP_POW, ast_func(FN_SIN, ic2), ast_number(2)));
        }
    }

    return expr;
}

/* ═══════════════════════════════════════════════════════════ */
/*           SYSTEMS OF EQUATIONS (CRAMER'S RULE)              */
/* ═══════════════════════════════════════════════════════════ */

int algebra_solve_system(const ast_node_t **equations, int n_eqs,
                         const char *vars, char *result_buf, int buf_size)
{
    ce_matrix_t A, Ai;
    ce_float_t rhs[3];
    ce_float_t det_A, det_Ai, solutions[3];
    int i, j, n;
    int pos = 0;

    n = (int)strlen(vars);
    if (n < 2 || n > 3 || n_eqs != n) {
        snprintf(result_buf, buf_size, "Need 2 or 3 equations");
        return 0;
    }

    steps_addf("System of %d equations in %d unknowns", n, n);

    /* Extract coefficients: coeff[i][j] = f(v_j=1, others=0) - f(all=0) */
    matrix_init(&A, (uint8_t)n, (uint8_t)n);

    for (i = 0; i < n; i++) {
        eval_ctx_t ctx;
        ce_float_t f_zero;

        /* Evaluate f with all vars = 0 */
        eval_init(&ctx);
        for (j = 0; j < n; j++)
            eval_set_var(&ctx, vars[j], 0.0f);
        f_zero = eval_expr(&ctx, equations[i]);
        if (eval_error(&ctx) != CE_OK) {
            snprintf(result_buf, buf_size, "Eval error in equation %d", i + 1);
            return 0;
        }
        rhs[i] = -f_zero;

        /* For each variable, get coefficient */
        for (j = 0; j < n; j++) {
            int k;
            ce_float_t f_one;
            eval_init(&ctx);
            for (k = 0; k < n; k++)
                eval_set_var(&ctx, vars[k], (k == j) ? 1.0f : 0.0f);
            f_one = eval_expr(&ctx, equations[i]);
            if (eval_error(&ctx) != CE_OK) {
                snprintf(result_buf, buf_size, "Eval error in equation %d", i + 1);
                return 0;
            }
            MAT_AT(&A, i, j) = f_one - f_zero;
        }
    }

    /* Verify linearity: check that the coefficient of v_j is constant
     * (doesn't depend on other variables). Compute the coefficient at
     * two different baseline values for the other variables. If they
     * differ, the system has cross-terms (e.g., x*y) and is nonlinear.
     * Also check for pure nonlinearity (x^2) via the superposition test. */
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            eval_ctx_t lctx;
            ce_float_t f_base0, f_one0, f_two0;
            ce_float_t f_base1, f_one1;
            int k;

            /* Baseline: other vars = 0 */
            eval_init(&lctx);
            for (k = 0; k < n; k++)
                eval_set_var(&lctx, vars[k], (k == j) ? 0.0f : 0.0f);
            f_base0 = eval_expr(&lctx, equations[i]);

            eval_init(&lctx);
            for (k = 0; k < n; k++)
                eval_set_var(&lctx, vars[k], (k == j) ? 1.0f : 0.0f);
            f_one0 = eval_expr(&lctx, equations[i]);

            eval_init(&lctx);
            for (k = 0; k < n; k++)
                eval_set_var(&lctx, vars[k], (k == j) ? 2.0f : 0.0f);
            f_two0 = eval_expr(&lctx, equations[i]);

            /* Check superposition (pure nonlinearity like x^2) */
            if (fabsf((f_two0 - f_base0) - 2.0f * (f_one0 - f_base0)) > 0.01f) {
                steps_addf("Equation %d is nonlinear in %c", i + 1, vars[j]);
                snprintf(result_buf, buf_size, "Nonlinear system");
                return 0;
            }

            /* Baseline: other vars = 1 — check for cross-terms (x*y) */
            eval_init(&lctx);
            for (k = 0; k < n; k++)
                eval_set_var(&lctx, vars[k], (k == j) ? 0.0f : 1.0f);
            f_base1 = eval_expr(&lctx, equations[i]);

            eval_init(&lctx);
            for (k = 0; k < n; k++)
                eval_set_var(&lctx, vars[k], (k == j) ? 1.0f : 1.0f);
            f_one1 = eval_expr(&lctx, equations[i]);

            /* If coefficient of v_j changes when other vars change, it's nonlinear */
            {
                ce_float_t coeff0 = f_one0 - f_base0;
                ce_float_t coeff1 = f_one1 - f_base1;
                if (fabsf(coeff0 - coeff1) > 0.01f) {
                    steps_addf("Equation %d has cross-term involving %c", i + 1, vars[j]);
                    snprintf(result_buf, buf_size, "Nonlinear system");
                    return 0;
                }
            }
        }
    }

    /* Show the coefficient matrix */
    if (n == 2) {
        steps_addf("[%.4g %.4g | %.4g]",
                   (double)MAT_AT(&A,0,0), (double)MAT_AT(&A,0,1), (double)rhs[0]);
        steps_addf("[%.4g %.4g | %.4g]",
                   (double)MAT_AT(&A,1,0), (double)MAT_AT(&A,1,1), (double)rhs[1]);
    } else {
        steps_addf("[%.3g %.3g %.3g | %.3g]",
                   (double)MAT_AT(&A,0,0), (double)MAT_AT(&A,0,1),
                   (double)MAT_AT(&A,0,2), (double)rhs[0]);
        steps_addf("[%.3g %.3g %.3g | %.3g]",
                   (double)MAT_AT(&A,1,0), (double)MAT_AT(&A,1,1),
                   (double)MAT_AT(&A,1,2), (double)rhs[1]);
        steps_addf("[%.3g %.3g %.3g | %.3g]",
                   (double)MAT_AT(&A,2,0), (double)MAT_AT(&A,2,1),
                   (double)MAT_AT(&A,2,2), (double)rhs[2]);
    }

    /* Compute det(A) */
    det_A = matrix_det(&A);
    steps_addf("det(A) = %.6g", (double)det_A);

    if (fabsf(det_A) < 1e-10f) {
        steps_addf("System is singular (no unique solution)");
        snprintf(result_buf, buf_size, "No unique solution (singular)");
        return 0;
    }

    /* Cramer's rule: for each variable, replace column j with RHS */
    for (j = 0; j < n; j++) {
        Ai = A;  /* copy */
        for (i = 0; i < n; i++)
            MAT_AT(&Ai, i, j) = rhs[i];
        det_Ai = matrix_det(&Ai);
        solutions[j] = det_Ai / det_A;
        steps_addf("det(A_%c) = %.6g", vars[j], (double)det_Ai);
        steps_addf("%c = %.6g / %.6g = %.6g",
                   vars[j], (double)det_Ai, (double)det_A, (double)solutions[j]);
    }

    /* Format result */
    for (j = 0; j < n; j++) {
        if (j > 0)
            pos += snprintf(result_buf + pos, buf_size - pos, ", ");
        pos += snprintf(result_buf + pos, buf_size - pos,
                        "%c = %.6g", vars[j], (double)solutions[j]);
    }

    return n;
}

/* ═══════════════════════════════════════════════════════════ */
/*             TRIANGLE SOLVER (LAW OF SINES / COSINES)        */
/* ═══════════════════════════════════════════════════════════ */

#define DEG2RAD(d) ((ce_float_t)(d) * (CE_PI / 180.0f))
#define RAD2DEG(r) ((ce_float_t)(r) * (180.0f / CE_PI))

int algebra_triangle(ce_float_t *sides, ce_float_t *angles,
                     char *result_buf, int buf_size)
{
    int n_sides = 0, n_angles = 0;
    int i;
    int pos = 0;

    steps_addf("Triangle solver");

    /* Count knowns */
    for (i = 0; i < 3; i++) {
        if (sides[i]  > 0) n_sides++;
        if (angles[i] > 0) n_angles++;
    }

    steps_addf("Known: %d side(s), %d angle(s)", n_sides, n_angles);

    /* Validate inputs */
    for (i = 0; i < 3; i++) {
        if (sides[i] < 0) {
            snprintf(result_buf, buf_size, "Sides must be positive");
            return 0;
        }
        if (angles[i] < 0 || angles[i] >= 180.0f) {
            snprintf(result_buf, buf_size, "Angles must be in (0,180)");
            return 0;
        }
    }

    /* Check angle sum for known angles */
    {
        ce_float_t angle_sum = 0;
        for (i = 0; i < 3; i++)
            angle_sum += angles[i];
        if (angle_sum >= 180.0f && n_angles < 3) {
            snprintf(result_buf, buf_size, "Angle sum exceeds 180");
            return 0;
        }
    }

    /* ── SSS: 3 sides known ── */
    if (n_sides == 3 && n_angles == 0) {
        ce_float_t a = sides[0], b = sides[1], c = sides[2];
        ce_float_t cosA, cosB, cosC;

        /* Triangle inequality */
        if (a + b <= c || a + c <= b || b + c <= a) {
            snprintf(result_buf, buf_size, "Triangle inequality violated");
            return 0;
        }

        steps_addf("SSS: a=%.4g, b=%.4g, c=%.4g", (double)a, (double)b, (double)c);

        cosA = (b*b + c*c - a*a) / (2*b*c);
        cosB = (a*a + c*c - b*b) / (2*a*c);
        cosC = (a*a + b*b - c*c) / (2*a*b);

        if (cosA < -1 || cosA > 1 || cosB < -1 || cosB > 1 ||
            cosC < -1 || cosC > 1) {
            snprintf(result_buf, buf_size, "Invalid triangle");
            return 0;
        }

        angles[0] = RAD2DEG(acosf(cosA));
        angles[1] = RAD2DEG(acosf(cosB));
        angles[2] = RAD2DEG(acosf(cosC));

        steps_addf("cos(A)=(b^2+c^2-a^2)/(2bc)=%.4g", (double)cosA);
        steps_addf("A = %.4g deg", (double)angles[0]);
        steps_addf("cos(B)=(a^2+c^2-b^2)/(2ac)=%.4g", (double)cosB);
        steps_addf("B = %.4g deg", (double)angles[1]);
        steps_addf("C = %.4g deg", (double)angles[2]);

        pos = snprintf(result_buf, buf_size,
                       "A=%.4g, B=%.4g, C=%.4g",
                       (double)angles[0], (double)angles[1], (double)angles[2]);
        return 1;
    }

    /* ── SAS: 2 sides + included angle ── */
    if (n_sides == 2 && n_angles == 1) {
        /* Check if the known angle is "included" (between the two known sides) */
        int si[2], ai = -1;
        int cnt = 0;
        for (i = 0; i < 3; i++) {
            if (sides[i] > 0) si[cnt++] = i;
            if (angles[i] > 0) ai = i;
        }

        /* The included angle for sides i,j is the angle at the vertex
         * opposite the unknown side. Map: sides 0,1 -> angle 2 is included,
         * sides 0,2 -> angle 1, sides 1,2 -> angle 0 */
        {
            int included = 3 - si[0] - si[1]; /* The "other" index */
            if (ai == included) {
                /* SAS case */
                ce_float_t a, b, C_rad, c_val, sinC, sinA;
                a = sides[si[0]];
                b = sides[si[1]];
                C_rad = DEG2RAD(angles[ai]);

                steps_addf("SAS: sides %.4g, %.4g, angle %.4g deg",
                           (double)a, (double)b, (double)angles[ai]);

                /* Law of cosines: c^2 = a^2 + b^2 - 2ab*cos(C) */
                c_val = sqrtf(a*a + b*b - 2*a*b*cosf(C_rad));
                sides[included] = c_val; /* Wait, 'included' is the angle index,
                                            which is also the OPPOSITE side index */
                /* Actually: angle[i] is opposite side[i]. So if angle[included]
                 * is known, the unknown side is side[included]. */
                sides[ai] = c_val;

                steps_addf("c^2=a^2+b^2-2ab*cos(C)");
                steps_addf("c = %.4g", (double)c_val);

                /* Now find remaining angles via law of sines */
                sinC = sinf(C_rad);
                if (fabsf(sinC) < 1e-10f || fabsf(c_val) < 1e-10f) {
                    snprintf(result_buf, buf_size, "Degenerate triangle");
                    return 0;
                }
                sinA = a * sinC / c_val;
                if (sinA > 1.0f) sinA = 1.0f;
                if (sinA < -1.0f) sinA = -1.0f;
                angles[si[0]] = RAD2DEG(asinf(sinA));
                angles[si[1]] = 180.0f - angles[si[0]] - angles[ai];

                steps_addf("sin(A)/a = sin(C)/c");
                steps_addf("A = %.4g deg", (double)angles[si[0]]);
                steps_addf("B = %.4g deg", (double)angles[si[1]]);

                pos = snprintf(result_buf, buf_size,
                    "a=%.4g, b=%.4g, c=%.4g, A=%.4g, B=%.4g, C=%.4g",
                    (double)sides[0], (double)sides[1], (double)sides[2],
                    (double)angles[0], (double)angles[1], (double)angles[2]);
                return 1;
            }
            /* If not included, fall through to SSA below */
        }
    }

    /* ── ASA / AAS: 2 angles + at least 1 side ── */
    if (n_angles >= 2 && n_sides >= 1) {
        ce_float_t angle_sum;

        /* Find the third angle */
        angle_sum = 0;
        for (i = 0; i < 3; i++)
            angle_sum += angles[i];

        if (n_angles == 2) {
            ce_float_t known1 = 0, known2 = 0;
            int j;
            for (j = 0; j < 3; j++) {
                if (angles[j] > 0) {
                    if (known1 == 0) known1 = angles[j];
                    else known2 = angles[j];
                }
            }
            for (i = 0; i < 3; i++) {
                if (angles[i] <= 0) {
                    angles[i] = 180.0f - angle_sum;
                    break;
                }
            }
            steps_addf("Third angle = 180 - %.4g - %.4g = %.4g",
                       (double)known1, (double)known2,
                       (double)angles[i]);
        }

        /* Validate */
        for (i = 0; i < 3; i++) {
            if (angles[i] <= 0 || angles[i] >= 180.0f) {
                snprintf(result_buf, buf_size, "Invalid angles");
                return 0;
            }
        }

        steps_addf("ASA/AAS: A=%.4g, B=%.4g, C=%.4g",
                   (double)angles[0], (double)angles[1], (double)angles[2]);

        /* Find the known side */
        {
            int known_side = -1;
            ce_float_t ratio;
            for (i = 0; i < 3; i++) {
                if (sides[i] > 0) { known_side = i; break; }
            }
            /* Law of sines: a/sin(A) = b/sin(B) = c/sin(C) */
            ratio = sides[known_side] / sinf(DEG2RAD(angles[known_side]));
            steps_addf("a/sin(A) = %.4g", (double)ratio);

            for (i = 0; i < 3; i++) {
                if (sides[i] <= 0) {
                    sides[i] = ratio * sinf(DEG2RAD(angles[i]));
                    steps_addf("side %c = %.4g", 'a' + i, (double)sides[i]);
                }
            }
        }

        pos = snprintf(result_buf, buf_size,
            "a=%.4g, b=%.4g, c=%.4g, A=%.4g, B=%.4g, C=%.4g",
            (double)sides[0], (double)sides[1], (double)sides[2],
            (double)angles[0], (double)angles[1], (double)angles[2]);
        return 1;
    }

    /* ── SSA: 2 sides + non-included angle ── */
    if (n_sides == 2 && n_angles == 1) {
        /* Find which side/angle we have */
        int known_angle = -1;
        int known_sides[2], ks = 0;
        ce_float_t a_side, b_side, A_ang, sinB, B_ang;

        for (i = 0; i < 3; i++) {
            if (angles[i] > 0) known_angle = i;
            if (sides[i] > 0) known_sides[ks++] = i;
        }

        /* a is the side opposite the known angle, b is the other known side */
        if (known_sides[0] == known_angle) {
            a_side = sides[known_sides[0]];
            b_side = sides[known_sides[1]];
        } else if (known_sides[1] == known_angle) {
            a_side = sides[known_sides[1]];
            b_side = sides[known_sides[0]];
        } else {
            /* Neither known side is opposite the known angle.
             * This means the angle IS opposite an unknown side.
             * Treat b_side as the one opposite unknown angle. */
            a_side = sides[known_sides[0]];
            b_side = sides[known_sides[1]];
            /* Actually, re-map: the known angle is opposite the unknown side.
             * We need to use the known angle with one of the known sides. */
            /* SSA: we know angle A and sides a (opposite A) and b.
             * If A is at index known_angle, then a = sides[known_angle].
             * But sides[known_angle] == 0 since it's not in known_sides.
             * In this case, this ISN'T a standard SSA — it's more like SAS
             * without the included angle. Fall through. */
            snprintf(result_buf, buf_size, "Insufficient information");
            return 0;
        }

        A_ang = DEG2RAD(angles[known_angle]);

        steps_addf("SSA: a=%.4g, b=%.4g, A=%.4g deg",
                   (double)a_side, (double)b_side, (double)angles[known_angle]);

        /* Law of sines: sin(B)/b = sin(A)/a */
        sinB = b_side * sinf(A_ang) / a_side;

        if (sinB > 1.0f) {
            steps_addf("sin(B) = %.4g > 1: no solution", (double)sinB);
            snprintf(result_buf, buf_size, "No solution (sin(B) > 1)");
            return 0;
        }

        B_ang = asinf(sinB);

        /* Check for ambiguous case (two possible triangles) */
        {
            ce_float_t B_alt = CE_PI - B_ang;
            bool two_solutions = (B_alt > 0 && B_alt + A_ang < CE_PI &&
                                  fabsf(sinB - 1.0f) > 1e-6f);

            if (two_solutions) {
                /* Two solutions */
                ce_float_t C1, C2, c1, c2;
                int other_side = 3 - known_sides[0] - known_sides[1];
                int other_angle;

                /* Find the third angle index */
                for (other_angle = 0; other_angle < 3; other_angle++) {
                    if (other_angle != known_angle &&
                        other_angle != known_sides[0] &&
                        other_angle != known_sides[1]) {
                        /* Actually we need the angle opposite to b_side */
                        break;
                    }
                }
                /* The angle opposite b_side */
                {
                    int b_idx = (known_sides[0] == known_angle) ?
                                 known_sides[1] : known_sides[0];

                    /* Solution 1 */
                    angles[b_idx] = RAD2DEG(B_ang);
                    C1 = CE_PI - A_ang - B_ang;
                    angles[other_side] = RAD2DEG(C1);
                    sides[other_side] = a_side * sinf(C1) / sinf(A_ang);

                    steps_addf("Solution 1: B=%.4g, C=%.4g, c=%.4g",
                               (double)angles[b_idx], (double)angles[other_side],
                               (double)sides[other_side]);
                    c1 = sides[other_side];

                    /* Solution 2 */
                    C2 = CE_PI - A_ang - B_alt;
                    c2 = a_side * sinf(C2) / sinf(A_ang);

                    steps_addf("Solution 2: B=%.4g, C=%.4g, c=%.4g",
                               (double)RAD2DEG(B_alt), (double)RAD2DEG(C2), (double)c2);

                    pos = snprintf(result_buf, buf_size,
                        "Two solutions: (B=%.4g, C=%.4g, c=%.4g) or "
                        "(B=%.4g, C=%.4g, c=%.4g)",
                        (double)RAD2DEG(B_ang), (double)RAD2DEG(C1), (double)c1,
                        (double)RAD2DEG(B_alt), (double)RAD2DEG(C2), (double)c2);
                    (void)pos;
                }
                return 2;
            } else {
                /* One solution */
                int b_idx = (known_sides[0] == known_angle) ?
                             known_sides[1] : known_sides[0];
                int other_side = 3 - known_sides[0] - known_sides[1];
                ce_float_t C_rad;

                angles[b_idx] = RAD2DEG(B_ang);
                C_rad = CE_PI - A_ang - B_ang;
                angles[other_side] = RAD2DEG(C_rad);
                sides[other_side] = a_side * sinf(C_rad) / sinf(A_ang);

                steps_addf("B = %.4g deg", (double)angles[b_idx]);
                steps_addf("C = %.4g deg", (double)angles[other_side]);
                steps_addf("c = %.4g", (double)sides[other_side]);

                pos = snprintf(result_buf, buf_size,
                    "a=%.4g, b=%.4g, c=%.4g, A=%.4g, B=%.4g, C=%.4g",
                    (double)sides[0], (double)sides[1], (double)sides[2],
                    (double)angles[0], (double)angles[1], (double)angles[2]);
                (void)pos;
                return 1;
            }
        }
    }

    snprintf(result_buf, buf_size, "Insufficient information");
    return 0;
}

/* ── Partial Fraction Decomposition ─────────────────────────── */

/* Helper: check if node is a single variable */
static bool pf_is_var(const ast_node_t *n, char v)
{
    return n && n->type == NODE_VARIABLE && n->d.variable == v;
}

ast_node_t *algebra_partial_fractions(const ast_node_t *expr, char var,
                                       char *result_buf, int buf_size)
{
    const ast_node_t *num, *den;
    const ast_node_t *L, *R;
    eval_ctx_t ctx;

    if (!expr || expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV) {
        snprintf(result_buf, buf_size, "Not a fraction");
        return NULL;
    }

    num = expr->d.binop.left;
    den = expr->d.binop.right;

    {
        char nbuf[60], dbuf[60];
        ast_to_str(num, nbuf, sizeof(nbuf));
        ast_to_str(den, dbuf, sizeof(dbuf));
        steps_addf("Partial fractions: (%s) / (%s)", nbuf, dbuf);
    }

    /* ── Pattern: c / (x * (x + a)) ─────────────────────── */
    if (den->type == NODE_BINOP && den->d.binop.op == OP_MUL) {
        L = den->d.binop.left;
        R = den->d.binop.right;

        /* c / (x * (x + a)) where c is constant, a is constant */
        if (pf_is_var(L, var) && R->type == NODE_BINOP &&
            (R->d.binop.op == OP_ADD || R->d.binop.op == OP_SUB) &&
            pf_is_var(R->d.binop.left, var) &&
            !ast_contains_var(R->d.binop.right, var) &&
            !ast_contains_var(num, var)) {

            ce_float_t c_val, a_val, A, B;

            eval_init(&ctx);
            c_val = eval_expr(&ctx, num);
            if (eval_error(&ctx) != CE_OK) goto fail;

            eval_init(&ctx);
            a_val = eval_expr(&ctx, R->d.binop.right);
            if (eval_error(&ctx) != CE_OK) goto fail;
            if (R->d.binop.op == OP_SUB) a_val = -a_val;
            if (fabsf(a_val) < 1e-10f) goto fail;

            A = c_val / a_val;
            B = -c_val / a_val;

            steps_addf("= %.4g/%c + %.4g/(%c%+.4g)",
                       (double)A, var, (double)B, var, (double)a_val);
            snprintf(result_buf, buf_size,
                     "%.4g/%c + %.4g/(%c%+.4g)",
                     (double)A, var, (double)B, var, (double)a_val);

            return ast_binop(OP_ADD,
                ast_binop(OP_DIV, ast_number(A), ast_variable(var)),
                ast_binop(OP_DIV, ast_number(B),
                    ast_binop(OP_ADD, ast_variable(var), ast_number(a_val))));
        }

        /* c / ((x+a)(x+b)) or (dx+e) / ((x+a)(x+b)) */
        if (L->type == NODE_BINOP &&
            (L->d.binop.op == OP_ADD || L->d.binop.op == OP_SUB) &&
            pf_is_var(L->d.binop.left, var) && !ast_contains_var(L->d.binop.right, var) &&
            R->type == NODE_BINOP &&
            (R->d.binop.op == OP_ADD || R->d.binop.op == OP_SUB) &&
            pf_is_var(R->d.binop.left, var) && !ast_contains_var(R->d.binop.right, var)) {

            ce_float_t a_val, b_val, A, B;

            eval_init(&ctx);
            a_val = eval_expr(&ctx, L->d.binop.right);
            if (eval_error(&ctx) != CE_OK) goto fail;
            if (L->d.binop.op == OP_SUB) a_val = -a_val;

            eval_init(&ctx);
            b_val = eval_expr(&ctx, R->d.binop.right);
            if (eval_error(&ctx) != CE_OK) goto fail;
            if (R->d.binop.op == OP_SUB) b_val = -b_val;

            if (fabsf(a_val - b_val) < 1e-10f) goto fail; /* repeated root */

            /* For constant numerator c: A = c/(b-a), B = c/(a-b)
             * For numerator (dx+e): A = (d*(-a)+e)/(b-a), B = (d*(-b)+e)/(a-b) */
            if (!ast_contains_var(num, var)) {
                ce_float_t c_val;
                eval_init(&ctx);
                c_val = eval_expr(&ctx, num);
                if (eval_error(&ctx) != CE_OK) goto fail;

                A = c_val / (b_val - a_val);
                B = c_val / (a_val - b_val);
            } else {
                /* Numerator contains var: evaluate at x=-a and x=-b */
                ce_float_t num_at_neg_a, num_at_neg_b;
                eval_init(&ctx);
                eval_set_var(&ctx, var, -a_val);
                num_at_neg_a = eval_expr(&ctx, num);
                if (eval_error(&ctx) != CE_OK) goto fail;

                eval_init(&ctx);
                eval_set_var(&ctx, var, -b_val);
                num_at_neg_b = eval_expr(&ctx, num);
                if (eval_error(&ctx) != CE_OK) goto fail;

                /* Heaviside cover-up: A = num(-a)/((-a+b)) = num(-a)/(b-a)
                 *                     B = num(-b)/((-b+a)) = num(-b)/(a-b) */
                A = num_at_neg_a / (b_val - a_val);
                B = num_at_neg_b / (a_val - b_val);
            }

            steps_addf("= %.4g/(%c%+.4g) + %.4g/(%c%+.4g)",
                       (double)A, var, (double)a_val, (double)B, var, (double)b_val);
            snprintf(result_buf, buf_size,
                     "%.4g/(%c%+.4g) + %.4g/(%c%+.4g)",
                     (double)A, var, (double)a_val, (double)B, var, (double)b_val);

            return ast_binop(OP_ADD,
                ast_binop(OP_DIV, ast_number(A),
                    ast_binop(OP_ADD, ast_variable(var), ast_number(a_val))),
                ast_binop(OP_DIV, ast_number(B),
                    ast_binop(OP_ADD, ast_variable(var), ast_number(b_val))));
        }
    }

    /* ── Pattern: c / (x^2 - a^2) = c / ((x-a)(x+a)) ───── */
    if (!ast_contains_var(num, var) &&
        den->type == NODE_BINOP && den->d.binop.op == OP_SUB &&
        den->d.binop.left->type == NODE_BINOP &&
        den->d.binop.left->d.binop.op == OP_POW &&
        pf_is_var(den->d.binop.left->d.binop.left, var) &&
        den->d.binop.left->d.binop.right->type == NODE_NUMBER &&
        fabsf(den->d.binop.left->d.binop.right->d.number - 2) < 1e-10f &&
        !ast_contains_var(den->d.binop.right, var)) {

        ce_float_t c_val, a2, a_val, A, B;

        eval_init(&ctx);
        c_val = eval_expr(&ctx, num);
        if (eval_error(&ctx) != CE_OK) goto fail;

        eval_init(&ctx);
        a2 = eval_expr(&ctx, den->d.binop.right);
        if (eval_error(&ctx) != CE_OK || a2 <= 0) goto fail;
        a_val = sqrtf(a2);

        A = c_val / (2.0f * a_val);
        B = -c_val / (2.0f * a_val);

        steps_addf("Factor: %c^2 - %.4g = (%c-%.4g)(%c+%.4g)",
                   var, (double)a2, var, (double)a_val, var, (double)a_val);
        steps_addf("= %.4g/(%c-%.4g) + %.4g/(%c+%.4g)",
                   (double)A, var, (double)a_val, (double)B, var, (double)a_val);
        snprintf(result_buf, buf_size,
                 "%.4g/(%c-%.4g) + %.4g/(%c+%.4g)",
                 (double)A, var, (double)a_val, (double)B, var, (double)a_val);

        return ast_binop(OP_ADD,
            ast_binop(OP_DIV, ast_number(A),
                ast_binop(OP_SUB, ast_variable(var), ast_number(a_val))),
            ast_binop(OP_DIV, ast_number(B),
                ast_binop(OP_ADD, ast_variable(var), ast_number(a_val))));
    }

fail:
    snprintf(result_buf, buf_size, "Cannot decompose");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════ */
/*                COMPLETE THE SQUARE                          */
/* ═══════════════════════════════════════════════════════════ */

int algebra_complete_square(const ast_node_t *expr, char var,
                             char *result_buf, int buf_size)
{
    poly_t p;
    ce_float_t a, b, c, h, k;

    if (!var) var = find_var(expr);


    if (!var || !ast_to_poly(expr, var, &p) || p.degree != 2) {
        snprintf(result_buf, buf_size, "Not a quadratic");
        return 0;
    }

    a = p.coeff[2];
    b = p.coeff[1];
    c = p.coeff[0];

    if (fabsf(a) < 1e-10f) {
        snprintf(result_buf, buf_size, "Not a quadratic");
        return 0;
    }

    h = b / (2.0f * a);
    k = c - (b * b) / (4.0f * a);

    steps_addf("Complete the square: %.4g%c^2 + %.4g%c + %.4g",
               (double)a, var, (double)b, var, (double)c);
    steps_addf("Factor out a=%.4g", (double)a);
    steps_addf("Half of b/a = %.4g/(2*%.4g) = %.4g",
               (double)b, (double)a, (double)h);
    steps_addf("k = c - b^2/(4a) = %.4g", (double)k);

    if (fabsf(a - 1.0f) < 1e-6f) {
        if (fabsf(k) < 1e-6f)
            snprintf(result_buf, buf_size, "(%c + %.4g)^2",
                     var, (double)h);
        else
            snprintf(result_buf, buf_size, "(%c + %.4g)^2 + %.4g",
                     var, (double)h, (double)k);
    } else {
        if (fabsf(k) < 1e-6f)
            snprintf(result_buf, buf_size, "%.4g(%c + %.4g)^2",
                     (double)a, var, (double)h);
        else
            snprintf(result_buf, buf_size, "%.4g(%c + %.4g)^2 + %.4g",
                     (double)a, var, (double)h, (double)k);
    }

    steps_addf("= %s", result_buf);
    return 1;
}

/* ═══════════════════════════════════════════════════════════ */
/*                DESCARTES' RULE OF SIGNS                     */
/* ═══════════════════════════════════════════════════════════ */

int algebra_descartes(const ast_node_t *expr, char var,
                       char *result_buf, int buf_size)
{
    poly_t p;
    int pos_changes = 0, neg_changes = 0;
    int i;
    ce_float_t prev;

    if (!var) var = find_var(expr);


    if (!var || !ast_to_poly(expr, var, &p) || p.degree < 1) {
        snprintf(result_buf, buf_size, "Not a polynomial");
        return 0;
    }

    steps_addf("Descartes' Rule for degree %d polynomial", p.degree);

    /* Count sign changes in f(x) coefficients (high to low) */
    prev = 0;
    for (i = p.degree; i >= 0; i--) {
        if (fabsf(p.coeff[i]) < 1e-8f) continue;
        if (prev != 0 && ((prev > 0) != (p.coeff[i] > 0)))
            pos_changes++;
        prev = p.coeff[i];
    }

    /* Count sign changes in f(-x): flip sign of odd-degree coefficients */
    prev = 0;
    for (i = p.degree; i >= 0; i--) {
        ce_float_t ci = (i % 2 == 1) ? -p.coeff[i] : p.coeff[i];
        if (fabsf(ci) < 1e-8f) continue;
        if (prev != 0 && ((prev > 0) != (ci > 0)))
            neg_changes++;
        prev = ci;
    }

    steps_addf("Sign changes in f(x): %d", pos_changes);
    steps_addf("Sign changes in f(-x): %d", neg_changes);
    steps_addf("Max positive real roots: %d", pos_changes);
    steps_addf("Max negative real roots: %d", neg_changes);

    snprintf(result_buf, buf_size,
             "Positive roots: at most %d, Negative roots: at most %d",
             pos_changes, neg_changes);
    return 1;
}

/* ═══════════════════════════════════════════════════════════ */
/*                LINE FROM TWO POINTS                         */
/* ═══════════════════════════════════════════════════════════ */

int algebra_line_two_points(ce_float_t x1, ce_float_t y1,
                             ce_float_t x2, ce_float_t y2,
                             char *result_buf, int buf_size)
{
    steps_addf("Line through (%.4g, %.4g) and (%.4g, %.4g)",
               (double)x1, (double)y1, (double)x2, (double)y2);

    /* Vertical line */
    if (fabsf(x2 - x1) < 1e-10f) {
        if (fabsf(y2 - y1) < 1e-10f) {
            snprintf(result_buf, buf_size, "Duplicate points");
            return 0;
        }
        steps_addf("Vertical line: x = %.4g", (double)x1);
        snprintf(result_buf, buf_size, "x = %.6g", (double)x1);
        return 1;
    }

    {
        ce_float_t m = (y2 - y1) / (x2 - x1);
        ce_float_t b = y1 - m * x1;

        steps_addf("m = (%.4g - %.4g) / (%.4g - %.4g) = %.6g",
                   (double)y2, (double)y1, (double)x2, (double)x1, (double)m);
        steps_addf("b = %.4g - %.6g * %.4g = %.6g",
                   (double)y1, (double)m, (double)x1, (double)b);

        if (fabsf(m) < 1e-10f)
            snprintf(result_buf, buf_size, "y = %.6g", (double)b);
        else if (fabsf(b) < 1e-10f)
            snprintf(result_buf, buf_size, "y = %.6g*x", (double)m);
        else
            snprintf(result_buf, buf_size, "y = %.6g*x + %.6g",
                     (double)m, (double)b);

        steps_addf("= %s", result_buf);
        return 1;
    }
}

/* ═══════════════════════════════════════════════════════════ */
/*              LINE FROM SLOPE + POINT                        */
/* ═══════════════════════════════════════════════════════════ */

int algebra_line_slope_point(ce_float_t m, ce_float_t x1, ce_float_t y1,
                              char *result_buf, int buf_size)
{
    ce_float_t b = y1 - m * x1;

    steps_addf("Point-slope form: y - %.4g = %.4g(x - %.4g)",
               (double)y1, (double)m, (double)x1);
    steps_addf("b = %.4g - %.4g * %.4g = %.6g",
               (double)y1, (double)m, (double)x1, (double)b);

    if (fabsf(m) < 1e-10f)
        snprintf(result_buf, buf_size, "y = %.6g", (double)b);
    else if (fabsf(b) < 1e-10f)
        snprintf(result_buf, buf_size, "y = %.6g*x", (double)m);
    else
        snprintf(result_buf, buf_size, "y = %.6g*x + %.6g",
                 (double)m, (double)b);

    steps_addf("= %s", result_buf);
    return 1;
}

/* ═══════════════════════════════════════════════════════════ */
/*             PARABOLA FROM VERTEX + POINT                    */
/* ═══════════════════════════════════════════════════════════ */

int algebra_parabola_vertex(ce_float_t vx, ce_float_t vy,
                             ce_float_t px, ce_float_t py,
                             char *result_buf, int buf_size)
{
    ce_float_t dx, a;

    steps_addf("Vertex: (%.4g, %.4g), Point: (%.4g, %.4g)",
               (double)vx, (double)vy, (double)px, (double)py);

    dx = px - vx;
    if (fabsf(dx) < 1e-10f) {
        snprintf(result_buf, buf_size, "Point at vertex x-coordinate");
        return 0;
    }

    a = (py - vy) / (dx * dx);

    steps_addf("y = a(x - h)^2 + k");
    steps_addf("a = (%.4g - %.4g) / (%.4g - %.4g)^2 = %.6g",
               (double)py, (double)vy, (double)px, (double)vx, (double)a);

    if (fabsf(vx) < 1e-10f && fabsf(vy) < 1e-10f)
        snprintf(result_buf, buf_size, "y = %.6g*x^2", (double)a);
    else if (fabsf(vx) < 1e-10f)
        snprintf(result_buf, buf_size, "y = %.6g*x^2 + %.6g",
                 (double)a, (double)vy);
    else if (fabsf(vy) < 1e-10f)
        snprintf(result_buf, buf_size, "y = %.6g*(x - %.6g)^2",
                 (double)a, (double)vx);
    else
        snprintf(result_buf, buf_size, "y = %.6g*(x - %.6g)^2 + %.6g",
                 (double)a, (double)vx, (double)vy);

    steps_addf("= %s", result_buf);
    return 1;
}

/* ═══════════════════════════════════════════════════════════ */
/*         TRIG POWER-REDUCTION EXPAND                         */
/* ═══════════════════════════════════════════════════════════ */

ast_node_t *algebra_trig_expand(ast_node_t *expr)
{
    func_id_t fn;
    const ast_node_t *arg;

    if (!expr) return NULL;

    /* Recurse into children first */
    switch (expr->type) {
    case NODE_BINOP:
        expr->d.binop.left  = algebra_trig_expand(expr->d.binop.left);
        expr->d.binop.right = algebra_trig_expand(expr->d.binop.right);
        break;
    case NODE_UNOP:
        expr->d.unop.operand = algebra_trig_expand(expr->d.unop.operand);
        break;
    case NODE_FUNC:
        expr->d.func.arg = algebra_trig_expand(expr->d.func.arg);
        break;
    default:
        return expr;
    }

    /* sin^2(x) = (1 - cos(2x)) / 2 */
    if (is_trig_sq(expr, &fn, &arg) && fn == FN_SIN) {
        ast_node_t *two_arg = ast_binop(OP_MUL, ast_number(2), ast_clone(arg));
        ast_node_t *result = ast_binop(OP_DIV,
            ast_binop(OP_SUB, ast_number(1),
                      ast_func(FN_COS, two_arg)),
            ast_number(2));
        ast_free(expr);
        steps_addf("sin^2(u) = (1-cos(2u))/2");
        return result;
    }

    /* cos^2(x) = (1 + cos(2x)) / 2 */
    if (is_trig_sq(expr, &fn, &arg) && fn == FN_COS) {
        ast_node_t *two_arg = ast_binop(OP_MUL, ast_number(2), ast_clone(arg));
        ast_node_t *result = ast_binop(OP_DIV,
            ast_binop(OP_ADD, ast_number(1),
                      ast_func(FN_COS, two_arg)),
            ast_number(2));
        ast_free(expr);
        steps_addf("cos^2(u) = (1+cos(2u))/2");
        return result;
    }

    return expr;
}

/* ═══════════════════════════════════════════════════════════ */
/*             RATIONAL ROOT THEOREM (PUBLIC)                   */
/* ═══════════════════════════════════════════════════════════ */

int algebra_rational_roots(const ast_node_t *expr, char var,
                           char *result_buf, int buf_size)
{
    poly_t p;
    ce_float_t cands[128];
    int n_cands, i, pos = 0;
    int a0, an;

    if (!result_buf || buf_size <= 0) return 0;
    result_buf[0] = '\0';

    if (!algebra_ast_to_poly(expr, var, &p) || p.degree < 1) {
        snprintf(result_buf, buf_size, "Not a polynomial");
        return 0;
    }

    a0 = (int)(p.coeff[0] + (p.coeff[0] >= 0 ? 0.5f : -0.5f));
    an = (int)(p.coeff[p.degree] + (p.coeff[p.degree] >= 0 ? 0.5f : -0.5f));

    steps_addf("Rational Root Theorem");
    steps_addf("Constant term a0 = %d, leading coeff an = %d", a0, an);
    steps_addf("Candidates: +/- (factors of |%d|) / (factors of |%d|)", a0, an);

    n_cands = build_rational_candidates(&p, cands, 128);

    pos += snprintf(result_buf + pos, buf_size - pos, "Possible rational roots: ");
    for (i = 0; i < n_cands && pos < buf_size - 1; i++) {
        if (i > 0) pos += snprintf(result_buf + pos, buf_size - pos, ", ");
        pos += snprintf(result_buf + pos, buf_size - pos, "%.6g", (double)cands[i]);
    }

    return n_cands;
}

/* ═══════════════════════════════════════════════════════════ */
/*             INEQUALITY SOLVING                              */
/* ═══════════════════════════════════════════════════════════ */

int algebra_solve_inequality(const ast_node_t *expr, char var, char sign,
                              char *result_buf, int buf_size)
{
    poly_t p;
    char buf[60];
    const char *op_str;

    if (!var) var = find_var(expr);

    ast_to_str(expr, buf, sizeof(buf));

    switch (sign) {
    case '<': op_str = "<"; break;
    case '>': op_str = ">"; break;
    case 'L': op_str = "<="; break;
    case 'G': op_str = ">="; break;
    default:
        snprintf(result_buf, buf_size, "Invalid sign");
        return 0;
    }

    steps_addf("Solve: %s %s 0 for %c", buf, op_str, var);

    if (!var || !ast_to_poly(expr, var, &p)) {
        snprintf(result_buf, buf_size, "Cannot convert to polynomial");
        return 0;
    }

    /* Degree 0: constant inequality */
    if (p.degree == 0) {
        bool holds = false;
        ce_float_t c = p.coeff[0];
        switch (sign) {
        case '<': holds = (c < 0); break;
        case '>': holds = (c > 0); break;
        case 'L': holds = (c <= 0); break;
        case 'G': holds = (c >= 0); break;
        }
        snprintf(result_buf, buf_size, holds ? "All real numbers" : "No solution");
        return holds ? 1 : 0;
    }

    /* Degree 1: linear inequality a*x + b <op> 0 */
    if (p.degree == 1) {
        ce_float_t a = p.coeff[1];
        ce_float_t b = p.coeff[0];
        ce_float_t boundary = -b / a;
        bool flip = (a < 0); /* dividing by negative flips the inequality */
        char effective;

        steps_addf("Linear: %.4g*%c + %.4g %s 0", (double)a, var, (double)b, op_str);
        steps_addf("%c = %.6g is the boundary", var, (double)boundary);

        /* Determine effective comparison after dividing by a */
        if (!flip)
            effective = sign;
        else {
            switch (sign) {
            case '<': effective = '>'; break;
            case '>': effective = '<'; break;
            case 'L': effective = 'G'; break;
            case 'G': effective = 'L'; break;
            default:  effective = sign;
            }
            steps_addf("Divide by negative (%.4g): flip inequality", (double)a);
        }

        switch (effective) {
        case '<': snprintf(result_buf, buf_size, "%c < %.6g", var, (double)boundary); break;
        case '>': snprintf(result_buf, buf_size, "%c > %.6g", var, (double)boundary); break;
        case 'L': snprintf(result_buf, buf_size, "%c <= %.6g", var, (double)boundary); break;
        case 'G': snprintf(result_buf, buf_size, "%c >= %.6g", var, (double)boundary); break;
        }
        steps_addf("= %s", result_buf);
        return 1;
    }

    /* Degree 2: quadratic inequality a*x^2 + b*x + c <op> 0 */
    if (p.degree == 2) {
        ce_float_t a = p.coeff[2], b = p.coeff[1], c = p.coeff[0];
        ce_float_t disc = b * b - 4 * a * c;

        steps_addf("Quadratic: %.4g%c^2 + %.4g%c + %.4g %s 0",
                   (double)a, var, (double)b, var, (double)c, op_str);
        steps_addf("disc = %.6g", (double)disc);

        if (disc < -1e-6f) {
            /* No real roots — parabola doesn't cross x-axis */
            bool always_pos = (a > 0);
            bool want_pos = (sign == '>' || sign == 'G');
            bool want_neg = (sign == '<' || sign == 'L');

            steps_addf("No real roots; parabola is always %s",
                       always_pos ? "positive" : "negative");

            if ((always_pos && want_pos) || (!always_pos && want_neg))
                snprintf(result_buf, buf_size, "All real numbers");
            else
                snprintf(result_buf, buf_size, "No solution");
            return 1;
        }

        if (disc < 1e-6f) disc = 0;

        {
            ce_float_t sd = sqrtf(disc);
            ce_float_t r1 = (-b - sd) / (2 * a);
            ce_float_t r2 = (-b + sd) / (2 * a);
            ce_float_t lo_r, hi_r;
            bool opens_up = (a > 0);

            /* Ensure r1 <= r2 */
            if (r1 > r2) { lo_r = r2; hi_r = r1; } else { lo_r = r1; hi_r = r2; }

            steps_addf("Roots: %.6g, %.6g", (double)lo_r, (double)hi_r);
            steps_addf("Parabola opens %s", opens_up ? "up" : "down");

            /* For a>0: f(x) < 0 between roots, f(x) > 0 outside roots
             * For a<0: opposite */
            if (sign == '<' || sign == 'L') {
                if (opens_up) {
                    /* Want f < 0: between roots */
                    if (fabsf(disc) < 1e-6f) {
                        snprintf(result_buf, buf_size,
                                 sign == 'L' ? "%c = %.6g" : "No solution",
                                 var, (double)lo_r);
                    } else {
                        snprintf(result_buf, buf_size,
                                 "%c %s %.6g and %c %s %.6g",
                                 var, sign == '<' ? ">" : ">=", (double)lo_r,
                                 var, sign == '<' ? "<" : "<=", (double)hi_r);
                    }
                } else {
                    /* a<0, want f < 0: outside roots */
                    if (fabsf(disc) < 1e-6f) {
                        snprintf(result_buf, buf_size,
                                 "%c != %.6g", var, (double)lo_r);
                    } else {
                        snprintf(result_buf, buf_size,
                                 "%c %s %.6g or %c %s %.6g",
                                 var, sign == '<' ? "<" : "<=", (double)lo_r,
                                 var, sign == '<' ? ">" : ">=", (double)hi_r);
                    }
                }
            } else {
                /* sign == '>' || sign == 'G' */
                if (opens_up) {
                    /* Want f > 0: outside roots */
                    if (fabsf(disc) < 1e-6f) {
                        snprintf(result_buf, buf_size,
                                 sign == 'G' ? "All real numbers" : "%c != %.6g",
                                 var, (double)lo_r);
                    } else {
                        snprintf(result_buf, buf_size,
                                 "%c %s %.6g or %c %s %.6g",
                                 var, sign == '>' ? "<" : "<=", (double)lo_r,
                                 var, sign == '>' ? ">" : ">=", (double)hi_r);
                    }
                } else {
                    /* a<0, want f > 0: between roots */
                    if (fabsf(disc) < 1e-6f) {
                        snprintf(result_buf, buf_size,
                                 sign == 'G' ? "%c = %.6g" : "No solution",
                                 var, (double)lo_r);
                    } else {
                        snprintf(result_buf, buf_size,
                                 "%c %s %.6g and %c %s %.6g",
                                 var, sign == '>' ? ">" : ">=", (double)lo_r,
                                 var, sign == '>' ? "<" : "<=", (double)hi_r);
                    }
                }
            }
            steps_addf("= %s", result_buf);
            return 1;
        }
    }

    snprintf(result_buf, buf_size, "Only linear/quadratic inequalities supported");
    return 0;
}

/* ═══════════════════════════════════════════════════════════ */
/*           DECIMAL TO FRACTION CONVERSION                    */
/* ═══════════════════════════════════════════════════════════ */

/*
 * Stern-Brocot / continued fraction best rational approximation.
 * Finds p/q such that |p/q - value| is minimized with q <= max_denom.
 */
int algebra_dec_to_frac(ce_float_t value, int max_denom,
                         char *result_buf, int buf_size)
{
    int sign = 1;
    ce_float_t x;
    /* Mediant variables for Stern-Brocot tree search */
    int p0, q0, p1, q1, pm, qm;
    int best_p, best_q;
    ce_float_t best_err;
    int iters;


    /* Handle special cases */
    if (!ce_isfinite(value)) {
        snprintf(result_buf, buf_size, "Not a finite number");
        return 0;
    }

    /* Handle negative values */
    if (value < 0) { sign = -1; value = -value; }

    /* Handle zero */
    if (value < 1e-10f) {
        snprintf(result_buf, buf_size, "0");
        steps_addf("0 = 0/1");
        return 1;
    }

    /* Handle integers */
    {
        ce_float_t rounded = (float)(int)(value + 0.5f);
        if (fabsf(value - rounded) < 1e-7f) {
            int iv = (int)(rounded) * sign;
            snprintf(result_buf, buf_size, "%d", iv);
            steps_addf("%.6g = %d/1", (double)(value * sign), iv);
            return 1;
        }
    }

    x = value;
    steps_addf("Convert %.8g to fraction", (double)(x * sign));

    /* Stern-Brocot tree: binary search between p0/q0=0/1 and p1/q1=ceil(x)/1 */
    p0 = 0; q0 = 1;
    p1 = (int)x + 1; q1 = 1;

    best_p = (int)(x + 0.5f);
    best_q = 1;
    best_err = fabsf(x - (ce_float_t)best_p);

    for (iters = 0; iters < 200; iters++) {
        pm = p0 + p1;
        qm = q0 + q1;

        /* Check denominator limit */
        if (qm > max_denom) break;

        {
            ce_float_t mediant = (ce_float_t)pm / (ce_float_t)qm;
            ce_float_t err = fabsf(x - mediant);

            if (err < best_err) {
                best_p = pm;
                best_q = qm;
                best_err = err;
            }

            /* Good enough? */
            if (err < 1e-7f) break;

            if (mediant < x) {
                p0 = pm; q0 = qm;
            } else {
                p1 = pm; q1 = qm;
            }
        }
    }

    /* Check if the approximation is close enough */
    if (best_err > 0.001f) {
        snprintf(result_buf, buf_size, "No simple fraction found");
        return 0;
    }

    best_p *= sign;

    /* Simplify by GCD (should already be simplified from Stern-Brocot) */
    {
        int a = best_p < 0 ? -best_p : best_p;
        int b = best_q;
        while (b) { int t = b; b = a % b; a = t; }
        if (a > 1) { best_p /= a; best_q /= a; }
    }

    if (best_q == 1) {
        snprintf(result_buf, buf_size, "%d", best_p);
    } else {
        snprintf(result_buf, buf_size, "%d/%d", best_p, best_q);
    }

    steps_addf("%.8g %s %d/%d",
               (double)(value * sign), "\xe2\x89\x88",
               best_p, best_q);
    steps_addf("Verify: %d/%d = %.8g", best_p, best_q,
               (double)((ce_float_t)best_p / (ce_float_t)best_q));

    return 1;
}

/* ═══════════════════════════════════════════════════════════ */
/*                    UNIT CIRCLE REFERENCE                     */
/* ═══════════════════════════════════════════════════════════ */

/*
 * Unit circle lookup: given an angle in degrees, return exact trig values.
 * Writes formatted string: "sin=..., cos=..., tan=..."
 * Supports 0, 30, 45, 60, 90, 120, 135, 150, 180, 210, 225, 240, 270, 300, 315, 330, 360.
 */
int algebra_unit_circle(ce_float_t angle_deg, char *result_buf, int buf_size)
{
    /* Normalize to [0, 360) */
    int deg;
    typedef struct {
        int deg;
        const char *sin_s; const char *cos_s; const char *tan_s;
    } uc_entry_t;

    static const uc_entry_t table[] = {
        {  0,   "0",          "1",          "0"          },
        { 30,   "1/2",        "sqrt(3)/2",  "sqrt(3)/3"  },
        { 45,   "sqrt(2)/2",  "sqrt(2)/2",  "1"          },
        { 60,   "sqrt(3)/2",  "1/2",        "sqrt(3)"    },
        { 90,   "1",          "0",          "undef"      },
        {120,   "sqrt(3)/2",  "-1/2",       "-sqrt(3)"   },
        {135,   "sqrt(2)/2",  "-sqrt(2)/2", "-1"         },
        {150,   "1/2",        "-sqrt(3)/2", "-sqrt(3)/3" },
        {180,   "0",          "-1",         "0"          },
        {210,   "-1/2",       "-sqrt(3)/2", "sqrt(3)/3"  },
        {225,   "-sqrt(2)/2", "-sqrt(2)/2", "1"          },
        {240,   "-sqrt(3)/2", "-1/2",       "sqrt(3)"    },
        {270,   "-1",         "0",          "undef"      },
        {300,   "-sqrt(3)/2", "1/2",        "-sqrt(3)"   },
        {315,   "-sqrt(2)/2", "sqrt(2)/2",  "-1"         },
        {330,   "-1/2",       "sqrt(3)/2",  "-sqrt(3)/3" },
        {360,   "0",          "1",          "0"          },
        { -1,   NULL,         NULL,         NULL         }
    };
    int i;

    /* Normalize angle */
    deg = (int)angle_deg;
    deg = deg % 360;
    if (deg < 0) deg += 360;

    steps_addf("Unit circle: %d degrees", deg);

    for (i = 0; table[i].deg >= 0; i++) {
        if (table[i].deg == deg) {
            snprintf(result_buf, buf_size,
                     "sin(%d)=%s, cos(%d)=%s, tan(%d)=%s",
                     deg, table[i].sin_s,
                     deg, table[i].cos_s,
                     deg, table[i].tan_s);
            steps_addf("sin(%d) = %s", deg, table[i].sin_s);
            steps_addf("cos(%d) = %s", deg, table[i].cos_s);
            steps_addf("tan(%d) = %s", deg, table[i].tan_s);
            return 1;
        }
    }

    snprintf(result_buf, buf_size,
             "%d deg not a standard unit circle angle", deg);
    steps_addf("%d is not a standard angle", deg);
    return 0;
}

/* ═══════════════════════════════════════════════════════════ */
/*          NONLINEAR SYSTEM SOLVER (NEWTON'S METHOD)          */
/* ═══════════════════════════════════════════════════════════ */

/* Set when any eval_2var() since the last reset failed (domain, pole,
 * recursion limit). Its value is then 0, which is not a residual: the
 * Newton start that produced it is abandoned instead of "converging". */
static bool s_nl_eval_failed;

/* Evaluate expression with two variables bound */
static ce_float_t eval_2var(const ast_node_t *expr,
                             char v1, ce_float_t x1,
                             char v2, ce_float_t x2)
{
    eval_ctx_t ctx;
    ce_float_t v;
    eval_init(&ctx);
    eval_set_var(&ctx, v1, x1);
    eval_set_var(&ctx, v2, x2);
    v = eval_expr(&ctx, expr);
    if (eval_error(&ctx) != CE_OK) s_nl_eval_failed = true;
    return v;
}

/* Partial derivative via central difference */
static ce_float_t partial_deriv(const ast_node_t *expr,
                                 char v1, ce_float_t x1,
                                 char v2, ce_float_t x2,
                                 int wrt)  /* 0=v1, 1=v2 */
{
    ce_float_t h = 1e-4f;
    ce_float_t fp, fm;

    if (wrt == 0) {
        fp = eval_2var(expr, v1, x1 + h, v2, x2);
        fm = eval_2var(expr, v1, x1 - h, v2, x2);
    } else {
        fp = eval_2var(expr, v1, x1, v2, x2 + h);
        fm = eval_2var(expr, v1, x1, v2, x2 - h);
    }
    return (fp - fm) / (2.0f * h);
}

int algebra_solve_nonlinear(const ast_node_t **equations,
                             const char *vars,
                             char *result_buf, int buf_size)
{
    /* Solution storage: up to 8 solutions */
    ce_float_t sols_x[8], sols_y[8];
    int n_sols = 0;
    int g, iter;
    char v1, v2;

    /* Grid of initial guesses covering [-5,5]x[-5,5] */
    static const ce_float_t guesses[] = {
        0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 5.0f, -5.0f,
        0.5f, -0.5f, 1.5f, -1.5f, 4.0f, -4.0f
    };
    int n_guesses = 15;

    if (strlen(vars) != 2) {
        snprintf(result_buf, buf_size, "Need exactly 2 variables");
        return 0;
    }

    v1 = vars[0];
    v2 = vars[1];

    steps_addf("Nonlinear system: 2 equations in %c, %c", v1, v2);
    steps_addf("Using Newton's method with multiple initial guesses");

    /* Try each combination of initial guesses */
    for (g = 0; g < n_guesses * n_guesses && n_sols < 8; g++) {
        ce_float_t x = guesses[g / n_guesses];
        ce_float_t y = guesses[g % n_guesses];
        bool converged = false;

        s_nl_eval_failed = false;
        /* Newton's method for systems: J * delta = -F */
        for (iter = 0; iter < 50; iter++) {
            ce_float_t f1 = eval_2var(equations[0], v1, x, v2, y);
            ce_float_t f2 = eval_2var(equations[1], v1, x, v2, y);

            /* Check convergence (a failed evaluation is not a residual) */
            if (!s_nl_eval_failed && fabsf(f1) < 1e-6f && fabsf(f2) < 1e-6f) {
                converged = true;
                break;
            }

            /* Jacobian matrix */
            {
                ce_float_t j11 = partial_deriv(equations[0], v1, x, v2, y, 0);
                ce_float_t j12 = partial_deriv(equations[0], v1, x, v2, y, 1);
                ce_float_t j21 = partial_deriv(equations[1], v1, x, v2, y, 0);
                ce_float_t j22 = partial_deriv(equations[1], v1, x, v2, y, 1);
                ce_float_t det = j11 * j22 - j12 * j21;
                ce_float_t dx, dy;

                /* Abandon this start on a failed evaluation, or a
                 * singular Jacobian */
                if (s_nl_eval_failed || fabsf(det) < 1e-12f) break;

                /* Solve: [dx, dy] = J^-1 * [-f1, -f2] */
                dx = (-f1 * j22 + f2 * j12) / det;
                dy = ( f1 * j21 - f2 * j11) / det;

                x += dx;
                y += dy;

                /* Guard against divergence */
                if (!ce_isfinite(x) || !ce_isfinite(y) ||
                    fabsf(x) > 1e6f || fabsf(y) > 1e6f)
                    break;
            }
        }

        if (converged) {
            /* Round to clean values */
            ce_float_t rx = roundf(x * 1e4f) / 1e4f;
            ce_float_t ry = roundf(y * 1e4f) / 1e4f;
            bool duplicate = false;
            int s;

            /* Check for duplicate solutions */
            for (s = 0; s < n_sols; s++) {
                if (fabsf(sols_x[s] - rx) < 0.001f &&
                    fabsf(sols_y[s] - ry) < 0.001f) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                sols_x[n_sols] = rx;
                sols_y[n_sols] = ry;
                n_sols++;
            }
        }
    }

    /* Format results */
    if (n_sols == 0) {
        snprintf(result_buf, buf_size, "No solution found");
        steps_addf("Newton's method did not converge");
        return 0;
    }

    {
        int pos = 0;
        int s;
        for (s = 0; s < n_sols; s++) {
            if (s > 0)
                pos += snprintf(result_buf + pos, buf_size - pos, "; ");
            pos += snprintf(result_buf + pos, buf_size - pos,
                            "(%c, %c) = (%.6g, %.6g)",
                            v1, v2, (double)sols_x[s], (double)sols_y[s]);
            steps_addf("Solution %d: %c=%.6g, %c=%.6g",
                       s + 1, v1, (double)sols_x[s], v2, (double)sols_y[s]);
        }
    }

    return n_sols;
}

/* ═══════════════════════════════════════════════════════════ */
/*          ABSOLUTE VALUE EQUATION SOLVER                      */
/* ═══════════════════════════════════════════════════════════ */

/* Check if expression is |f(x)| (top-level abs node) */
static const ast_node_t *extract_abs_inner(const ast_node_t *expr)
{
    if (!expr) return NULL;
    if (expr->type == NODE_FUNC && expr->d.func.id == FN_ABS)
        return expr->d.func.arg;
    return NULL;
}

/* Solve |f(x)| = c where c is a constant.
 * Strategy: |f(x)| = c splits into f(x) = c and f(x) = -c (if c > 0).
 * More generally, for |f(x)| = g(x), we solve f(x) = g(x) and f(x) = -g(x),
 * then verify each candidate.
 *
 * For the common case |f(x)| <op> 0: if the expression is |f(x)| - c = 0,
 * we detect |f(x)| on the left side of a subtraction. */
int algebra_solve_abs(const ast_node_t *expr, char var,
                      char *result_buf, int buf_size)
{
    const ast_node_t *abs_inner = NULL;
    const ast_node_t *rhs_node = NULL;
    ce_float_t rhs_val = 0;
    char buf[60];
    int pos = 0;
    int n_solutions = 0;

    poly_too_deep = false;
    if (!var) var = find_var(expr);

    /* Pattern: |f(x)| - c = 0  (i.e. |f| = c) */
    if (expr && expr->type == NODE_BINOP && expr->d.binop.op == OP_SUB) {
        abs_inner = extract_abs_inner(expr->d.binop.left);
        rhs_node = expr->d.binop.right;
    }
    /* Pattern: |f(x)| + c = 0  (i.e. |f| = -c) */
    if (!abs_inner && expr && expr->type == NODE_BINOP &&
        expr->d.binop.op == OP_ADD) {
        abs_inner = extract_abs_inner(expr->d.binop.left);
        if (abs_inner && expr->d.binop.right->type == NODE_NUMBER) {
            /* |f| + c = 0 means |f| = -c, only solvable if c <= 0 */
            rhs_val = -expr->d.binop.right->d.number;
            rhs_node = NULL;  /* use rhs_val directly */
        } else {
            abs_inner = NULL;
        }
    }
    /* Pattern: |f(x)| = 0 (standalone abs) */
    if (!abs_inner) {
        abs_inner = extract_abs_inner(expr);
        if (abs_inner) {
            rhs_val = 0;
            rhs_node = NULL;
        }
    }

    if (!abs_inner) {
        snprintf(result_buf, buf_size, "No absolute value detected");
        return 0;
    }

    /* Record the step only once an |f(x)| was actually found: the solver
     * tries this path first for every equation, so logging earlier put
     * "Solve abs equation" in front of plain linear/quadratic solves. */
    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Solve abs equation: %s = 0", buf);

    /* Determine the RHS value */
    if (rhs_node) {
        eval_ctx_t ctx;
        eval_init(&ctx);
        rhs_val = eval_expr(&ctx, rhs_node);
        if (eval_error(&ctx) != CE_OK) {
            snprintf(result_buf, buf_size, "Cannot evaluate RHS");
            return 0;
        }
    }

    steps_addf("|f(%c)| = %.6g", var, (double)rhs_val);

    if (rhs_val < 0) {
        snprintf(result_buf, buf_size, "No solution (|f| = negative)");
        steps_addf("|f(%c)| cannot equal a negative number", var);
        return 0;
    }

    /* Case 1: f(x) = rhs_val */
    {
        poly_t p;
        if (ast_to_poly(abs_inner, var, &p)) {
            /* Linear case: a*x + b = rhs_val → a*x + (b - rhs_val) = 0 */
            poly_t p1 = p;
            p1.coeff[0] -= rhs_val;

            steps_addf("Case 1: f(%c) = %.6g", var, (double)rhs_val);

            if (p1.degree == 1 && fabsf(p1.coeff[1]) > 1e-10f) {
                ce_float_t sol = -p1.coeff[0] / p1.coeff[1];
                steps_addf("%c = %.6g", var, (double)sol);
                pos += snprintf(result_buf + pos, buf_size - pos,
                                "%c = %.6g", var, (double)sol);
                n_solutions++;
            } else if (p1.degree == 2) {
                ce_float_t a = p1.coeff[2], b = p1.coeff[1], c = p1.coeff[0];
                ce_float_t disc = b * b - 4 * a * c;
                if (disc >= 0) {
                    ce_float_t sd = sqrtf(disc);
                    ce_float_t r1 = (-b + sd) / (2 * a);
                    ce_float_t r2 = (-b - sd) / (2 * a);
                    steps_addf("%c = %.6g", var, (double)r1);
                    pos += snprintf(result_buf + pos, buf_size - pos,
                                    "%c = %.6g", var, (double)r1);
                    n_solutions++;
                    if (fabsf(r1 - r2) > 1e-6f) {
                        steps_addf("%c = %.6g", var, (double)r2);
                        pos += snprintf(result_buf + pos, buf_size - pos,
                                        ", %c = %.6g", var, (double)r2);
                        n_solutions++;
                    }
                }
            }

            /* Case 2: f(x) = -rhs_val (only if rhs_val > 0) */
            if (rhs_val > 1e-10f) {
                poly_t p2 = p;
                p2.coeff[0] += rhs_val;

                steps_addf("Case 2: f(%c) = %.6g", var, (double)(-rhs_val));

                if (p2.degree == 1 && fabsf(p2.coeff[1]) > 1e-10f) {
                    ce_float_t sol = -p2.coeff[0] / p2.coeff[1];
                    steps_addf("%c = %.6g", var, (double)sol);
                    if (n_solutions > 0)
                        pos += snprintf(result_buf + pos, buf_size - pos, ", ");
                    pos += snprintf(result_buf + pos, buf_size - pos,
                                    "%c = %.6g", var, (double)sol);
                    n_solutions++;
                } else if (p2.degree == 2) {
                    ce_float_t a = p2.coeff[2], b = p2.coeff[1], c = p2.coeff[0];
                    ce_float_t disc = b * b - 4 * a * c;
                    if (disc >= 0) {
                        ce_float_t sd = sqrtf(disc);
                        ce_float_t r1 = (-b + sd) / (2 * a);
                        ce_float_t r2 = (-b - sd) / (2 * a);
                        if (n_solutions > 0)
                            pos += snprintf(result_buf + pos, buf_size - pos, ", ");
                        pos += snprintf(result_buf + pos, buf_size - pos,
                                        "%c = %.6g", var, (double)r1);
                        steps_addf("%c = %.6g", var, (double)r1);
                        n_solutions++;
                        if (fabsf(r1 - r2) > 1e-6f) {
                            pos += snprintf(result_buf + pos, buf_size - pos,
                                            ", %c = %.6g", var, (double)r2);
                            steps_addf("%c = %.6g", var, (double)r2);
                            n_solutions++;
                        }
                    }
                }
            }
        }
    }

    if (poly_too_deep && n_solutions == 0) {
        snprintf(result_buf, buf_size, "Expression too deep for solve");
        return -1;
    }
    if (n_solutions == 0) {
        snprintf(result_buf, buf_size, "No solution");
        steps_addf("No solutions found");
    }

    return n_solutions;
}
