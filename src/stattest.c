/*
 * stattest.c — Hypothesis tests and confidence intervals
 *
 * Implements z-test, t-test, 2-sample tests, proportion tests,
 * chi-squared goodness-of-fit, and confidence intervals.
 */
#include "stattest.h"
#include "stats.h"
#include "prob.h"
#include <math.h>
#include <string.h>

/* ── Promoted prob helpers ───────────────────────────────── */

ce_float_t prob_std_normal_pdf(ce_float_t x)
{
    return (1.0f / sqrtf(2.0f * CE_PI)) * expf(-0.5f * x * x);
}

ce_float_t prob_std_normal_cdf(ce_float_t x)
{
    ce_float_t t, d, p;
    static const ce_float_t b1 =  0.319381530f;
    static const ce_float_t b2 = -0.356563782f;
    static const ce_float_t b3 =  1.781477937f;
    static const ce_float_t b4 = -1.821255978f;
    static const ce_float_t b5 =  1.330274429f;

    if (x < -8.0f) return 0.0f;
    if (x >  8.0f) return 1.0f;
    if (x < 0) return 1.0f - prob_std_normal_cdf(-x);

    t = 1.0f / (1.0f + 0.2316419f * x);
    d = prob_std_normal_pdf(x);
    p = d * t * (b1 + t * (b2 + t * (b3 + t * (b4 + t * b5))));
    return 1.0f - p;
}

ce_float_t prob_gamma_half_int(ce_float_t x)
{
    ce_float_t result;
    int n, i;
    if (x == 0) return INFINITY;
    if (x < 0 && fabsf(x - floorf(x)) < 0.01f) return NAN;
    if (fabsf(x - floorf(x)) < 0.01f) {
        n = (int)x;
        result = 1.0f;
        for (i = 2; i < n; i++) result *= (ce_float_t)i;
        return result;
    }
    n = (int)(x - 0.5f);
    result = sqrtf(CE_PI);
    for (i = 0; i < n; i++)
        result *= (2*i + 1) / 2.0f;
    return result;
}

ce_float_t prob_t_pdf(ce_float_t t, ce_float_t df)
{
    ce_float_t coeff = prob_gamma_half_int((df + 1.0f) / 2.0f) /
                       (sqrtf(df * CE_PI) * prob_gamma_half_int(df / 2.0f));
    return coeff * powf(1.0f + t*t/df, -(df + 1.0f) / 2.0f);
}

ce_float_t prob_chi2_pdf(ce_float_t x, ce_float_t df)
{
    ce_float_t k2 = df / 2.0f;
    if (x <= 0) return 0;
    /* 2^k2 as expf(k2*ln 2): clang turns powf(2.0f, x) into exp2f(x), and
     * CEdev v11.2 exp2f computes expf(x*log2(e)) instead of expf(x*ln 2),
     * so exp2f(1) gives about 4.23 instead of 2 on the CE. */
    return (1.0f / (expf(k2 * 0.693147182f) * prob_gamma_half_int(k2))) *
           powf(x, k2 - 1.0f) * expf(-x / 2.0f);
}

