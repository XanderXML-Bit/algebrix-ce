/*
 * series.c -- Series, sequences, Taylor/Fourier engine
 *
 * Implements:
 *   - Convergence test wizard: nth-term, geometric, p-series,
 *     ratio, root, alternating series tests
 *   - Taylor/Maclaurin polynomial generation via repeated diff
 *   - Fourier series (numerical coefficients via Simpson's rule)
 *   - Power series radius of convergence estimate
 *   - Step-by-step recording via steps module
 */
#include "series.h"
#include "deriv.h"
#include "eval.h"
#include "steps.h"
#include "memory.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef CE_SERIES_USE_SIMPLIFY
#define CE_SERIES_USE_SIMPLIFY 1
#endif

#if CE_SERIES_USE_SIMPLIFY
#include "simplify.h"
#else
static ast_node_t *ast_simplify(ast_node_t *expr)
{
    return expr;
}
#endif

#define MAX_TAYLOR_DEG  10
#define SIMPSON_N       100   /* steps for numerical integration */

/* eval_at() now lives in eval.c as a public API */

/* ═══════════════════════════════════════════════════════════ */
/*                    CONVERGENCE TESTS                       */
/* ═══════════════════════════════════════════════════════════ */

/* ── 1. nth-term test ────────────────────────────────────── */

static bool test_nth_term(const ast_node_t *expr, char var,
                          conv_info_t *info)
{
    ce_error_t e1, e2;
    ce_float_t v1, v2;

    v1 = eval_at(expr, var, 1000, &e1);
    v2 = eval_at(expr, var, 10000, &e2);
    if (e1 != CE_OK || e2 != CE_OK) return false;

    steps_addf("nth-term: a(1000)=%.4g, a(10000)=%.4g",
               (double)v1, (double)v2);

    if (fabsf(v2) > 0.001f) {
        info->result    = CONV_DIVERGES;
        info->test_name = "nth-term";
        steps_addf("lim a(n) != 0 => DIVERGES");
        return true;
    }

    steps_addf("a(n)->0 (necessary, not sufficient)");
    return false;
}

/* ── 2. Geometric series test ────────────────────────────── */

static bool test_geometric(const ast_node_t *expr, char var,
                           conv_info_t *info)
{
    ce_error_t e1, e2, e3;
    ce_float_t a1, a2, a3, r1, r2;

    a1 = eval_at(expr, var, 1, &e1);
    a2 = eval_at(expr, var, 2, &e2);
    a3 = eval_at(expr, var, 3, &e3);
    if (e1 != CE_OK || e2 != CE_OK || e3 != CE_OK) return false;
    if (fabsf(a1) < 1e-20f || fabsf(a2) < 1e-20f) return false;

    r1 = a2 / a1;
    r2 = a3 / a2;

    /* Check if ratio is constant */
    if (fabsf(r1 - r2) > 0.01f * (1 + fabsf(r1))) return false;

    /* Verify with larger n */
    {
        ce_float_t a50, a51;
        ce_error_t e50, e51;
        a50 = eval_at(expr, var, 50, &e50);
        a51 = eval_at(expr, var, 51, &e51);
        if (e50 != CE_OK || e51 != CE_OK || fabsf(a50) < 1e-30f)
            return false;
        r2 = a51 / a50;
        if (fabsf(r1 - r2) > 0.05f * (1 + fabsf(r1))) return false;
    }

    steps_addf("Geometric: r = %.6g", (double)r1);

    if (fabsf(r1) < 1.0f) {
        info->result     = CONV_CONVERGES;
        info->test_name  = "geometric";
        info->has_sum    = true;
        info->sum_estimate = a1 / (1.0f - r1);
        steps_addf("|r|=%.4g < 1: CONVERGES, S=a/(1-r)=%.6g",
                   (double)fabsf(r1), (double)info->sum_estimate);
        return true;
    } else {
        info->result    = CONV_DIVERGES;
        info->test_name = "geometric";
        steps_addf("|r|=%.4g >= 1: DIVERGES", (double)fabsf(r1));
        return true;
    }
}

/* ── 3. p-series test ────────────────────────────────────── */

static bool test_p_series(const ast_node_t *expr, char var,
                          conv_info_t *info)
{
    /* Check if a_n ~ c/n^p by evaluating ratios */
    ce_error_t e1, e2;
    ce_float_t a100, a200, log_ratio, p_est;

    a100 = eval_at(expr, var, 100, &e1);
    a200 = eval_at(expr, var, 200, &e2);
    if (e1 != CE_OK || e2 != CE_OK) return false;
    if (fabsf(a100) < 1e-20f || fabsf(a200) < 1e-20f) return false;

    /* Skip alternating series — let the alternating test handle those */
    {
        ce_error_t ea;
        ce_float_t a101 = eval_at(expr, var, 101, &ea);
        if (ea == CE_OK && a100 * a101 < 0)
            return false;
    }

    /* If a_n = c/n^p, then a(100)/a(200) = (200/100)^p = 2^p */
    log_ratio = logf(fabsf(a100 / a200));
    p_est = log_ratio / logf(2.0f);

    /* Verify with another pair */
    {
        ce_float_t a500, ratio2, p_est2;
        ce_error_t e5;
        a500 = eval_at(expr, var, 500, &e5);
        if (e5 != CE_OK || fabsf(a500) < 1e-30f) return false;
        ratio2 = logf(fabsf(a200 / a500));
        p_est2 = ratio2 / logf(2.5f);
        if (fabsf(p_est - p_est2) > 0.3f) return false;

        /* Third verification at large n to detect log-modified decay.
         * For true p-series, p_est stays constant across scales.
         * For log-modified series like 1/(n*ln(n)), p drifts toward 1. */
        {
            ce_float_t a5000, ratio3, p_est3;
            ce_error_t e50;
            a5000 = eval_at(expr, var, 5000, &e50);
            if (e50 == CE_OK && fabsf(a5000) > 1e-30f) {
                ratio3 = logf(fabsf(a500 / a5000));
                p_est3 = ratio3 / logf(10.0f);
                /* If p drops significantly at larger n, it's not a true
                 * p-series — skip and let the integral test handle it */
                if (p_est > 1.0f && p_est3 < p_est - 0.05f)
                    return false;
            }
        }
    }

    if (p_est < 0.1f) return false;  /* not a p-series pattern */

    steps_addf("p-series: a(n) ~ 1/n^%.2g", (double)p_est);

    if (p_est > 1.05f) {
        info->result    = CONV_CONVERGES;
        info->test_name = "p-series";
        steps_addf("p=%.2g > 1: CONVERGES", (double)p_est);
        return true;
    } else {
        /* p <= 1 (including harmonic series p=1) diverges */
        info->result    = CONV_DIVERGES;
        info->test_name = "p-series";
        steps_addf("p=%.2g <= 1: DIVERGES", (double)p_est);
        return true;
    }
}

/* ── 4. Ratio test ───────────────────────────────────────── */

static bool test_ratio(const ast_node_t *expr, char var,
                       conv_info_t *info)
{
    ce_error_t e1, e2, e3, e4;
    ce_float_t an1, an2, an3, an4, r1, r2;

    an1 = eval_at(expr, var, 100, &e1);
    an2 = eval_at(expr, var, 101, &e2);
    an3 = eval_at(expr, var, 500, &e3);
    an4 = eval_at(expr, var, 501, &e4);

    if (e1 != CE_OK || e2 != CE_OK || e3 != CE_OK || e4 != CE_OK)
        return false;
    if (fabsf(an1) < 1e-30f || fabsf(an3) < 1e-30f) return false;

    r1 = fabsf(an2 / an1);
    r2 = fabsf(an4 / an3);

    /* Check ratios converge */
    if (fabsf(r1 - r2) > 0.1f) return false;

    {
        ce_float_t L = r2;
        steps_addf("Ratio: |a(n+1)/a(n)| -> %.6g", (double)L);

        if (L < 0.99f) {
            info->result    = CONV_CONVERGES;
            info->test_name = "ratio";
            steps_addf("L=%.4g < 1: CONVERGES (ratio)", (double)L);
            return true;
        }
        if (L > 1.01f) {
            info->result    = CONV_DIVERGES;
            info->test_name = "ratio";
            steps_addf("L=%.4g > 1: DIVERGES (ratio)", (double)L);
            return true;
        }
    }

    steps_addf("L ~ 1: ratio test inconclusive");
    return false;
}

/* ── 5. Root test ────────────────────────────────────────── */

