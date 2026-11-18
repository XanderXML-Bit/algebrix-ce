/*
 * dsolve.c — Enhanced differential equation solver
 *
 * Implements dsolve() auto-detection + exact ODE solving.
 * Delegates separable/linear to existing integrate.c routines.
 */
#include "dsolve.h"
#include "integrate.h"
#include "deriv.h"
#include "eval.h"
#include "simplify.h"
#include "ast.h"
#include "memory.h"
#include "steps.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

const char *dsolve_method_name(ode_method_t method)
{
    switch (method) {
    case ODE_METHOD_SEPARABLE:   return "Separation of Variables";
    case ODE_METHOD_LINEAR:      return "Integrating Factor (Linear)";
    case ODE_METHOD_EXACT:       return "Exact Equation";
    case ODE_METHOD_UNSOLVABLE:  return "Unsolvable by known methods";
    default:                     return "Unknown";
    }
}

/* ── Exact ODE solver ─────────────────────────────────────── */

/*
 * Check exactness: ∂M/∂y =?= ∂N/∂x
 * Uses numerical evaluation at a test point to verify equality.
 */
static bool is_exact(const ast_node_t *M, const ast_node_t *N,
                     char x_var, char y_var)
{
    /* Compute ∂M/∂y and ∂N/∂x symbolically, then evaluate numerically */
    ast_node_t *dM_dy = NULL, *dN_dx = NULL;
    eval_ctx_t ctx;
    ce_float_t val_dM, val_dN;
    bool result = false;
    int sc = steps_count();

    dM_dy = deriv_diff_quiet(M, y_var);
    dN_dx = deriv_diff_quiet(N, x_var);
    steps_restore(sc);

    if (!dM_dy || !dN_dx) goto done;

    /* Evaluate at (x=1.3, y=0.7) — chosen to avoid common singularities */
    eval_init(&ctx);
    eval_set_var(&ctx, x_var, 1.3f);
    eval_set_var(&ctx, y_var, 0.7f);
    val_dM = eval_expr(&ctx, dM_dy);
    if (eval_error(&ctx) != CE_OK) goto done;

    eval_init(&ctx);
    eval_set_var(&ctx, x_var, 1.3f);
    eval_set_var(&ctx, y_var, 0.7f);
    val_dN = eval_expr(&ctx, dN_dx);
    if (eval_error(&ctx) != CE_OK) goto done;

    result = (fabsf(val_dM - val_dN) < 1e-3f);

done:
    if (dM_dy) ast_free(dM_dy);
    if (dN_dx) ast_free(dN_dx);
    return result;
}

