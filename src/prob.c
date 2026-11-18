/*
 * prob.c — Probability distribution functions
 *
 * Implements PDF/CDF for normal, binomial, Poisson, geometric,
 * Student's t, and chi-squared distributions.
 * Uses Abramowitz & Stegun approximations for standard normal CDF,
 * Acklam's rational approximation for inverse normal,
 * and Simpson's rule for t and chi-squared CDFs.
 */
#include "prob.h"
#include "stattest.h"
#include <math.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────── */

static void prob_err(eval_ctx_t *ctx, const char *msg)
{
    ctx->error = CE_ERR_DOMAIN;
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
}

static bool prob_arg_to_int(ce_float_t v, int *out)
{
    const ce_float_t int_min_f = -2147483648.0f;
    const ce_float_t int_max_f = 2147483647.0f;
    ce_float_t rounded;
    if (!out || !ce_isfinite(v)) return false;
    if (v < int_min_f || v > int_max_f) return false;
    rounded = roundf(v);
    if (fabsf(v - rounded) > 1e-4f) return false;
    *out = (int)rounded;
    return true;
}

/* Use the canonical implementations from stattest.c (declared in stattest.h)
 * to avoid duplicating the Abramowitz & Stegun approximation. */
#define std_normal_pdf prob_std_normal_pdf
#define std_normal_cdf prob_std_normal_cdf

/* Gamma function for positive real arguments.
 * Uses exact computation for integers and half-integers,
 * Lanczos approximation (g=7, n=9) for general positive reals.
 * Used for t and chi-squared PDF normalization constants. */
static ce_float_t gamma_half_int(ce_float_t x)
{
    ce_float_t result;
    int n, i;

    if (x == 0) return INFINITY;
    if (x < 0 && fabsf(x - floorf(x)) < 0.01f) return NAN;

    /* Check if integer — use exact factorial */
    if (fabsf(x - floorf(x)) < 0.01f && x > 0) {
        n = (int)(x + 0.5f);
        result = 1.0f;
        for (i = 2; i < n; i++) result *= (ce_float_t)i;
        return result;
    }

    /* Check if half-integer — use exact formula */
    if (x > 0 && fabsf(x - floorf(x) - 0.5f) < 0.01f) {
        n = (int)(x - 0.5f + 0.01f);
        result = sqrtf(CE_PI);
        for (i = 0; i < n; i++)
            result *= (2*i + 1) / 2.0f;
        return result;
    }

    /* Lanczos approximation for general positive reals.
     * Gamma(z) = sqrt(2*pi) * (z + g - 0.5)^(z-0.5) * exp(-(z+g-0.5)) * Ag(z)
     * Using g=7 with 9 coefficients (Numerical Recipes). */
    {
        static const double c[9] = {
             0.99999999999980993,
           676.5203681218851,
          -1259.1392167224028,
           771.32342877765313,
          -176.61502916214059,
            12.507343278686905,
            -0.13857109526572012,
             9.9843695780195716e-6,
             1.5056327351493116e-7
        };
        double z = (double)x;
        double ag = c[0];
        double t_val;
        for (i = 1; i < 9; i++)
            ag += c[i] / (z - 1.0 + (double)i);
        t_val = z - 1.0 + 7.5;  /* z + g - 0.5 */
        return (ce_float_t)(sqrt(2.0 * 3.14159265358979) *
               pow(t_val, z - 0.5) * exp(-t_val) * ag);
    }
}

/* Log-space binomial coefficient: log(C(n,k)) */
static ce_float_t log_ncr(int n, int k)
{
    ce_float_t result = 0;
    int i;
    if (k < 0 || k > n) return -1e30f;
    if (k > n - k) k = n - k;
    for (i = 0; i < k; i++)
        result += logf((ce_float_t)(n - i)) - logf((ce_float_t)(i + 1));
    return result;
}

/* ── Normal distribution ─────────────────────────────────── */

/* normalcdf(lo, hi [, mu, sigma]) */
ce_float_t prob_normalcdf(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t lo, hi, mu = 0, sigma = 1;
    if (n < 2) { prob_err(ctx, "normalcdf needs 2-4 args"); return 0; }
    lo = args[0]; hi = args[1];
    if (n >= 3) mu = args[2];
    if (n >= 4) { sigma = args[3]; if (sigma <= 0) { prob_err(ctx, "sigma must be > 0"); return 0; } }
    return std_normal_cdf((hi - mu) / sigma) - std_normal_cdf((lo - mu) / sigma);
}

