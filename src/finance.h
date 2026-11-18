/*
 * finance.h — Time Value of Money (TVM) functions for CalcEngine
 * Implements PV, FV, PMT, NPER, RATE for TI-84 parity.
 */
#ifndef FINANCE_H
#define FINANCE_H

#include "calcengine.h"
#include "eval.h"

/* All functions take an array of arguments + count, and an eval context
 * for error reporting. They return the computed value. */

ce_float_t finance_pv(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_fv(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_pmt(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_nper(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_rate(const ce_float_t *args, int n, eval_ctx_t *ctx);

/* Interest conversion: nom(eff_rate, n) and eff(nom_rate, n) */
ce_float_t finance_nom(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_eff(const ce_float_t *args, int n, eval_ctx_t *ctx);

/* Amortization: bal(npmt, rate, pv, pmt),
 *               sumprn(pmt1, pmt2, rate, pv, pmt),
 *               sumint(pmt1, pmt2, rate, pv, pmt) */
ce_float_t finance_bal(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_sum_prn(const ce_float_t *args, int n, eval_ctx_t *ctx);
ce_float_t finance_sum_int(const ce_float_t *args, int n, eval_ctx_t *ctx);

#endif /* FINANCE_H */
