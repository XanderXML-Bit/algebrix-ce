/*
 * disambig.c — Smart disambiguation for ambiguous expressions
 *
 * Analyses an AST to decide whether the user should be prompted to
 * choose between genuinely different mathematical operations.
 *
 * Decision criteria:
 *   - Polynomial with variable(s) → factor / roots / expand / simplify / eval
 *     (all yield different forms / answers)
 *   - Rational function (poly ÷ poly with variable) → pfrac / simplify / eval
 *   - Expandable expression (product/power of sums) → expand / eval
 *   - Explicit operation node (NODE_INTEGRAL, NODE_DERIV, …) → never ambiguous
 *   - Pure numeric → never ambiguous
 */
#include "disambig.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Internal helpers ────────────────────────────────────────── */

/* Recursion depth limit matching the parser cap (eZ80 ~4KB stack guard). */
#define DISAMBIG_MAX_DEPTH 50

/* True if the subtree contains no variable references */
static bool is_pure_numeric(const ast_node_t *node)
{
    if (!node) return true;
    switch (node->type) {
    case NODE_NUMBER:
    case NODE_CONSTANT:
        return true;
    case NODE_UNOP:
        return is_pure_numeric(node->d.unop.operand);
    case NODE_BINOP:
        return is_pure_numeric(node->d.binop.left) &&
               is_pure_numeric(node->d.binop.right);
    case NODE_FUNC:
        return is_pure_numeric(node->d.func.arg);
    case NODE_VARIABLE:
    default:
        return false;
    }
}

/*
 * True if node is a polynomial expression in `var`.
 * Allows: +, -, *, -(unary), var^n (n ≥ 0 integer), numeric sub-trees.
 * Does NOT allow: /, trig/log/exp functions, other variables as exponents.
 */
static bool is_poly_like_r(const ast_node_t *node, char var, int depth)
{
    ce_float_t exp_val;

    if (!node) return false;
    if (depth >= DISAMBIG_MAX_DEPTH) return false;  /* eZ80 stack guard */

    /* Sub-tree with no var → numeric coefficient, always OK */
    if (!ast_contains_var(node, var)) return true;

    switch (node->type) {
    case NODE_VARIABLE:
        return (node->d.variable == var);

    case NODE_UNOP:
        if (node->d.unop.op == UOP_NEG)
            return is_poly_like_r(node->d.unop.operand, var, depth + 1);
        return false;

    case NODE_BINOP:
        switch (node->d.binop.op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
            return is_poly_like_r(node->d.binop.left,  var, depth + 1) &&
                   is_poly_like_r(node->d.binop.right, var, depth + 1);
        case OP_POW:
            /* base must be poly-like; exponent must be non-negative integer */
            if (node->d.binop.right->type != NODE_NUMBER) return false;
            exp_val = node->d.binop.right->d.number;
            if (exp_val < 0.0f || exp_val != (float)(int)exp_val) return false;
            return is_poly_like_r(node->d.binop.left, var, depth + 1);
        case OP_DIV:
            /* Division → not a pure polynomial */
            return false;
        default:
            return false;
        }

    case NODE_FUNC:
        /* Functions applied to var are not polynomial */
        return false;

    default:
        return false;
    }
}

static bool is_poly_like(const ast_node_t *node, char var)
{
    return is_poly_like_r(node, var, 0);
}

/*
 * True if the tree is a rational function: a division where at least one
 * side contains `var`.  The numerator and denominator need not each be
 * polynomial — we just need the top-level DIV with a variable somewhere.
 */
static bool is_rational_func(const ast_node_t *node, char var)
{
    if (!node) return false;
    return (node->type == NODE_BINOP &&
            node->d.binop.op == OP_DIV &&
            ast_contains_var(node, var));
}

/*
 * True if the expression has a structure that would produce a meaningfully
 * different result after expand():
 *   - product of two sums:  (a+b)*(c+d)
 *   - a sum raised to an integer power:  (a+b)^n  (n ≥ 2)
 */
