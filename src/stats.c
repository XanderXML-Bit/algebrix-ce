/*
 * stats.c — Statistical computation functions
 *
 * Pure computation on float arrays. No AST or parser dependencies.
 * Uses insertion sort (O(n^2)) which is fine for n <= 50.
 */
#include "stats.h"
#include <math.h>

/* ── Helpers ─────────────────────────────────────────────── */

/* Insertion sort — good for small n, no extra memory. */
static void isort(ce_float_t *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        ce_float_t key = a[i];
        j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

/* ── Single-value statistics ────────────────────────────── */

ce_float_t stat_mean(const ce_float_t *data, int n)
{
    ce_float_t sum = 0;
    int i;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) sum += data[i];
    return sum / (ce_float_t)n;
}

ce_float_t stat_median(ce_float_t *data, int n)
{
    if (n <= 0) return 0;
    isort(data, n);
    if (n % 2 == 1)
        return data[n / 2];
    else
        return (data[n / 2 - 1] + data[n / 2]) / 2.0f;
}

ce_float_t stat_mode(const ce_float_t *data, int n)
{
    /* Copy to scratch, sort, find longest run. */
    ce_float_t scratch[MAX_STAT_DATA];
    int i, best_count = 0, cur_count = 1;
    ce_float_t best_val, cur_val;

    if (n <= 0) return 0;
    if (n == 1) return data[0];

    for (i = 0; i < n && i < MAX_STAT_DATA; i++) scratch[i] = data[i];
    if (n > MAX_STAT_DATA) n = MAX_STAT_DATA;  /* clamp to scratch size */
    isort(scratch, n);

    best_val = cur_val = scratch[0];
    for (i = 1; i < n; i++) {
        if (fabsf(scratch[i] - cur_val) < 1e-6f) {
            cur_count++;
        } else {
            if (cur_count > best_count) {
                best_count = cur_count;
                best_val   = cur_val;
            }
            cur_val   = scratch[i];
            cur_count = 1;
        }
    }
    if (cur_count > best_count) {
        best_val = cur_val;
    }
    return best_val;
}

ce_float_t stat_variance(const ce_float_t *data, int n)
{
    ce_float_t m, sum_sq = 0;
    int i;
    if (n <= 1) return 0;
    m = stat_mean(data, n);
    for (i = 0; i < n; i++) {
        ce_float_t d = data[i] - m;
        sum_sq += d * d;
    }
    return sum_sq / (ce_float_t)(n - 1);  /* sample variance */
}

ce_float_t stat_stdev(const ce_float_t *data, int n)
{
    return sqrtf(stat_variance(data, n));
}

ce_float_t stat_min(const ce_float_t *data, int n)
{
    ce_float_t v;
    int i;
    if (n <= 0) return 0;
    v = data[0];
    for (i = 1; i < n; i++)
        if (data[i] < v) v = data[i];
    return v;
}

ce_float_t stat_max(const ce_float_t *data, int n)
{
    ce_float_t v;
    int i;
    if (n <= 0) return 0;
    v = data[0];
    for (i = 1; i < n; i++)
        if (data[i] > v) v = data[i];
    return v;
}

/* ── Linear regression ──────────────────────────────────── */

ce_error_t stat_linreg(const ce_float_t *data, int n,
                       ce_float_t *m, ce_float_t *b, ce_float_t *r)
{
    int pairs = n / 2;
    ce_float_t sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    ce_float_t denom;
    int i;

    if (pairs < 2) return CE_ERR_DOMAIN;

    for (i = 0; i < pairs; i++) {
        ce_float_t x = data[2 * i];
        ce_float_t y = data[2 * i + 1];
        sx  += x;
        sy  += y;
        sxx += x * x;
        syy += y * y;
        sxy += x * y;
    }

    denom = (ce_float_t)pairs * sxx - sx * sx;
    if (fabsf(denom) < 1e-30f) return CE_ERR_DOMAIN;

    *m = ((ce_float_t)pairs * sxy - sx * sy) / denom;
    *b = (sy - *m * sx) / (ce_float_t)pairs;

    /* Pearson correlation coefficient */
    {
        ce_float_t num = (ce_float_t)pairs * sxy - sx * sy;
        ce_float_t d1  = (ce_float_t)pairs * sxx - sx * sx;
        ce_float_t d2  = (ce_float_t)pairs * syy - sy * sy;
        ce_float_t den = sqrtf(fabsf(d1 * d2));
        *r = (den > 1e-30f) ? num / den : 0;
    }

    return CE_OK;
}

/* ── Combinatorics ─────────────────────────────────────── */