/* Inverse normal using Acklam's approximation */
ce_float_t prob_inv_normal(ce_float_t area)
{
    ce_float_t z, r, q;
    static const ce_float_t a1 = -3.969683028665376e+01f;
    static const ce_float_t a2 =  2.209460984245205e+02f;
    static const ce_float_t a3 = -2.759285104469687e+02f;
    static const ce_float_t a4 =  1.383577518672690e+02f;
    static const ce_float_t a5 = -3.066479806614716e+01f;
    static const ce_float_t a6 =  2.506628277459239e+00f;
    static const ce_float_t b1 = -5.447609879822406e+01f;
    static const ce_float_t b2 =  1.615858368580409e+02f;
    static const ce_float_t b3 = -1.556989798598866e+02f;
    static const ce_float_t b4 =  6.680131188771972e+01f;
    static const ce_float_t b5 = -1.328068155288572e+01f;
    static const ce_float_t c1 = -7.784894002430293e-03f;
    static const ce_float_t c2 = -3.223964580411365e-01f;
    static const ce_float_t c3 = -2.400758277161838e+00f;
    static const ce_float_t c4 = -2.549732539343734e+00f;
    static const ce_float_t c5 =  4.374664141464968e+00f;
    static const ce_float_t c6 =  2.938163982698783e+00f;
    static const ce_float_t d1 =  7.784695709041462e-03f;
    static const ce_float_t d2 =  3.224671290700398e-01f;
    static const ce_float_t d3 =  2.445134137142996e+00f;
    static const ce_float_t d4 =  3.754408661907416e+00f;

    if (area <= 0 || area >= 1) return 0;

    if (area < 0.02425f) {
        q = sqrtf(-2.0f * logf(area));
        z = (((((c1*q+c2)*q+c3)*q+c4)*q+c5)*q+c6) /
            ((((d1*q+d2)*q+d3)*q+d4)*q+1.0f);
    } else if (area > 0.97575f) {
        q = sqrtf(-2.0f * logf(1.0f - area));
        z = -(((((c1*q+c2)*q+c3)*q+c4)*q+c5)*q+c6) /
            ((((d1*q+d2)*q+d3)*q+d4)*q+1.0f);
    } else {
        q = area - 0.5f;
        r = q * q;
        z = (((((a1*r+a2)*r+a3)*r+a4)*r+a5)*r+a6) * q /
            (((((b1*r+b2)*r+b3)*r+b4)*r+b5)*r+1.0f);
    }
    return z;
}

/* Simpson's rule for t-CDF from -inf(approx) to t */
static ce_float_t t_cdf(ce_float_t t_val, ce_float_t df)
{
    ce_float_t lo = -50.0f, h, sum;
    int steps = 200, i;
    if (t_val <= lo) return 0;
    h = (t_val - lo) / (ce_float_t)steps;
    sum = prob_t_pdf(lo, df) + prob_t_pdf(t_val, df);
    for (i = 1; i < steps; i++) {
        ce_float_t xi = lo + (ce_float_t)i * h;
        sum += ((i % 2 == 1) ? 4.0f : 2.0f) * prob_t_pdf(xi, df);
    }
    return sum * h / 3.0f;
}

/* Chi-squared CDF from 0 to x */
static ce_float_t chi2_cdf(ce_float_t x, ce_float_t df)
{
    ce_float_t h, sum, start;
    int steps = 400, i;

    if (x <= 0) return 0;

    /* Special case df=1 */
    if (fabsf(df - 1.0f) < 0.01f)
        return (x > 0) ? 2.0f * prob_std_normal_cdf(sqrtf(x)) - 1.0f : 0.0f;
    /* Special case df=2 */
    if (fabsf(df - 2.0f) < 0.01f)
        return 1.0f - expf(-x / 2.0f);

    start = (df < 3.0f) ? 0.001f : 0.0f;
    if (start >= x) return 0;
    h = (x - start) / (ce_float_t)steps;
    sum = prob_chi2_pdf(start, df) + prob_chi2_pdf(x, df);
    for (i = 1; i < steps; i++) {
        ce_float_t xi = start + (ce_float_t)i * h;
        sum += ((i % 2 == 1) ? 4.0f : 2.0f) * prob_chi2_pdf(xi, df);
    }
    return sum * h / 3.0f;
}

/* ── Z-test p-value helper ───────────────────────────────── */
static ce_float_t z_pvalue(ce_float_t z, alt_hyp_t alt)
{
    switch (alt) {
    case ALT_LESS:      return prob_std_normal_cdf(z);
    case ALT_GREATER:   return 1.0f - prob_std_normal_cdf(z);
    case ALT_TWO_SIDED:
    default:            return 2.0f * (1.0f - prob_std_normal_cdf(fabsf(z)));
    }
}

/* ── T-test p-value helper ───────────────────────────────── */
static ce_float_t t_pvalue(ce_float_t t_val, ce_float_t df, alt_hyp_t alt)
{
    switch (alt) {
    case ALT_LESS:      return t_cdf(t_val, df);
    case ALT_GREATER:   return 1.0f - t_cdf(t_val, df);
    case ALT_TWO_SIDED:
    default: {
        ce_float_t right_tail = 1.0f - t_cdf(fabsf(t_val), df);
        return 2.0f * right_tail;
    }
    }
}

/* ── Hypothesis tests ────────────────────────────────────── */

