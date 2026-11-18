/*
 * simplify_example.c — Example: Algebraic simplification patterns
 *
 * Demonstrates the bottom-up recursive tree walk approach used throughout
 * CalcEngine for AST transformations. Key patterns:
 *   - Pattern matching on node types
 *   - Proper memory management (free nodes when removing from tree)
 *   - Static helper functions
 *   - Recursive simplification after structural changes
 *
 * THIS IS A REFERENCE EXAMPLE — see src/simplify.c for the full implementation.
 */
#include "simplify.h"
#include "memory.h"
#include <math.h>

/* ── Helpers ─────────────────────────────────────────────── */

static bool is_num(const ast_node_t *n, ce_float_t v)
{
    return n && n->type == NODE_NUMBER && n->d.number == v;
}

/* Structural equality of two AST subtrees. */
static bool ast_equal(const ast_node_t *a, const ast_node_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    switch (a->type) {
    case NODE_NUMBER:   return a->d.number == b->d.number;
    case NODE_VARIABLE: return a->d.variable == b->d.variable;
    case NODE_CONSTANT: return a->d.constant == b->d.constant;
    case NODE_BINOP:
        return a->d.binop.op == b->d.binop.op &&
               ast_equal(a->d.binop.left,  b->d.binop.left) &&
               ast_equal(a->d.binop.right, b->d.binop.right);
    case NODE_UNOP:
        return a->d.unop.op == b->d.unop.op &&
               ast_equal(a->d.unop.operand, b->d.unop.operand);
    case NODE_FUNC:
        return a->d.func.id == b->d.func.id &&
               ast_equal(a->d.func.arg, b->d.func.arg);
    default: return false;
    }
}

/* ── Simplify addition (example subset) ─────────────────── */

static ast_node_t *simp_add(ast_node_t *node)
{
    ast_node_t *L = node->d.binop.left;
    ast_node_t *R = node->d.binop.right;
    ast_node_t *r;

    /* 0 + R → R */
    if (is_num(L, 0)) { mem_free_node(L); r = R; mem_free_node(node); return r; }
    /* L + 0 → L */
    if (is_num(R, 0)) { mem_free_node(R); r = L; mem_free_node(node); return r; }

    /* Constant folding: num + num → result */
    if (L->type == NODE_NUMBER && R->type == NODE_NUMBER) {
        ce_float_t v = L->d.number + R->d.number;
        mem_free_node(L);
        mem_free_node(R);
        node->type     = NODE_NUMBER;
        node->d.number = v;
        return node;
    }

    return node;
}

/* ── Main simplification entry point ─────────────────────── */

ast_node_t *ast_simplify(ast_node_t *node)
{
    if (!node) return NULL;

    switch (node->type) {
    case NODE_BINOP:
        /* Simplify children first (bottom-up) */
        node->d.binop.left  = ast_simplify(node->d.binop.left);
        node->d.binop.right = ast_simplify(node->d.binop.right);
        /* Then simplify this node */
        if (node->d.binop.op == OP_ADD)
            return simp_add(node);
        /* ... other ops handled similarly ... */
        return node;

    case NODE_UNOP:
        node->d.unop.operand = ast_simplify(node->d.unop.operand);
        return node;

    case NODE_FUNC:
        node->d.func.arg = ast_simplify(node->d.func.arg);
        return node;

    default:
        return node;
    }
}