static bool test_root(const ast_node_t *expr, char var,
                      conv_info_t *info)
{
    ce_error_t e1, e2;
    ce_float_t an1, an2, r1, r2;

    an1 = eval_at(expr, var, 100, &e1);
    an2 = eval_at(expr, var, 500, &e2);
    if (e1 != CE_OK || e2 != CE_OK) return false;
    if (fabsf(an1) < 1e-30f || fabsf(an2) < 1e-30f) return false;

    r1 = powf(fabsf(an1), 1.0f / 100);
    r2 = powf(fabsf(an2), 1.0f / 500);

    if (fabsf(r1 - r2) > 0.1f) return false;

    {
        ce_float_t L = r2;

        /* Verify at even larger n to check convergence of the root limit.
         * For true convergent root test cases (e.g., (1/2)^n), the root
         * stabilizes well below 1. For series like 1/(n*ln(n)), the root
         * slowly approaches 1 from below — a third sample catches this. */
        if (L < 0.99f) {
            ce_float_t an3, r3;
            ce_error_t e3;
            an3 = eval_at(expr, var, 2000, &e3);
            if (e3 == CE_OK && fabsf(an3) > 1e-30f) {
                r3 = powf(fabsf(an3), 1.0f / 2000);
                /* If root at n=2000 is closer to 1 than at n=500,
                 * the limit is drifting toward 1 (inconclusive). */
                if (r3 > r2 + 0.002f) {
                    steps_addf("Root drifting toward 1: inconclusive");
                    return false;
                }
            }
        }

        steps_addf("Root: |a(n)|^(1/n) -> %.6g", (double)L);

        if (L < 0.99f) {
            info->result    = CONV_CONVERGES;
            info->test_name = "root";
            steps_addf("L=%.4g < 1: CONVERGES (root)", (double)L);
            return true;
        }
        if (L > 1.01f) {
            info->result    = CONV_DIVERGES;
            info->test_name = "root";
            steps_addf("L=%.4g > 1: DIVERGES (root)", (double)L);
            return true;
        }
    }

    steps_addf("L ~ 1: root test inconclusive");
    return false;
}

/* ── 5b. Telescoping series test ─────────────────────────── */

static bool test_telescoping(const ast_node_t *expr, char var,
                             conv_info_t *info)
{
    /*
     * Telescoping: if partial sums stabilize (cancel pairwise),
     * compute S_N for increasing N and check convergence.
     * Detection: S_N = sum_{k=1}^{N} a_k converges, and the
     * tail |S_{2N} - S_N| shrinks rapidly.
     */
    ce_error_t err;
    ce_float_t S_50  = 0, S_100 = 0, S_200 = 0;
    ce_float_t tail1, tail2;
    int i;

    /* Compute partial sums */
    for (i = 1; i <= 50; i++) {
        ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
        if (err != CE_OK) return false;
        S_50 += v;
    }
    S_100 = S_50;
    for (i = 51; i <= 100; i++) {
        ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
        if (err != CE_OK) return false;
        S_100 += v;
    }
    S_200 = S_100;
    for (i = 101; i <= 200; i++) {
        ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
        if (err != CE_OK) return false;
        S_200 += v;
    }

    tail1 = fabsf(S_100 - S_50);
    tail2 = fabsf(S_200 - S_100);

    /* For a telescoping series, tails shrink very fast and S_N stabilizes */
    if (tail2 < tail1 * 0.6f && tail2 < 0.1f &&
        fabsf(S_200 - S_100) < 0.01f * (1 + fabsf(S_200))) {

        steps_addf("Telescoping: S(50)=%.4g, S(100)=%.4g, S(200)=%.4g",
                   (double)S_50, (double)S_100, (double)S_200);
        steps_addf("Tails: %.4g -> %.4g (rapid shrink)",
                   (double)tail1, (double)tail2);

        info->result       = CONV_CONVERGES;
        info->test_name    = "telescoping";
        info->has_sum      = true;
        info->sum_estimate = S_200;

        steps_addf("CONVERGES (telescoping), sum ~ %.6g",
                   (double)S_200);
        return true;
    }

    return false;
}

/* ── 6. Alternating series test ──────────────────────────── */

static bool test_alternating(const ast_node_t *expr, char var,
                             conv_info_t *info)
{
    ce_error_t errs[6];
    ce_float_t vals[6];
    bool alternating = true;
    bool decreasing  = true;
    int i;

    for (i = 0; i < 6; i++) {
        vals[i] = eval_at(expr, var, (ce_float_t)(i + 1), &errs[i]);
        if (errs[i] != CE_OK) return false;
    }

    /* Check alternating signs */
    for (i = 0; i < 5; i++) {
        if (vals[i] * vals[i + 1] > 0) { alternating = false; break; }
    }
    if (!alternating) return false;

    /* Check |a_n| is decreasing */
    for (i = 0; i < 5; i++) {
        if (fabsf(vals[i + 1]) > fabsf(vals[i]) + 0.001f) {
            decreasing = false;
            break;
        }
    }

    /* Check lim |a_n| = 0 */
    {
        ce_error_t e;
        ce_float_t big = eval_at(expr, var, 10000, &e);
        if (e != CE_OK) return false;

        steps_addf("Alternating: signs alternate, |a(n)| decreasing");

        if (fabsf(big) < 0.001f && decreasing) {
            info->result    = CONV_CONVERGES;
            info->test_name = "alternating";
            steps_addf("|a(n)|->0: CONVERGES (alt. series)");
            return true;
        }
    }

    return false;
}

/* ── 7. Tail ratio test (partial sum convergence heuristic) ── */

static bool test_tail_ratio(const ast_node_t *expr, char var,
                            conv_info_t *info)
{
    /*
     * Tail ratio heuristic: for positive, decreasing terms, compare
     * partial sum tails at different cutoffs. If tail(501-2000) is
     * significantly smaller than tail(101-500), the series converges.
     * This is NOT the classical integral test (which compares sum to
     * improper integral) — it is a numerical partial sum heuristic.
     */
    ce_error_t err;
    ce_float_t a1, a2, a5;
    ce_float_t sum_100, sum_500, sum_2000;
    int i;

    /* Check that terms are positive and decreasing */
    a1 = eval_at(expr, var, 1, &err);
    if (err != CE_OK || a1 <= 0) return false;
    a2 = eval_at(expr, var, 2, &err);
    if (err != CE_OK || a2 <= 0 || a2 > a1 + 0.001f) return false;
    a5 = eval_at(expr, var, 5, &err);
    if (err != CE_OK || a5 <= 0 || a5 > a2 + 0.001f) return false;

    /* Compute partial sums at different cutoffs */
    sum_100 = 0;
    for (i = 1; i <= 100; i++) {
        ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
        if (err != CE_OK) return false;
        sum_100 += v;
    }
    if (check_cancel()) return false;
    sum_500 = sum_100;
    for (i = 101; i <= 500; i++) {
        ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
        if (err != CE_OK) return false;
        sum_500 += v;
    }
    if (check_cancel()) return false;
    progress_check();
    sum_2000 = sum_500;
    for (i = 501; i <= 2000; i++) {
        ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
        if (err != CE_OK) return false;
        sum_2000 += v;
    }

    steps_addf("Tail ratio: S(100)=%.4g, S(500)=%.4g, S(2000)=%.4g",
               (double)sum_100, (double)sum_500, (double)sum_2000);

    /* Check if tail sums are shrinking (convergent) */
    {
        ce_float_t tail1 = sum_500 - sum_100;   /* sum from 101 to 500 */
        ce_float_t tail2 = sum_2000 - sum_500;   /* sum from 501 to 2000 */

        /* If tail2 (4x more terms) is smaller than tail1, converging */
        if (tail2 < tail1 * 0.5f && tail2 < 1.0f) {
            info->result    = CONV_CONVERGES;
            info->test_name = "tail ratio";
            info->has_sum   = true;
            info->sum_estimate = sum_2000;
            steps_addf("Tails shrinking: CONVERGES (tail ratio test)");
            return true;
        }

        /* If tail2 is still large relative to tail1, diverging */
        if (tail2 > tail1 * 0.8f && tail2 > 0.5f) {
            info->result    = CONV_DIVERGES;
            info->test_name = "tail ratio";
            steps_addf("Tails not shrinking: DIVERGES (tail ratio test)");
            return true;
        }
    }

    return false;
}

/* ── 8. Comparison test ──────────────────────────────────── */

/*
 * Try limit comparison against a specific reference series b_n.
 * Computes L = lim a_n / b_n at several sample points.
 * Returns true if L stabilizes to a finite positive constant.
 */
