/*
 * dsolve.h — Enhanced differential equation solver
 *
 * Provides dsolve(equation, y, x) frontend that auto-detects:
 *   - Separable ODEs: dy/dx = f(x)*g(y)
 *   - First-order linear: y' + P(x)y = Q(x)
 *   - Exact: M(x,y)dx + N(x,y)dy = 0
 * Shows step-by-step work via the steps module.
 */
#ifndef DSOLVE_H
#define DSOLVE_H

#include "calcengine.h"
#include "ast.h"

/* ── ODE method identifiers ────────────────────────────────── */
typedef enum {
    ODE_METHOD_UNKNOWN    = 0,
    ODE_METHOD_SEPARABLE,        /* dy/dx = f(x)*g(y)           */
    ODE_METHOD_LINEAR,           /* y' + P(x)y = Q(x)           */
    ODE_METHOD_EXACT,            /* M dx + N dy = 0             */
    ODE_METHOD_UNSOLVABLE
} ode_method_t;

/*
 * Auto-detect and solve a first-order ODE.
 * rhs: the right-hand side of y' = rhs  (already parsed as AST)
 * x_var: independent variable character (e.g. 'x')
 * y_var: dependent variable character (e.g. 'y')
 * result_buf: receives the solution string
 * method_used: if non-NULL, receives which method succeeded
 * Returns true on success.
 */
bool dsolve_auto(const ast_node_t *rhs, char x_var, char y_var,
                 char *result_buf, int buf_size,
                 ode_method_t *method_used);

/*
 * Attempt to solve as exact ODE: M(x,y)dx + N(x,y)dy = 0.
 * Checks ∂M/∂y == ∂N/∂x (exactness condition).
 * On success writes implicit solution F(x,y) = C to result_buf.
 */
bool dsolve_exact(const ast_node_t *M, const ast_node_t *N,
                  char x_var, char y_var,
                  char *result_buf, int buf_size);

/*
 * Check if expression rhs = M(x,y)/N(x,y) form for an exact ODE
 * from input "M(x,y) + N(x,y)*y' = 0" format.
 * Writes M_out and N_out (caller must ast_free if non-NULL).
 */
bool dsolve_extract_exact_form(const ast_node_t *lhs_minus_rhs,
                                char x_var, char y_var,
                                ast_node_t **M_out, ast_node_t **N_out);

/* Human-readable name for an ODE method. */
const char *dsolve_method_name(ode_method_t method);

/*
 * Solve second-order linear ODE with constant coefficients:
 *   a*y'' + b*y' + c*y = f(x)
 *
 * a_val, b_val, c_val: constant coefficients (already evaluated)
 * f_expr: forcing function AST node (NULL or NODE_NUMBER(0) for homogeneous)
 * x_var: independent variable character
 * result_buf: receives solution string y = yh + yp
 * buf_size: capacity of result_buf
 * Returns true on success.
 *
 * Method:
 *   1. Characteristic equation: a*r^2 + b*r + c = 0
 *      - Two distinct real roots: yh = C1*e^(r1*x) + C2*e^(r2*x)
 *      - Repeated root:           yh = (C1+C2*x)*e^(r*x)
 *      - Complex roots α±βi:      yh = e^(αx)*(C1*cos(βx)+C2*sin(βx))
 *   2. Particular solution (undetermined coefficients) for common f(x):
 *      - Constant, polynomial, exponential, sin/cos
 */
bool dsolve2_const_coeff(ce_float_t a_val, ce_float_t b_val, ce_float_t c_val,
                          const ast_node_t *f_expr, char x_var,
                          char *result_buf, int buf_size);

#endif /* DSOLVE_H */
