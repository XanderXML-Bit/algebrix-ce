/*
 * finance.c — Time Value of Money (TVM) solver
 *
 * Implements the standard TVM equation:
 *   END mode:   PV * (1+r)^n + PMT * ((1+r)^n - 1) / r + FV = 0
 *   BEGIN mode: PV * (1+r)^n + PMT * (1+r) * ((1+r)^n - 1) / r + FV = 0
 *
 * Functions: pv, fv, pmt, nper, rate, eff, nom, bal, sumprn, sumint
 * Rate uses Newton-Raphson iteration.
 * All rates are per-period (not annual percentage).
 * Payment mode controlled by g_settings.pmt_at_begin.
 */
#include "finance.h"
#include <math.h>
#include <string.h>

static void fin_err(eval_ctx_t *ctx, const char *msg)
{
    ctx->error = CE_ERR_DOMAIN;
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
}

/* BEGIN/END mode multiplier: 1 for END, (1+r) for BEGIN */
static ce_float_t pmt_mode_factor(ce_float_t rate)
{
    return g_settings.pmt_at_begin ? (1.0f + rate) : 1.0f;
}

/* pv(n, rate, pmt, fv)
 * PV = -(PMT * K * ((1+r)^n - 1) / r + FV) / (1+r)^n */
ce_float_t finance_pv(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t nper, rate, pmt, fv_val, factor, k;

    if (n < 4) { fin_err(ctx, "pv needs 4 args"); return 0; }
    nper = args[0]; rate = args[1]; pmt = args[2]; fv_val = args[3];

    if (fabsf(rate) < 1e-10f) {
        return -(pmt * nper + fv_val);
    }
    k = pmt_mode_factor(rate);
    factor = powf(1.0f + rate, nper);
    return -(pmt * k * (factor - 1.0f) / rate + fv_val) / factor;
}

/* fv(n, rate, pv, pmt)
 * FV = -(PV * (1+r)^n + PMT * K * ((1+r)^n - 1) / r) */
ce_float_t finance_fv(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t nper, rate, pv_val, pmt, factor, k;

    if (n < 4) { fin_err(ctx, "fv needs 4 args"); return 0; }
    nper = args[0]; rate = args[1]; pv_val = args[2]; pmt = args[3];

    if (rate == 0.0f) {
        return -(pv_val + pmt * nper);
    }
    if (fabsf(rate) <= 0.001f) {
        ce_float_t nr = nper * rate;

        if (fabsf(nr) <= 0.01f) {
            /* Binomial series for ((1+r)^n - 1)/r = n * (1 + correction).
             * The first omitted term relative to n is below 3e-10 here.
             * Keep the corrections apart from the zero-rate sum so they
             * survive when the present value and payments cancel. */
            ce_float_t correction = ((nper - 1.0f) * rate) *
                (0.5f + ((nper - 2.0f) * rate) *
                (1.0f / 6.0f + ((nper - 3.0f) * rate) / 24.0f));
            ce_float_t payments = pmt * nper;
            ce_float_t interest = (pv_val * nr) * (1.0f + correction) +
                                  payments * correction;
            if (g_settings.pmt_at_begin) {
                interest += (payments * rate) * (1.0f + correction);
            }
            return -(pv_val + payments + interest);
        }
        /* The accumulated rate is large enough to subtract 1 from the
         * growth factor. Expand log(1+r) to preserve r in the exponent. */
        factor = expf(nr * (1.0f - rate * (0.5f - rate / 3.0f)));
    } else {
        factor = powf(1.0f + rate, nper);
    }
    k = pmt_mode_factor(rate);
    return -(pv_val * factor + pmt * k * (factor - 1.0f) / rate);
}

/* pmt(n, rate, pv, fv)
 * PMT = -(PV * (1+r)^n + FV) * r / (K * ((1+r)^n - 1)) */
ce_float_t finance_pmt(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t nper, rate, pv_val, fv_val, factor, k;

    if (n < 4) { fin_err(ctx, "pmt needs 4 args"); return 0; }
    nper = args[0]; rate = args[1]; pv_val = args[2]; fv_val = args[3];

    if (fabsf(rate) < 1e-10f) {
        if (fabsf(nper) < 1e-10f) { fin_err(ctx, "pmt: n and rate both zero"); return 0; }
        return -(pv_val + fv_val) / nper;
    }
    k = pmt_mode_factor(rate);
    factor = powf(1.0f + rate, nper);
    if (fabsf(factor - 1.0f) < 1e-10f) { fin_err(ctx, "pmt: degenerate"); return 0; }
    return -(pv_val * factor + fv_val) * rate / (k * (factor - 1.0f));
}

