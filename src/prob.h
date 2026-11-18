/*
 * prob.h — Probability distribution functions for CalcEngine
 * Normal, binomial, Poisson, geometric, t, chi-squared distributions.
 */
#ifndef PROB_H
#define PROB_H

#include "calcengine.h"
#include "eval.h"

/* All functions take an array of arguments + count, and an eval context
 * for error reporting. They return the computed value. */

ce_float_t prob_normalcdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_invnorm(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_binompdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_binomcdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_poissonpdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_poissoncdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_geometpdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_geometcdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_normalpdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_tcdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_chi2cdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_invt(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_fpdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_fcdf(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_invchi2(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t prob_invf(const ce_float_t *args, int n, eval_ctx_t *ctx);

#endif /* PROB_H */