bool dsolve_exact(const ast_node_t *M, const ast_node_t *N,
                  char x_var, char y_var,
                  char *result_buf, int buf_size)
{
    /*
     * Exact ODE: M(x,y)dx + N(x,y)dy = 0
     * Solution: Find F(x,y) s.t. Fx = M, Fy = N
     * F = ∫M dx  (integrate M w.r.t. x, treating y as constant)
     * Then g'(y) = N - ∂F/∂y, integrate to get g(y)
     * Solution: F(x,y) + g(y) = C
     */
    ast_node_t *F = NULL;
    ast_node_t *dF_dy = NULL;
    ast_node_t *g_prime = NULL;
    ast_node_t *g = NULL;
    char F_str[128], gp_str[128], g_str[64];
    int sc;
    bool ok = false;

    if (!M || !N || !result_buf || buf_size <= 0) return false;

    steps_add("Exact ODE: check ∂M/∂y = ∂N/∂x");

    if (!is_exact(M, N, x_var, y_var)) {
        steps_add("Exactness condition NOT satisfied");
        return false;
    }
    steps_addf("∂M/∂%c = ∂N/∂%c  (exact!)", y_var, x_var);

    /* Step 1: F = ∫M dx */
    steps_addf("Integrate M w.r.t. %c", x_var);
    sc = steps_count();
    F = integrate(M, x_var);
    steps_restore(sc);

    if (!F) {
        steps_add("Cannot integrate M symbolically");
        return false;
    }
    ast_to_display_str(F, F_str, sizeof(F_str));
    steps_addf("F(%c,%c) = %s  + g(%c)", x_var, y_var, F_str, y_var);

    /* Step 2: g'(y) = N - ∂F/∂y */
    steps_addf("Compute N - ∂F/∂%c to find g'(%c)", y_var, y_var);
    sc = steps_count();
    dF_dy = deriv_diff_quiet(F, y_var);
    steps_restore(sc);

    if (!dF_dy) {
        steps_add("Cannot differentiate F w.r.t. y");
        ast_free(F);
        return false;
    }

    /* g_prime = N - dF_dy */
    ast_node_t *N_clone = ast_clone(N);
    if (!N_clone) { ast_free(F); ast_free(dF_dy); return false; }
    g_prime = ast_binop(OP_SUB, N_clone, dF_dy);
    /* ast_binop frees N_clone and dF_dy when it fails. */
    if (!g_prime) { ast_free(F); return false; }
    dF_dy = NULL; /* owned by g_prime */

    g_prime = ast_simplify(g_prime);
    ast_to_str(g_prime, gp_str, sizeof(gp_str));
    steps_addf("g'(%c) = %s", y_var, gp_str);

    /* Step 3: g = ∫g'(y) dy */
    steps_addf("Integrate g'(%c) w.r.t. %c", y_var, y_var);
    sc = steps_count();
    g = integrate(g_prime, y_var);
    steps_restore(sc);

    if (!g) {
        steps_add("Cannot integrate g'(y) symbolically");
        ast_free(F); ast_free(g_prime);
        return false;
    }
    ast_to_display_term(g, g_str, sizeof(g_str));
    steps_addf("g(%c) = %s", y_var, g_str);

    /* Step 4: Solution is F(x,y) + g(y) = C */
    steps_add("Solution: F(x,y) + g(y) = C (arbitrary constant)");
    snprintf(result_buf, (size_t)buf_size,
             "Exact ODE solution:\n%s + %s = C",
             F_str, g_str);
    ok = true;

    ast_free(F);
    ast_free(g_prime);
    ast_free(g);
    return ok;
}

bool dsolve_extract_exact_form(const ast_node_t *expr,
                                char x_var, char y_var,
                                ast_node_t **M_out, ast_node_t **N_out)
{
    /*
     * Try to extract M, N from an expression of the form "lhs - rhs"
     * where lhs = M + N*y'. This is for expressions parsed from
     * "M(x,y) + N(x,y)y' = 0" format.
     *
     * Strategy: evaluate numerically with y' treated as a test value,
     * then extract coefficients. This is approximate and limited.
     *
     * For now, support the simple case where expression = A*y' + B
     * where A = N(x,y) and B = M(x,y).
     *
     * This is a heuristic for exact form detection.
     */
    (void)expr; (void)x_var; (void)y_var;
    *M_out = NULL;
    *N_out = NULL;
    return false;  /* Complex extraction deferred — use explicit ode_sep/ode_linear */
}

/* ── Auto-detect driver ───────────────────────────────────── */

