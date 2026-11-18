/*
 * complex.c — Complex number operations
 *
 * Parallel evaluator for complex mode. When complex_mode is OFF,
 * the existing eval_expr() runs (zero regression risk). When ON,
 * ceval_expr() handles arithmetic, the i constant, and all functions.
 */
#include "complex.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ── Helpers ─────────────────────────────────────────────── */

static ce_complex_t make_c(ce_float_t re, ce_float_t im)
{
    ce_complex_t z;
    z.re = re; z.im = im;
    return z;
}

static void set_err(eval_ctx_t *ctx, ce_error_t code, const char *msg)
{
    ctx->error = code;
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
}

/* Convert angle to radians if degree mode is active */
static ce_float_t to_rad(ce_float_t a)
{
    if (g_settings.angle_mode == ANGLE_DEGREES)
        return a * (CE_PI / 180.0f);
    return a;
}

static ce_float_t from_rad(ce_float_t a)
{
    if (g_settings.angle_mode == ANGLE_DEGREES)
        return a * (180.0f / CE_PI);
    return a;
}

/* ── Complex arithmetic ──────────────────────────────────── */

ce_complex_t cadd(ce_complex_t a, ce_complex_t b)
{
    return make_c(a.re + b.re, a.im + b.im);
}

ce_complex_t csub(ce_complex_t a, ce_complex_t b)
{
    return make_c(a.re - b.re, a.im - b.im);
}

ce_complex_t cmul(ce_complex_t a, ce_complex_t b)
{
    return make_c(a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re);
}

ce_complex_t cdiv(ce_complex_t a, ce_complex_t b)
{
    ce_float_t d = b.re*b.re + b.im*b.im;
    if (d == 0) return make_c(NAN, NAN);
    return make_c((a.re*b.re + a.im*b.im) / d,
                  (a.im*b.re - a.re*b.im) / d);
}

ce_complex_t ce_cpow(ce_complex_t base, ce_complex_t exp)
{
    /* z^w = exp(w * ln(z)) */
    ce_complex_t lnz, wlnz;
    if (base.re == 0 && base.im == 0) return make_c(0, 0);
    lnz = cln_z(base);
    wlnz = cmul(exp, lnz);
    return cexp_z(wlnz);
}

/* ── Complex functions ───────────────────────────────────── */

ce_float_t cabs_val(ce_complex_t z)
{
    return sqrtf(z.re*z.re + z.im*z.im);
}

ce_float_t ce_carg(ce_complex_t z)
{
    return atan2f(z.im, z.re);
}

ce_complex_t cconj(ce_complex_t z)
{
    return make_c(z.re, -z.im);
}

ce_complex_t cexp_z(ce_complex_t z)
{
    ce_float_t r = expf(z.re);
    return make_c(r * cosf(z.im), r * sinf(z.im));
}

ce_complex_t cln_z(ce_complex_t z)
{
    if (cabs_val(z) == 0) return make_c(-INFINITY, 0);
    return make_c(logf(cabs_val(z)), ce_carg(z));
}

ce_complex_t csqrt_z(ce_complex_t z)
{
    ce_float_t r, th;
    /* Handle pure real case explicitly to avoid -0 atan2 issues */
    if (fabsf(z.im) < 1e-30f) {
        if (z.re >= 0) return make_c(sqrtf(z.re), 0);
        return make_c(0, sqrtf(-z.re));
    }
    r = sqrtf(cabs_val(z));
    th = ce_carg(z) / 2.0f;
    return make_c(r * cosf(th), r * sinf(th));
}

ce_complex_t csin_z(ce_complex_t z)
{
    /* sin(a+bi) = sin(a)cosh(b) + i*cos(a)sinh(b) */
    return make_c(sinf(z.re) * coshf(z.im),
                  cosf(z.re) * sinhf(z.im));
}

ce_complex_t ccos_z(ce_complex_t z)
{
    /* cos(a+bi) = cos(a)cosh(b) - i*sin(a)sinh(b) */
    return make_c(cosf(z.re) * coshf(z.im),
                  -sinf(z.re) * sinhf(z.im));
}

ce_complex_t ctan_z(ce_complex_t z)
{
    ce_complex_t s = csin_z(z);
    ce_complex_t c = ccos_z(z);
    return cdiv(s, c);
}