ztest_result_t stattest_ztest(ce_float_t xbar, ce_float_t mu0,
                              ce_float_t sigma, ce_float_t n, alt_hyp_t alt)
{
    ztest_result_t r;
    if (n <= 0 || fabsf(sigma) < 1e-10f || sigma < 0) { r.z = 0; r.p = 1; return r; }
    r.z = (xbar - mu0) / (sigma / sqrtf(n));
    r.p = z_pvalue(r.z, alt);
    return r;
}

ttest_result_t stattest_ttest(ce_float_t xbar, ce_float_t mu0,
                              ce_float_t s, ce_float_t n, alt_hyp_t alt)
{
    ttest_result_t r;
    if (n <= 1 || fabsf(s) < 1e-10f || s < 0) { r.t = 0; r.p = 1; r.df = 0; return r; }
    r.df = n - 1.0f;
    r.t = (xbar - mu0) / (s / sqrtf(n));
    r.p = t_pvalue(r.t, r.df, alt);
    return r;
}

zsamp2_result_t stattest_2sampztest(ce_float_t xbar1, ce_float_t sigma1, ce_float_t n1,
                                    ce_float_t xbar2, ce_float_t sigma2, ce_float_t n2,
                                    alt_hyp_t alt)
{
    zsamp2_result_t r;
    ce_float_t se;
    if (n1 <= 0 || n2 <= 0 || sigma1 < 0.0f || sigma2 < 0.0f) {
        r.z = 0;
        r.p = 1;
        return r;
    }
    se = sqrtf(sigma1*sigma1/n1 + sigma2*sigma2/n2);
    if (se < 1e-10f) { r.z = 0; r.p = 1; return r; }
    r.z = (xbar1 - xbar2) / se;
    r.p = z_pvalue(r.z, alt);
    return r;
}

tsamp2_result_t stattest_2sampttest(ce_float_t xbar1, ce_float_t s1, ce_float_t n1,
                                    ce_float_t xbar2, ce_float_t s2, ce_float_t n2,
                                    int pooled, alt_hyp_t alt)
{
    tsamp2_result_t r;
    ce_float_t se;

    if (n1 <= 1 || n2 <= 1 || s1 < 0.0f || s2 < 0.0f) {
        r.t = 0;
        r.p = 1;
        r.df = 0;
        return r;
    }
    if (fabsf(s1) < 1e-10f && fabsf(s2) < 1e-10f) { r.t = 0; r.p = 1; r.df = n1 + n2 - 2; return r; }

    if (pooled) {
        ce_float_t sp2 = ((n1-1)*s1*s1 + (n2-1)*s2*s2) / (n1 + n2 - 2);
        se = sqrtf(sp2 * (1.0f/n1 + 1.0f/n2));
        r.df = n1 + n2 - 2;
    } else {
        /* Welch's approximation for df */
        ce_float_t v1 = s1*s1/n1, v2 = s2*s2/n2;
        ce_float_t num = (v1 + v2) * (v1 + v2);
        ce_float_t den = v1*v1/(n1-1) + v2*v2/(n2-1);
        se = sqrtf(v1 + v2);
        r.df = (den > 0) ? num / den : 1.0f;
    }
    r.t = (xbar1 - xbar2) / se;
    r.p = t_pvalue(r.t, r.df, alt);
    return r;
}

prop1z_result_t stattest_1propztest(ce_float_t x, ce_float_t n,
                                    ce_float_t p0, alt_hyp_t alt)
{
    prop1z_result_t r;
    ce_float_t phat, se;
    if (n <= 0 || p0 < 0.0f || p0 > 1.0f || x < 0.0f || x > n) {
        r.z = 0;
        r.p = 1;
        return r;
    }
    phat = x / n;
    se = sqrtf(p0 * (1 - p0) / n);
    r.z = (se > 0) ? (phat - p0) / se : 0;
    r.p = z_pvalue(r.z, alt);
    if (!ce_isfinite(r.p) || r.p < 0.0f || r.p > 1.0f) r.p = 1.0f;
    return r;
}