/* invnorm(area [, mu, sigma]) — Acklam's rational approximation */
ce_float_t prob_invnorm(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t area, mu = 0, sigma = 1, z, r, q;
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

    if (n < 1) { prob_err(ctx, "invnorm needs 1-3 args"); return 0; }
    area = args[0];
    if (n >= 2) mu = args[1];
    if (n >= 3) { sigma = args[2]; if (sigma <= 0) { prob_err(ctx, "sigma must be > 0"); return 0; } }
    if (area <= 0 || area >= 1) { prob_err(ctx, "invnorm: area must be in (0,1)"); return 0; }

    /* Rational approximation */
    if (area < 0.02425f) {
        /* Lower tail */
        q = sqrtf(-2.0f * logf(area));
        z = (((((c1*q+c2)*q+c3)*q+c4)*q+c5)*q+c6) /
            ((((d1*q+d2)*q+d3)*q+d4)*q+1.0f);
    } else if (area > 0.97575f) {
        /* Upper tail */
        q = sqrtf(-2.0f * logf(1.0f - area));
        z = -(((((c1*q+c2)*q+c3)*q+c4)*q+c5)*q+c6) /
            ((((d1*q+d2)*q+d3)*q+d4)*q+1.0f);
    } else {
        /* Central region */
        q = area - 0.5f;
        r = q * q;
        z = (((((a1*r+a2)*r+a3)*r+a4)*r+a5)*r+a6) * q /
            (((((b1*r+b2)*r+b3)*r+b4)*r+b5)*r+1.0f);
    }

    return mu + sigma * z;
}

/* ── Binomial distribution ───────────────────────────────── */

/* binompdf(n, p, k) */
ce_float_t prob_binompdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    int ni, k;
    ce_float_t p, log_prob;

    if (n_args < 3) { prob_err(ctx, "binompdf needs 3 args"); return 0; }
    p = args[1];
    if (!prob_arg_to_int(args[0], &ni) || !prob_arg_to_int(args[2], &k)) {
        prob_err(ctx, "binompdf n and k must be integers");
        return 0;
    }
    if (ni < 0 || p < 0 || p > 1 || k < 0 || k > ni) {
        prob_err(ctx, "binompdf domain error");
        return 0;
    }
    /* Compute in log-space to avoid overflow for large n */
    log_prob = log_ncr(ni, k) + (ce_float_t)k * logf(p + 1e-30f)
               + (ce_float_t)(ni - k) * logf(1.0f - p + 1e-30f);
    return expf(log_prob);
}

/* binomcdf(n, p, k) — P(X <= k) */
ce_float_t prob_binomcdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    int ni, k, i;
    ce_float_t p, sum = 0, a3[3];

    if (n_args < 3) { prob_err(ctx, "binomcdf needs 3 args"); return 0; }
    p = args[1];
    if (!prob_arg_to_int(args[0], &ni) || !prob_arg_to_int(args[2], &k)) {
        prob_err(ctx, "binomcdf n and k must be integers");
        return 0;
    }
    if (ni < 0 || p < 0 || p > 1 || k < 0) {
        prob_err(ctx, "binomcdf domain error");
        return 0;
    }
    if (k > ni) k = ni;
    a3[0] = args[0]; a3[1] = p;
    for (i = 0; i <= k; i++) {
        a3[2] = (ce_float_t)i;
        sum += prob_binompdf(a3, 3, ctx);
        if (ctx->error) return 0;
    }
    return sum;
}

/* ── Poisson distribution ────────────────────────────────── */

/* poissonpdf(lambda, k) */
ce_float_t prob_poissonpdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    ce_float_t lambda, log_prob;
    int k, i;

    if (n_args < 2) { prob_err(ctx, "poissonpdf needs 2 args"); return 0; }
    lambda = args[0];
    if (!prob_arg_to_int(args[1], &k)) {
        prob_err(ctx, "poissonpdf k must be an integer");
        return 0;
    }
    if (lambda < 0 || k < 0) { prob_err(ctx, "poissonpdf domain error"); return 0; }
    if (lambda == 0) return (k == 0) ? 1.0f : 0.0f;

    /* log-space: log(lambda^k * e^-lambda / k!) */
    log_prob = (ce_float_t)k * logf(lambda) - lambda;
    for (i = 2; i <= k; i++) log_prob -= logf((ce_float_t)i);
    return expf(log_prob);
}

