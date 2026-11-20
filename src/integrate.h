/*
 * integrate.h -- Symbolic integration engine
 */
#ifndef INTEGRATE_H
#define INTEGRATE_H

#include "calcengine.h"
#include "ast.h"

/*
 * Symbolically integrate expr with respect to var (indefinite).
 * Returns a NEW simplified AST (caller must ast_free it).
 * Records steps via the steps module.
 * Returns NULL if the integral cannot be solved symbolically.
 */
ast_node_t *integrate(const ast_node_t *expr, char var);

typedef enum {
    INTEG_STRATEGY_AUTO = 0,
    INTEG_STRATEGY_PARTS,
    INTEG_STRATEGY_TRIGSUB,
    INTEG_STRATEGY_USUB,
    INTEG_STRATEGY_PARTIAL_FRAC
} integ_strategy_t;

/*
 * Integrate with an explicit strategy selection.
 * For INTEG_STRATEGY_USUB, u_hint may be non-NULL to force a specific substitution.
 * On failure returns NULL and records a detailed reason retrievable via
 * integrate_last_failure_reason().
 */
ast_node_t *integrate_with_strategy(const ast_node_t *expr, char var,
                                    integ_strategy_t strategy,
                                    const ast_node_t *u_hint);

/* Last symbolic-integration failure reason string (never NULL). */
const char *integrate_last_failure_reason(void);
/* True when the last integrate(), integrate_with_strategy() or
 * integrate_tabular() request reached INTEG_MAX_DEPTH nested integ() calls;
 * a failed request then reports "Expression too deep".  integrate_tabular()
 * can still return the rows built before the cap, with this set. */
bool integrate_hit_depth_cap(void);

/* Failure reasons that callers outside integrate.c also compare or print.
 *
 * CECIAD (src/modules/calc_int/adv_main.c) tries the strategy workers in
 * turn, integration by parts (CEIPRT) last. When CEIPRT answers one of its
 * two no-match reasons, INTEG_PARTS_NOT_PRODUCT_MSG or
 * INTEG_PARTS_NO_SPLIT_MSG, every strategy has refused, and CECIAD answers
 * INTEG_NO_ANTIDERIV_MSG instead: the text the desktop processor prints
 * when integrate() fails (processor.c spells it as a literal, because
 * tests/cemu/test_ce_numeric_formatting.py requires literal formats). */
#define INTEG_TOO_DEEP_MSG "Expression too deep"
#define INTEG_OOM_MSG "Out of memory. Reduce input"
#define INTEG_NO_ANTIDERIV_MSG "Cannot find symbolic antiderivative"
#define INTEG_PARTS_NOT_PRODUCT_MSG \
    "Integration by parts requires a product-like integrand"
#define INTEG_PARTS_NO_SPLIT_MSG \
    "Integration by parts could not find a reducible split"

/*
 * Produce a ranked integration strategy suggestion summary for expr.
 * Writes a human-readable report into buf.
 */
void integrate_strategy_report(const ast_node_t *expr, char var,
                               char *buf, int buf_size);

/*
 * Solve separable ODE: y' = rhs(x,y) by separation of variables.
 * On success writes a complete symbolic solution to result_buf.
 * If the complete answer does not fit, returns false and writes
 * ODE_TOO_LONG_MSG (itself cut to capacity) instead of partial math.
 * The same output-capacity contract applies to ode_solve_linear().
 */
#define ODE_TOO_LONG_MSG "ODE answer too long"
bool ode_solve_separable(const ast_node_t *rhs, char x_var, char y_var,
                         char *result_buf, int buf_size);

/*
 * Solve first-order linear ODE: y' = rhs(x,y), interpreted as
 * y' + P(x)y = Q(x). On success writes solution to result_buf.
 */
bool ode_solve_linear(const ast_node_t *rhs, char x_var, char y_var,
                      char *result_buf, int buf_size);

/*
 * Evaluate a definite integral: compute F(hi) - F(lo)
 * where F = integrate(expr, var).
 * Stores the symbolic antiderivative string in antideriv_buf (if non-NULL).
 * Returns the numerical result; sets *err on failure.
 */