prop2z_result_t stattest_2propztest(ce_float_t x1, ce_float_t n1,
                                    ce_float_t x2, ce_float_t n2, alt_hyp_t alt)
{
    prop2z_result_t r;
    ce_float_t p1, p2, ppool, se;
    if (n1 <= 0 || n2 <= 0 ||
        x1 < 0.0f || x1 > n1 || x2 < 0.0f || x2 > n2) {
        r.z = 0;
        r.p = 1;
        return r;
    }
    p1 = x1 / n1; p2 = x2 / n2;
    ppool = (x1 + x2) / (n1 + n2);
    se = sqrtf(ppool * (1 - ppool) * (1.0f/n1 + 1.0f/n2));
    r.z = (se > 0) ? (p1 - p2) / se : 0;
    r.p = z_pvalue(r.z, alt);
    if (!ce_isfinite(r.p) || r.p < 0.0f || r.p > 1.0f) r.p = 1.0f;
    return r;
}

chi2_result_t stattest_chi2test(const ce_float_t *observed,
                                const ce_float_t *expected, int k)
{
    chi2_result_t r;
    int i;
    if (k <= 0) { r.chi2 = 0; r.df = 0; r.p = 1; return r; }
    r.chi2 = 0;
    r.df = (ce_float_t)(k - 1);
    for (i = 0; i < k; i++) {
        ce_float_t diff = observed[i] - expected[i];
        if (expected[i] > 0)
            r.chi2 += diff * diff / expected[i];
    }
    r.p = 1.0f - chi2_cdf(r.chi2, r.df);
    return r;
}

/* ── Inverse t-distribution (bisection on t_cdf) ─────────── */
static ce_float_t inv_t(ce_float_t area, ce_float_t df)
{
    ce_float_t lo, hi, mid, cdf_val, rdf;
    int iter;

    /* For large df (>=30), use Wilson-Hilferty correction on the
     * normal quantile.  This gives t ≈ z*(1 + z²/(4*df)), which is
     * significantly more accurate than the raw normal z for moderate df. */
    if (df >= 30.0f) {
        ce_float_t z = prob_inv_normal(area);
        return z * (1.0f + (z * z) / (4.0f * df));
    }

    /* prob_gamma_half_int only handles integer/half-integer args.
     * Round df to nearest integer for correct gamma computation. */
    rdf = floorf(df + 0.5f);
    if (rdf < 1.0f) rdf = 1.0f;

    /* Bisection: find t such that t_cdf(t, rdf) = area */
    lo = -100.0f; hi = 100.0f;
    for (iter = 0; iter < 80; iter++) {
        mid = (lo + hi) / 2.0f;
        cdf_val = t_cdf(mid, rdf);
        if (cdf_val < area)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) / 2.0f;
}

/* ── Confidence intervals ────────────────────────────────── */

ci_result_t stattest_zinterval(ce_float_t xbar, ce_float_t sigma,
                               ce_float_t n, ce_float_t clevel)
{
    ci_result_t r;
    ce_float_t alpha, z_star;
    if (n <= 0 || sigma < 0.0f || clevel <= 0 || clevel >= 1) {
        r.me = 0; r.center = xbar; r.lower = xbar; r.upper = xbar; return r;
    }
    alpha = 1.0f - clevel;
    z_star = prob_inv_normal(1.0f - alpha/2.0f);
    r.me = z_star * sigma / sqrtf(n);
    r.center = xbar;
    r.lower = xbar - r.me;
    r.upper = xbar + r.me;
    return r;
}

ci_result_t stattest_tinterval(ce_float_t xbar, ce_float_t s,
                               ce_float_t n, ce_float_t clevel)
{
    ci_result_t r;
    ce_float_t alpha, t_star, df;
    if (n <= 1 || s < 0.0f || clevel <= 0 || clevel >= 1) {
        r.me = 0; r.center = xbar; r.lower = xbar; r.upper = xbar; return r;
    }
    alpha = 1.0f - clevel;
    df = n - 1.0f;
    t_star = inv_t(1.0f - alpha/2.0f, df);
    r.me = t_star * s / sqrtf(n);
    r.center = xbar;
    r.lower = xbar - r.me;
    r.upper = xbar + r.me;
    return r;
}