static bool try_limit_compare(const ce_float_t *a_vals,
                               const ce_float_t *b_vals, int npts,
                               ce_float_t *out_L)
{
    int i;
    ce_float_t l_min, l_max, l_avg;
    ce_float_t spread;

    l_avg = 0;
    l_min = 1e30f;
    l_max = -1e30f;
    for (i = 0; i < npts; i++) {
        ce_float_t li;
        if (fabsf(b_vals[i]) < 1e-30f) return false;
        li = fabsf(a_vals[i]) / fabsf(b_vals[i]);
        if (!ce_isfinite(li) || li <= 0) return false;
        l_avg += li;
        if (li < l_min) l_min = li;
        if (li > l_max) l_max = li;
    }
    l_avg /= npts;
    spread = (l_avg > 1e-12f) ? ((l_max - l_min) / l_avg) : 999.0f;
    if (spread > 0.40f) return false;
    *out_L = l_avg;
    return true;
}

static bool test_comparison(const ast_node_t *expr, char var,
                            conv_info_t *info)
{
    /*
     * Limit comparison test (strengthened):
     *   1. Sample a_n at 5 points for robust p-estimation.
     *   2. Use 3 independent (n_i, n_j) pairs to estimate p,
     *      requiring all 3 to agree within tolerance.
     *   3. Try known reference series first:
     *      - 1/n^2 (convergent p-series) if a_n decays like 1/n^2+
     *      - 1/n   (harmonic, divergent) if a_n decays like 1/n
     *      - 1/n^p for estimated p otherwise
     *   4. Show clear bounding argument in steps.
     */
    ce_error_t e;
    const ce_float_t ns[5] = {50.0f, 150.0f, 500.0f, 1500.0f, 5000.0f};
    ce_float_t avals[5];
    ce_float_t p_est1, p_est2, p_est3, p_est;
    ce_float_t bvals[5];
    ce_float_t L;
    int i;

    /* Sample the series at 5 points */
    for (i = 0; i < 5; i++) {
        avals[i] = eval_at(expr, var, ns[i], &e);
        if (e != CE_OK) return false;
        if (fabsf(avals[i]) < 1e-30f) return false;
    }

    /* Skip mixed-sign/alternating terms (handled by alternating test). */
    for (i = 0; i < 4; i++) {
        if (avals[i] * avals[i + 1] < 0) return false;
    }

    /* Estimate decay exponent p from 3 independent pairs:
     *   a_n ~ C / n^p  =>  p = log(|a_i/a_j|) / log(n_j/n_i)
     */
    p_est1 = logf(fabsf(avals[0] / avals[1])) / logf(ns[1] / ns[0]);
    p_est2 = logf(fabsf(avals[1] / avals[2])) / logf(ns[2] / ns[1]);
    p_est3 = logf(fabsf(avals[2] / avals[3])) / logf(ns[3] / ns[2]);
    if (!ce_isfinite(p_est1) || !ce_isfinite(p_est2) || !ce_isfinite(p_est3))
        return false;

    /* All 3 estimates must agree within 0.35 of each other */
    {
        ce_float_t pmin = p_est1, pmax = p_est1;
        if (p_est2 < pmin) pmin = p_est2;
        if (p_est3 < pmin) pmin = p_est3;
        if (p_est2 > pmax) pmax = p_est2;
        if (p_est3 > pmax) pmax = p_est3;
        if (pmax - pmin > 0.35f) return false;
    }

    p_est = (p_est1 + p_est2 + p_est3) / 3.0f;
    if (p_est < 0.05f) return false;

    steps_addf("Comparison: a_n decays like 1/n^p");
    steps_addf("  p estimates: %.3g, %.3g, %.3g (avg=%.3g)",
               (double)p_est1, (double)p_est2, (double)p_est3,
               (double)p_est);

    /* --- Try well-known reference series first --- */

    /* If p ≈ 2 or higher, try direct comparison with 1/n^2 */
    if (p_est > 1.8f) {
        for (i = 0; i < 5; i++) bvals[i] = 1.0f / (ns[i] * ns[i]);
        if (try_limit_compare(avals, bvals, 5, &L)) {
            info->result    = CONV_CONVERGES;
            info->test_name = "direct comparison";
            steps_addf("  Compare with b_n = 1/n^2 (convergent p-series)");
            steps_addf("  L = lim a_n/b_n = %.4g (finite, positive)", (double)L);
            steps_addf("  Since 0 < L < inf and sum(1/n^2) converges,");
            steps_addf("  sum(a_n) CONVERGES by limit comparison test");
            return true;
        }
    }

    /* If p ≈ 1, try comparison with harmonic series 1/n */
    if (p_est > 0.7f && p_est < 1.3f) {
        for (i = 0; i < 5; i++) bvals[i] = 1.0f / ns[i];
        if (try_limit_compare(avals, bvals, 5, &L)) {
            if (p_est < 0.95f) {
                info->result    = CONV_DIVERGES;
                info->test_name = "direct comparison";
                steps_addf("  Compare with b_n = 1/n (harmonic, diverges)");
                steps_addf("  L = lim a_n/b_n = %.4g (finite, positive)", (double)L);
                steps_addf("  Since 0 < L < inf and sum(1/n) diverges,");
                steps_addf("  sum(a_n) DIVERGES by limit comparison test");
                return true;
            }
            /* p ≈ 1 with harmonic match: boundary case, defer */
            steps_addf("  L with 1/n = %.4g but p~1: boundary case", (double)L);
        }
    }

    /* --- General p-series comparison with estimated p --- */
    for (i = 0; i < 5; i++) bvals[i] = 1.0f / powf(ns[i], p_est);
    if (!try_limit_compare(avals, bvals, 5, &L)) {
        steps_addf("  L = lim a_n * n^p did not stabilize: inconclusive");
        return false;
    }

    steps_addf("  b_n = 1/n^%.3g,  L = lim a_n/b_n = %.6g", (double)p_est, (double)L);

    if (p_est > 1.05f) {
        info->result    = CONV_CONVERGES;
        info->test_name = "limit comparison";
        steps_addf("  Since p = %.3g > 1, sum(1/n^p) converges", (double)p_est);
        steps_addf("  0 < L < inf => sum(a_n) CONVERGES");
        return true;
    }
    if (p_est < 0.95f) {
        info->result    = CONV_DIVERGES;
        info->test_name = "limit comparison";
        steps_addf("  Since p = %.3g < 1, sum(1/n^p) diverges", (double)p_est);
        steps_addf("  0 < L < inf => sum(a_n) DIVERGES");
        return true;
    }

    /* Near harmonic boundary p≈1: leave to integral/other tests. */
    steps_addf("  p ~ 1 boundary: limit comparison inconclusive");
    return false;
}

/* ── 9. Numerical integral test ────────────────────────────── */

