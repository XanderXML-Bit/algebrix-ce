/*
 * deriv.h — Symbolic differentiation engine
 */
#ifndef DERIV_H
#define DERIV_H

#include "calcengine.h"
#include "ast.h"

/*
 * Symbolically differentiate expr with respect to var.
 * Returns a NEW simplified AST (caller must ast_free it).
 * Records steps via the steps module.
 * Returns NULL on error.
 */
ast_node_t *deriv_diff(const ast_node_t *expr, char var);

/*
 * Differentiate without clearing/adding to the step buffer.
 * Used by the limits engine (L'Hopital) to avoid clobbering steps.
 */
ast_node_t *deriv_diff_quiet(const ast_node_t *expr, char var);

/*
 * Compute the nth derivative of expr with respect to var.
 * Applies differentiation n times with step-by-step recording.
 * Returns a NEW simplified AST (caller must ast_free it).
 * Returns NULL if any intermediate derivative fails.
 */
ast_node_t *deriv_nth(const ast_node_t *expr, char var, int n);

/*
 * Implicit differentiation: compute dy/dx for an equation f(x,y)=0.
 * Treats var_y as a function of var_x.
 * Returns AST representing dy/dx = -f_x / f_y.
 * Returns NULL if f_y is zero or differentiation fails.
 */
ast_node_t *deriv_implicit(const ast_node_t *expr, char var_x, char var_y);

/*
 * Logarithmic differentiation for f(x)^g(x) expressions.
 * Shows step-by-step: y = f^g → ln(y) = g*ln(f) → differentiate.
 * Falls back to deriv_diff() if only one side depends on var.
 * Returns a NEW simplified AST (caller must ast_free it).
 */
ast_node_t *deriv_log_diff(const ast_node_t *expr, char var);

/*
 * Derivative from the limit definition:
 *   f'(x) = lim[h→0] (f(x+h) - f(x)) / h
 * Shows pedagogical steps. Computes via standard rules internally.
 * Returns a NEW simplified AST (caller must ast_free it).
 */
ast_node_t *deriv_from_definition(const ast_node_t *expr, char var);

#endif /* DERIV_H */