/* nper(rate, pv, pmt, fv)
 * N = log((-FV*r + PMT*K) / (PV*r + PMT*K)) / log(1+r) */
ce_float_t finance_nper(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t rate, pv_val, pmt, fv_val, num, den, k;

    if (n < 4) { fin_err(ctx, "nper needs 4 args"); return 0; }
    rate = args[0]; pv_val = args[1]; pmt = args[2]; fv_val = args[3];

    if (fabsf(rate) < 1e-10f) {
        if (fabsf(pmt) < 1e-10f) { fin_err(ctx, "nper: rate and pmt both zero"); return 0; }
        return -(pv_val + fv_val) / pmt;
    }
    k = pmt_mode_factor(rate);
    num = -fv_val * rate + pmt * k;
    den = pv_val * rate + pmt * k;
    if (den == 0 || num / den <= 0) { fin_err(ctx, "nper: no solution"); return 0; }
    return logf(num / den) / logf(1.0f + rate);
}

/* rate(n, pv, pmt, fv) — Newton-Raphson iteration
 * Solves: PV*(1+r)^n + PMT*K*((1+r)^n - 1)/r + FV = 0
 * Uses double precision internally for numerical stability */
ce_float_t finance_rate(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    double nper, pv_val, pmt_val, fv_val;
    double r, f, df, factor, dfactor, step, k;
    int i;
    bool begin = g_settings.pmt_at_begin;

    if (n < 4) { fin_err(ctx, "rate needs 4 args"); return 0; }
    nper = (double)args[0]; pv_val = (double)args[1];
    pmt_val = (double)args[2]; fv_val = (double)args[3];

    /* Try multiple initial guesses to find convergence */
    {
        double guesses[] = {0.01, 0.005, 0.05, 0.001, 0.1, 0.002, -0.01, -0.05, -0.1};
        int g;
        for (g = 0; g < 9; g++) {
            r = guesses[g];
            for (i = 0; i < 500; i++) {
                if (r < 1e-12 && r > -1e-12) r = 1e-6;
                if (r <= -1.0) r = -0.99;
                if (r > 10.0) r = 10.0;
                k = begin ? (1.0 + r) : 1.0;
                factor = pow(1.0 + r, nper);
                if (!ce_isfinite(factor)) break;
                f = pv_val * factor + pmt_val * k * (factor - 1.0) / r + fv_val;
                if (fabs(f) < 1e-6) return (ce_float_t)r;
                dfactor = nper * pow(1.0 + r, nper - 1.0);
                if (begin) {
                    /* d/dr [K * (F-1)/r] where K=(1+r), F=(1+r)^n */
                    df = pv_val * dfactor +
                         pmt_val * ((factor - 1.0) / r +
                                    k * (dfactor * r - (factor - 1.0)) / (r * r));
                } else {
                    df = pv_val * dfactor + pmt_val * (dfactor * r - (factor - 1.0)) / (r * r);
                }
                if (fabs(df) < 1e-30) break;
                step = f / df;
                if (step > 0.5) step = 0.5;
                if (step < -0.5) step = -0.5;
                r = r - step;
            }
            /* Recompute f for the current r before checking */
            k = begin ? (1.0 + r) : 1.0;
            factor = pow(1.0 + r, nper);
            if (ce_isfinite(factor) && fabs(r) > 1e-30) {
                f = pv_val * factor + pmt_val * k * (factor - 1.0) / r + fv_val;
                if (fabs(f) < 0.1) return (ce_float_t)r;
            }
        }
    }
    fin_err(ctx, "rate: no convergence");
    return 0;
}

/* eff(nom_rate, n) = (1 + nom_rate/n)^n - 1
 * Converts nominal rate to effective annual rate. */
ce_float_t finance_eff(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t nom_rate, periods;
    ce_float_t base;

    if (n < 2) { fin_err(ctx, "eff needs 2 args"); return 0; }
    nom_rate = args[0]; periods = args[1];

    if (periods <= 0 || periods != floorf(periods)) {
        fin_err(ctx, "eff: n must be positive integer");
        return 0;
    }
    base = 1.0f + nom_rate / periods;
    if (base <= 0.0f) {
        fin_err(ctx, "eff: 1+nom/n must be > 0");
        return 0;
    }
    return powf(base, periods) - 1.0f;
}