ci_result_t stattest_1propzint(ce_float_t x, ce_float_t n, ce_float_t clevel)
{
    ci_result_t r;
    ce_float_t alpha, phat, z_star;
    if (n <= 0 || clevel <= 0 || clevel >= 1 || x < 0.0f || x > n) {
        r.me = 0;
        r.center = (n > 0) ? (x / n) : 0;
        r.lower = r.center;
        r.upper = r.center;
        return r;
    }
    alpha = 1.0f - clevel;
    phat = x / n;
    z_star = prob_inv_normal(1.0f - alpha/2.0f);
    r.me = z_star * sqrtf(phat * (1 - phat) / n);
    r.center = phat;
    r.lower = phat - r.me;
    r.upper = phat + r.me;
    return r;
}

ci_result_t stattest_2propzint(ce_float_t x1, ce_float_t n1,
                               ce_float_t x2, ce_float_t n2, ce_float_t clevel)
{
    ci_result_t r;
    ce_float_t alpha, p1, p2, z_star, se;
    if (n1 <= 0 || n2 <= 0 || clevel <= 0 || clevel >= 1 ||
        x1 < 0.0f || x1 > n1 || x2 < 0.0f || x2 > n2) {
        r.center = 0;
        r.me = 0;
        r.lower = 0;
        r.upper = 0;
        return r;
    }
    alpha = 1.0f - clevel;
    p1 = x1 / n1;
    p2 = x2 / n2;
    z_star = prob_inv_normal(1.0f - alpha/2.0f);
    se = sqrtf(p1*(1-p1)/n1 + p2*(1-p2)/n2);
    r.center = p1 - p2;
    r.me = z_star * se;
    r.lower = r.center - r.me;
    r.upper = r.center + r.me;
    return r;
}

ci_result_t stattest_2sampzint(ce_float_t xbar1, ce_float_t sigma1, ce_float_t n1,
                               ce_float_t xbar2, ce_float_t sigma2, ce_float_t n2,
                               ce_float_t clevel)
{
    ci_result_t r;
    ce_float_t alpha, z_star, se;
    if (n1 <= 0 || n2 <= 0 || sigma1 < 0.0f || sigma2 < 0.0f ||
        clevel <= 0 || clevel >= 1) {
        r.center = xbar1 - xbar2;
        r.me = 0;
        r.lower = r.center;
        r.upper = r.center;
        return r;
    }
    alpha = 1.0f - clevel;
    z_star = prob_inv_normal(1.0f - alpha/2.0f);
    se = sqrtf(sigma1*sigma1/n1 + sigma2*sigma2/n2);
    r.center = xbar1 - xbar2;
    r.me = z_star * se;
    r.lower = r.center - r.me;
    r.upper = r.center + r.me;
    return r;
}

ci_result_t stattest_2samptint(ce_float_t xbar1, ce_float_t s1, ce_float_t n1,
                               ce_float_t xbar2, ce_float_t s2, ce_float_t n2,
                               ce_float_t clevel)
{
    ci_result_t r;
    ce_float_t alpha, t_star, se, df;
    ce_float_t v1, v2, num, den;
    if (n1 <= 1 || n2 <= 1 || s1 < 0.0f || s2 < 0.0f ||
        clevel <= 0 || clevel >= 1) {
        r.me = 0; r.center = xbar1 - xbar2; r.lower = r.center; r.upper = r.center; return r;
    }
    alpha = 1.0f - clevel;
    v1 = s1*s1/n1; v2 = s2*s2/n2;
    num = (v1 + v2) * (v1 + v2);
    den = v1*v1/(n1-1) + v2*v2/(n2-1);
    df = (den > 0) ? num / den : 1.0f;
    t_star = inv_t(1.0f - alpha/2.0f, df);
    se = sqrtf(v1 + v2);
    r.center = xbar1 - xbar2;
    r.me = t_star * se;
    r.lower = r.center - r.me;
    r.upper = r.center + r.me;
    return r;
}

/* ── LinReg t-test ──────────────────────────────────────── */

