#include "algebra.h"

#include <math.h>
#include <string.h>

static void poly_init(poly_t *p)
{
    memset(p, 0, sizeof(*p));
}

static void poly_trim(poly_t *p)
{
    while (p->degree > 0 && fabsf(p->coeff[p->degree]) < 1e-8f) {
        p->degree--;
    }
}

static ce_float_t poly_eval(const poly_t *p, ce_float_t x)
{
    ce_float_t r = p->coeff[p->degree];
    int i;
    for (i = p->degree - 1; i >= 0; i--) {
        r = r * x + p->coeff[i];
    }
    return r;
}

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

static bool ast_to_poly_inner(const ast_node_t *e, char var, poly_t *p, int depth)
{
    poly_init(p);
    if (!e) return false;
    if (depth >= CE_POLY_MAX_DEPTH) return false;

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
        if (e->d.constant == CONST_PI) {
            p->coeff[0] = CE_PI;
            return true;
        }
        if (e->d.constant == CONST_E) {
            p->coeff[0] = CE_E;
            return true;
        }
        return false;

    case NODE_UNOP:
        if (e->d.unop.op == UOP_NEG) {
            int i;
            if (!ast_to_poly_inner(e->d.unop.operand, var, p, depth + 1)) return false;
            for (i = 0; i <= p->degree; i++) {
                p->coeff[i] = -p->coeff[i];
            }
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

static ast_node_t *poly_to_ast(const poly_t *p, char var)
{
    ast_node_t *result = NULL;
    int i;

    for (i = p->degree; i >= 0; i--) {
        ast_node_t *term;
        ce_float_t c = p->coeff[i];
        ce_float_t ac = fabsf(c);
        bool neg;

        if (ac < 1e-10f) continue;
        neg = (c < 0);

        if (i == 0) {
            term = ast_number(ac);
        } else {
            ast_node_t *xterm;
            if (i == 1) {
                xterm = ast_variable(var);
            } else {
                xterm = ast_binop(OP_POW, ast_variable(var), ast_number((ce_float_t)i));
            }
            if (fabsf(ac - 1.0f) < 1e-6f) {
                term = xterm;
            } else {
                term = ast_binop(OP_MUL, ast_number(ac), xterm);
            }
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

static bool poly_divide(const poly_t *a, const poly_t *b, poly_t *q, poly_t *r)
{
    int i, j;
    ce_float_t c;

    if (!a || !b || !q || !r) return false;

    if (a->degree < 0 || a->degree > MAX_POLY_DEG ||
        b->degree < 0 || b->degree > MAX_POLY_DEG) return false;

    if (fabsf(b->coeff[b->degree]) < 1e-20f) return false;

    poly_init(q);
    *r = *a;

    if (a->degree < b->degree) return true;
    q->degree = a->degree - b->degree;

    for (i = a->degree; i >= b->degree; i--) {
        c = r->coeff[i] / b->coeff[b->degree];
        q->coeff[i - b->degree] = c;
        for (j = 0; j <= b->degree; j++) {
            r->coeff[i - b->degree + j] -= c * b->coeff[j];
        }
    }

    r->degree = (b->degree > 0) ? b->degree - 1 : 0;
    poly_trim(r);
    poly_trim(q);
    return true;
}

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

bool algebra_poly_divide(const poly_t *a, const poly_t *b, poly_t *q, poly_t *r)
{
    return poly_divide(a, b, q, r);
}