static bool is_expandable(const ast_node_t *node)
{
    bool left_sum, right_sum;
    ce_float_t exp_val;

    if (!node) return false;

    if (node->type == NODE_BINOP) {
        if (node->d.binop.op == OP_MUL) {
            left_sum  = (node->d.binop.left->type == NODE_BINOP &&
                         (node->d.binop.left->d.binop.op  == OP_ADD ||
                          node->d.binop.left->d.binop.op  == OP_SUB));
            right_sum = (node->d.binop.right->type == NODE_BINOP &&
                         (node->d.binop.right->d.binop.op == OP_ADD ||
                          node->d.binop.right->d.binop.op == OP_SUB));
            return left_sum || right_sum;
        }
        if (node->d.binop.op == OP_POW &&
            node->d.binop.right->type == NODE_NUMBER) {
            exp_val = node->d.binop.right->d.number;
            if (exp_val >= 2.0f && exp_val == (float)(int)exp_val) {
                /* (sum)^n */
                return (node->d.binop.left->type == NODE_BINOP &&
                        (node->d.binop.left->d.binop.op == OP_ADD ||
                         node->d.binop.left->d.binop.op == OP_SUB));
            }
        }
    }
    return false;
}

/* Estimate polynomial degree (max power of var in the expression) */
static int poly_degree_r(const ast_node_t *node, char var, int depth)
{
    int dl, dr;
    ce_float_t exp_val;

    if (!node) return 0;
    if (!ast_contains_var(node, var)) return 0;
    if (depth >= DISAMBIG_MAX_DEPTH) return 0;  /* eZ80 stack guard */

    switch (node->type) {
    case NODE_VARIABLE:
        return (node->d.variable == var) ? 1 : 0;
    case NODE_UNOP:
        return poly_degree_r(node->d.unop.operand, var, depth + 1);
    case NODE_BINOP:
        switch (node->d.binop.op) {
        case OP_ADD:
        case OP_SUB: {
            dl = poly_degree_r(node->d.binop.left,  var, depth + 1);
            dr = poly_degree_r(node->d.binop.right, var, depth + 1);
            return dl > dr ? dl : dr;
        }
        case OP_MUL:
            dl = poly_degree_r(node->d.binop.left,  var, depth + 1);
            dr = poly_degree_r(node->d.binop.right, var, depth + 1);
            return dl + dr;
        case OP_POW:
            if (node->d.binop.right->type == NODE_NUMBER) {
                exp_val = node->d.binop.right->d.number;
                dl = poly_degree_r(node->d.binop.left, var, depth + 1);
                return dl * (int)exp_val;
            }
            return 0;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

static int poly_degree(const ast_node_t *node, char var)
{
    return poly_degree_r(node, var, 0);
}

/* ── Public API ──────────────────────────────────────────────── */

int ce_disambig_detect(const ast_node_t     *root,
                       ce_disambig_option_t *opts,
                       int                   max_opts,
                       bool                  show_method_choices)
{
    int   n = 0;
    char  var = 0;
    int   deg;
    bool  poly, rational, expandable;
    /* Variable search order: x, y, z, t, n, k, u, v, a, b, c, m, s, w */
    static const char var_order[] = "xyztnkuvabcmswpqrfghjl";
    int i;

    if (!root || !opts || max_opts <= 0) return 0;

    /* Explicit operation nodes are never ambiguous */
    switch (root->type) {
    case NODE_DERIV:
    case NODE_INTEGRAL:
    case NODE_LIMIT:
    case NODE_SUM:
    case NODE_TAYLOR:
    case NODE_FOURIER:
    case NODE_EXPAND:
    case NODE_FACTOR:
    case NODE_SOLVE:
    case NODE_STAT:
    case NODE_POLAR:
    case NODE_MATRIX_OP:
    case NODE_MATRIX:
    case NODE_PRODUCT:
    case NODE_TANGENT:
    case NODE_LIST_OP:
    case NODE_LIST_LITERAL:
    case NODE_STORE:
    case NODE_PIECEWISE:
        return 0;
    default:
        break;
    }

    /* Pure numeric: just evaluate, no choice needed */
    if (is_pure_numeric(root)) return 0;

    /* Find first variable in the preferred order */
    for (i = 0; var_order[i] && !var; i++) {
        if (ast_contains_var(root, var_order[i]))
            var = var_order[i];
    }

    if (!var) return 0;   /* no variable → just eval */

    /* Classify the expression */
    rational   = is_rational_func(root, var);
    poly       = !rational && is_poly_like(root, var);
    expandable = is_expandable(root);
    deg        = poly ? poly_degree(root, var) : 0;

    /* ── Rational function ────────────────────────────────────
     * Different outcomes: pfrac (decomposes), simplify (cancels),
     * or numeric evaluation.  These can all yield different forms.    */
    if (rational) {
        if (n < max_opts) { opts[n].label = "Evaluate";           opts[n].action = DISAMBIG_EVAL;     n++; }
        if (n < max_opts) { opts[n].label = "Partial Fractions";  opts[n].action = DISAMBIG_PFRAC;    n++; }
        if (n < max_opts) { opts[n].label = "Simplify";           opts[n].action = DISAMBIG_SIMPLIFY; n++; }
        return n;
    }

    /* ── Polynomial of degree ≥ 2 ─────────────────────────────
     * Different outcomes: factor vs roots vs simplify vs expand vs eval.
     * Only show the dialog if at least 2 operations are distinct.     */
    if (poly && deg >= 2) {
        if (n < max_opts) { opts[n].label = "Evaluate";    opts[n].action = DISAMBIG_EVAL;     n++; }
        if (n < max_opts) { opts[n].label = "Factor";      opts[n].action = DISAMBIG_FACTOR;   n++; }
        if (n < max_opts) { opts[n].label = "Find Roots";  opts[n].action = DISAMBIG_ROOTS;    n++; }
        if (expandable && n < max_opts) {
            opts[n].label = "Expand";
            opts[n].action = DISAMBIG_EXPAND;
            n++;
        }
        if (n < max_opts) { opts[n].label = "Simplify";   opts[n].action = DISAMBIG_SIMPLIFY; n++; }
        return n;
    }

    /* ── Polynomial of degree 1 (linear) ─────────────────────
     * Factor and expand are trivial; just offer solve vs eval.        */
    if (poly && deg == 1) {
        if (n < max_opts) { opts[n].label = "Evaluate";   opts[n].action = DISAMBIG_EVAL;     n++; }
        if (n < max_opts) { opts[n].label = "Solve (=0)"; opts[n].action = DISAMBIG_ROOTS;    n++; }
        return n;
    }

    /* ── Expandable expression that isn't purely polynomial ──
     * E.g. (sin(x)+1)*(sin(x)-1) — expand gives a different form.    */
    if (expandable) {
        if (n < max_opts) { opts[n].label = "Evaluate"; opts[n].action = DISAMBIG_EVAL;     n++; }
        if (n < max_opts) { opts[n].label = "Expand";   opts[n].action = DISAMBIG_EXPAND;   n++; }
        if (n < max_opts) { opts[n].label = "Simplify"; opts[n].action = DISAMBIG_SIMPLIFY; n++; }
        return n;
    }

    /* ── show_method_choices: surface eval vs simplify even for plain
     * expressions that otherwise would just be evaluated.              */
    if (show_method_choices) {
        if (n < max_opts) { opts[n].label = "Evaluate"; opts[n].action = DISAMBIG_EVAL;     n++; }
        if (n < max_opts) { opts[n].label = "Simplify"; opts[n].action = DISAMBIG_SIMPLIFY; n++; }
        return n;
    }

    /* Unambiguous — just evaluate */
    return 0;
}

bool ce_disambig_build_expr(const char           *input_buf,
                             char                  var,
                             ce_disambig_action_t  action,
                             char                 *out_buf,
                             int                   out_size)
{
    if (!input_buf || !out_buf || out_size <= 0) return false;

    switch (action) {
    case DISAMBIG_EVAL:
    case DISAMBIG_SIMPLIFY:
        /* Simplify and eval use the same tokenize/parse/eval path;
         * the simplifier runs automatically during evaluation.        */
        snprintf(out_buf, out_size, "%s", input_buf);
        return true;

    case DISAMBIG_FACTOR:
        snprintf(out_buf, out_size, "factor(%s)", input_buf);
        return true;

    case DISAMBIG_ROOTS:
        if (var)
            snprintf(out_buf, out_size, "solve(%s,%c)", input_buf, var);
        else
            snprintf(out_buf, out_size, "solve(%s,x)", input_buf);
        return true;

    case DISAMBIG_PFRAC:
        if (var)
            snprintf(out_buf, out_size, "pfrac(%s,%c)", input_buf, var);
        else
            snprintf(out_buf, out_size, "pfrac(%s,x)", input_buf);
        return true;

    case DISAMBIG_EXPAND:
        snprintf(out_buf, out_size, "expand(%s)", input_buf);
        return true;
    }

    return false;
}