/* poissoncdf(lambda, k) — P(X <= k) */
ce_float_t prob_poissoncdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    ce_float_t lambda, sum = 0, a2[2];
    int k, i;

    if (n_args < 2) { prob_err(ctx, "poissoncdf needs 2 args"); return 0; }
    lambda = args[0];
    if (!prob_arg_to_int(args[1], &k)) {
        prob_err(ctx, "poissoncdf k must be an integer");
        return 0;
    }
    if (lambda < 0 || k < 0) { prob_err(ctx, "poissoncdf domain error"); return 0; }
    a2[0] = lambda;
    for (i = 0; i <= k; i++) {
        a2[1] = (ce_float_t)i;
        sum += prob_poissonpdf(a2, 2, ctx);
        if (ctx->error) return 0;
    }
    return sum;
}

/* ── Geometric distribution ──────────────────────────────── */

/* geometpdf(p, k) — P(X = k), k = 1,2,3,... (first success on trial k) */
ce_float_t prob_geometpdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    ce_float_t p;
    int k;

    if (n_args < 2) { prob_err(ctx, "geometpdf needs 2 args"); return 0; }
    p = args[0];
    if (!prob_arg_to_int(args[1], &k)) {
        prob_err(ctx, "geometpdf k must be an integer");
        return 0;
    }
    if (p <= 0 || p > 1 || k < 1) { prob_err(ctx, "geometpdf domain error"); return 0; }
    return p * powf(1.0f - p, (ce_float_t)(k - 1));
}

/* geometcdf(p, k) — P(X <= k) = 1 - (1-p)^k */
ce_float_t prob_geometcdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    ce_float_t p;
    int k;

    if (n_args < 2) { prob_err(ctx, "geometcdf needs 2 args"); return 0; }
    p = args[0];
    if (!prob_arg_to_int(args[1], &k)) {
        prob_err(ctx, "geometcdf k must be an integer");
        return 0;
    }
    if (p <= 0 || p > 1 || k < 1) { prob_err(ctx, "geometcdf domain error"); return 0; }
    return 1.0f - powf(1.0f - p, (ce_float_t)k);
}

/* ── Student's t-distribution ────────────────────────────── */

/* t-distribution PDF */
static ce_float_t t_pdf(ce_float_t t, ce_float_t df)
{
    ce_float_t coeff = gamma_half_int((df + 1.0f) / 2.0f) /
                       (sqrtf(df * CE_PI) * gamma_half_int(df / 2.0f));
    return coeff * powf(1.0f + t*t/df, -(df + 1.0f) / 2.0f);
}

/* tcdf(lo, hi, df) — Simpson's rule integration */
ce_float_t prob_tcdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    ce_float_t lo, hi, df, h, sum, clamp;
    int steps, i;

    if (n_args < 3) { prob_err(ctx, "tcdf needs 3 args"); return 0; }
    lo = args[0]; hi = args[1]; df = args[2];
    if (df <= 0) { prob_err(ctx, "tcdf: df must be > 0"); return 0; }

    /* df=1 (Cauchy): exact CDF = 0.5 + atan(t)/pi */
    if (fabsf(df - 1.0f) < 0.01f) {
        ce_float_t cdf_hi = 0.5f + atanf(hi) / CE_PI;
        ce_float_t cdf_lo = 0.5f + atanf(lo) / CE_PI;
        return cdf_hi - cdf_lo;
    }

    /* Adaptive clamping: heavy tails for low df */
    if (df < 3.0f)       { clamp = 500.0f; steps = 600; }
    else if (df < 5.0f)  { clamp = 200.0f; steps = 400; }
    else                  { clamp = 100.0f; steps = 200; }

    if (lo < -clamp) lo = -clamp;
    if (hi >  clamp) hi =  clamp;
    if (lo >= hi) return 0;

    h = (hi - lo) / (ce_float_t)steps;
    sum = t_pdf(lo, df) + t_pdf(hi, df);
    for (i = 1; i < steps; i++) {
        ce_float_t xi = lo + (ce_float_t)i * h;
        sum += ((i % 2 == 1) ? 4.0f : 2.0f) * t_pdf(xi, df);
    }
    return sum * h / 3.0f;
}