ce_float_t stat_ncr(ce_float_t n, ce_float_t r)
{
    /* C(n,r) = product(i=0..r-1, (n-i)/(i+1)) — iterative to avoid overflow */
    int i, ni = (int)n, ri = (int)r;
    ce_float_t result = 1;
    if (ri < 0 || ri > ni || ni < 0) return 0;
    if (ri > ni - ri) ri = ni - ri;  /* C(n,r) = C(n,n-r) */
    for (i = 0; i < ri; i++)
        result = result * (ce_float_t)(ni - i) / (ce_float_t)(i + 1);
    return result;
}

ce_float_t stat_npr(ce_float_t n, ce_float_t r)
{
    /* P(n,r) = n * (n-1) * ... * (n-r+1) */
    int i, ni = (int)n, ri = (int)r;
    ce_float_t result = 1;
    if (ri < 0 || ri > ni || ni < 0) return 0;
    for (i = 0; i < ri; i++)
        result *= (ce_float_t)(ni - i);
    return result;
}

/* ── Extended math ─────────────────────────────────────── */

ce_float_t stat_gcd(ce_float_t a, ce_float_t b)
{
    int ia = (int)fabsf(a), ib = (int)fabsf(b), tmp;
    if (ia == 0) return (ce_float_t)ib;
    if (ib == 0) return (ce_float_t)ia;
    while (ib != 0) { tmp = ib; ib = ia % ib; ia = tmp; }
    return (ce_float_t)ia;
}

ce_float_t stat_lcm(ce_float_t a, ce_float_t b)
{
    ce_float_t g = stat_gcd(a, b);
    if (g == 0) return 0;
    return fabsf(a / g) * fabsf(b);
}

/* ── Additional regressions ────────────────────────────── */

ce_error_t stat_quadreg(const ce_float_t *data, int n,
                        ce_float_t *a, ce_float_t *b, ce_float_t *c, ce_float_t *r)
{
    /* Least-squares quadratic: y = a*x^2 + b*x + c
     * Normal equations via Cramer's rule on 3x3 system */
    int pairs = n / 2;
    ce_float_t sx = 0, sx2 = 0, sx3 = 0, sx4 = 0;
    ce_float_t sy = 0, sxy = 0, sx2y = 0;
    ce_float_t det, det_a, det_b, det_c;
    ce_float_t np, ss_res, ss_tot, y_mean;
    int i;

    if (pairs < 3) return CE_ERR_DOMAIN;
    np = (ce_float_t)pairs;

    for (i = 0; i < pairs; i++) {
        ce_float_t x = data[2*i], y = data[2*i+1];
        ce_float_t x2 = x*x;
        sx   += x;    sx2  += x2;   sx3  += x2*x;
        sx4  += x2*x2; sy  += y;    sxy  += x*y;
        sx2y += x2*y;
    }

    /* 3x3 determinant: | np  sx  sx2 |
                        | sx  sx2 sx3 |
                        | sx2 sx3 sx4 | */
    det = np*(sx2*sx4 - sx3*sx3) - sx*(sx*sx4 - sx3*sx2) + sx2*(sx*sx3 - sx2*sx2);
    if (fabsf(det) < 1e-20f) return CE_ERR_DOMAIN;

    det_c = sy*(sx2*sx4 - sx3*sx3) - sx*(sxy*sx4 - sx2y*sx3) + sx2*(sxy*sx3 - sx2y*sx2);
    det_b = np*(sxy*sx4 - sx2y*sx3) - sy*(sx*sx4 - sx3*sx2) + sx2*(sx*sx2y - sxy*sx2);
    det_a = np*(sx2*sx2y - sx3*sxy) - sx*(sx*sx2y - sxy*sx2) + sy*(sx*sx3 - sx2*sx2);

    *a = det_a / det;
    *b = det_b / det;
    *c = det_c / det;

    /* R^2 */
    y_mean = sy / np;
    ss_res = 0; ss_tot = 0;
    for (i = 0; i < pairs; i++) {
        ce_float_t x = data[2*i], y = data[2*i+1];
        ce_float_t yp = *a * x*x + *b * x + *c;
        ss_res += (y - yp)*(y - yp);
        ss_tot += (y - y_mean)*(y - y_mean);
    }
    *r = (ss_tot > 1e-20f) ? sqrtf(fmaxf(0.0f, 1.0f - ss_res/ss_tot)) : 1.0f;
    return CE_OK;
}