ce_complex_t csec_z(ce_complex_t z)
{
    ce_complex_t c = ccos_z(z);
    return cdiv(make_c(1, 0), c);
}

ce_complex_t ccsc_z(ce_complex_t z)
{
    ce_complex_t s = csin_z(z);
    return cdiv(make_c(1, 0), s);
}

ce_complex_t ccot_z(ce_complex_t z)
{
    ce_complex_t s = csin_z(z);
    ce_complex_t c = ccos_z(z);
    return cdiv(c, s);
}

ce_complex_t casin_z(ce_complex_t z)
{
    /* asin(z) = -i * ln(iz + sqrt(1 - z^2)) */
    ce_complex_t iz = cmul(make_c(0, 1), z);
    ce_complex_t z2 = cmul(z, z);
    ce_complex_t one_minus_z2 = csub(make_c(1, 0), z2);
    ce_complex_t sq = csqrt_z(one_minus_z2);
    ce_complex_t inner = cadd(iz, sq);
    ce_complex_t ln_inner = cln_z(inner);
    return cmul(make_c(0, -1), ln_inner);
}

ce_complex_t cacos_z(ce_complex_t z)
{
    /* acos(z) = -i * ln(z + i*sqrt(1 - z^2)) */
    ce_complex_t z2 = cmul(z, z);
    ce_complex_t one_minus_z2 = csub(make_c(1, 0), z2);
    ce_complex_t sq = csqrt_z(one_minus_z2);
    ce_complex_t isq = cmul(make_c(0, 1), sq);
    ce_complex_t inner = cadd(z, isq);
    ce_complex_t ln_inner = cln_z(inner);
    return cmul(make_c(0, -1), ln_inner);
}

ce_complex_t catan_z(ce_complex_t z)
{
    /* atan(z) = (i/2) * ln((1 - iz) / (1 + iz)) */
    ce_complex_t iz = cmul(make_c(0, 1), z);
    ce_complex_t num = csub(make_c(1, 0), iz);
    ce_complex_t den = cadd(make_c(1, 0), iz);
    ce_complex_t frac = cdiv(num, den);
    ce_complex_t ln_frac = cln_z(frac);
    return cmul(make_c(0, 0.5f), ln_frac);
}

/* ── Complex inverse reciprocal trig ─────────────────────── */

ce_complex_t casec_z(ce_complex_t z)
{
    /* asec(z) = acos(1/z) */
    return cacos_z(cdiv(make_c(1, 0), z));
}

ce_complex_t cacsc_z(ce_complex_t z)
{
    /* acsc(z) = asin(1/z) */
    return casin_z(cdiv(make_c(1, 0), z));
}

ce_complex_t cacot_z(ce_complex_t z)
{
    /* acot(z) = atan(1/z) */
    return catan_z(cdiv(make_c(1, 0), z));
}

/* ── Complex hyperbolic functions ────────────────────────── */

ce_complex_t csinh_z(ce_complex_t z)
{
    /* sinh(z) = (e^z - e^(-z)) / 2 */
    ce_complex_t ez = cexp_z(z);
    ce_complex_t enz = cexp_z(make_c(-z.re, -z.im));
    return make_c((ez.re - enz.re) / 2.0f, (ez.im - enz.im) / 2.0f);
}

ce_complex_t ccosh_z(ce_complex_t z)
{
    /* cosh(z) = (e^z + e^(-z)) / 2 */
    ce_complex_t ez = cexp_z(z);
    ce_complex_t enz = cexp_z(make_c(-z.re, -z.im));
    return make_c((ez.re + enz.re) / 2.0f, (ez.im + enz.im) / 2.0f);
}

ce_complex_t ctanh_z(ce_complex_t z)
{
    /* tanh(z) = sinh(z) / cosh(z) */
    return cdiv(csinh_z(z), ccosh_z(z));
}

ce_complex_t casinh_z(ce_complex_t z)
{
    /* asinh(z) = ln(z + sqrt(z^2 + 1)) */
    ce_complex_t z2 = cmul(z, z);
    ce_complex_t inner = cadd(z2, make_c(1, 0));
    ce_complex_t sq = csqrt_z(inner);
    return cln_z(cadd(z, sq));
}

