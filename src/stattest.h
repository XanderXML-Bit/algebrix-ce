/*
 * stattest.h — Hypothesis tests and confidence intervals for CalcEngine
 * Z-test, t-test, 2-sample tests, proportion tests, chi-squared, CIs.
 */
#ifndef STATTEST_H
#define STATTEST_H

#include "calcengine.h"

/* ── Alternative hypothesis direction ────────────────────── */
typedef enum {
    ALT_TWO_SIDED = 0,   /* H1: mu != mu0 */
    ALT_LESS,             /* H1: mu < mu0  */
    ALT_GREATER           /* H1: mu > mu0  */
} alt_hyp_t;

/* ── Result structures ───────────────────────────────────── */
typedef struct {
    ce_float_t z;
    ce_float_t p;
} ztest_result_t;

typedef struct {
    ce_float_t t;
    ce_float_t p;
    ce_float_t df;
} ttest_result_t;

typedef struct {
    ce_float_t z;
    ce_float_t p;
} zsamp2_result_t;

typedef struct {
    ce_float_t t;
    ce_float_t p;
    ce_float_t df;
} tsamp2_result_t;

typedef struct {
    ce_float_t z;
    ce_float_t p;
} prop1z_result_t;

typedef struct {
    ce_float_t z;
    ce_float_t p;
} prop2z_result_t;

typedef struct {
    ce_float_t chi2;
    ce_float_t p;
    ce_float_t df;
} chi2_result_t;

typedef struct {
    ce_float_t lower;
    ce_float_t upper;
    ce_float_t center;
    ce_float_t me;
} ci_result_t;

/* ── Hypothesis tests ────────────────────────────────────── */
ztest_result_t   stattest_ztest(ce_float_t xbar, ce_float_t mu0,
                                ce_float_t sigma, ce_float_t n, alt_hyp_t alt);
ttest_result_t   stattest_ttest(ce_float_t xbar, ce_float_t mu0,
                                ce_float_t s, ce_float_t n, alt_hyp_t alt);
zsamp2_result_t  stattest_2sampztest(ce_float_t xbar1, ce_float_t sigma1, ce_float_t n1,
                                     ce_float_t xbar2, ce_float_t sigma2, ce_float_t n2,
                                     alt_hyp_t alt);
tsamp2_result_t  stattest_2sampttest(ce_float_t xbar1, ce_float_t s1, ce_float_t n1,
                                     ce_float_t xbar2, ce_float_t s2, ce_float_t n2,
                                     int pooled, alt_hyp_t alt);
prop1z_result_t  stattest_1propztest(ce_float_t x, ce_float_t n,
                                     ce_float_t p0, alt_hyp_t alt);
prop2z_result_t  stattest_2propztest(ce_float_t x1, ce_float_t n1,
                                     ce_float_t x2, ce_float_t n2, alt_hyp_t alt);
chi2_result_t    stattest_chi2test(const ce_float_t *observed,
                                   const ce_float_t *expected, int k);

/* ── Confidence intervals ────────────────────────────────── */
ci_result_t stattest_zinterval(ce_float_t xbar, ce_float_t sigma,
                               ce_float_t n, ce_float_t clevel);
ci_result_t stattest_tinterval(ce_float_t xbar, ce_float_t s,
                               ce_float_t n, ce_float_t clevel);
ci_result_t stattest_1propzint(ce_float_t x, ce_float_t n, ce_float_t clevel);
ci_result_t stattest_2propzint(ce_float_t x1, ce_float_t n1,
                               ce_float_t x2, ce_float_t n2, ce_float_t clevel);
ci_result_t stattest_2sampzint(ce_float_t xbar1, ce_float_t sigma1, ce_float_t n1,
                               ce_float_t xbar2, ce_float_t sigma2, ce_float_t n2,
                               ce_float_t clevel);
ci_result_t stattest_2samptint(ce_float_t xbar1, ce_float_t s1, ce_float_t n1,
                               ce_float_t xbar2, ce_float_t s2, ce_float_t n2,
                               ce_float_t clevel);

/* ── LinReg t-test ───────────────────────────────────────── */
/* Tests H0: slope = 0. Takes paired data (x0,y0,x1,y1,...).
 * t = r * sqrt(n-2) / sqrt(1-r^2), df = n-2 where n = pairs. */
typedef struct {
    ce_float_t t;
    ce_float_t p;
    ce_float_t df;
    ce_float_t r;     /* correlation coefficient */
    ce_float_t m;     /* slope */
    ce_float_t b;     /* intercept */
} linreg_ttest_result_t;

linreg_ttest_result_t stattest_linreg_ttest(const ce_float_t *data, int n);

/* ── One-way ANOVA ──────────────────────────────────────── */
/* Tests H0: all group means are equal.
 * Takes k groups via list_t pointers. */
typedef struct {
    ce_float_t F;
    ce_float_t p;
    ce_float_t df_between;
    ce_float_t df_within;
    ce_float_t SS_between;
    ce_float_t SS_within;
    ce_float_t MS_between;
    ce_float_t MS_within;
} anova_result_t;

anova_result_t stattest_anova(const ce_float_t *const *groups,
                              const int *group_sizes, int k);

/* ── Inverse t-distribution (public wrapper for testing) ─── */
ce_float_t stattest_inv_t(ce_float_t area, ce_float_t df);

/* ── Promoted prob helpers (used by stattest) ────────────── */
ce_float_t prob_std_normal_cdf(ce_float_t x);
ce_float_t prob_std_normal_pdf(ce_float_t x);
ce_float_t prob_t_pdf(ce_float_t t, ce_float_t df);
ce_float_t prob_chi2_pdf(ce_float_t x, ce_float_t df);
ce_float_t prob_gamma_half_int(ce_float_t x);
ce_float_t prob_inv_normal(ce_float_t area);

#endif /* STATTEST_H */
