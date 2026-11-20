/*
 * polar.c -- Polar, parametric & advanced calculus operations
 *
 * All heavy computation uses Simpson's rule for numerical integration.
 * Symbolic differentiation is attempted first to record steps.
 */
#include "polar.h"
#include "ast.h"
#include "eval.h"
#include "deriv.h"
#include "steps.h"
#include <math.h>

#ifndef CE_POLAR_USE_SIMPLIFY
#define CE_POLAR_USE_SIMPLIFY 1
#endif

#if CE_POLAR_USE_SIMPLIFY
#include "simplify.h"
#else
static ast_node_t *ast_simplify(ast_node_t *expr)
{
    return expr;
}
#endif

/* ── Simpson's rule parameters ─────────────────────────────── */
#define SIMPSON_N 200   /* number of sub-intervals (must be even) */

/* Set when a sample since the last simpson_cb() start failed to evaluate
 * (domain, pole, recursion limit). The NaN polar_eval() returns then is no
 * marker on the CE: its float routines turn NaN and inf operands into finite
 * values (src/ce_float_ops.h), so each caller reports the failure from this
 * flag rather than testing the returned value. */
static bool s_polar_failed;
#define POLAR_STATUS (s_polar_failed ? CE_ERR_DOMAIN : CE_OK)

/* ── Internal: evaluate f(var=val) ─────────────────────────── */
static ce_float_t polar_eval(const ast_node_t *expr, char var, ce_float_t val)
{
    eval_ctx_t ctx;
    ce_float_t result;
    eval_init(&ctx);
    if (!eval_set_var(&ctx, var, val)) { s_polar_failed = true; return NAN; }
    result = eval_expr(&ctx, expr);
    if (ctx.error != CE_OK || !ce_isfinite(result)) { s_polar_failed = true; return NAN; }
    return result;
}

/*
 * Simpson's rule for an integrand defined by a C callback.
 * The callback receives (var_value, user_data).
 */
typedef struct {
    const ast_node_t *expr1;
    const ast_node_t *expr2;  /* NULL for single-expr integrands */
    const ast_node_t *deriv1;
    const ast_node_t *deriv2; /* NULL if not needed */
    char var;
} integrand_ctx_t;

/* Evaluate sqrt(1 + [f'(x)]^2) for Cartesian arc length */
static ce_float_t arclen_cartesian_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t fp = polar_eval(ic->deriv1, ic->var, val);
    return sqrtf(1.0f + fp * fp);
}

/* Evaluate sqrt([dx/dt]^2 + [dy/dt]^2) for parametric arc length */
static ce_float_t arclen_param_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t dxdt = polar_eval(ic->deriv1, ic->var, val);
    ce_float_t dydt = polar_eval(ic->deriv2, ic->var, val);
    return sqrtf(dxdt * dxdt + dydt * dydt);
}

/* Evaluate |f(x)| * sqrt(1 + [f'(x)]^2) for surface area */
static ce_float_t surfrev_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t fx = polar_eval(ic->expr1, ic->var, val);
    ce_float_t fp = polar_eval(ic->deriv1, ic->var, val);
    return fabsf(fx) * sqrtf(1.0f + fp * fp);
}

/* Evaluate [f(x)]^2 for disk method */
static ce_float_t disk_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t fx = polar_eval(ic->expr1, ic->var, val);
    return fx * fx;
}

/* Evaluate [R(x)]^2 - [r(x)]^2 for washer method */
static ce_float_t washer_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t R = polar_eval(ic->expr1, ic->var, val);
    ce_float_t r = polar_eval(ic->expr2, ic->var, val);
    return R * R - r * r;
}

/* Evaluate x * f(x) for shell method */
static ce_float_t shell_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t fx = polar_eval(ic->expr1, ic->var, val);
    return val * fx;
}

/*
 * Generic Simpson's rule with a callback function.
 * fn_ptr evaluates the integrand at a single point.
 */
typedef ce_float_t (*integrand_fn)(ce_float_t, const integrand_ctx_t *);