ce_complex_t cacosh_z(ce_complex_t z)
{
    /* acosh(z) = ln(z + sqrt(z^2 - 1)) */
    ce_complex_t z2 = cmul(z, z);
    ce_complex_t inner = csub(z2, make_c(1, 0));
    ce_complex_t sq = csqrt_z(inner);
    return cln_z(cadd(z, sq));
}

ce_complex_t catanh_z(ce_complex_t z)
{
    /* atanh(z) = (1/2) * ln((1+z) / (1-z)) */
    ce_complex_t num = cadd(make_c(1, 0), z);
    ce_complex_t den = csub(make_c(1, 0), z);
    ce_complex_t frac = cdiv(num, den);
    ce_complex_t ln_frac = cln_z(frac);
    return make_c(ln_frac.re / 2.0f, ln_frac.im / 2.0f);
}

/* ── Complex reciprocal hyperbolic ────────────────────────── */

ce_complex_t csech_z(ce_complex_t z)
{
    /* sech(z) = 1/cosh(z) */
    return cdiv(make_c(1, 0), ccosh_z(z));
}

ce_complex_t ccsch_z(ce_complex_t z)
{
    /* csch(z) = 1/sinh(z) */
    return cdiv(make_c(1, 0), csinh_z(z));
}

ce_complex_t ccoth_z(ce_complex_t z)
{
    /* coth(z) = cosh(z)/sinh(z) */
    return cdiv(ccosh_z(z), csinh_z(z));
}

/* ── Complex evaluator ───────────────────────────────────── */

static ce_complex_t ceval_inner(eval_ctx_t *ctx, const ast_node_t *node);

ce_complex_t ceval_expr(eval_ctx_t *ctx, const ast_node_t *node)
{
    ce_complex_t ret;
    if (!node) { set_err(ctx, CE_ERR_INTERNAL, "NULL node"); return make_c(0,0); }
    if (ctx->error != CE_OK) return make_c(0,0);
    if (ctx->eval_depth >= 50) {
        set_err(ctx, CE_ERR_OVERFLOW, "Eval recursion limit");
        return make_c(0,0);
    }
    ctx->eval_depth++;
    ret = ceval_inner(ctx, node);
    ctx->eval_depth--;
    return ret;
}

static ce_float_t lookup_var(eval_ctx_t *ctx, char name)
{
    int i;
    for (i = 0; i < MAX_VARS; i++) {
        if (ctx->vars[i].bound && ctx->vars[i].name == name)
            return ctx->vars[i].value;
    }
    /* Unlike eval.c's NODE_VARIABLE read, this does not set
     * ctx->undefined_var; a caller that names the variable must add it. */
    set_err(ctx, CE_ERR_UNDEFINED, "Undefined var");
    return 0;
}