ce_float_t integrate_definite(const ast_node_t *expr, char var,
                              ce_float_t lo, ce_float_t hi,
                              char *antideriv_buf, int buf_size,
                              ce_error_t *err);

/* Endpoint limits of a finite-bound (Type 2) improper integral.
 * 1 = symbolic compute_limit() (links limits.c). 0 = no limits.c: finite
 * bounds return IMPROP_FAILED, so callers say they cannot decide. The
 * definite-only worker CECIDF uses 0 to fit within its 51,200 B limit. */
#ifndef CE_INTEG_IMPROPER_USE_LIMITS
#define CE_INTEG_IMPROPER_USE_LIMITS 1
#endif

/* Whether integrate_definite() prints F into antideriv_buf. A worker that
 * neither simplifies nor folds would print raw rule output such as
 * ((X^(2+1))/(2+1)), so it shows the value only (processor.c
 * INT_SHOW_ANTIDERIV follows this). Without the call, ast_print (about
 * 4 KB with its tables) is not linked into it (CECIDF). */
#ifndef CE_INTEG_DEFINITE_PRINT_F
#if defined(CE_INTEG_USE_SIMPLIFY) && !CE_INTEG_USE_SIMPLIFY && \
    !(defined(CE_INTEG_USE_FOLD_LITE) && CE_INTEG_USE_FOLD_LITE)
#define CE_INTEG_DEFINITE_PRINT_F 0
#else
#define CE_INTEG_DEFINITE_PRINT_F 1
#endif
#endif

/* Evaluate F at var = x in a fresh context; false on any eval error. */
bool integrate_eval_at(const ast_node_t *F, char var, ce_float_t x,
                       ce_float_t *out);

#if CE_INTEG_IMPROPER_USE_LIMITS
/* Fujiwara bound on the real roots of p as a polynomial in var (|r| <=
 * *bound), or false when p is not a polynomial of degree <= 32. */
bool integrate_poly_root_bound(const ast_node_t *p, char var, ce_float_t *bound);
/* q = N/D with D(x0) = 0, both analytic at x0 and N's zero provably of at
 * least D's order (structural, so a large term cannot mask a pole): x0 is
 * a removable point of q. */
bool integrate_quot_bounded(const ast_node_t *q, char var, ce_float_t x0);
#endif

/* Rounding-error estimate of evaluating n at var = x (CE_INTEG_ENABLE_DEFINITE
 * builds): about 2^-24 times the magnitudes summed over n's operations on
 * var. Negative when n cannot be evaluated or the estimate overflows. */
ce_float_t integrate_err_scale(const ast_node_t *n, char var, ce_float_t x);

/* Improper integral result status */
typedef enum {
    IMPROP_OK,          /* finite result computed                */
    IMPROP_DIVERGES,    /* integral diverges                     */
    IMPROP_FAILED       /* could not determine                   */
} improp_status_t;

/*
 * Evaluate an improper integral.
 * Handles: infinite limits (lo=-inf, hi=inf), and discontinuities
 * at the endpoints.
 * inf_lo/inf_hi: if true, the corresponding bound is +/- infinity.
 * Returns the numerical result in *result; status indicates outcome.
 */
/* Rounding uncertainty of the last IMPROP_OK result of
 * integrate_improper(), or a negative value when it is unknown. */
ce_float_t integrate_improper_unc(void);

improp_status_t integrate_improper(const ast_node_t *expr, char var,
                                    ce_float_t lo, bool inf_lo,
                                    ce_float_t hi, bool inf_hi,
                                    ce_float_t *result);

/* ── Numerical integration with error bounds ──────────────── */

typedef struct {
    ce_float_t value;       /* approximate integral value        */
    ce_float_t error_bound; /* upper bound on absolute error     */
    int        n_steps;     /* number of subintervals used       */
    ce_error_t error;       /* CE_OK or error code               */
} numint_result_t;

/*
 * Trapezoidal rule with error bound.
 * E_T <= (b-a)^3 / (12*n^2) * max|f''(x)|
 * Records steps via steps module.
 */