static ce_float_t simpson_cb(integrand_fn fn, const integrand_ctx_t *ic,
                             ce_float_t a, ce_float_t b)
{
    int i;
    ce_float_t h = (b - a) / SIMPSON_N;
    ce_float_t sum;

    s_polar_failed = false;
    sum = fn(a, ic) + fn(b, ic);

    for (i = 1; i < SIMPSON_N; i++) {
        ce_float_t xi = a + i * h;
        ce_float_t fi = fn(xi, ic);
        sum += (i % 2 == 0) ? 2.0f * fi : 4.0f * fi;
    }
    return sum * h / 3.0f;
}

/* ── Public API ─────────────────────────────────────────────── */

void polar_to_rect(ce_float_t r, ce_float_t theta,
                   ce_float_t *x, ce_float_t *y)
{
    *x = r * cosf(theta);
    *y = r * sinf(theta);
}

void rect_to_polar(ce_float_t x, ce_float_t y,
                   ce_float_t *r, ce_float_t *theta)
{
    *r     = sqrtf(x * x + y * y);
    *theta = atan2f(y, x);
}

/* Integrand 0.5 * r^2 for polar area */
static ce_float_t polar_area_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t r = polar_eval(ic->expr1, ic->var, val);
    return 0.5f * r * r;
}

ce_float_t polar_area(const ast_node_t *r_expr, char var,
                      ce_float_t a, ce_float_t b,
                      ce_error_t *err)
{
    integrand_ctx_t ic;
    ce_float_t result;

    if (!r_expr) { *err = CE_ERR_INTERNAL; return 0; }

    steps_addf("Polar area: A = (1/2)*int(r^2, %c, %.4g, %.4g)",
               var, (double)a, (double)b);

    ic.expr1  = r_expr;
    ic.var    = var;

    result = simpson_cb(polar_area_fn, &ic, a, b);
    steps_addf("A = %.6g (Simpson's, n=%d)", (double)result, SIMPSON_N);

    *err = POLAR_STATUS;
    return result;
}

/* Integrand sqrt(r^2 + [dr/dtheta]^2) for polar arc length */
static ce_float_t arclen_polar_fn(ce_float_t val, const integrand_ctx_t *ic)
{
    ce_float_t r  = polar_eval(ic->expr1, ic->var, val);
    ce_float_t dr = polar_eval(ic->deriv1, ic->var, val);
    return sqrtf(r * r + dr * dr);
}

ce_float_t polar_arclen_polar(const ast_node_t *r_expr, char var,
                              ce_float_t a, ce_float_t b,
                              ce_error_t *err)
{
    ast_node_t *dr;
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("Polar arc len: int(sqrt(r^2+[dr/d%c]^2), %c, %.4g, %.4g)",
               var, var, (double)a, (double)b);

    dr = deriv_diff_quiet(r_expr, var);
    if (!dr) {
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }
    dr = ast_simplify(dr);
    if (!dr) { *err = CE_ERR_TOO_COMPLEX; return 0; }

    {
        char buf[120];
        ast_to_str(dr, buf, sizeof(buf));
        steps_addf("dr/d%c = %s", var, buf);
    }

    ic.expr1  = r_expr;
    ic.deriv1 = dr;
    ic.var    = var;

    result = simpson_cb(arclen_polar_fn, &ic, a, b);
    steps_addf("S = %.6g (Simpson's, n=%d)", (double)result, SIMPSON_N);

    ast_free(dr);
    *err = POLAR_STATUS;
    return result;
}

ce_float_t polar_arclen(const ast_node_t *f, char var,
                        ce_float_t a, ce_float_t b,
                        ce_error_t *err)
{
    ast_node_t *fp;
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("Arc length: S = int(sqrt(1+[f'(%c)]^2), %c, %.4g, %.4g)",
               var, var, (double)a, (double)b);

    /* Symbolic differentiation */
    fp = deriv_diff_quiet(f, var);
    if (!fp) {
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }
    fp = ast_simplify(fp);
    if (!fp) { *err = CE_ERR_TOO_COMPLEX; return 0; }

    {
        char buf[120];
        ast_to_str(fp, buf, sizeof(buf));
        steps_addf("f'(%c) = %s", var, buf);
    }

    ic.expr1  = f;
    ic.deriv1 = fp;
    ic.var    = var;

    result = simpson_cb(arclen_cartesian_fn, &ic, a, b);
    steps_addf("S = %.6g (Simpson's, n=%d)", (double)result, SIMPSON_N);

    ast_free(fp);
    *err = POLAR_STATUS;
    return result;
}

