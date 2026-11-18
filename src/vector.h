/*
 * vector.h — Vector calculus operations
 *
 * Supports: dot product, cross product, gradient, divergence, curl.
 * Vectors are passed as PAIR chains of AST nodes (3 components max).
 * All symbolic operations use deriv_diff internally.
 */
#ifndef VECTOR_H
#define VECTOR_H

#include "calcengine.h"
#include "ast.h"
#include "eval.h"

/*
 * Dot product of two 3-component vectors (numerical).
 * v1, v2: NULL-terminated arrays of evaluated float components.
 * n: number of components (2 or 3).
 */
ce_float_t vector_dot(const ce_float_t *v1, const ce_float_t *v2, int n);

/*
 * Cross product of two 3-component vectors (numerical).
 * Result stored in out[3].
 */
void vector_cross(const ce_float_t *a, const ce_float_t *b, ce_float_t *out);

/*
 * Gradient of scalar field f(x,y) or f(x,y,z) — symbolic.
 * vars[]: variable names (e.g. {'x','y','z'})
 * n: number of variables (2 or 3)
 * Writes symbolic gradient components to result_buf.
 * Returns true on success.
 */
bool vector_gradient(const ast_node_t *f, const char *vars, int n,
                     char *result_buf, int buf_size);

/*
 * Divergence of F = (F1, F2[, F3]) — symbolic.
 * fields[]: array of n AST expressions for each component
 * vars[]:   variable names
 * n:        2 or 3
 * Writes div(F) = ∂F1/∂x + ∂F2/∂y [+ ∂F3/∂z] to result_buf.
 */
bool vector_divergence(const ast_node_t *const *fields,
                       const char *vars, int n,
                       char *result_buf, int buf_size);

/*
 * Curl of F = (F1, F2, F3) — symbolic (3D only).
 * Writes curl components to result_buf.
 */
bool vector_curl(const ast_node_t *F1, const ast_node_t *F2,
                 const ast_node_t *F3,
                 char xv, char yv, char zv,
                 char *result_buf, int buf_size);

/*
 * Evaluate PAIR-chain of numeric values into a flat float array.
 * Returns number of values stored (up to max_n).
 */
int vector_eval_pair_chain(const ast_node_t *pair_chain,
                           ce_float_t *out, int max_n,
                           eval_ctx_t *ctx);

/*
 * Evaluate PAIR-chain of AST expressions into an array of ast_node_t*.
 * Caller must ast_free each returned node.
 * Returns count of nodes stored (up to max_n).
 */
int vector_collect_pair_exprs(const ast_node_t *pair_chain,
                               const ast_node_t **out, int max_n);

#endif /* VECTOR_H */
