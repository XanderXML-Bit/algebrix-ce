/*
 * series.h -- Series, sequences, and Taylor/Fourier engine
 */
#ifndef SERIES_H
#define SERIES_H

#include "calcengine.h"
#include "ast.h"

/* ── Convergence test result ────────────────────────────────── */
typedef enum {
    CONV_CONVERGES,
    CONV_DIVERGES,
    CONV_INCONCLUSIVE
} conv_result_t;

typedef struct {
    conv_result_t result;
    const char   *test_name;
    ce_float_t    sum_estimate;
    bool          has_sum;
} conv_info_t;

/*
 * Run convergence test wizard on an infinite series.
 * expr is the general term a_n, var is the index variable (usually n).
 * Tests: nth-term, geometric, p-series, ratio, root, alternating.
 * Records steps via steps module.
 */
conv_info_t series_converge(const ast_node_t *expr, char var);

/*
 * Explicit limit comparison test between two positive-term series:
 *   a_n (target) and b_n (benchmark).
 * If lim |a_n / b_n| = L with 0 < L < inf, both series share convergence behavior.
 * Returns true when a usable finite-positive limit is detected and writes:
 *   - *result_out: CONV_CONVERGES or CONV_DIVERGES (copied from benchmark b_n)
 *   - *limit_out: estimated limit L
 * Returns false when the ratio limit is not stable/usable.
 */
bool series_limit_comparison(const ast_node_t *a_expr, const ast_node_t *b_expr,
                             char var, conv_result_t *result_out,
                             ce_float_t *limit_out);

/*
 * Generate Taylor polynomial of degree 'deg' about 'center'.
 * Returns a NEW AST (caller must ast_free it), or NULL on failure.
 * Records steps via steps module.
 */
ast_node_t *taylor_series(const ast_node_t *expr, char var,
                          ce_float_t center, int deg);

/*
 * Generate Fourier series approximation with n_terms harmonics.
 * Returns: a0/2 + sum(a_k*cos(k*var) + b_k*sin(k*var))
 * Coefficients computed numerically on [-pi, pi].
 */
ast_node_t *fourier_series(const ast_node_t *expr, char var,
                           int n_terms);

/*
 * Estimate the radius of convergence of a power series
 * sum(a_n * x^n) by computing lim |a_n/a_{n+1}|.
 */
ce_float_t power_series_radius(const ast_node_t *coeff_expr, char var);

/*
 * Compute the Lagrange error bound for a Taylor polynomial of degree n
 * centered at 'center', evaluated at 'x':
 *   |R_n(x)| <= M * |x - center|^(n+1) / (n+1)!
 * where M = max |f^(n+1)(c)| for c between center and x.
 * Returns the error bound value, or -1 on failure.
 */
ce_float_t taylor_lagrange_error(const ast_node_t *expr, char var,
                                  ce_float_t center, int deg,
                                  ce_float_t x);

/*
 * Compute the interval of convergence for a power series
 * sum(a_n * x^n), given the coefficient expression a_n.
 * Writes the interval in standard notation to buf (e.g., "(-1, 1]").
 * Returns the radius of convergence.
 */
ce_float_t power_series_interval(const ast_node_t *coeff_expr, char var,
                                  ce_float_t center,
                                  char *buf, int buf_size);

/* ── Sequence convergence ─────────────────────────────────── */

typedef enum {
    SEQ_CONVERGES,
    SEQ_DIVERGES,
    SEQ_FAILED
} seq_status_t;

typedef struct {
    seq_status_t status;
    ce_float_t   limit_value;
    bool         has_limit;
} seq_limit_t;

/*
 * Evaluate the limit of a sequence: lim(n→∞) a_n.
 * Samples a_n at large values of var to determine convergence.
 * Records steps via steps module.
 */
seq_limit_t series_sequence_limit(const ast_node_t *expr, char var);

/* ── Recursive sequence evaluation ────────────────────────── */

/*
 * Evaluate a recursive sequence: s_0 = init, s_n = f(s_{n-1}).
 * Computes terms s_0 through s_k and writes them to out_terms.
 * expr: the recurrence formula (function of var)
 * var: the variable representing the previous term (e.g., 'x' for s_{n-1})
 * init: the initial value s_0
 * k: the number of terms to compute (computes s_0 through s_k)
 * out_terms: output array (caller allocates, size >= k+1)
 * max_terms: size of out_terms
 * Returns: number of terms computed, 0 on failure.
 * Records steps via steps module.
 */
int series_recseq(const ast_node_t *expr, char var,
                  ce_float_t init, int k,
                  ce_float_t *out_terms, int max_terms);

#endif /* SERIES_H */