ce_float_t polar_arclen_param(const ast_node_t *xt, const ast_node_t *yt,
                              char var, ce_float_t a, ce_float_t b,
                              ce_error_t *err)
{
    ast_node_t *dxdt, *dydt;
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("Param arc len: int(sqrt([dx/d%c]^2+[dy/d%c]^2), %c, %.4g, %.4g)",
               var, var, var, (double)a, (double)b);

    dxdt = deriv_diff_quiet(xt, var);
    dydt = deriv_diff_quiet(yt, var);
    if (!dxdt || !dydt) {
        if (dxdt) ast_free(dxdt);
        if (dydt) ast_free(dydt);
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }
    dxdt = ast_simplify(dxdt);
    dydt = ast_simplify(dydt);
    if (!dxdt || !dydt) {
        ast_free(dxdt); ast_free(dydt);
        *err = CE_ERR_TOO_COMPLEX; return 0;
    }

    {
        char buf1[64], buf2[64];
        ast_to_str(dxdt, buf1, sizeof(buf1));
        ast_to_str(dydt, buf2, sizeof(buf2));
        steps_addf("dx/d%c = %s", var, buf1);
        steps_addf("dy/d%c = %s", var, buf2);
    }

    ic.deriv1 = dxdt;
    ic.deriv2 = dydt;
    ic.var    = var;

    result = simpson_cb(arclen_param_fn, &ic, a, b);
    steps_addf("S = %.6g", (double)result);

    ast_free(dxdt);
    ast_free(dydt);
    *err = POLAR_STATUS;
    return result;
}

ce_float_t polar_surfrev(const ast_node_t *f, char var,
                         ce_float_t a, ce_float_t b,
                         ce_error_t *err)
{
    ast_node_t *fp;
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("SurfArea (x-axis): 2pi*int(|f|*sqrt(1+f'^2), %c, %.4g, %.4g)",
               var, (double)a, (double)b);

    fp = deriv_diff_quiet(f, var);
    if (!fp) {
        *err = CE_ERR_TOO_COMPLEX;
        return 0;
    }
    fp = ast_simplify(fp);
    if (!fp) { *err = CE_ERR_TOO_COMPLEX; return 0; }

    {
        char buf[120];
        ast_to_str(fp, buf, sizeof(buf));
        steps_addf("f'(%c) = %s", var, buf);
    }

    ic.expr1  = f;
    ic.deriv1 = fp;
    ic.var    = var;

    result = 2.0f * CE_PI * simpson_cb(surfrev_fn, &ic, a, b);
    steps_addf("SA = %.6g", (double)result);

    ast_free(fp);
    *err = POLAR_STATUS;
    return result;
}

ce_float_t polar_disk(const ast_node_t *f, char var,
                      ce_float_t a, ce_float_t b,
                      ce_error_t *err)
{
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("Disk method: V = pi*int([f(%c)]^2, %c, %.4g, %.4g)",
               var, var, (double)a, (double)b);

    ic.expr1 = f;
    ic.var   = var;

    result = CE_PI * simpson_cb(disk_fn, &ic, a, b);
    steps_addf("V = %.6g", (double)result);

    *err = POLAR_STATUS;
    return result;
}

ce_float_t polar_washer(const ast_node_t *R, const ast_node_t *r,
                        char var, ce_float_t a, ce_float_t b,
                        ce_error_t *err)
{
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("Washer: V = pi*int(R^2-r^2, %c, %.4g, %.4g)",
               var, (double)a, (double)b);

    ic.expr1 = R;
    ic.expr2 = r;
    ic.var   = var;

    result = CE_PI * simpson_cb(washer_fn, &ic, a, b);
    steps_addf("V = %.6g", (double)result);

    *err = POLAR_STATUS;
    return result;
}

ce_float_t polar_shell(const ast_node_t *f, char var,
                       ce_float_t a, ce_float_t b,
                       ce_error_t *err)
{
    integrand_ctx_t ic;
    ce_float_t result;

    steps_addf("Shell: V = 2pi*int(%c*f(%c), %c, %.4g, %.4g)",
               var, var, var, (double)a, (double)b);

    ic.expr1 = f;
    ic.var   = var;

    result = 2.0f * CE_PI * simpson_cb(shell_fn, &ic, a, b);
    steps_addf("V = %.6g", (double)result);

    *err = POLAR_STATUS;
    return result;
}