static bool test_integral_comparison(const ast_node_t *expr, char var,
                                     conv_info_t *info)
{
    /*
     * Integral test: for positive, decreasing f(x), sum f(n) and
     * int_1^inf f(x) dx have the same convergence behavior.
     * Numerically integrate f(x) from 10 to M for M=100, 1000, 10000
     * using composite trapezoidal rule. If integral grows without bound,
     * the series diverges; if it converges, the series converges.
     * This catches log-modified series like 1/(n*ln(n)).
     */
    ce_error_t err;
    ce_float_t a2, a10, a100;
    ce_float_t integ_100, integ_1k, integ_10k;
    int i;

    /* First verify terms are positive and decreasing */
    a2   = eval_at(expr, var, 2, &err);
    if (err != CE_OK || a2 <= 0) return false;
    a10  = eval_at(expr, var, 10, &err);
    if (err != CE_OK || a10 <= 0 || a10 > a2 + 1e-6f) return false;
    a100 = eval_at(expr, var, 100, &err);
    if (err != CE_OK || a100 <= 0 || a100 > a10 + 1e-6f) return false;

    /* Trapezoidal integration from 10 to 100 */
    integ_100 = 0;
    {
        int n_steps = 90;
        ce_float_t h = 90.0f / n_steps;
        ce_float_t f_prev = eval_at(expr, var, 10.0f, &err);
        if (err != CE_OK) return false;
        for (i = 1; i <= n_steps; i++) {
            ce_float_t x = 10.0f + i * h;
            ce_float_t f_cur = eval_at(expr, var, x, &err);
            if (err != CE_OK) return false;
            integ_100 += (f_prev + f_cur) * h * 0.5f;
            f_prev = f_cur;
        }
    }

    /* Trapezoidal integration from 100 to 1000 */
    integ_1k = 0;
    {
        int n_steps = 100;
        ce_float_t h = 900.0f / n_steps;
        ce_float_t f_prev = eval_at(expr, var, 100.0f, &err);
        if (err != CE_OK) return false;
        for (i = 1; i <= n_steps; i++) {
            ce_float_t x = 100.0f + i * h;
            ce_float_t f_cur = eval_at(expr, var, x, &err);
            if (err != CE_OK) return false;
            integ_1k += (f_prev + f_cur) * h * 0.5f;
            f_prev = f_cur;
        }
    }

    /* Trapezoidal integration from 1000 to 10000 */
    integ_10k = 0;
    {
        int n_steps = 100;
        ce_float_t h = 9000.0f / n_steps;
        ce_float_t f_prev = eval_at(expr, var, 1000.0f, &err);
        if (err != CE_OK) return false;
        for (i = 1; i <= n_steps; i++) {
            ce_float_t x = 1000.0f + i * h;
            ce_float_t f_cur = eval_at(expr, var, x, &err);
            if (err != CE_OK) return false;
            integ_10k += (f_prev + f_cur) * h * 0.5f;
            f_prev = f_cur;
        }
    }

    steps_addf("Integral test: I[10,100]=%.4g, I[100,1k]=%.4g, I[1k,10k]=%.4g",
               (double)integ_100, (double)integ_1k, (double)integ_10k);

    /* If each decade-interval integral stays roughly the same or grows,
     * the improper integral diverges → series diverges.
     * For 1/(n*ln(n)), integ grows like ln(ln(M)) — slowly but unbounded. */
    if (integ_1k > integ_100 * 0.5f && integ_10k > integ_1k * 0.5f &&
        integ_10k > 0.01f) {
        /* Integral tails not decaying — diverges */
        info->result    = CONV_DIVERGES;
        info->test_name = "integral";
        steps_addf("Integral not converging: DIVERGES (integral test)");
        return true;
    }

    /* If the integral tails are shrinking rapidly, converges */
    if (integ_10k < integ_1k * 0.3f && integ_1k < integ_100 * 0.5f &&
        integ_10k < 0.01f) {
        info->result    = CONV_CONVERGES;
        info->test_name = "integral";
        steps_addf("Integral converging: CONVERGES (integral test)");
        return true;
    }

    return false;
}

/* ── Public: convergence test wizard ─────────────────────── */

conv_info_t series_converge(const ast_node_t *expr, char var)
{
    conv_info_t info = { CONV_INCONCLUSIVE, "none", 0, false };
    char buf[80];

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Convergence of sum(%s, %c=1..inf)", buf, var);

    if (test_nth_term(expr, var, &info))    return info;
    if (test_geometric(expr, var, &info))   return info;
    if (test_alternating(expr, var, &info)) return info;
    if (test_telescoping(expr, var, &info)) return info;
    if (test_p_series(expr, var, &info))    return info;
    if (test_ratio(expr, var, &info))       return info;
    if (test_root(expr, var, &info))        return info;
    if (test_tail_ratio(expr, var, &info))  return info;
    if (test_integral_comparison(expr, var, &info)) return info;
    if (test_comparison(expr, var, &info))  return info;

    /* Estimate partial sum */
    {
        ce_float_t sum = 0;
        ce_error_t err;
        int i;
        for (i = 1; i <= 1000; i++) {
            ce_float_t v = eval_at(expr, var, (ce_float_t)i, &err);
            if (err != CE_OK) break;
            sum += v;
        }
        info.sum_estimate = sum;
        info.has_sum = true;
        steps_addf("Partial sum (1000 terms) = %.6g", (double)sum);
    }

    steps_addf("All tests inconclusive");
    return info;
}

bool series_limit_comparison(const ast_node_t *a_expr, const ast_node_t *b_expr,
                             char var, conv_result_t *result_out,
                             ce_float_t *limit_out)
{
    ce_error_t err;
    const ce_float_t n_vals[4] = { 200.0f, 600.0f, 2000.0f, 6000.0f };
    ce_float_t ratios[4];
    ce_float_t a_vals[4], b_vals[4];
    ce_float_t r_avg, r_min, r_max, spread;
    int i;
    conv_info_t b_info;

    if (!a_expr || !b_expr || !result_out || !limit_out)
        return false;

    for (i = 0; i < 4; i++) {
        a_vals[i] = eval_at(a_expr, var, n_vals[i], &err);
        if (err != CE_OK || !ce_isfinite(a_vals[i])) return false;
        b_vals[i] = eval_at(b_expr, var, n_vals[i], &err);
        if (err != CE_OK || !ce_isfinite(b_vals[i])) return false;
        if (fabsf(b_vals[i]) < 1e-30f) return false;

        ratios[i] = fabsf(a_vals[i] / b_vals[i]);
        if (!ce_isfinite(ratios[i]) || ratios[i] <= 0.0f) return false;
    }

    /* Require eventual same-sign terms for both series at sample points. */
    for (i = 1; i < 4; i++) {
        if (a_vals[i] * a_vals[i - 1] < 0.0f) return false;
        if (b_vals[i] * b_vals[i - 1] < 0.0f) return false;
    }

    r_avg = 0.25f * (ratios[0] + ratios[1] + ratios[2] + ratios[3]);
    r_min = ratios[0];
    r_max = ratios[0];
    for (i = 1; i < 4; i++) {
        if (ratios[i] < r_min) r_min = ratios[i];
        if (ratios[i] > r_max) r_max = ratios[i];
    }
    spread = (fabsf(r_avg) > 1e-12f) ? (r_max - r_min) / fabsf(r_avg) : 999.0f;

    steps_addf("Limit comparison: L = lim |a_n/b_n|");
    steps_addf("Ratio samples: %.4g, %.4g, %.4g, %.4g",
               (double)ratios[0], (double)ratios[1],
               (double)ratios[2], (double)ratios[3]);

    if (spread > 0.40f) {
        steps_addf("Ratio limit unstable (spread %.3g): inconclusive", (double)spread);
        return false;
    }

    *limit_out = r_avg;
    steps_addf("Estimated L = %.6g (finite, positive)", (double)*limit_out);

    b_info = series_converge(b_expr, var);
    if (b_info.result == CONV_INCONCLUSIVE) {
        steps_addf("Benchmark convergence inconclusive; cannot transfer result");
        return false;
    }

    *result_out = b_info.result;
    steps_addf("Benchmark by %s => %s",
               b_info.test_name,
               (b_info.result == CONV_CONVERGES) ? "CONVERGES" : "DIVERGES");
    steps_addf("By limit comparison, target series has same behavior");
    return true;
}

/* ═══════════════════════════════════════════════════════════ */
/*                    TAYLOR SERIES                           */
/* ═══════════════════════════════════════════════════════════ */

ast_node_t *taylor_series(const ast_node_t *expr, char var,
                          ce_float_t center, int deg)
{
    ast_node_t *current;
    ast_node_t *result = NULL;
    eval_ctx_t  ctx;
    ce_float_t  fact = 1.0f;
    int         k;
    char        buf[60];

    if (deg > MAX_TAYLOR_DEG) {
        deg = MAX_TAYLOR_DEG;
        steps_addf("Note: degree capped at %d", MAX_TAYLOR_DEG);
    } else {
    }
    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Taylor of %s about %c=%.4g, deg %d",
               buf, var, (double)center, deg);

    current = ast_clone(expr);

    for (k = 0; k <= deg; k++) {
        ce_float_t coeff;

        if (k > 0) fact *= k;   /* k! */

        /* Evaluate kth derivative at center */
        eval_init(&ctx);
        eval_set_var(&ctx, var, center);
        coeff = eval_expr(&ctx, current);
        if (eval_error(&ctx) != CE_OK) {
            ast_free(current);
            if (result) ast_free(result);
            return NULL;
        }

        coeff /= fact;   /* c_k = f^(k)(center) / k! */

        if (fabsf(coeff) > 1e-10f) {
            ast_node_t *term;

            steps_addf("f^(%d)(%.4g)/%d! = %.6g",
                       k, (double)center, k, (double)coeff);

            if (k == 0) {
                term = ast_number(coeff);
            } else {
                ast_node_t *base;
                if (fabsf(center) < 1e-10f)
                    base = ast_variable(var);
                else
                    base = ast_binop(OP_SUB, ast_variable(var),
                                     ast_number(center));

                if (k == 1) {
                    if (fabsf(coeff - 1.0f) < 1e-6f)
                        term = base;
                    else
                        term = ast_binop(OP_MUL, ast_number(coeff), base);
                } else {
                    ast_node_t *power = ast_binop(OP_POW, base,
                                                  ast_number((ce_float_t)k));
                    if (fabsf(coeff - 1.0f) < 1e-6f)
                        term = power;
                    else
                        term = ast_binop(OP_MUL, ast_number(coeff), power);
                }
            }

            if (!result)
                result = term;
            else
                result = ast_binop(OP_ADD, result, term);
        }

        /* Differentiate for next iteration */
        if (k < deg) {
            int sc = steps_count();
            ast_node_t *d = deriv_diff_quiet(current, var);
            steps_restore(sc);

            ast_free(current);
            current = d;

            if (!current) {
                steps_addf("Cannot diff past order %d", k + 1);
                break;
            }
        }
    }

    ast_free(current);

    if (!result) result = ast_number(0);
    result = ast_simplify(result);

    ast_to_str(result, buf, sizeof(buf));
    steps_addf("= %s", buf);

    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*                    FOURIER SERIES                          */
