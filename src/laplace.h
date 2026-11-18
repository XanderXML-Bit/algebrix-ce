/*
 * laplace.h — Laplace transform table lookup
 *
 * Supports forward and inverse Laplace transforms for common functions:
 *   Polynomials, exponentials, trig, hyperbolic, unit step, combinations.
 * Uses linearity + table matching rather than integral evaluation.
 */
#ifndef LAPLACE_H
#define LAPLACE_H

#include "calcengine.h"
#include "ast.h"

/*
 * Compute forward Laplace transform: L{f(t)} = F(s).
 * f:       AST for f(t) (function of t_var)
 * t_var:   time variable (e.g. 't')
 * s_var:   frequency variable (e.g. 's')
 * result_buf: receives F(s) as a string
 * Returns true on success (f matched a table entry or linearity rule).
 */
bool laplace_forward(const ast_node_t *f, char t_var, char s_var,
                     char *result_buf, int buf_size);

/*
 * Compute inverse Laplace transform: L^{-1}{F(s)} = f(t).
 * F:       AST for F(s) (function of s_var)
 * s_var:   frequency variable (e.g. 's')
 * t_var:   time variable (e.g. 't')
 * result_buf: receives f(t) as a string
 * Returns true on success.
 */
bool laplace_inverse(const ast_node_t *F, char s_var, char t_var,
                     char *result_buf, int buf_size);

#endif /* LAPLACE_H */