ce_error_t stat_expreg(const ce_float_t *data, int n,
                       ce_float_t *a, ce_float_t *b, ce_float_t *r)
{
    /* y = a*b^x via ln-transform: ln(y) = ln(a) + x*ln(b) → linreg */
    ce_float_t transformed[MAX_STAT_DATA];
    int pairs = n / 2, i;
    ce_float_t m_ln, b_ln, r_val;
    ce_error_t err;

    if (n < 0) n = 0;
    if (pairs < 2) return CE_ERR_DOMAIN;
    /* Clamp pairs so 2*pairs-1 fits within transformed[] (size MAX_STAT_DATA) */
    if (pairs > MAX_STAT_DATA / 2) pairs = MAX_STAT_DATA / 2;
    for (i = 0; i < pairs; i++) {
        ce_float_t y = data[2*i+1];
        if (y <= 0) return CE_ERR_DOMAIN;
        transformed[2*i]   = data[2*i];
        transformed[2*i+1] = logf(y);
    }
    err = stat_linreg(transformed, pairs * 2, &m_ln, &b_ln, &r_val);
    if (err != CE_OK) return err;
    *a = expf(b_ln);
    *b = expf(m_ln);
    *r = r_val;
    return CE_OK;
}

ce_error_t stat_pwrreg(const ce_float_t *data, int n,
                       ce_float_t *a, ce_float_t *b, ce_float_t *r)
{
    /* y = a*x^b via ln-ln-transform: ln(y) = ln(a) + b*ln(x) → linreg */
    ce_float_t transformed[MAX_STAT_DATA];
    int pairs = n / 2, i;
    ce_float_t m_ln, b_ln, r_val;
    ce_error_t err;

    if (n < 0) n = 0;
    if (pairs < 2) return CE_ERR_DOMAIN;
    /* Clamp pairs so 2*pairs-1 fits within transformed[] (size MAX_STAT_DATA) */
    if (pairs > MAX_STAT_DATA / 2) pairs = MAX_STAT_DATA / 2;
    for (i = 0; i < pairs; i++) {
        ce_float_t x = data[2*i], y = data[2*i+1];
        if (x <= 0 || y <= 0) return CE_ERR_DOMAIN;
        transformed[2*i]   = logf(x);
        transformed[2*i+1] = logf(y);
    }
    err = stat_linreg(transformed, pairs * 2, &m_ln, &b_ln, &r_val);
    if (err != CE_OK) return err;
    *a = expf(b_ln);
    *b = m_ln;
    *r = r_val;
    return CE_OK;
}

ce_error_t stat_lnreg(const ce_float_t *data, int n,
                      ce_float_t *a, ce_float_t *b, ce_float_t *r)
{
    /* y = a + b*ln(x) via ln(x)-transform → linreg */
    ce_float_t transformed[MAX_STAT_DATA];
    int pairs = n / 2, i;
    ce_error_t err;

    if (n < 0) n = 0;
    if (pairs < 2) return CE_ERR_DOMAIN;
    /* Clamp pairs so 2*pairs-1 fits within transformed[] (size MAX_STAT_DATA) */
    if (pairs > MAX_STAT_DATA / 2) pairs = MAX_STAT_DATA / 2;
    for (i = 0; i < pairs; i++) {
        ce_float_t x = data[2*i];
        if (x <= 0) return CE_ERR_DOMAIN;
        transformed[2*i]   = logf(x);
        transformed[2*i+1] = data[2*i+1];
    }
    err = stat_linreg(transformed, pairs * 2, b, a, r);
    return err;
}

/* ── 2-Variable Statistics ────────────────────────────────── */

ce_float_t stat_sxy(const ce_float_t *data, int n)
{
    int pairs = n / 2, i;
    ce_float_t sx = 0, sy = 0, sum = 0;
    if (pairs < 2) return 0;
    for (i = 0; i < pairs; i++) { sx += data[2*i]; sy += data[2*i+1]; }
    sx /= (ce_float_t)pairs;
    sy /= (ce_float_t)pairs;
    for (i = 0; i < pairs; i++)
        sum += (data[2*i] - sx) * (data[2*i+1] - sy);
    return sum;
}

ce_float_t stat_covariance(const ce_float_t *data, int n)
{
    int pairs = n / 2;
    if (pairs < 2) return 0;
    return stat_sxy(data, n) / (ce_float_t)(pairs - 1);
}

ce_error_t stat_2varstats(const ce_float_t *data, int n, twovar_result_t *out)
{
    int pairs = n / 2, i;
    ce_float_t sx = 0, sy = 0, sxx = 0, syy = 0, sxy_val = 0;
    ce_float_t xbar, ybar, den;

    if (pairs < 2) return CE_ERR_DOMAIN;

    for (i = 0; i < pairs; i++) { sx += data[2*i]; sy += data[2*i+1]; }
    xbar = sx / (ce_float_t)pairs;
    ybar = sy / (ce_float_t)pairs;

    for (i = 0; i < pairs; i++) {
        ce_float_t dx = data[2*i] - xbar;
        ce_float_t dy = data[2*i+1] - ybar;
        sxx += dx * dx;
        syy += dy * dy;
        sxy_val += dx * dy;
    }

    out->xbar = xbar;
    out->ybar = ybar;
    out->sx   = sqrtf(sxx / (ce_float_t)(pairs - 1));
    out->sy   = sqrtf(syy / (ce_float_t)(pairs - 1));
    out->sxy  = sxy_val;
    out->n    = pairs;

    den = sqrtf(fabsf(sxx * syy));
    out->r  = (den > 1e-30f) ? sxy_val / den : 0;
    out->r2 = out->r * out->r;

    return CE_OK;
}

