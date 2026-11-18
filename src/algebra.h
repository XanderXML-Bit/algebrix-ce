/*
 * algebra.h -- Algebra & trigonometry engine
 *
 * Provides: expand, factor, solve, polynomial division,
 * trig identity simplification, systems of equations,
 * triangle solver (law of sines/cosines).
 */
#ifndef ALGEBRA_H
#define ALGEBRA_H

#include "calcengine.h"
#include "ast.h"

/* Stack-tight workers select a measured bound in their makefile. Left
 * arithmetic spines are iterative; other shapes use bounded recursion. */
#ifndef CE_POLY_MAX_DEPTH
#define CE_POLY_MAX_DEPTH 64
#endif

/* Maximum polynomial degree */
#define MAX_POLY_DEG 10

/* Polynomial: coeff[0] + coeff[1]*x + ... + coeff[degree]*x^degree */
typedef struct {
    ce_float_t coeff[MAX_POLY_DEG + 1];
    int        degree;
} poly_t;

/* Convert AST to polynomial representation (returns false if not polynomial) */
bool algebra_ast_to_poly(const ast_node_t *expr, char var, poly_t *p);

/* Evaluate polynomial at a point */
ce_float_t algebra_poly_eval(const poly_t *p, ce_float_t x);

/* Convert polynomial back to AST */
ast_node_t *algebra_poly_to_ast(const poly_t *p, char var);

/* Polynomial division: a / b = q remainder r. Returns false on failure. */
bool algebra_poly_divide(const poly_t *a, const poly_t *b,
                          poly_t *q, poly_t *r);

/*
 * Expand an expression (distribute products, FOIL, binomial).
 * Returns a NEW simplified AST (caller must ast_free).
 * Records steps via steps module.
 */
ast_node_t *algebra_expand(const ast_node_t *expr, char var);

/*
 * Factor a polynomial expression into linear/quadratic factors.
 * Returns a NEW AST (caller must ast_free).
 * Records steps via steps module.
 */
ast_node_t *algebra_factor(const ast_node_t *expr, char var);

/*
 * Solve expr = 0 for var.
 * Writes human-readable solution(s) to result_buf.
 * Returns the number of solutions found.
 * Handles: linear, quadratic (with complex roots), higher degree
 * via rational roots, trig equations, Newton fallback.
 */
int algebra_solve(const ast_node_t *expr, char var,
                  char *result_buf, int buf_size);

/*
 * Polynomial long division: dividend / divisor.
 * Returns quotient AST.  Writes remainder string to rem_buf.
 * Returns NULL if divisor is zero or inputs aren't polynomial.
 */
ast_node_t *algebra_poly_div(const ast_node_t *dividend,
                              const ast_node_t *divisor,
                              char var,
                              char *rem_buf, int rem_size);

/*
 * Apply trig identity simplifications (sin^2+cos^2=1, etc.).
 * Modifies expr in place (like ast_simplify).  Returns modified tree.
 */
ast_node_t *algebra_trig_simplify(ast_node_t *expr);

/*
 * Solve a system of N linear equations (N=2 or 3) via Cramer's Rule.
 *   equations: array of AST expressions, each representing expr=0
 *   n_eqs:     number of equations (2 or 3)
 *   vars:      variable names, e.g. "xy" or "xyz"
 *   result_buf: output buffer for human-readable solution
 *   buf_size:   size of result_buf
 * Returns: number of variables solved (0 = singular/failed).
 */
int algebra_solve_system(const ast_node_t **equations, int n_eqs,
                         const char *vars, char *result_buf, int buf_size);

/*
 * Partial fraction decomposition of a rational expression.
 * Writes the decomposition as a string to result_buf.
 * Returns the decomposed AST, or NULL if not applicable.
 * Handles: c/(x*(x+a)), c/((x+a)(x+b)), c/(x^2-a^2), x/((x+a)(x+b)).
 */
ast_node_t *algebra_partial_fractions(const ast_node_t *expr, char var,
                                       char *result_buf, int buf_size);

/*
 * Solve a triangle given partial information (law of sines/cosines).
 *   sides[3]:  a, b, c (0 = unknown)
 *   angles[3]: A, B, C in degrees (0 = unknown)
 * Fills in unknowns in-place.
 * Returns: 1 = solved, 0 = impossible/insufficient, 2 = ambiguous SSA.
 */
