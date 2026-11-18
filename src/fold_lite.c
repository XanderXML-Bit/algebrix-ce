/*
 * fold_lite.c — small exact constant fold for CE integration workers.
 *
 * See fold_lite.h.  Folding is bottom-up and integer-only: a constant is a
 * NUMBER node holding an exact integer with |v| <= 2047.  The bound keeps
 * every product inside the eZ80's 24-bit int (32-bit long code is several
 * times larger).  A division folds only when it is exact, so 2/3 stays 2/3,
 * and a value that is not a small exact integer (1.5, 1e9, NaN) is never
 * treated as a constant: nothing is rounded.
 *
 * Every rewrite reuses or frees existing nodes and nothing is allocated, so
 * the pass cannot run the pool dry.
 */
#include <stddef.h>

#include "calcengine.h"
#include "fold_lite.h"
#include "memory.h"

#define FL_MAX 2047

/* Each nesting level costs one fl_fold frame; deeper subtrees are returned
 * unfolded, which is still a correct expression. */
#ifndef FOLD_LITE_MAX_DEPTH
#define FOLD_LITE_MAX_DEPTH 40
#endif
#if FOLD_LITE_MAX_DEPTH > 255
#error "FOLD_LITE_MAX_DEPTH must fit fl_depth (uint8_t)"
#endif

/* a+(-b) -> a-b, a-(-b) -> a+b, (-a)+b -> b-a.  Cosmetic; about 170 B on
 * eZ80 (168 B in CECISM), so a module at the size ceiling may build with 0. */
#ifndef FOLD_LITE_SIGN_RULES
#define FOLD_LITE_SIGN_RULES 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FL_NOINLINE __attribute__((noinline))
#else
#define FL_NOINLINE
#endif

static FL_NOINLINE bool fl_num(const ast_node_t *n, int *out)
{
    ce_float_t v;
    int i;

    if (n->type != NODE_NUMBER) return false;
    v = n->d.number;
    /* Written so NaN fails both comparisons (CEdev isnan() is unreliable). */
    if (!(v >= (ce_float_t)-FL_MAX && v <= (ce_float_t)FL_MAX)) return false;
    i = (int)v;
    if ((ce_float_t)i != v) return false;
    *out = i;
    return true;
}

static FL_NOINLINE void fl_put(ast_node_t *num, int v)
{
    num->d.number = (ce_float_t)v;
}

/* Turn binop `n` into the integer constant v. */
static FL_NOINLINE ast_node_t *fl_set(ast_node_t *n, int v)
{
    ast_free(n->d.binop.left);
    ast_free(n->d.binop.right);
    n->type = NODE_NUMBER;
    fl_put(n, v);
    return n;
}

/* Replace binop `n` by its operand `keep`; `drop` is the other operand. */
static FL_NOINLINE ast_node_t *fl_keep(ast_node_t *n, ast_node_t *keep, ast_node_t *drop)
{
    ast_free(drop);
    mem_free_node(n);
    return keep;
}

static bool fl_is_neg(const ast_node_t *n)
{
    return n->type == NODE_UNOP && n->d.unop.op == UOP_NEG;
}

static FL_NOINLINE ast_node_t *fl_unop(ast_node_t *n)
{
    ast_node_t *c = n->d.unop.operand;

    if (n->d.unop.op != UOP_NEG) return n;
    if (c->type == NODE_NUMBER) {
        /* 0 - v, not -v, so -(0) gives 0 rather than -0. */
        c->d.number = (ce_float_t)0 - c->d.number;
    } else if (fl_is_neg(c)) {                   /* -(-E) -> E */
        ast_node_t *inner = c->d.unop.operand;
        mem_free_node(c);
        c = inner;
    } else {
        return n;
    }
    mem_free_node(n);
    return c;
}