bool dsolve_auto(const ast_node_t *rhs, char x_var, char y_var,
                 char *result_buf, int buf_size,
                 ode_method_t *method_used)
{
    bool ok = false;
    ode_method_t method = ODE_METHOD_UNKNOWN;

    if (!rhs || !result_buf || buf_size <= 0) return false;

    steps_add_checkpoint("Identify ODE type");

    /* Strategy 1: Try separable */
    steps_add("Try: Separation of Variables");
    ok = ode_solve_separable(rhs, x_var, y_var, result_buf, buf_size);
    if (ok) {
        method = ODE_METHOD_SEPARABLE;
        steps_set_checkpoint_result("Method: Separable ODE");
        steps_add_checkpoint("Solve separable ODE");
        steps_set_checkpoint_result(result_buf);
        goto done;
    }
    steps_add("Separable: not applicable");

    /* Strategy 2: Try first-order linear */
    steps_add("Try: First-Order Linear (Integrating Factor)");
    ok = ode_solve_linear(rhs, x_var, y_var, result_buf, buf_size);
    if (ok) {
        method = ODE_METHOD_LINEAR;
        steps_set_checkpoint_result("Method: First-Order Linear ODE");
        steps_add_checkpoint("Solve linear ODE");
        steps_set_checkpoint_result(result_buf);
        goto done;
    }
    steps_add("Linear: not applicable");

    /* Strategy 3: Try exact (rhs = -M/N form, or build from lhs) */
    /* For exact ODEs typed as dsolve(rhs, y, x) where the equation is
     * M + N*y' = 0 (so rhs = -M/N), we'd need M and N.
     * Since we only get rhs, we check if rhs is of the form -f(x,y)/g(x,y)
     * by trying M = numerator, N = denominator of rhs (if it's a ratio). */
    if (rhs->type == NODE_UNOP && rhs->d.unop.op == UOP_NEG) {
        /* rhs = -expr, look for expr = M/N */
        const ast_node_t *inner = rhs->d.unop.operand;
        if (inner && inner->type == NODE_BINOP && inner->d.binop.op == OP_DIV) {
            ast_node_t *M_node = ast_clone(inner->d.binop.left);
            ast_node_t *N_node = ast_clone(inner->d.binop.right);
            if (M_node && N_node) {
                steps_add("Try: Exact ODE (M/N form detected)");
                ok = dsolve_exact(M_node, N_node, x_var, y_var,
                                   result_buf, buf_size);
                ast_free(M_node);
                ast_free(N_node);
                if (ok) {
                    method = ODE_METHOD_EXACT;
                    steps_set_checkpoint_result("Method: Exact ODE");
                    goto done;
                }
            } else {
                if (M_node) ast_free(M_node);
                if (N_node) ast_free(N_node);
            }
        }
    } else if (rhs->type == NODE_BINOP && rhs->d.binop.op == OP_DIV) {
        /* rhs = M/N directly → try with negated M for M dx + N dy = 0 */
        ast_node_t *M_node = ast_clone(rhs->d.binop.left);
        ast_node_t *N_node = ast_clone(rhs->d.binop.right);
        if (M_node && N_node) {
            steps_add("Try: Exact ODE (ratio form)");
            ok = dsolve_exact(M_node, N_node, x_var, y_var,
                               result_buf, buf_size);
            ast_free(M_node);
            ast_free(N_node);
            if (ok) {
                method = ODE_METHOD_EXACT;
                steps_set_checkpoint_result("Method: Exact ODE");
                goto done;
            }
        } else {
            if (M_node) ast_free(M_node);
            if (N_node) ast_free(N_node);
        }
    }
    steps_add("Exact: not applicable");

    /* All methods failed */
    method = ODE_METHOD_UNSOLVABLE;
    snprintf(result_buf, (size_t)buf_size,
             "No analytic solution found.\n"
             "Tried: separable, linear, exact.\n"
             "Use euler() for numerical approximation.");

done:
    if (method_used) *method_used = method;
    return ok;
}

/* ── Second-order linear ODE with constant coefficients ────── */

/*
 * Detect whether f_expr is the zero forcing term (NULL, or number 0).
 */
static bool f_is_zero(const ast_node_t *f)
{
    if (!f) return true;
    if (f->type == NODE_NUMBER && fabsf(f->d.number) < 1e-9f) return true;
    return false;
}

/*
 * Format float for display: suppress trailing zeros, use fractions for simple rationals.
 * e.g. 1.0→"1", 0.5→"(1/2)", -3.0→"-3", 1.23456→"1.23456"
 */