/* ── Residuals ─────────────────────────────────────────── */

ce_error_t stat_residuals(const ce_float_t *data, int n,
                          ce_float_t *out, int *out_count)
{
    ce_float_t m, b, r;
    ce_error_t err;
    int pairs, i;

    if (!out || !out_count) return CE_ERR_DOMAIN;
    *out_count = 0;

    err = stat_linreg(data, n, &m, &b, &r);
    if (err != CE_OK) return err;

    pairs = n / 2;
    for (i = 0; i < pairs; i++) {
        ce_float_t xi = data[2 * i];
        ce_float_t yi = data[2 * i + 1];
        ce_float_t predicted = m * xi + b;
        out[i] = yi - predicted;
    }
    *out_count = pairs;
    return CE_OK;
}

/* ── Stat function name ─────────────────────────────────── */

const char *stat_fn_name(stat_fn_t fn)
{
    switch (fn) {
    case STAT_MEAN:   return "mean";
    case STAT_MEDIAN: return "median";
    case STAT_MODE:   return "mode";
    case STAT_STDEV:  return "stdev";
    case STAT_VAR:    return "var";
    case STAT_MIN:    return "min";
    case STAT_MAX:    return "max";
    case STAT_LINREG: return "linreg";
    case STAT_NCR:    return "nCr";
    case STAT_NPR:    return "nPr";
    case STAT_GCD:    return "gcd";
    case STAT_LCM:    return "lcm";
    case STAT_MOD:    return "mod";
    case STAT_RANDINT:return "randint";
    case STAT_NORMALCDF:  return "normalcdf";
    case STAT_INVNORM:    return "invnorm";
    case STAT_BINOMPDF:   return "binompdf";
    case STAT_BINOMCDF:   return "binomcdf";
    case STAT_POISSONPDF: return "poissonpdf";
    case STAT_POISSONCDF: return "poissoncdf";
    case STAT_GEOMETPDF:  return "geometpdf";
    case STAT_GEOMETCDF:  return "geometcdf";
    case STAT_TCDF:       return "tcdf";
    case STAT_CHI2CDF:    return "chi2cdf";
    case STAT_NORMALPDF:  return "normalpdf";
    case STAT_INVT:       return "invt";
    case STAT_FPDF:       return "fpdf";
    case STAT_FCDF:       return "fcdf";
    case STAT_INVCHI2:    return "invchi2";
    case STAT_INVF:       return "invf";
    case STAT_QUADREG: return "quadreg";
    case STAT_EXPREG:  return "expreg";
    case STAT_PWRREG:  return "pwrreg";
    case STAT_LNREG:   return "lnreg";
    case STAT_FIN_PV:  return "pv";
    case STAT_FIN_FV:  return "fv";
    case STAT_FIN_PMT: return "pmt";
    case STAT_FIN_NPER:return "nper";
    case STAT_FIN_RATE:return "rate";
    case STAT_FIN_NOM: return "nom";
    case STAT_FIN_EFF: return "eff";
    case STAT_FIN_BAL: return "bal";
    case STAT_FIN_SUMPRN: return "sumprn";
    case STAT_FIN_SUMINT: return "sumint";
    case STAT_COVAR:   return "covar";
    case STAT_2VARSTATS: return "2varstats";
    case STAT_ZTEST:      return "ztest";
    case STAT_TTEST:      return "ttest";
    case STAT_2SAMPZTEST: return "2sampztest";
    case STAT_2SAMPTTEST: return "2sampttest";
    case STAT_1PROPZTEST: return "1propztest";
    case STAT_2PROPZTEST: return "2propztest";
    case STAT_CHI2TEST:   return "chi2test";
    case STAT_ZINTERVAL:  return "zinterval";
    case STAT_TINTERVAL:  return "tinterval";
    case STAT_1PROPZINT:  return "1propzint";
    case STAT_2PROPZINT:  return "2propzint";
    case STAT_2SAMPZINT:  return "2sampzint";
    case STAT_2SAMPTINT:  return "2samptint";
    case STAT_LINREGTTEST: return "linregttest";
    case STAT_ANOVA:      return "anova";
    case STAT_RESID:      return "resid";
    default:          return "stat?";
    }
}
