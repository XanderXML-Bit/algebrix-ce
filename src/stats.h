/*
 * stats.h — Statistical functions for CalcEngine
 * Operates on float arrays. All functions are pure computation.
 */
#ifndef STATS_H
#define STATS_H

#include "calcengine.h"

/* ── Stat function identifiers ──────────────────────────── */
typedef enum {
    STAT_MEAN, STAT_MEDIAN, STAT_MODE,
    STAT_STDEV, STAT_VAR,
    STAT_MIN, STAT_MAX,
    STAT_LINREG,
    STAT_NCR, STAT_NPR,
    /* Extended math (multi-arg) */
    STAT_GCD, STAT_LCM, STAT_MOD, STAT_RANDINT,
    /* Probability distributions */
    STAT_NORMALCDF, STAT_INVNORM,
    STAT_BINOMPDF, STAT_BINOMCDF,
    STAT_POISSONPDF, STAT_POISSONCDF,
    STAT_GEOMETPDF, STAT_GEOMETCDF,
    STAT_TCDF, STAT_CHI2CDF,
    STAT_NORMALPDF, STAT_INVT,
    STAT_FPDF, STAT_FCDF, STAT_INVCHI2, STAT_INVF,
    /* Additional regressions */
    STAT_QUADREG, STAT_EXPREG, STAT_PWRREG, STAT_LNREG,
    /* Finance TVM */
    STAT_FIN_PV, STAT_FIN_FV, STAT_FIN_PMT,
    STAT_FIN_NPER, STAT_FIN_RATE,
    STAT_FIN_NOM, STAT_FIN_EFF,
    STAT_FIN_BAL, STAT_FIN_SUMPRN, STAT_FIN_SUMINT,
    /* 2-Var Stats */
    STAT_COVAR, STAT_2VARSTATS,
    /* Statistical tests */
    STAT_ZTEST, STAT_TTEST,
    STAT_2SAMPZTEST, STAT_2SAMPTTEST,
    STAT_1PROPZTEST, STAT_2PROPZTEST,
    STAT_CHI2TEST,
    STAT_ZINTERVAL, STAT_TINTERVAL,
    STAT_1PROPZINT, STAT_2PROPZINT,
    STAT_2SAMPZINT, STAT_2SAMPTINT,
    /* LinReg t-test, ANOVA, Residuals */
    STAT_LINREGTTEST, STAT_ANOVA, STAT_RESID
} stat_fn_t;

#define MAX_STAT_DATA 50

/* ── Single-value statistics ────────────────────────────── */
ce_float_t stat_mean(const ce_float_t *data, int n);
ce_float_t stat_median(ce_float_t *data, int n);   /* sorts in-place */
ce_float_t stat_mode(const ce_float_t *data, int n);
ce_float_t stat_variance(const ce_float_t *data, int n);
ce_float_t stat_stdev(const ce_float_t *data, int n);
ce_float_t stat_min(const ce_float_t *data, int n);
ce_float_t stat_max(const ce_float_t *data, int n);

/* ── Linear regression ──────────────────────────────────── */
/* Takes paired data: x0,y0,x1,y1,... (n must be even, n/2 points).
 * Returns slope via *m, intercept via *b, correlation via *r.
 * Returns CE_OK on success, CE_ERR_DOMAIN if < 2 points. */
ce_error_t stat_linreg(const ce_float_t *data, int n,
                       ce_float_t *m, ce_float_t *b, ce_float_t *r);

/* ── Combinatorics ─────────────────────────────────────── */
ce_float_t stat_ncr(ce_float_t n, ce_float_t r);  /* n choose r */
ce_float_t stat_npr(ce_float_t n, ce_float_t r);  /* n permute r */

/* ── Extended math ──────────────────────────────────────── */
ce_float_t stat_gcd(ce_float_t a, ce_float_t b);
ce_float_t stat_lcm(ce_float_t a, ce_float_t b);

/* ── Additional regressions ────────────────────────────── */
ce_error_t stat_quadreg(const ce_float_t *data, int n,
                        ce_float_t *a, ce_float_t *b, ce_float_t *c, ce_float_t *r);
ce_error_t stat_expreg(const ce_float_t *data, int n,
                       ce_float_t *a, ce_float_t *b, ce_float_t *r);
ce_error_t stat_pwrreg(const ce_float_t *data, int n,
                       ce_float_t *a, ce_float_t *b, ce_float_t *r);
ce_error_t stat_lnreg(const ce_float_t *data, int n,
                      ce_float_t *a, ce_float_t *b, ce_float_t *r);

/* ── 2-Variable Statistics ────────────────────────────────── */
/* Takes paired data: x0,y0,x1,y1,... (n must be even, n/2 points). */
ce_float_t stat_covariance(const ce_float_t *data, int n);
ce_float_t stat_sxy(const ce_float_t *data, int n);

typedef struct {
    ce_float_t xbar, ybar;
    ce_float_t sx, sy;
    ce_float_t sxy;
    ce_float_t r, r2;
    int n;
} twovar_result_t;

ce_error_t stat_2varstats(const ce_float_t *data, int n, twovar_result_t *out);

/* ── Residuals (observed - predicted from linreg) ────────── */
/* Takes paired data: x0,y0,x1,y1,... (n must be even, n/2 points).
 * Computes linear regression, then residual_i = y_i - (m*x_i + b).
 * Returns residuals via *out (caller provides array of n/2 floats).
 * Returns CE_OK on success, CE_ERR_DOMAIN if < 2 points. */
ce_error_t stat_residuals(const ce_float_t *data, int n,
                          ce_float_t *out, int *out_count);

/* ── Stat function name (for display) ───────────────────── */
const char *stat_fn_name(stat_fn_t fn);

#endif /* STATS_H */