/* ═══════════════════════════════════════════════════════════ */

/* The explicit walk bounds memory independently of the eZ80 call stack.
 * Eight denominator entries avoid repeat scans; overflow is scanned in place.
 * Not reentrant: numerical evaluation does not call fourier_series(). */
#define FOURIER_DEN_MAX 8
#define FOURIER_WALK_MAX 64
#if defined(__GNUC__) || defined(__clang__)
#define FOURIER_NOINLINE __attribute__((noinline))
#else
#define FOURIER_NOINLINE
#endif

typedef struct {
    const ast_node_t *expr;
    const ast_node_t *owner;
    uint8_t fn;                  /* zero: expr; otherwise fn-1 applied to expr */
} fourier_den_t;

static const ast_node_t *fourier_child(const ast_node_t *n, unsigned i)
{
    if (i > 3) return NULL;
    switch (n->type) {
    case NODE_BINOP: return i == 0 ? n->d.binop.left : i == 1 ? n->d.binop.right : NULL;
    case NODE_UNOP: return i == 0 ? n->d.unop.operand : NULL;
    case NODE_FUNC: return i == 0 ? n->d.func.arg : NULL;
    case NODE_PAIR: return i == 0 ? n->d.pair.head : i == 1 ? n->d.pair.tail : NULL;
    case NODE_STAT: return i == 0 ? n->d.stat.args : NULL;
    case NODE_PIECEWISE: return i == 0 ? n->d.piecewise.pieces : NULL;
    case NODE_DERIV: return i == 0 ? n->d.deriv.expr : NULL;
    case NODE_INTEGRAL: return i == 0 ? n->d.integral.expr : i == 1 ? n->d.integral.lo : i == 2 ? n->d.integral.hi : NULL;
    case NODE_LIMIT: return i == 0 ? n->d.limit.expr : i == 1 ? n->d.limit.point : NULL;
    case NODE_SUM: return i == 0 ? n->d.sum.expr : i == 1 ? n->d.sum.lo : i == 2 ? n->d.sum.hi : NULL;
    case NODE_PRODUCT: return i == 0 ? n->d.product.expr : i == 1 ? n->d.product.lo : i == 2 ? n->d.product.hi : NULL;
    case NODE_TAYLOR: return i == 0 ? n->d.taylor.expr : i == 1 ? n->d.taylor.center : i == 2 ? n->d.taylor.degree : NULL;
    case NODE_FOURIER: return i == 0 ? n->d.fourier.expr : i == 1 ? n->d.fourier.nterms : NULL;
    case NODE_EXPAND: return i == 0 ? n->d.expand.expr : NULL;
    case NODE_FACTOR: return i == 0 ? n->d.factor.expr : NULL;
    case NODE_SOLVE: return i == 0 ? n->d.solve.expr : NULL;
    case NODE_TANGENT: return i == 0 ? n->d.tangent.expr : i == 1 ? n->d.tangent.point : NULL;
    case NODE_MATRIX_OP: return i == 0 ? n->d.matrix_op.arg : NULL;
    case NODE_POLAR: return i == 0 ? n->d.polar.expr1 : i == 1 ? n->d.polar.expr2 : i == 2 ? n->d.polar.lo : n->d.polar.hi;
    case NODE_LIST_LITERAL: return i == 0 ? n->d.list_literal.elems : NULL;
    case NODE_LIST_OP: return i == 0 ? n->d.list_op.arg : NULL;
    case NODE_LIST_ELEM: return i == 0 ? n->d.list_elem.index : NULL;
    case NODE_STORE: return i == 0 ? n->d.store.expr : NULL;
    default: return NULL;
    }
}

static bool fourier_sample(const ast_node_t *expr, char var,
                           ce_float_t x, ce_float_t *out)
{
    ce_error_t err;
    *out = eval_at(expr, var, x, &err);
    return err == CE_OK && ce_isfinite(*out);
}

static FOURIER_NOINLINE bool fourier_den_sample(const fourier_den_t *g,
                                      char var, ce_float_t x, ce_float_t *out)
{
    ast_node_t wrapper;
    bool refused = g_eval_refused, ok;
    ce_float_t ans = g_eval_refused_ans;
    /* Sampling does not preserve enclosing sum/product variable bindings.
     * This speculative subtree may also be in an unselected lazy branch:
     * its failure is no candidate evidence, not a failure of the real f. */
    if (!g->fn) ok = fourier_sample(g->expr, var, x, out);
    else {
        /* Borrow the argument without allocating or freeing the integrand.
         * Normal evaluator nodes preserve radians/degrees semantics. */
        wrapper.type = NODE_FUNC;
        wrapper.d.func.id = (func_id_t)(g->fn - 1);
        wrapper.d.func.arg = (ast_node_t *)g->expr;
        ok = fourier_sample(&wrapper, var, x, out);
    }
    g_eval_refused = refused;
    g_eval_refused_ans = ans;
    return ok;
}

static FOURIER_NOINLINE bool fourier_den_scan(const ast_node_t *expr,
                                             char var, const fourier_den_t *g);

static FOURIER_NOINLINE bool fourier_denominators(const ast_node_t *n,
                                  char var, fourier_den_t *den, unsigned *count)
{
    static struct {
        const ast_node_t *n;
        uint8_t next, vars, left, right;
    } open[FOURIER_WALK_MAX];
    const ast_node_t *expr = n;
    unsigned depth = 0;
    *count = 0;
    for (;;) {
        unsigned bits;
        if (!n || depth == FOURIER_WALK_MAX) return false;
        open[depth].n = n;
        open[depth].next = open[depth].left = open[depth].right = 0;
        open[depth].vars = n->type == NODE_VARIABLE ?
                          (n->d.variable == var ? 3 : 2) : 0;
        depth++;
        for (;;) {
            unsigned top = depth - 1;
            const ast_node_t *g = NULL;
            uint8_t fn = 0;
            n = open[top].n;
            if (open[top].next < 4) {
                const ast_node_t *child = fourier_child(n, open[top].next++);
                if (child) { n = child; break; }
                continue;
            }
            if (n->type == NODE_BINOP) {
                if (n->d.binop.op == OP_DIV && (open[top].right & 1))
                    g = n->d.binop.right;
                if (n->d.binop.op == OP_POW && (open[top].left & 1)) {
                    ce_float_t exponent;
                    fourier_den_t advisory = { n->d.binop.right, n, 0 };
                    /* A variable-free exponent is advisory too: a lazy f
                     * can skip it even when this isolated probe hits a cap. */
                    if ((open[top].right & 2) ||
                        !fourier_den_sample(&advisory, var, 0, &exponent) || exponent < 0)
                        g = n->d.binop.left;
                }
            } else if (n->type == NODE_FUNC && (open[top].vars & 1)) {
                switch (n->d.func.id) {
                case FN_TAN: case FN_SEC: fn = FN_COS + 1; break;
                case FN_CSC: case FN_COT: fn = FN_SIN + 1; break;
                case FN_CSCH: case FN_COTH: fn = FN_SINH + 1; break;
                default: break; /* sech has no real poles; logs are integrable */
                }
                if (fn) g = n->d.func.arg;
            }
            if (g) {
                unsigned i;
                /* Normalize explicit cos/sin/sinh denominators and implicit
                 * pole-function wrappers before comparing equal owners. */
                if (!fn && g->type == NODE_FUNC &&
                    (g->d.func.id == FN_COS || g->d.func.id == FN_SIN ||
                     g->d.func.id == FN_SINH)) {
                    fn = g->d.func.id + 1;
                    g = g->d.func.arg;
                }
                /* Subtrees already passed the 64-level walk bound, so
                 * ast_same_exact recursion is bounded by the same depth. */
                for (i = 0; i < *count; i++)
                    if (den[i].fn == fn && ast_same_exact(den[i].expr, g) &&
                        ast_same_exact(den[i].owner, n)) break;
                if (i == *count) {
                    if (i == FOURIER_DEN_MAX) {
                        fourier_den_t extra;
                        extra.expr = g;
                        extra.fn = fn;
                        extra.owner = n;
                        if (!fourier_den_scan(expr, var, &extra)) return false;
                    } else {
                        den[i].expr = g;
                        den[i].fn = fn;
                        den[i].owner = n;
                        (*count)++;
                    }
                }
            }
            bits = open[top].vars;
            if (--depth == 0) return true;
            top--;
            open[top].vars |= bits;
            if (open[top].next == 1) open[top].left = bits;
            if (open[top].next == 2) open[top].right = bits;
        }
    }
}