/* ── Chi-squared distribution ────────────────────────────── */

/* Chi-squared PDF */
static ce_float_t chi2_pdf(ce_float_t x, ce_float_t df)
{
    ce_float_t k2 = df / 2.0f;
    if (x <= 0) return 0;
    /* 2^k2 as expf(k2*ln 2): clang turns powf(2.0f, x) into exp2f(x), and
     * CEdev v11.2 exp2f computes expf(x*log2(e)) instead of expf(x*ln 2),
     * so exp2f(1) = 4.23 on the CE (internal review). */
    return (1.0f / (expf(k2 * 0.693147182f) * gamma_half_int(k2))) *
           powf(x, k2 - 1.0f) * expf(-x / 2.0f);
}

/* Wilson-Hilferty normal approximation for chi-squared CDF.
 * Robust for moderate/large df where direct gamma-space PDF integration
 * can overflow or lose precision in single-precision math. */
static ce_float_t chi2_cdf_wilson_hilferty(ce_float_t x, ce_float_t df)
{
    ce_float_t c, base, z, cdf;
    if (x <= 0.0f || df <= 0.0f) return 0.0f;
    c = 2.0f / (9.0f * df);
    base = x / df;
    if (base <= 0.0f) return 0.0f;
    z = (powf(base, 1.0f / 3.0f) - (1.0f - c)) / sqrtf(c);
    cdf = std_normal_cdf(z);
    if (cdf < 0.0f) return 0.0f;
    if (cdf > 1.0f) return 1.0f;
    return cdf;
}

/* normalpdf(x [, mu, sigma]) */
ce_float_t prob_normalpdf(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t x, mu = 0, sigma = 1, z;
    if (n < 1) { prob_err(ctx, "normalpdf needs 1-3 args"); return 0; }
    x = args[0];
    if (n >= 2) mu = args[1];
    if (n >= 3) { sigma = args[2]; if (sigma <= 0) { prob_err(ctx, "sigma must be > 0"); return 0; } }
    z = (x - mu) / sigma;
    return (1.0f / (sigma * sqrtf(2.0f * CE_PI))) * expf(-0.5f * z * z);
}

/* invt(area, df) — inverse t-distribution via bisection on tcdf */
ce_float_t prob_invt(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t area, df, lo, hi, mid, cdf_val;
    ce_float_t tcdf_args[3];
    int iter;

    if (n < 2) { prob_err(ctx, "invt needs 2 args (area, df)"); return 0; }
    area = args[0]; df = args[1];
    if (area <= 0 || area >= 1) { prob_err(ctx, "invt: area must be in (0,1)"); return 0; }
    if (df <= 0) { prob_err(ctx, "invt: df must be > 0"); return 0; }

    /* For large df, t ≈ normal */
    if (df >= 30.0f) {
        ce_float_t inv_args[1] = { area };
        return prob_invnorm(inv_args, 1, ctx);
    }

    /* Bisection: find t such that tcdf(-100, t, df) = area */
    lo = -50.0f; hi = 50.0f;
    tcdf_args[2] = df;
    for (iter = 0; iter < 60; iter++) {
        mid = (lo + hi) / 2.0f;
        tcdf_args[0] = -100.0f;
        tcdf_args[1] = mid;
        cdf_val = prob_tcdf(tcdf_args, 3, ctx);
        if (cdf_val < area)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) / 2.0f;
}

/* chi2cdf(lo, hi, df) — uses special formulas for df=1,2
 * and Simpson's rule for larger df */