numint_result_t integrate_trapezoidal(const ast_node_t *expr, char var,
                                       ce_float_t lo, ce_float_t hi,
                                       int n_steps);

/*
 * Simpson's rule with error bound.
 * E_S <= (b-a)^5 / (180*n^4) * max|f''''(x)|
 * Records steps via steps module.
 */
numint_result_t integrate_simpson(const ast_node_t *expr, char var,
                                   ce_float_t lo, ce_float_t hi,
                                   int n_steps);

/* ── IVP: Antiderivative with initial condition ──────────── */
/*
 * Given f'(x) = expr and f(x0) = y0, find f(x) = F(x) + C.
 * Stores the constant C in *c_out (if non-NULL).
 * Returns a NEW AST for f(x) (caller must ast_free it).
 */
ast_node_t *integrate_ivp(const ast_node_t *expr, char var,
                           ce_float_t x0, ce_float_t y0,
                           ce_float_t *c_out, ce_error_t *err);

/* ── Tabular integration (repeated IBP) ──────────────────── */
/*
 * Display repeated integration by parts as a table with
 * alternating signs. Stops when derivative reaches 0 or 8 rows.
 * Returns a NEW AST for the result (caller must ast_free it).
 */
ast_node_t *integrate_tabular(const ast_node_t *u_part,
                               const ast_node_t *dv_part,
                               char var);

/* ── Symbolic arc length ─────────────────────────────────── */
/*
 * L = ∫_a^b sqrt(1 + [f'(x)]^2) dx
 * Sets *symbolic_ok = true if solved symbolically.
 * Falls back to polar_arclen() numerical.
 */
ce_float_t integrate_arclength(const ast_node_t *f, char var,
                                ce_float_t a, ce_float_t b,
                                bool *symbolic_ok, ce_error_t *err);

/* ── Symbolic surface area of revolution ─────────────────── */
/*
 * SA = 2π * ∫_a^b |f(x)| * sqrt(1 + [f'(x)]^2) dx
 * Falls back to polar_surfrev() numerical.
 */
ce_float_t integrate_surfarea(const ast_node_t *f, char var,
                               ce_float_t a, ce_float_t b,
                               bool *symbolic_ok, ce_error_t *err);

/* ── Symbolic volume: disk method ────────────────────────── */
/*
 * V = π * ∫_a^b [f(x)]^2 dx
 * Falls back to polar_disk() numerical.
 */
ce_float_t integrate_disk(const ast_node_t *f, char var,
                           ce_float_t a, ce_float_t b,
                           bool *symbolic_ok, ce_error_t *err);

/* ── Symbolic volume: washer method ──────────────────────── */
/*
 * V = π * ∫_a^b ([R(x)]^2 - [r(x)]^2) dx
 * Falls back to polar_washer() numerical.
 */
ce_float_t integrate_washer(const ast_node_t *R, const ast_node_t *r,
                             char var, ce_float_t a, ce_float_t b,
                             bool *symbolic_ok, ce_error_t *err);

/* ── Symbolic volume: shell method ───────────────────────── */
/*
 * V = 2π * ∫_a^b x * f(x) dx
 * Falls back to polar_shell() numerical.
 */
ce_float_t integrate_shell(const ast_node_t *f, char var,
                            ce_float_t a, ce_float_t b,
                            bool *symbolic_ok, ce_error_t *err);

/* ── Area between two curves ─────────────────────────────── */
/*
 * A = ∫_a^b |f(x) - g(x)| dx
 *
 * Algorithm:
 *   1. Sample f(x)-g(x) on [a,b] at AREA_SAMPLE_N points.
 *   2. Detect sign changes → find intersection x-coordinates via bisection.
 *   3. Split [a,b] at intersections into sub-intervals.
 *   4. On each sub-interval determine which function is on top.
 *   5. Sum ∫ (top - bottom) dx for each piece.
 *
 * Records step-by-step work via the steps module.
 * Returns the total area; sets *err on failure (CE_OK on success).
 */
ce_float_t area_between_curves(const ast_node_t *f, const ast_node_t *g,
                                char var, ce_float_t a, ce_float_t b,
                                ce_error_t *err);

#endif /* INTEGRATE_H */