/* h/16384 is about 3.83e-6, over 16 binary32 ULPs at pi.
 * Five offset samples cancel linear trends and require two growth ratios, without f(c).
 * Binary32 probes cannot distinguish an unresolved narrow bounded peak
 * from a pole; Simpson's coarser grid cannot integrate that peak either.
 * Accepted limits: a denominator search can miss a second zero in one cell
 * when the first is removable; signals below the binary32 noise floor pass.
 * A background tuned to the probe scales can hide a pole in the full f.
 * Singularities with 0.86165 < p < 1 may refuse: coarse Simpson is inaccurate.
 * Local sum/product bindings are unavailable to denominator sampling, and
 * the 64-entry traversal guard can refuse otherwise evaluable lazy ASTs.
 * Any failed/non-finite integrand probe still refuses, including an unrelated
 * integrable singularity at an offset. No sentinel is used. */
static FOURIER_NOINLINE bool fourier_growth_safe(const ast_node_t *expr,
                                     char var, ce_float_t c, ce_float_t h, bool owner)
{
    int side;
    bool pole = false;
    if (c < -CE_PI) c = -CE_PI;
    if (c > CE_PI) c = CE_PI;
    for (side = -1; side <= 1; side += 2) {
        ce_float_t d = side * (h / 4.0f), v[5], magnitude = 0;
        ce_float_t e0, e1, e2, e3, f0, f1, f2;
        int k;
        if (c + d < -CE_PI || c + d > CE_PI) continue;
        for (k = 0; k < 5; k++, d /= 8.0f) {
            ce_float_t value;
            if (!fourier_sample(expr, var, c + d, &value)) return owner;
            if (fabsf(value) > magnitude) magnitude = fabsf(value);
            /* Quarter scaling bounds E and F even for opposite finite
             * extremes. Scale the noise floor identically below. */
            v[k] = value * 0.25f;
        }
        e0 = v[0] - v[1];
        e1 = v[1] - v[2];
        e2 = v[2] - v[3];
        e3 = v[3] - v[4];
        f0 = e1 - e0 / 8.0f;
        f1 = e2 - e1 / 8.0f;
        f2 = e3 - e2 / 8.0f;
        if (!ce_isfinite(f0) || !ce_isfinite(f1) || !ce_isfinite(f2)) return owner;
        /* Owner evidence need only be unbounded (e.g. an integrable sqrt
         * reciprocal raised to an outer fourth power). Full f must still
         * have non-integrable growth and grow in absolute magnitude. */
        if (((f0 > 0 && f1 > 0 && f2 > 0) || (f0 < 0 && f1 < 0 && f2 < 0)) &&
            fabsf(f1) / (owner ? 1.0f : 6.0f) > fabsf(f0) &&
            fabsf(f2) / (owner ? 1.0f : 6.0f) > fabsf(f1) &&
            (owner || fabsf(v[4]) > fabsf(v[0])) &&
            fabsf(f2) > (magnitude * 0.25f) / 131072.0f) {
            if (!owner) return false;
            pole = true;
        }
    }
    return !pole;
}

/* A failed owner probe supplies no pole evidence. A failed full-integrand
 * probe still refuses. Identical owner/root nodes need only one test. */
static FOURIER_NOINLINE bool fourier_owner_safe(const ast_node_t *expr, char var,
                    const fourier_den_t *g, ce_float_t c, ce_float_t h)
{
    if (g->owner != expr) {
        bool refused = g_eval_refused;
        ce_float_t ans = g_eval_refused_ans;
        bool safe = fourier_growth_safe(g->owner, var, c, h, true);
        /* A Q-only depth failure is no pole evidence, even when eval's
         * sticky backstop flag would otherwise override the real result. */
        if (g_eval_refused && !refused) safe = true;
        g_eval_refused = refused;
        g_eval_refused_ans = ans;
        if (safe) return true;
    }
    return fourier_growth_safe(expr, var, c, h, false);
}

static FOURIER_NOINLINE bool fourier_zero_safe(const ast_node_t *expr,
                    char var, const fourier_den_t *g, ce_float_t lo,
                    ce_float_t hi, ce_float_t flo, ce_float_t scale,
                    ce_float_t h, bool bracket)
{
    ce_float_t c = lo, value;
    int i;
    /* Resolve the candidate to float spacing: a 24-step ternary interval
     * can be as wide as the finest probe and inflate fractional orders. */
    for (i = 0; i < 40; i++) {
        if (bracket) {
            c = lo + (hi - lo) / 2.0f;
            /* A pole or domain gap of g is not itself a zero of g. */
            if (!fourier_den_sample(g, var, c, &value)) return true;
            if (value == 0) break;
            if ((value < 0) == (flo < 0)) { lo = c; flo = value; }
            else hi = c;
        } else {
            ce_float_t third = (hi - lo) / 3.0f;
            ce_float_t a = lo + third, b = hi - third, fa, fb;
            if (!fourier_den_sample(g, var, a, &fa) ||
                !fourier_den_sample(g, var, b, &fb)) return true;
            if (fabsf(fa) > fabsf(fb)) lo = a;
            else hi = b;
            c = lo + (hi - lo) / 2.0f;
        }
    }
    if (!fourier_den_sample(g, var, c, &value) ||
        fabsf(value) > 0.001f * scale) return true;
    return fourier_owner_safe(expr, var, g, c, h);
}

static FOURIER_NOINLINE bool fourier_den_scan(const ast_node_t *expr,
                                             char var, const fourier_den_t *g)
{
    ce_float_t h = (CE_PI - (-CE_PI)) / SIMPSON_N, scale = 0;
    ce_float_t previous = 0, before = 0;
    bool prev_ok = false, before_ok = false;
    int i;
    /* First stream establishes the global cost-filter scale. No grid array. */
    for (i = 0; i <= SIMPSON_N; i++) {
        ce_float_t value;
        if (fourier_den_sample(g, var, -CE_PI + i * h, &value) && fabsf(value) > scale)
            scale = fabsf(value);
    }
    for (i = 0; i <= SIMPSON_N; i++) {
        ce_float_t x = -CE_PI + i * h, current = 0;
        bool ok = fourier_den_sample(g, var, x, &current);
        if (ok && current == 0 && !fourier_owner_safe(expr, var, g, x, h)) return false;
        if (ok && prev_ok) {
            ce_float_t px = -CE_PI + (i - 1) * h;
            if (((previous < 0 && current > 0) || (previous > 0 && current < 0)) &&
                !fourier_zero_safe(expr, var, g, px, x, previous, scale, h, true)) return false;
            if (((i == 1 && fabsf(previous) <= fabsf(current)) ||
                 (i > 1 && before_ok && fabsf(previous) <= fabsf(before) &&
                  fabsf(previous) <= fabsf(current) &&
                  (fabsf(previous) < fabsf(before) || fabsf(previous) < fabsf(current)))) &&
                !fourier_zero_safe(expr, var, g, i == 1 ? -CE_PI : -CE_PI + (i - 2) * h,
                                   x, 0, scale, h, false)) return false;
            if (i == SIMPSON_N && fabsf(current) <= fabsf(previous) &&
                !fourier_zero_safe(expr, var, g, px, x, 0, scale, h, false)) return false;
        }
        before = previous; before_ok = prev_ok;
        previous = current; prev_ok = ok;
    }
    return true;
}

static FOURIER_NOINLINE bool fourier_pole_scan(const ast_node_t *expr, char var)
{
    static fourier_den_t den[FOURIER_DEN_MAX];
    unsigned count, i;
    if (!fourier_denominators(expr, var, den, &count)) return false;
    for (i = 0; i < count; i++)
        if (!fourier_den_scan(expr, var, &den[i])) return false;
    return true;
}