static ce_complex_t ceval_inner(eval_ctx_t *ctx, const ast_node_t *node)
{
    switch (node->type) {
    case NODE_NUMBER:
        return make_c(node->d.number, 0);

    case NODE_VARIABLE:
        return make_c(lookup_var(ctx, node->d.variable), 0);

    case NODE_CONSTANT:
        if (node->d.constant == CONST_PI) return make_c(CE_PI, 0);
        if (node->d.constant == CONST_E)  return make_c(CE_E, 0);
        if (node->d.constant == CONST_I)  return make_c(0, 1);
        return make_c(CE_INF, 0);

    case NODE_BINOP: {
        ce_complex_t l = ceval_expr(ctx, node->d.binop.left);
        ce_complex_t r = ceval_expr(ctx, node->d.binop.right);
        if (ctx->error) return make_c(0,0);
        switch (node->d.binop.op) {
        case OP_ADD: return cadd(l, r);
        case OP_SUB: return csub(l, r);
        case OP_MUL: return cmul(l, r);
        case OP_DIV:
            if (r.re == 0 && r.im == 0) {
                set_err(ctx, CE_ERR_DIV_ZERO, "Division by zero");
                return make_c(0,0);
            }
            return cdiv(l, r);
        case OP_POW: return ce_cpow(l, r);
        }
        break;
    }

    case NODE_UNOP: {
        ce_complex_t v = ceval_expr(ctx, node->d.unop.operand);
        if (ctx->error) return make_c(0,0);
        if (node->d.unop.op == UOP_NEG) return make_c(-v.re, -v.im);
        break;
    }

    case NODE_FUNC: {
        ce_complex_t a = ceval_expr(ctx, node->d.func.arg);
        if (ctx->error) return make_c(0,0);
        switch (node->d.func.id) {
        case FN_SIN: {
            ce_complex_t ar = make_c(to_rad(a.re), a.im);
            return csin_z(ar);
        }
        case FN_COS: {
            ce_complex_t ar = make_c(to_rad(a.re), a.im);
            return ccos_z(ar);
        }
        case FN_TAN: {
            ce_complex_t ar = make_c(to_rad(a.re), a.im);
            return ctan_z(ar);
        }
        case FN_SEC: {
            ce_complex_t ar = make_c(to_rad(a.re), a.im);
            return csec_z(ar);
        }
        case FN_CSC: {
            ce_complex_t ar = make_c(to_rad(a.re), a.im);
            return ccsc_z(ar);
        }
        case FN_COT: {
            ce_complex_t ar = make_c(to_rad(a.re), a.im);
            return ccot_z(ar);
        }
        case FN_ASIN:
            return casin_z(a);
        case FN_ACOS:
            return cacos_z(a);
        case FN_ATAN:
            return catan_z(a);
        case FN_ASEC:
            return casec_z(a);
        case FN_ACSC:
            return cacsc_z(a);
        case FN_ACOT:
            return cacot_z(a);
        case FN_SQRT:
            return csqrt_z(a);
        case FN_ABS:
            return make_c(cabs_val(a), 0);
        case FN_EXP:
            return cexp_z(a);
        case FN_LN:
            return cln_z(a);
        case FN_LOG:
            /* log10(z) = ln(z) / ln(10) */
            return make_c(cln_z(a).re / logf(10.0f), cln_z(a).im / logf(10.0f));
        /* real/imag/conj/angle */
        case FN_REAL: return make_c(a.re, 0);
        case FN_IMAG: return make_c(a.im, 0);
        case FN_CONJ: return cconj(a);
        case FN_ANGLE: return make_c(from_rad(ce_carg(a)), 0);
        case FN_SINH:  return csinh_z(a);
        case FN_COSH:  return ccosh_z(a);
        case FN_TANH:  return ctanh_z(a);
        case FN_ASINH: return casinh_z(a);
        case FN_ACOSH: return cacosh_z(a);
        case FN_ATANH: return catanh_z(a);
        case FN_SECH:  return csech_z(a);
        case FN_CSCH:  return ccsch_z(a);
        case FN_COTH:  return ccoth_z(a);
        default:
            /* For other functions, fall back to real evaluation */
            if (fabsf(a.im) < 1e-10f) {
                ce_float_t rv = eval_expr(ctx, node);
                return make_c(rv, 0);
            }
            set_err(ctx, CE_ERR_TOO_COMPLEX, "Complex func unsupported");
            return make_c(0,0);
        }
    }

    default:
        /* Fall back to real evaluator for other node types */
        {
            ce_float_t rv = eval_expr(ctx, node);
            return make_c(rv, 0);
        }
    }
    return make_c(0,0);
}

/* ── Display ─────────────────────────────────────────────── */

int complex_to_str(ce_complex_t z, char *buf, int size)
{
    if (size <= 0) return 0;

    if (fabsf(z.im) < 1e-7f) {
        return snprintf(buf, size, "%.6g", (double)z.re);
    }

    if (g_settings.complex_mode == COMPLEX_POLAR) {
        ce_float_t r = cabs_val(z);
        ce_float_t th = from_rad(ce_carg(z));
        return snprintf(buf, size, "%.6g/_%.6g", (double)r, (double)th);
    }

    /* Rectangular form */
    if (fabsf(z.re) < 1e-7f) {
        if (fabsf(z.im - 1.0f) < 1e-7f) return snprintf(buf, size, "i");
        if (fabsf(z.im + 1.0f) < 1e-7f) return snprintf(buf, size, "-i");
        return snprintf(buf, size, "%.6gi", (double)z.im);
    }

    if (fabsf(z.im - 1.0f) < 1e-7f)
        return snprintf(buf, size, "%.6g+i", (double)z.re);
    if (fabsf(z.im + 1.0f) < 1e-7f)
        return snprintf(buf, size, "%.6g-i", (double)z.re);
    if (z.im > 0)
        return snprintf(buf, size, "%.6g+%.6gi", (double)z.re, (double)z.im);
    else
        return snprintf(buf, size, "%.6g%.6gi", (double)z.re, (double)z.im);
}