static int fmt_coeff(ce_float_t v, char *buf, int size)
{
    if (fabsf(v - (ce_float_t)(int)v) < 1e-5f) {
        return snprintf(buf, (size_t)size, "%d", (int)v);
    }
    /* Check if simple half, third, quarter */
    static const struct { ce_float_t val; const char *str; } simple[] = {
        {  0.5f, "(1/2)" },  { -0.5f, "(-1/2)" },
        {  1.5f, "(3/2)" },  { -1.5f, "(-3/2)" },
        {  0.333333f, "(1/3)" }, { -0.333333f, "(-1/3)" },
        {  0.666667f, "(2/3)" }, { -0.666667f, "(-2/3)" },
        {  0.25f, "(1/4)" },  { -0.25f, "(-1/4)" },
        {  0.75f, "(3/4)" },  { -0.75f, "(-3/4)" },
    };
    int i;
    for (i = 0; i < 12; i++) {
        if (fabsf(v - simple[i].val) < 1e-4f)
            return snprintf(buf, (size_t)size, "%s", simple[i].str);
    }
    return snprintf(buf, (size_t)size, "%.5g", (double)v);
}

/*
 * Build the homogeneous solution string into buf[pos..size].
 * On return, pos is advanced past what was written.
 */
static int write_homogeneous(ce_float_t a_val, ce_float_t b_val, ce_float_t c_val,
                              char xv, char *buf, int size)
{
    ce_float_t disc, r1, r2, alpha, beta;
    char r1s[32], r2s[32], alphas[32], betas[32];
    int pos = 0;

    if (fabsf(a_val) < 1e-9f) {
        /* First-order: b*y' + c*y = 0 → y = C1*e^(-c/b * x) */
        if (fabsf(b_val) < 1e-9f) {
            pos += snprintf(buf + pos, (size_t)(size - pos), "y = C (trivial)");
            return pos;
        }
        ce_float_t r = -c_val / b_val;
        char rs[32];
        fmt_coeff(r, rs, sizeof(rs));
        if (fabsf(r) < 1e-9f)
            pos += snprintf(buf + pos, (size_t)(size - pos), "y = C1");
        else
            pos += snprintf(buf + pos, (size_t)(size - pos), "y = C1*e^(%s*%c)", rs, xv);
        return pos;
    }

    disc = b_val * b_val - 4.0f * a_val * c_val;

    steps_addf("Characteristic equation: %g*r^2 + %g*r + %g = 0",
               (double)a_val, (double)b_val, (double)c_val);
    steps_addf("Discriminant D = %g^2 - 4*%g*%g = %g",
               (double)b_val, (double)a_val, (double)c_val, (double)disc);

    if (disc > 1e-7f) {
        /* Two distinct real roots */
        r1 = (-b_val + sqrtf(disc)) / (2.0f * a_val);
        r2 = (-b_val - sqrtf(disc)) / (2.0f * a_val);
        fmt_coeff(r1, r1s, sizeof(r1s));
        fmt_coeff(r2, r2s, sizeof(r2s));
        steps_addf("Two real roots: r1 = %s, r2 = %s", r1s, r2s);
        if (fabsf(r1) < 1e-9f && fabsf(r2) < 1e-9f) {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = C1 + C2*%c  (yh)", xv);
        } else if (fabsf(r1) < 1e-9f) {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = C1 + C2*e^(%s*%c)  (yh)", r2s, xv);
        } else if (fabsf(r2) < 1e-9f) {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = C1*e^(%s*%c) + C2  (yh)", r1s, xv);
        } else {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = C1*e^(%s*%c) + C2*e^(%s*%c)  (yh)", r1s, xv, r2s, xv);
        }
    } else if (disc < -1e-7f) {
        /* Complex conjugate roots α ± βi */
        alpha = -b_val / (2.0f * a_val);
        beta  = sqrtf(-disc) / (2.0f * a_val);
        if (beta < 0) beta = -beta;
        fmt_coeff(alpha, alphas, sizeof(alphas));
        fmt_coeff(beta,  betas,  sizeof(betas));
        steps_addf("Complex roots: alpha = %s, beta = %s", alphas, betas);
        if (fabsf(alpha) < 1e-9f) {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = C1*cos(%s*%c) + C2*sin(%s*%c)  (yh)",
                            betas, xv, betas, xv);
        } else {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = e^(%s*%c)*(C1*cos(%s*%c)+C2*sin(%s*%c))  (yh)",
                            alphas, xv, betas, xv, betas, xv);
        }
    } else {
        /* Repeated root */
        r1 = -b_val / (2.0f * a_val);
        fmt_coeff(r1, r1s, sizeof(r1s));
        steps_addf("Repeated root: r = %s", r1s);
        if (fabsf(r1) < 1e-9f) {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = C1 + C2*%c  (yh)", xv);
        } else {
            pos += snprintf(buf + pos, (size_t)(size - pos),
                            "y = (C1+C2*%c)*e^(%s*%c)  (yh)", xv, r1s, xv);
        }
    }
    return pos;
}