/* Simpson's rule for int(f(x)*g(x), -pi, pi). A sample that fails to
 * evaluate (domain, pole, recursion limit) makes the integral unknown, not
 * 0: return false. (No inf sentinel: the CE's float routines turn inf
 * operands into finite values, src/ce_float_ops.h.) */
static bool simpson_integral(const ast_node_t *expr, char var,
                             ce_float_t mul_freq, bool use_cos, ce_float_t *out)
{
    int i;
    ce_float_t a = -CE_PI, b = CE_PI;
    ce_float_t h = (b - a) / SIMPSON_N;
    ce_float_t sum = 0;
    eval_ctx_t ctx;

    for (i = 0; i <= SIMPSON_N; i++) {
        ce_float_t x = a + i * h;
        ce_float_t w = (i == 0 || i == SIMPSON_N) ? 1.0f :
                       (i % 2 == 1) ? 4.0f : 2.0f;
        ce_float_t fx, gx;

        eval_init(&ctx);
        eval_set_var(&ctx, var, x);
        fx = eval_expr(&ctx, expr);
        if (eval_error(&ctx) != CE_OK || !ce_isfinite(fx)) return false;

        gx = use_cos ? cosf(mul_freq * x) : sinf(mul_freq * x);
        sum += w * fx * gx;
    }

    *out = sum * h / 3.0f / CE_PI;
    return ce_isfinite(*out);
}