linreg_ttest_result_t stattest_linreg_ttest(const ce_float_t *data, int n)
{
    linreg_ttest_result_t res;
    ce_float_t m, b, r;
    ce_error_t err;
    int pairs;

    memset(&res, 0, sizeof(res));

    err = stat_linreg(data, n, &m, &b, &r);
    if (err != CE_OK) return res;

    pairs = n / 2;
    res.m = m;
    res.b = b;
    res.r = r;
    res.df = (ce_float_t)(pairs - 2);

    if (res.df < 1.0f) return res;

    /* t = r * sqrt(n-2) / sqrt(1 - r^2) */
    {
        ce_float_t r2 = r * r;
        ce_float_t denom = 1.0f - r2;
        if (denom < 1e-10f) {
            /* Perfect correlation: t is infinite, p is 0 */
            res.t = (r >= 0) ? 1e6f : -1e6f;
            res.p = 0.0f;
            return res;
        }
        res.t = r * sqrtf(res.df) / sqrtf(denom);
    }

    /* Two-sided p-value from t-distribution */
    {
        ce_float_t cdf_val = t_cdf(fabsf(res.t), res.df);
        res.p = 2.0f * (1.0f - cdf_val);
        if (res.p < 0.0f) res.p = 0.0f;
        if (res.p > 1.0f) res.p = 1.0f;
    }

    return res;
}

/* ── One-way ANOVA ──────────────────────────────────────── */

anova_result_t stattest_anova(const ce_float_t *const *groups,
                              const int *group_sizes, int k)
{
    anova_result_t res;
    ce_float_t grand_sum = 0.0f;
    int N = 0;
    ce_float_t grand_mean;
    int i, j;

    memset(&res, 0, sizeof(res));

    if (k < 2) return res;

    /* Compute grand mean */
    for (i = 0; i < k; i++) {
        if (group_sizes[i] < 1) return res;
        for (j = 0; j < group_sizes[i]; j++) {
            grand_sum += groups[i][j];
        }
        N += group_sizes[i];
    }
    if (N <= k) return res;  /* need at least k+1 total observations */
    grand_mean = grand_sum / (ce_float_t)N;

    /* Compute SS_between and SS_within */
    res.SS_between = 0.0f;
    res.SS_within = 0.0f;
    for (i = 0; i < k; i++) {
        ce_float_t group_mean = 0.0f;
        for (j = 0; j < group_sizes[i]; j++)
            group_mean += groups[i][j];
        group_mean /= (ce_float_t)group_sizes[i];

        /* SS_between += n_i * (group_mean - grand_mean)^2 */
        {
            ce_float_t diff = group_mean - grand_mean;
            res.SS_between += (ce_float_t)group_sizes[i] * diff * diff;
        }

        /* SS_within += sum((x_ij - group_mean)^2) */
        for (j = 0; j < group_sizes[i]; j++) {
            ce_float_t diff = groups[i][j] - group_mean;
            res.SS_within += diff * diff;
        }
    }

    res.df_between = (ce_float_t)(k - 1);
    res.df_within = (ce_float_t)(N - k);

    res.MS_between = res.SS_between / res.df_between;
    res.MS_within = (res.df_within > 0) ? res.SS_within / res.df_within : 0.0f;

    /* F = MS_between / MS_within */
    if (res.MS_within < 1e-10f) {
        res.F = (res.MS_between > 0) ? 1e6f : 0.0f;
        res.p = (res.MS_between > 0) ? 0.0f : 1.0f;
        return res;
    }
    res.F = res.MS_between / res.MS_within;

    /* p-value from F-distribution: P(F > observed) = 1 - fcdf(0, F, df1, df2)
     * For large F values (>50 with small df), the fcdf integration is essentially 1,
     * so p is effectively 0. We use a direct computation for moderate F values
     * and a shortcut for extreme values to avoid numerical issues with the
     * Simpson's-rule-based fcdf which clamps the integration at 200. */
    if (res.F > 20.0f && res.df_within >= 2.0f) {
        /* F > 20 with reasonable df: p is very small.
         * The Simpson's-rule fcdf loses accuracy for large F with
         * small df1 (singularity at 0), so we use a shortcut. */
        res.p = 0.0f;
    } else {
        ce_float_t args[4];
        eval_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        args[0] = 0.0f;
        args[1] = res.F;
        args[2] = res.df_between;
        args[3] = res.df_within;
        res.p = 1.0f - prob_fcdf(args, 4, &ctx);
        if (res.p < 0.0f) res.p = 0.0f;
        if (res.p > 1.0f) res.p = 1.0f;
    }

    return res;
}

/* ── Public wrapper for inv_t (testing) ──────────────────── */
ce_float_t stattest_inv_t(ce_float_t area, ce_float_t df)
{
    return inv_t(area, df);
}