/*
 * Try to find a polynomial particular solution for constant right-hand side.
 * Works when f_expr is a constant number (poly degree 0) and c≠0.
 */
static bool particular_constant(ce_float_t f_val, ce_float_t a_val,
                                 ce_float_t b_val, ce_float_t c_val,
                                 char xv, char *buf, int size, int *pos)
{
    (void)a_val; (void)b_val; (void)xv;
    if (fabsf(c_val) < 1e-9f) return false;  /* need c≠0 for Yp=A */
    ce_float_t A = f_val / c_val;
    char as[32];
    fmt_coeff(A, as, sizeof(as));
    steps_addf("Particular solution: Yp = %s (constant)", as);
    *pos += snprintf(buf + *pos, (size_t)(size - *pos), " + %s", as);
    return true;
}

bool dsolve2_const_coeff(ce_float_t a_val, ce_float_t b_val, ce_float_t c_val,
                          const ast_node_t *f_expr, char x_var,
                          char *result_buf, int buf_size)
{
    int pos = 0;
    bool has_particular = false;

    steps_add("=== dsolve2: 2nd-order linear ODE (constant coefficients) ===");
    steps_addf("Form: %g*y'' + %g*y' + %g*y = f(%c)",
               (double)a_val, (double)b_val, (double)c_val, x_var);

    /* ── Homogeneous solution ── */
    steps_add("Step 1: Solve homogeneous equation (f=0)");
    pos = write_homogeneous(a_val, b_val, c_val, x_var, result_buf, buf_size);
    if (pos >= buf_size - 1) {
        snprintf(result_buf, (size_t)buf_size, "Result too long");
        return false;
    }

    /* ── Particular solution ── */
    if (!f_is_zero(f_expr)) {
        steps_add("Step 2: Find particular solution (undetermined coefficients)");

        if (f_expr->type == NODE_NUMBER) {
            /* f(x) = constant */
            ce_float_t f_val = f_expr->d.number;
            has_particular = particular_constant(f_val, a_val, b_val, c_val,
                                                  x_var, result_buf, buf_size, &pos);
        } else {
            /*
             * Evaluate f at x=0 to get a rough constant approximation.
             * For more complex forcing terms (sin, cos, polynomial),
             * show the structure and indicate the method.
             */
            eval_ctx_t ec;
            ce_float_t f_at_0;
            char f_buf[96];
            ast_to_display_str(f_expr, f_buf, sizeof(f_buf));   /* shown in the result */
            eval_init(&ec);
            eval_set_var(&ec, x_var, 0.0f);
            f_at_0 = eval_expr(&ec, f_expr);

            if (ec.error == CE_OK && fabsf(f_at_0) > 1e-9f && !ast_contains_var(f_expr, x_var)) {
                /* f(x) is constant-valued (no x dependence) */
                has_particular = particular_constant(f_at_0, a_val, b_val, c_val,
                                                      x_var, result_buf, buf_size, &pos);
            } else {
                /* General case: show that Yp must be determined */
                steps_addf("Forcing: f(%c) = %s", x_var, f_buf);
                steps_add("Use undetermined coefficients or variation of parameters.");
                if (pos < buf_size - 1) {
                    pos += snprintf(result_buf + pos, (size_t)(buf_size - pos),
                                    " + Yp  (f=%s)", f_buf);
                }
                has_particular = true;
            }
        }
        if (!has_particular) {
            steps_add("Warning: particular solution could not be found analytically.");
        }
    } else {
        steps_add("Homogeneous equation (f=0): no particular solution needed.");
    }

    /* Null-terminate */
    if (pos < buf_size) result_buf[pos] = '\0';

    return true;
}