ast_node_t *fourier_series(const ast_node_t *expr, char var,
                           int n_terms)
{
    ast_node_t *result;
    ce_float_t  a0, ak, bk;
    int         k;
    char        buf[60];

    if (n_terms > 10) {
        n_terms = 10;
        steps_addf("Note: harmonics capped at 10");
    } else {
    }
    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Fourier of %s, %d terms", buf, n_terms);

    if (!fourier_pole_scan(expr, var)) return NULL;

    /* a_0 = (1/pi) * int(f(x), -pi, pi) */
    if (!simpson_integral(expr, var, 0, true, &a0)) return NULL;
    steps_addf("a0/2 = %.6g", (double)(a0 / 2));

    result = ast_number(a0 / 2);

    for (k = 1; k <= n_terms; k++) {
        /* a_k = (1/pi) * int(f(x)*cos(kx), -pi, pi),
         * b_k = (1/pi) * int(f(x)*sin(kx), -pi, pi) */
        if (!simpson_integral(expr, var, (ce_float_t)k, true, &ak) ||
            !simpson_integral(expr, var, (ce_float_t)k, false, &bk)) {
            ast_free(result);
            return NULL;
        }

        if (fabsf(ak) > 1e-6f) {
            ast_node_t *cos_term = ast_binop(OP_MUL,
                ast_number(ak),
                ast_func(FN_COS,
                    ast_binop(OP_MUL, ast_number((ce_float_t)k),
                              ast_variable(var))));
            result = ast_binop(OP_ADD, result, cos_term);
            steps_addf("a%d = %.6g", k, (double)ak);
        }

        if (fabsf(bk) > 1e-6f) {
            ast_node_t *sin_term = ast_binop(OP_MUL,
                ast_number(bk),
                ast_func(FN_SIN,
                    ast_binop(OP_MUL, ast_number((ce_float_t)k),
                              ast_variable(var))));
            result = ast_binop(OP_ADD, result, sin_term);
            steps_addf("b%d = %.6g", k, (double)bk);
        }
    }

    result = ast_simplify(result);

    ast_to_str(result, buf, sizeof(buf));
    steps_addf("= %s", buf);

    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*                 POWER SERIES RADIUS                        */
/* ═══════════════════════════════════════════════════════════ */

ce_float_t power_series_radius(const ast_node_t *coeff_expr, char var)
{
    ce_error_t e1, e2;
    ce_float_t an, an1, ratio1, ratio2;

    /* R = lim |a_n / a_{n+1}|
     * Start with small n to avoid float overflow (e.g. n! terms),
     * then verify with slightly larger n.
     */
    an  = eval_at(coeff_expr, var, 5, &e1);
    an1 = eval_at(coeff_expr, var, 6, &e2);
    if (e1 != CE_OK || e2 != CE_OK || fabsf(an1) < 1e-30f)
        return 0;
    ratio1 = fabsf(an / an1);

    an  = eval_at(coeff_expr, var, 10, &e1);
    an1 = eval_at(coeff_expr, var, 11, &e2);
    if (e1 != CE_OK || e2 != CE_OK || fabsf(an1) < 1e-30f) {
        /* If evaluation fails at n=10, use only the first ratio */
        steps_addf("R.O.C. = lim|a(n)/a(n+1)| ~ %.6g", (double)ratio1);
        return ratio1;
    }
    ratio2 = fabsf(an / an1);

    /* Check if ratio is growing (R → ∞) */
    if (ratio2 > ratio1 * 1.5f && ratio2 > 5.0f) {
        /* Ratio is diverging — R is very large / infinite */
        steps_addf("R.O.C. = lim|a(n)/a(n+1)| -> inf (est %.6g)", (double)ratio2);
        return ratio2;
    }

    /* Check convergence of the ratio */
    if (fabsf(ratio1 - ratio2) > 0.5f * (1 + fabsf(ratio2)))
        return 0;  /* didn't converge */

    steps_addf("R.O.C. = lim|a(n)/a(n+1)| = %.6g", (double)ratio2);
    return ratio2;
}

/* ═══════════════════════════════════════════════════════════ */
/*              LAGRANGE ERROR BOUND                           */
/* ═══════════════════════════════════════════════════════════ */

ce_float_t taylor_lagrange_error(const ast_node_t *expr, char var,
                                  ce_float_t center, int deg,
                                  ce_float_t x)
{
    /*
     * |R_n(x)| <= M * |x - center|^(n+1) / (n+1)!
     * where M = max |f^(n+1)(c)| for c between center and x.
     *
     * We estimate M by sampling f^(n+1) at several points in [center, x].
     */
    ast_node_t *current;
    eval_ctx_t ctx;
    ce_float_t M, fact, dx, bound;
    ce_float_t lo, hi, step;
    int k, samples;

    if (deg > MAX_TAYLOR_DEG) deg = MAX_TAYLOR_DEG;

    steps_addf("Lagrange error for degree %d at x=%.4g", deg, (double)x);

    /* Compute the (n+1)th derivative */
    current = ast_clone(expr);
    for (k = 0; k <= deg; k++) {
        int sc = steps_count();
        ast_node_t *d = deriv_diff_quiet(current, var);
        steps_restore(sc);
        ast_free(current);
        current = d;
        if (!current) {
            steps_addf("Cannot compute derivative of order %d", k + 1);
            return -1.0f;
        }
    }

    /* Sample the (n+1)th derivative to find M */
    lo = (center < x) ? center : x;
    hi = (center < x) ? x : center;
    if (fabsf(hi - lo) < 1e-10f) {
        ast_free(current);
        return 0;
    }

    samples = 20;
    step = (hi - lo) / samples;
    M = 0;

    for (k = 0; k <= samples; k++) {
        ce_float_t c = lo + k * step;
        ce_float_t v;
        eval_init(&ctx);
        eval_set_var(&ctx, var, c);
        v = eval_expr(&ctx, current);
        if (eval_error(&ctx) == CE_OK && ce_isfinite(v)) {
            if (fabsf(v) > M) M = fabsf(v);
        }
    }
    ast_free(current);

    steps_addf("M = max|f^(%d)(c)| = %.6g", deg + 1, (double)M);

    /* Compute (n+1)! */
    fact = 1.0f;
    for (k = 1; k <= deg + 1; k++) fact *= k;

    dx = fabsf(x - center);
    bound = M * powf(dx, (ce_float_t)(deg + 1)) / fact;

    /* %.9g: (n+1)! prints as a whole number up to 12! (CE has no %f). */
    steps_addf("|R_%d(%.4g)| <= %.6g * %.6g / %.9g",
               deg, (double)x, (double)M,
               (double)powf(dx, (ce_float_t)(deg + 1)), (double)fact);
    steps_addf("Error bound = %.6g", (double)bound);

    return bound;
}

/* ═══════════════════════════════════════════════════════════ */
/*            INTERVAL OF CONVERGENCE                          */
/* ═══════════════════════════════════════════════════════════ */

ce_float_t power_series_interval(const ast_node_t *coeff_expr, char var,
                                  ce_float_t center,
                                  char *buf, int buf_size)
{
    ce_float_t R;
    ce_float_t left_x, right_x;
    bool left_conv = false, right_conv = false;
    int sc;

    sc = steps_count();
    R = power_series_radius(coeff_expr, var);
    steps_restore(sc);

    if (R <= 0 || !ce_isfinite(R)) {
        snprintf(buf, buf_size, "Cannot determine");
        return 0;
    }

    steps_addf("Radius of convergence R = %.6g", (double)R);

    /* If R is very large, interval is all reals */
    if (R > 1e6f) {
        snprintf(buf, buf_size, "(-inf, inf)");
        steps_addf("R -> inf: converges everywhere");
        return R;
    }

    left_x  = center - R;
    right_x = center + R;

    /* Test left endpoint: plug x = center - R into the power series
     * coefficients become a_n * (-R)^n = a_n * R^n * (-1)^n
     * We test convergence of sum(a_n * R^n * (-1)^n) */
    {
        ce_float_t sum = 0;
        ce_error_t err;
        int i;

        for (i = 1; i <= 200; i++) {
            ce_float_t an = eval_at(coeff_expr, var, (ce_float_t)i, &err);
            if (err != CE_OK) break;
            /* Term: a_n * R^n * (-1)^n */
            ce_float_t term = an * powf(R, (ce_float_t)i);
            if (i % 2 == 1) term = -term;
            sum += term;

        }

        /* Check if partial sums converge */
        {
            ce_float_t sum2 = 0;
            for (i = 1; i <= 400; i++) {
                ce_float_t an = eval_at(coeff_expr, var, (ce_float_t)i, &err);
                if (err != CE_OK) break;
                ce_float_t term = an * powf(R, (ce_float_t)i);
                if (i % 2 == 1) term = -term;
                sum2 += term;
            }
            if (fabsf(sum2 - sum) < 0.5f * (1 + fabsf(sum))) {
                left_conv = true;
                steps_addf("Left endpoint x=%.4g: CONVERGES", (double)left_x);
            } else {
                steps_addf("Left endpoint x=%.4g: DIVERGES", (double)left_x);
            }
        }
    }

    /* Test right endpoint: sum(a_n * R^n) */
    {
        ce_float_t sum = 0;
        ce_error_t err;
        int i;

        for (i = 1; i <= 200; i++) {
            ce_float_t an = eval_at(coeff_expr, var, (ce_float_t)i, &err);
            if (err != CE_OK) break;
            ce_float_t term = an * powf(R, (ce_float_t)i);
            sum += term;
        }
        {
            ce_float_t sum2 = 0;
            for (i = 1; i <= 400; i++) {
                ce_float_t an = eval_at(coeff_expr, var, (ce_float_t)i, &err);
                if (err != CE_OK) break;
                ce_float_t term = an * powf(R, (ce_float_t)i);
                sum2 += term;
            }
            if (fabsf(sum2 - sum) < 0.5f * (1 + fabsf(sum))) {
                right_conv = true;
                steps_addf("Right endpoint x=%.4g: CONVERGES", (double)right_x);
            } else {
                steps_addf("Right endpoint x=%.4g: DIVERGES", (double)right_x);
            }
        }
    }

    /* Format interval notation */
    snprintf(buf, buf_size, "%c%.4g, %.4g%c",
             left_conv ? '[' : '(',
             (double)left_x, (double)right_x,
             right_conv ? ']' : ')');

    steps_addf("Interval: %s", buf);

    return R;
}

/* ═══════════════════════════════════════════════════════════ */
/*              SEQUENCE CONVERGENCE                           */
/* ═══════════════════════════════════════════════════════════ */

/*
 * Evaluate lim(n→∞) a_n for a sequence.
 * Samples a_n at increasingly large values and checks convergence.
 */
seq_limit_t series_sequence_limit(const ast_node_t *expr, char var)
{
    seq_limit_t result = { SEQ_FAILED, 0, false };
    ce_error_t err;
    ce_float_t v1, v2, v3, v4;
    char buf[80];

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Sequence limit: lim(%c->inf) %s", var, buf);

    /* Evaluate at several large values */
    v1 = eval_at(expr, var, 100.0f, &err);
    if (err != CE_OK) {
        steps_addf("Cannot evaluate a_100");
        return result;
    }
    steps_addf("a_100 = %.6g", (double)v1);

    v2 = eval_at(expr, var, 1000.0f, &err);
    if (err != CE_OK) {
        steps_addf("Cannot evaluate a_1000");
        return result;
    }
    steps_addf("a_1000 = %.6g", (double)v2);

    v3 = eval_at(expr, var, 10000.0f, &err);
    if (err != CE_OK) {
        steps_addf("Cannot evaluate a_10000");
        return result;
    }
    steps_addf("a_10000 = %.6g", (double)v3);

    v4 = eval_at(expr, var, 100000.0f, &err);
    if (err != CE_OK) v4 = v3;  /* use v3 if v4 fails */

    /* Check for divergence: values growing without bound */
    if (!ce_isfinite(v2) || !ce_isfinite(v3)) {
        result.status = SEQ_DIVERGES;
        steps_addf("Sequence DIVERGES (values unbounded)");
        return result;
    }

    /* Check for oscillation: values not settling */
    if (fabsf(v3 - v2) > fabsf(v2 - v1) && fabsf(v2) > 1e-6f) {
        /* Diverging differences → diverges or oscillates */
        result.status = SEQ_DIVERGES;
        steps_addf("Sequence DIVERGES (values not converging)");
        return result;
    }

    /* Check convergence: v3 and v4 should be close */
    if (fabsf(v4 - v3) < 0.001f * (1.0f + fabsf(v3)) &&
        fabsf(v3 - v2) < 0.01f * (1.0f + fabsf(v3))) {
        result.status = SEQ_CONVERGES;
        result.limit_value = v4;
        result.has_limit = true;

        /* Check if limit is zero */
        if (fabsf(v4) < 1e-5f) {
            result.limit_value = 0.0f;
            steps_addf("Sequence CONVERGES to 0");
        } else {
            steps_addf("Sequence CONVERGES to %.6g", (double)v4);
        }
        return result;
    }

    /* Check if values are growing slowly (diverges but slowly) */
    if ((v3 > v2 && v2 > v1 && v3 > v1 + 0.1f) ||
        (v3 < v2 && v2 < v1 && v3 < v1 - 0.1f)) {
        result.status = SEQ_DIVERGES;
        steps_addf("Sequence DIVERGES (slow growth)");
        return result;
    }

    /* Borderline — treat as convergent with best estimate */
    result.status = SEQ_CONVERGES;
    result.limit_value = v3;
    result.has_limit = true;
    steps_addf("Sequence appears to converge to %.6g",
               (double)v3);
    return result;
}

/* ═══════════════════════════════════════════════════════════ */
/*              RECURSIVE SEQUENCE EVALUATION                  */
/* ═══════════════════════════════════════════════════════════ */

int series_recseq(const ast_node_t *expr, char var,
                  ce_float_t init, int k,
                  ce_float_t *out_terms, int max_terms)
{
    int i, count;
    char buf[80];

    if (!expr || k < 0 || !out_terms || max_terms < 1) return 0;

    /* Cap k to avoid runaway computation */
    if (k > 200) k = 200;
    count = (k + 1 < max_terms) ? k + 1 : max_terms;

    ast_to_str(expr, buf, sizeof(buf));
    steps_addf("Recursive seq: s(0)=%.6g, s(n)=%s", (double)init, buf);

    out_terms[0] = init;
    steps_addf("s(0) = %.6g", (double)init);

    for (i = 1; i < count; i++) {
        ce_error_t err;
        ce_float_t val = eval_at(expr, var, out_terms[i - 1], &err);
        if (err != CE_OK || !ce_isfinite(val)) {
            steps_addf("s(%d): evaluation failed", i);
            return i;
        }
        out_terms[i] = val;
        if (i <= 10) {
            steps_addf("s(%d) = %.6g", i, (double)val);
        }
    }

    if (count > 11)
        steps_addf("... (showing first 10 terms)");

    /* Check convergence */
    if (count >= 3) {
        ce_float_t last = out_terms[count - 1];
        ce_float_t prev = out_terms[count - 2];
        if (fabsf(last - prev) < 1e-6f * (1.0f + fabsf(last))) {
            steps_addf("Sequence converges to %.6g", (double)last);
        }
    }

    steps_addf("Computed %d terms", count);
    return count;
}