/* nom(eff_rate, n) = n * ((1 + eff_rate)^(1/n) - 1)
 * Converts effective annual rate to nominal rate. */
ce_float_t finance_nom(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t eff_rate, periods;
    ce_float_t base;

    if (n < 2) { fin_err(ctx, "nom needs 2 args"); return 0; }
    eff_rate = args[0]; periods = args[1];

    if (periods <= 0 || periods != floorf(periods)) {
        fin_err(ctx, "nom: n must be positive integer");
        return 0;
    }
    base = 1.0f + eff_rate;
    if (base <= 0.0f) {
        fin_err(ctx, "nom: 1+eff must be > 0");
        return 0;
    }
    return periods * (powf(base, 1.0f / periods) - 1.0f);
}

/* bal(npmt, rate, pv, pmt)
 * Remaining balance after npmt payments.
 * BAL = PV*(1+r)^npmt + PMT*K*((1+r)^npmt - 1)/r */
ce_float_t finance_bal(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t npmt, rate, pv_val, pmt_val, factor, k;

    if (n < 4) { fin_err(ctx, "bal needs 4 args"); return 0; }
    npmt = args[0]; rate = args[1]; pv_val = args[2]; pmt_val = args[3];

    if (npmt < 0 || npmt != floorf(npmt)) {
        fin_err(ctx, "bal: npmt must be non-negative integer");
        return 0;
    }
    if (fabsf(rate) < 1e-10f) {
        return pv_val + pmt_val * npmt;
    }
    k = pmt_mode_factor(rate);
    factor = powf(1.0f + rate, npmt);
    return pv_val * factor + pmt_val * k * (factor - 1.0f) / rate;
}

/* sumprn(pmt1, pmt2, rate, pv, pmt)
 * Sum of principal portions of payments pmt1 through pmt2.
 * Principal_k = BAL(k) - BAL(k-1) = -(PMT + BAL(k-1)*r)...wait:
 * Actually: Principal_k = BAL(k) - BAL(k-1)
 *   = [PV*(1+r)^k + PMT*((1+r)^k-1)/r] - [PV*(1+r)^(k-1) + PMT*((1+r)^(k-1)-1)/r]
 * Simplifies to: sum_Prn(p1,p2) = BAL(p2) - BAL(p1-1) */
ce_float_t finance_sum_prn(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t pmt1, pmt2, rate, pv_val, pmt_val;
    ce_float_t bal_end, bal_start;
    ce_float_t bal_args[4];

    if (n < 5) { fin_err(ctx, "sumprn needs 5 args"); return 0; }
    pmt1 = args[0]; pmt2 = args[1]; rate = args[2];
    pv_val = args[3]; pmt_val = args[4];

    if (pmt1 < 1 || pmt2 < pmt1 || pmt1 != floorf(pmt1) || pmt2 != floorf(pmt2)) {
        fin_err(ctx, "sumprn: invalid payment range");
        return 0;
    }

    /* BAL(pmt2) */
    bal_args[0] = pmt2; bal_args[1] = rate;
    bal_args[2] = pv_val; bal_args[3] = pmt_val;
    bal_end = finance_bal(bal_args, 4, ctx);
    if (ctx->error) return 0;

    /* BAL(pmt1-1) */
    bal_args[0] = pmt1 - 1;
    bal_start = finance_bal(bal_args, 4, ctx);
    if (ctx->error) return 0;

    return bal_end - bal_start;
}

/* sumint(pmt1, pmt2, rate, pv, pmt)
 * Sum of interest portions of payments pmt1 through pmt2.
 * Total payments in range = PMT * (pmt2 - pmt1 + 1)
 * sum_Int = total_payments - sum_Prn */
ce_float_t finance_sum_int(const ce_float_t *args, int n, eval_ctx_t *ctx)
{
    ce_float_t pmt1, pmt2, pmt_val, total_pmt, sum_prn;

    if (n < 5) { fin_err(ctx, "sumint needs 5 args"); return 0; }
    pmt1 = args[0]; pmt2 = args[1]; pmt_val = args[4];

    if (pmt1 < 1 || pmt2 < pmt1 || pmt1 != floorf(pmt1) || pmt2 != floorf(pmt2)) {
        fin_err(ctx, "sumint: invalid payment range");
        return 0;
    }

    total_pmt = pmt_val * (pmt2 - pmt1 + 1);
    sum_prn = finance_sum_prn(args, n, ctx);
    if (ctx->error) return 0;

    return total_pmt - sum_prn;
}
