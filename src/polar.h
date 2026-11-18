/*
 * polar.h -- Polar, parametric & advanced calculus operations
 *
 * Phase 7: Polar/rect conversion, arc length, surface area of
 * revolution, and volume methods (disk/washer/shell).
 *
 * polar_op_t is defined in ast.h (shared with the AST node union).
 */
#ifndef POLAR_H
#define POLAR_H

#include "calcengine.h"

/* ── Polar <-> rectangular conversion ──────────────────────── */
void polar_to_rect(ce_float_t r, ce_float_t theta,
                   ce_float_t *x, ce_float_t *y);
void rect_to_polar(ce_float_t x, ce_float_t y,
                   ce_float_t *r, ce_float_t *theta);

/*
 * Polar area: (1/2) * integral_a^b r(theta)^2 dtheta
 * Uses Simpson's rule (N=200).
 */
ce_float_t polar_area(const ast_node_t *r_expr, char var,
                      ce_float_t a, ce_float_t b,
                      ce_error_t *err);

/*
 * Arc length (polar): integral_a^b sqrt(r^2 + [dr/dtheta]^2) dtheta
 * Uses symbolic differentiation + Simpson's rule.
 */
ce_float_t polar_arclen_polar(const ast_node_t *r_expr, char var,
                              ce_float_t a, ce_float_t b,
                              ce_error_t *err);

/*
 * Arc length (Cartesian): integral_a^b sqrt(1 + [f'(x)]^2) dx
 * Uses symbolic differentiation + Simpson's rule.
 */
ce_float_t polar_arclen(const ast_node_t *f, char var,
                        ce_float_t a, ce_float_t b,
                        ce_error_t *err);

/*
 * Arc length (parametric): integral_a^b sqrt([dx/dt]^2 + [dy/dt]^2) dt
 */
ce_float_t polar_arclen_param(const ast_node_t *xt, const ast_node_t *yt,
                              char var, ce_float_t a, ce_float_t b,
                              ce_error_t *err);

/*
 * Surface area of revolution about x-axis:
 * 2*pi * integral_a^b |f(x)| * sqrt(1 + [f'(x)]^2) dx
 */
ce_float_t polar_surfrev(const ast_node_t *f, char var,
                         ce_float_t a, ce_float_t b,
                         ce_error_t *err);

/*
 * Disk method volume: pi * integral_a^b [f(x)]^2 dx
 */
ce_float_t polar_disk(const ast_node_t *f, char var,
                      ce_float_t a, ce_float_t b,
                      ce_error_t *err);

/*
 * Washer method volume: pi * integral_a^b ([R(x)]^2 - [r(x)]^2) dx
 */
ce_float_t polar_washer(const ast_node_t *R, const ast_node_t *r,
                        char var, ce_float_t a, ce_float_t b,
                        ce_error_t *err);

/*
 * Shell method volume: 2*pi * integral_a^b x * f(x) dx
 */
ce_float_t polar_shell(const ast_node_t *f, char var,
                       ce_float_t a, ce_float_t b,
                       ce_error_t *err);

#endif /* POLAR_H */