int algebra_triangle(ce_float_t *sides, ce_float_t *angles,
                     char *result_buf, int buf_size);

/*
 * Complete the square: convert ax^2+bx+c to a(x+h)^2+k.
 * Writes human-readable string to result_buf.
 * Returns 1 on success, 0 if input is not quadratic.
 */
int algebra_complete_square(const ast_node_t *expr, char var,
                             char *result_buf, int buf_size);

/*
 * Descartes' Rule of Signs: count sign changes for positive/negative roots.
 * Writes summary to result_buf.
 * Returns 1 on success, 0 if not a polynomial.
 */
int algebra_descartes(const ast_node_t *expr, char var,
                       char *result_buf, int buf_size);

/*
 * Line from two points: compute y=mx+b (or x=c for vertical).
 * Returns 1 on success, 0 on error (duplicate points).
 */
int algebra_line_two_points(ce_float_t x1, ce_float_t y1,
                             ce_float_t x2, ce_float_t y2,
                             char *result_buf, int buf_size);

/*
 * Line from slope and point: compute y=mx+b.
 * Returns 1 always.
 */
int algebra_line_slope_point(ce_float_t m, ce_float_t x1, ce_float_t y1,
                              char *result_buf, int buf_size);

/*
 * Parabola from vertex (vx,vy) and another point (px,py).
 * Writes y=a(x-h)^2+k to result_buf.
 * Returns 1 on success, 0 if point is at vertex x-coordinate.
 */
int algebra_parabola_vertex(ce_float_t vx, ce_float_t vy,
                             ce_float_t px, ce_float_t py,
                             char *result_buf, int buf_size);

/*
 * Trig power-reduction expand (separate from trig_simplify).
 * sin^2(x) -> (1-cos(2x))/2, cos^2(x) -> (1+cos(2x))/2.
 * Returns modified tree.
 */
ast_node_t *algebra_trig_expand(ast_node_t *expr);

/*
 * Rational Root Theorem: list all possible rational roots p/q
 * of a polynomial expression.
 * Returns number of candidates, writes formatted list to result_buf.
 */
int algebra_rational_roots(const ast_node_t *expr, char var,
                           char *result_buf, int buf_size);

/*
 * Solve a linear or quadratic inequality: expr <sign> 0.
 *   sign: '<', '>', 'L' (<=), 'G' (>=)
 * Writes human-readable solution to result_buf.
 * Returns 1 on success, 0 on failure.
 */
int algebra_solve_inequality(const ast_node_t *expr, char var, char sign,
                              char *result_buf, int buf_size);

/*
 * Convert a decimal number to its simplest fraction representation.
 * Uses continued fraction algorithm (best rational approximation).
 * max_denom: largest denominator to try (e.g., 10000).
 * Writes "num/den" to result_buf.
 * Returns 1 on success, 0 if no good fraction found.
 */
int algebra_dec_to_frac(ce_float_t value, int max_denom,
                         char *result_buf, int buf_size);

/*
 * Unit circle reference: given angle in degrees, return exact trig values.
 * Writes "sin(d)=..., cos(d)=..., tan(d)=..." to result_buf.
 * Supports standard angles: 0, 30, 45, 60, 90, ... , 330, 360.
 * Returns 1 if angle found, 0 if not a standard angle.
 */
int algebra_unit_circle(ce_float_t angle_deg, char *result_buf, int buf_size);

/*
 * Solve a 2-equation nonlinear system via Newton's method.
 * Tries multiple initial guesses and deduplicates solutions.
 * Handles line+circle, circle+circle, parabola+line, etc.
 *   equations: array of 2 AST expressions (each = 0)
 *   vars:      2 variable names, e.g. "xy"
 *   result_buf: output buffer for human-readable solutions
 *   buf_size:   size of result_buf
 * Returns: number of solutions found (0, 1, or 2+).
 */
int algebra_solve_nonlinear(const ast_node_t **equations,
                             const char *vars,
                             char *result_buf, int buf_size);

/*
 * Solve an absolute value equation |f(x)| = g(x).
 * Splits into f(x)=g(x) and -f(x)=g(x) cases.
 * Returns: number of valid solutions found.
 */
int algebra_solve_abs(const ast_node_t *expr, char var,
                      char *result_buf, int buf_size);

#endif /* ALGEBRA_H */