ce_float_t prob_chi2cdf(const ce_float_t *args, int n_args, eval_ctx_t *ctx)
{
    ce_float_t lo, hi, df;

    if (n_args < 3) { prob_err(ctx, "chi2cdf needs 3 args"); return 0; }
    lo = args[0]; hi = args[1]; df = args[2];
    if (df <= 0) { prob_err(ctx, "chi2cdf: df must be > 0"); return 0; }
    if (lo < 0) lo = 0;
    if (lo >= hi) return 0;

    /* For moderate/large df, prefer Wilson-Hilferty approximation to avoid
     * overflow/underflow in direct PDF integration. */
    if (df >= 30.0f) {
        ce_float_t cdf_hi = chi2_cdf_wilson_hilferty(hi, df);
        ce_float_t cdf_lo = chi2_cdf_wilson_hilferty(lo, df);
        ce_float_t diff = cdf_hi - cdf_lo;
        if (diff < 0.0f) return 0.0f;
        if (diff > 1.0f) return 1.0f;
        return diff;
    }

    if (hi > 200.0f) hi = 200.0f;
    if (lo >= hi) return 0;

    /* df=1: chi-squared is Z^2 where Z~N(0,1)
     * CDF(x) = 2*Phi(sqrt(x)) - 1 */
    if (fabsf(df - 1.0f) < 0.01f) {
        ce_float_t cdf_hi = (hi > 0) ? 2.0f * std_normal_cdf(sqrtf(hi)) - 1.0f : 0.0f;
        ce_float_t cdf_lo = (lo > 0) ? 2.0f * std_normal_cdf(sqrtf(lo)) - 1.0f : 0.0f;
        return cdf_hi - cdf_lo;
    }

    /* df=2: exponential with rate 1/2
     * CDF(x) = 1 - exp(-x/2) */
    if (fabsf(df - 2.0f) < 0.01f) {
        ce_float_t cdf_hi = 1.0f - expf(-hi / 2.0f);
        ce_float_t cdf_lo = (lo > 0) ? 1.0f - expf(-lo / 2.0f) : 0.0f;
        return cdf_hi - cdf_lo;
    }

    /* General case: Simpson's rule */
    {
        int steps = 400, i;
        ce_float_t h, sum;
        /* For df<3, PDF has singularity near 0. Use a tiny start to
         * capture probability mass near zero without hitting the pole. */
        ce_float_t start = (lo < 1e-6f && df < 3.0f) ? 1e-6f : lo;
        if (start >= hi) return 0;
        h = (hi - start) / (ce_float_t)steps;
        sum = chi2_pdf(start, df) + chi2_pdf(hi, df);
        for (i = 1; i < steps; i++) {
            ce_float_t xi = start + (ce_float_t)i * h;
            sum += ((i % 2 == 1) ? 4.0f : 2.0f) * chi2_pdf(xi, df);
        }
        return sum * h / 3.0f;
    }
}

/* ── F-distribution ──────────────────────────────────────── */

/* F-distribution PDF:
 * f(x; d1, d2) = (1/B(d1/2,d2/2)) * (d1/d2)^(d1/2) * x^(d1/2-1)
 *                * (1 + d1*x/d2)^(-(d1+d2)/2)
 * Computed in log-space: B(a,b) = Gamma(a)*Gamma(b)/Gamma(a+b)
 */
static ce_float_t f_pdf(ce_float_t x, ce_float_t d1, ce_float_t d2)
{
    ce_float_t a, b, log_beta, log_pdf;
    if (x <= 0) return 0;
    a = d1 / 2.0f;
    b = d2 / 2.0f;
    /* log(Beta(a,b)) = log(Gamma(a)) + log(Gamma(b)) - log(Gamma(a+b)) */
    log_beta = logf(gamma_half_int(a)) + logf(gamma_half_int(b))
               - logf(gamma_half_int(a + b));
    /* log(pdf) = a*ln(d1/d2) + (a-1)*ln(x) - (a+b)*ln(1+d1*x/d2) - log_beta */
    log_pdf = a * logf(d1 / d2) + (a - 1.0f) * logf(x)
              - (a + b) * logf(1.0f + d1 * x / d2) - log_beta;
    return expf(log_pdf);
}

/* fpdf(x, d1, d2) */
ce_float_t prob_fpdf(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t x, d1, d2;
    if (n < 3) { prob_err(ctx, "fpdf needs 3 args (x, d1, d2)"); return 0; }
    x = args[0]; d1 = args[1]; d2 = args[2];
    if (d1 <= 0 || d2 <= 0) { prob_err(ctx, "fpdf: d1,d2 must be > 0"); return 0; }
    if (x < 0) return 0;
    return f_pdf(x, d1, d2);
}

