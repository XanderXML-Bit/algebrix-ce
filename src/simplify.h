/*
 * simplify.h — Algebraic simplification of AST trees
 */
#ifndef SIMPLIFY_H
#define SIMPLIFY_H

#include "calcengine.h"
#include "ast.h"

/*
 * Recursively simplify an AST (bottom-up).
 * May free nodes and return a different root.
 * Rules: constant folding, identity elimination (0+x, 1*x, x^0, …).
 */
ast_node_t *ast_simplify(ast_node_t *node);

/*
 * Apply product-to-sum trig identities (top-level only):
 *   sin(a)*cos(b) → [sin(a+b) + sin(a-b)] / 2
 *   cos(a)*sin(b) → [sin(a+b) - sin(a-b)] / 2
 *   cos(a)*cos(b) → [cos(a+b) + cos(a-b)] / 2
 *   sin(a)*sin(b) → [cos(a-b) - cos(a+b)] / 2
 * Returns a NEW AST (caller must ast_free). Returns NULL on failure.
 * Note: NOT applied automatically by ast_simplify to avoid infinite loops
 * with sum-to-product identities.
 */
ast_node_t *ast_prod_to_sum(const ast_node_t *node);

#endif /* SIMPLIFY_H */