/* Reuse binop `n` as -(e); `num` is its other operand, a NUMBER leaf. */
static ast_node_t *fl_negate(ast_node_t *n, ast_node_t *e, ast_node_t *num)
{
    mem_free_node(num);
    n->type = NODE_UNOP;
    n->d.unop.op = UOP_NEG;
    n->d.unop.operand = e;
    return fl_unop(n);
}

static FL_NOINLINE ast_node_t *fl_binop(ast_node_t *n)
{
    ast_node_t *l = n->d.binop.left;
    ast_node_t *r = n->d.binop.right;
    binop_t op = n->d.binop.op;
    int a, b, k, v;
    bool la = fl_num(l, &a);
    bool rb = fl_num(r, &b);

    if (la && rb && op != OP_POW) {
        /* |a|, |b| <= 2047, so a*b fits in 24 bits. */
        if (op == OP_ADD) {
            v = a + b;
        } else if (op == OP_SUB) {
            v = a - b;
        } else if (op == OP_MUL) {
            v = a * b;
        } else if (b != 0 && a % b == 0) {
            v = a / b;
        } else {
            v = FL_MAX + 1;
        }
        if (v >= -FL_MAX && v <= FL_MAX) return fl_set(n, v);
    }

    /* E+0, E-0, E*1, E/1, E^1 -> E */
    if (rb && (op <= OP_SUB ? b == 0 : b == 1)) return fl_keep(n, l, r);
    if (op <= OP_SUB) {
        if (op == OP_ADD && la && a == 0) return fl_keep(n, r, l);  /* 0+E */
#if FOLD_LITE_SIGN_RULES
        if (op == OP_ADD && fl_is_neg(l) && !fl_is_neg(r)) {
            n->d.binop.left = r;                 /* (-a) + b -> b + (-a) */
            n->d.binop.right = l;
            r = l;
        }
        if (fl_is_neg(r)) {                      /* a + (-b) -> a - b, */
            n->d.binop.op = (binop_t)(OP_ADD + OP_SUB - op); /* a - (-b) -> a + b */
            n->d.binop.right = r->d.unop.operand;
            mem_free_node(r);
        }
#endif
        return n;
    }
    if (op != OP_MUL) return n;
    /* 0*E, E*0 -> 0 */
    if ((la && a == 0) || (rb && b == 0)) return fl_set(n, 0);
    if (!la) return n;
    /* c*(E/k) -> (c/k)*E while k divides c, e.g. 3*((x^3)/3) -> x^3.  A
     * loop, not recursion: a subtree past the depth cap may still hold a
     * long chain of divisions. */
    while (r->type == NODE_BINOP && r->d.binop.op == OP_DIV &&
           fl_num(r->d.binop.right, &k) && k != 0 && a % k == 0) {
        a /= k;
        fl_put(l, a);
        r = fl_keep(r, r->d.binop.left, r->d.binop.right);
        n->d.binop.right = r;
    }
    if (a == 1) return fl_keep(n, r, l);                        /* 1*E */
    if (a == -1) return fl_negate(n, r, l);                     /* -1*E */
    return n;
}

/* Nesting depth of fl_fold; a counter instead of a parameter is smaller on
 * eZ80.  fl_fold is the only recursion in this file. */
static uint8_t fl_depth;

static ast_node_t *fl_fold(ast_node_t *n)
{
    if (fl_depth >= FOLD_LITE_MAX_DEPTH) return n;
    fl_depth++;
    if (n->type == NODE_BINOP) {
        n->d.binop.left = fl_fold(n->d.binop.left);
        n->d.binop.right = fl_fold(n->d.binop.right);
        n = fl_binop(n);
    } else if (n->type == NODE_UNOP) {
        n->d.unop.operand = fl_fold(n->d.unop.operand);
        n = fl_unop(n);
    } else if (n->type == NODE_FUNC) {
        n->d.func.arg = fl_fold(n->d.func.arg);
    }
    fl_depth--;
    return n;
}

ast_node_t *ast_fold_lite(ast_node_t *node)
{
    if (!node) return NULL;
    fl_depth = 0;
    return fl_fold(node);
}