/* fcdf(lo, hi, d1, d2) — Simpson's rule integration */
ce_float_t prob_fcdf(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t lo, hi, d1, d2, h, sum;
    int steps = 400, i;

    if (n < 4) { prob_err(ctx, "fcdf needs 4 args (lo, hi, d1, d2)"); return 0; }
    lo = args[0]; hi = args[1]; d1 = args[2]; d2 = args[3];
    if (d1 <= 0 || d2 <= 0) { prob_err(ctx, "fcdf: d1,d2 must be > 0"); return 0; }
    if (lo < 0) lo = 0;
    if (hi > 200.0f) hi = 200.0f;
    if (lo >= hi) return 0;

    h = (hi - lo) / (ce_float_t)steps;
    sum = f_pdf(lo, d1, d2) + f_pdf(hi, d1, d2);
    for (i = 1; i < steps; i++) {
        ce_float_t xi = lo + (ce_float_t)i * h;
        sum += ((i % 2 == 1) ? 4.0f : 2.0f) * f_pdf(xi, d1, d2);
    }
    return sum * h / 3.0f;
}

/* invchi2(area, df) — inverse chi-squared via bisection on chi2cdf */
ce_float_t prob_invchi2(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t area, df, lo, hi, mid, cdf_val;
    ce_float_t chi2_args[3];
    int iter;

    if (n < 2) { prob_err(ctx, "invchi2 needs 2 args (area, df)"); return 0; }
    area = args[0]; df = args[1];
    if (area <= 0 || area >= 1) { prob_err(ctx, "invchi2: area must be in (0,1)"); return 0; }
    if (df <= 0) { prob_err(ctx, "invchi2: df must be > 0"); return 0; }

    /* Wilson-Hilferty inverse approximation for moderate/large df.
     * q ≈ df * (1 - 2/(9df) + z*sqrt(2/(9df)))^3 where z = Phi^-1(area). */
    if (df >= 30.0f) {
        ce_float_t z = prob_inv_normal(area);
        ce_float_t c = 2.0f / (9.0f * df);
        ce_float_t term = 1.0f - c + z * sqrtf(c);
        if (term <= 0.0f) return 0.0f;
        return df * term * term * term;
    }

    /* Bisection: find x such that chi2cdf(0, x, df) = area */
    lo = 0.0f;
    hi = df + 10.0f * sqrtf(2.0f * df);  /* generous upper bound */
    if (hi < 10.0f) hi = 10.0f;
    /* Expand upper bound if needed */
    chi2_args[0] = 0.0f;
    chi2_args[1] = hi;
    chi2_args[2] = df;
    while (prob_chi2cdf(chi2_args, 3, ctx) < area && hi < 1000.0f) {
        hi *= 2.0f;
        chi2_args[1] = hi;
    }

    chi2_args[2] = df;
    for (iter = 0; iter < 80; iter++) {
        mid = (lo + hi) / 2.0f;
        chi2_args[0] = 0.0f;
        chi2_args[1] = mid;
        cdf_val = prob_chi2cdf(chi2_args, 3, ctx);
        if (ctx->error) return 0;
        if (cdf_val < area)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) / 2.0f;
}

/* invf(area, d1, d2) — inverse F-distribution via bisection on fcdf */
ce_float_t prob_invf(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t area, d1, d2, lo, hi, mid, cdf_val;
    ce_float_t fcdf_args[4];
    int iter;

    if (n < 3) { prob_err(ctx, "invf needs 3 args (area, d1, d2)"); return 0; }
    area = args[0]; d1 = args[1]; d2 = args[2];
    if (area <= 0 || area >= 1) { prob_err(ctx, "invf: area must be in (0,1)"); return 0; }
    if (d1 <= 0 || d2 <= 0) { prob_err(ctx, "invf: d1,d2 must be > 0"); return 0; }

    /* Bisection: find x such that fcdf(0, x, d1, d2) = area */
    lo = 0.0f;
    hi = 10.0f;
    /* Expand upper bound if needed */
    fcdf_args[0] = 0.0f;
    fcdf_args[1] = hi;
    fcdf_args[2] = d1;
    fcdf_args[3] = d2;
    while (prob_fcdf(fcdf_args, 4, ctx) < area && hi < 1000.0f) {
        hi *= 2.0f;
        fcdf_args[1] = hi;
    }

    for (iter = 0; iter < 80; iter++) {
        mid = (lo + hi) / 2.0f;
        fcdf_args[0] = 0.0f;
        fcdf_args[1] = mid;
        cdf_val = prob_fcdf(fcdf_args, 4, ctx);
        if (ctx->error) return 0;
        if (cdf_val < area)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) / 2.0f;
}
