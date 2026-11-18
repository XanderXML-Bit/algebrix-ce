/*
 * laplace.c — Laplace transform table lookup engine
 *
 * Table-based approach with linearity (sum/difference, scalar multiple).
 * Handles: 1, t^n, e^(at), sin(bt), cos(bt), sinh(bt), cosh(bt),
 *          t^n*e^(at), e^(at)*sin(bt), e^(at)*cos(bt), and δ(t).
 */
#include "laplace.h"
#include "eval.h"
#include "ast.h"
#include "steps.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Recursion guard ─────────────────────────────────────── */

/* eZ80 stack ~4KB; each laplace_forward/inverse frame allocates 160B
 * (left_buf+right_buf). Cap at 12 levels = 1920B recursion budget,
 * leaving room for the outer call chain and other locals. */
#define LAPLACE_MAX_DEPTH 12
static int g_laplace_depth = 0;

/* ── Internal helpers ────────────────────────────────────── */

/* Evaluate an AST node numerically (no variables beyond t_var set to 0). */
static ce_float_t eval_const(const ast_node_t *node)
{
    eval_ctx_t ctx;
    ce_float_t v;
    eval_init(&ctx);
    v = eval_expr(&ctx, node);
    if (eval_error(&ctx) != CE_OK) return 0;
    return v;
}

/* Check if node is a constant (no variables). */
static bool is_const(const ast_node_t *node, char t_var)
{
    if (!node) return true;
    switch (node->type) {
    case NODE_NUMBER:   return true;
    case NODE_CONSTANT: return true;
    case NODE_VARIABLE: return node->d.variable != t_var;
    case NODE_BINOP:
        return is_const(node->d.binop.left, t_var) &&
               is_const(node->d.binop.right, t_var);
    case NODE_UNOP:
        return is_const(node->d.unop.operand, t_var);
    case NODE_FUNC:
        return is_const(node->d.func.arg, t_var);
    default: return false;
    }
}

/* Check if node is exactly the variable t_var. */
static bool is_tvar(const ast_node_t *node, char t_var)
{
    return node && node->type == NODE_VARIABLE && node->d.variable == t_var;
}

/* Check if node is t^n for integer n >= 0. Returns n or -1. */
static int get_power_of_t(const ast_node_t *node, char t_var)
{
    if (!node) return -1;
    if (is_tvar(node, t_var)) return 1;
    if (node->type == NODE_BINOP && node->d.binop.op == OP_POW) {
        if (!is_tvar(node->d.binop.left, t_var)) return -1;
        if (!is_const(node->d.binop.right, t_var)) return -1;
        ce_float_t exp_val = eval_const(node->d.binop.right);
        int n = (int)roundf(exp_val);
        if (fabsf(exp_val - n) < 0.01f && n >= 0) return n;
    }
    return -1;
}

/* Check if node = e^(a*t_var) or e^(t_var) or e^(-t_var).
 * Returns true and sets *a_out to the coefficient a. */
static bool get_exp_coeff(const ast_node_t *node, char t_var, ce_float_t *a_out)
{
    if (!node) return false;
    /* FN_EXP(arg) */
    if (node->type == NODE_FUNC && node->d.func.id == FN_EXP) {
        const ast_node_t *arg = node->d.func.arg;
        if (is_tvar(arg, t_var)) { *a_out = 1; return true; }
        /* a*t */
        if (arg->type == NODE_BINOP && arg->d.binop.op == OP_MUL) {
            if (is_tvar(arg->d.binop.right, t_var) && is_const(arg->d.binop.left, t_var)) {
                *a_out = eval_const(arg->d.binop.left); return true;
            }
            if (is_tvar(arg->d.binop.left, t_var) && is_const(arg->d.binop.right, t_var)) {
                *a_out = eval_const(arg->d.binop.right); return true;
            }
        }
        /* -t */
        if (arg->type == NODE_UNOP && arg->d.unop.op == UOP_NEG &&
            is_tvar(arg->d.unop.operand, t_var)) {
            *a_out = -1; return true;
        }
    }
    /* e^arg via NODE_BINOP POW with left == e constant */
    if (node->type == NODE_BINOP && node->d.binop.op == OP_POW) {
        if (node->d.binop.left->type == NODE_CONSTANT &&
            node->d.binop.left->d.constant == CONST_E) {
            return get_exp_coeff(
                &(ast_node_t){ .type=NODE_FUNC,
                               .d.func.id=FN_EXP,
                               .d.func.arg=(ast_node_t*)node->d.binop.right },
                t_var, a_out);
        }
    }
    return false;
}

/* Check node = sin(b*t) or cos(b*t) etc. Returns b > 0. */
static bool get_trig_coeff(const ast_node_t *node, char t_var,
                            func_id_t *fn_out, ce_float_t *b_out)
{
    if (!node || node->type != NODE_FUNC) return false;
    func_id_t fn = node->d.func.id;
    if (fn != FN_SIN && fn != FN_COS && fn != FN_SINH && fn != FN_COSH)
        return false;
    const ast_node_t *arg = node->d.func.arg;
    if (is_tvar(arg, t_var)) { *fn_out = fn; *b_out = 1; return true; }
    if (arg->type == NODE_BINOP && arg->d.binop.op == OP_MUL) {
        if (is_tvar(arg->d.binop.right, t_var) && is_const(arg->d.binop.left, t_var)) {
            *fn_out = fn; *b_out = eval_const(arg->d.binop.left); return true;
        }
        if (is_tvar(arg->d.binop.left, t_var) && is_const(arg->d.binop.right, t_var)) {
            *fn_out = fn; *b_out = eval_const(arg->d.binop.right); return true;
        }
    }
    return false;
}

/* ── Forward Laplace table ───────────────────────────────── */

static bool laplace_forward_atom(const ast_node_t *f, char t_var, char s_var,
                                  char *out, int out_size)
{
    (void)s_var; /* used via format strings only */

    /* L{1} = 1/s */
    if (f->type == NODE_NUMBER && fabsf(f->d.number - 1) < 1e-7f) {
        steps_add("L{1} = 1/s");
        snprintf(out, (size_t)out_size, "1/%c", s_var); return true;
    }

    /* L{c} = c/s for any constant */
    if (is_const(f, t_var)) {
        ce_float_t c = eval_const(f);
        if (fabsf(c) < 1e-10f) { snprintf(out, (size_t)out_size, "0"); return true; }
        steps_addf("L{%.6g} = %.6g/%c", (double)c, (double)c, s_var);
        snprintf(out, (size_t)out_size, "%.6g/%c", (double)c, s_var); return true;
    }

    /* L{t^n} = n!/s^(n+1) */
    {
        int n = get_power_of_t(f, t_var);
        if (n >= 0) {
            /* factorial n! */
            int fact = 1, i;
            for (i = 2; i <= n; i++) fact *= i;
            steps_addf("L{%c^%d} = %d!/%c^%d = %d/%c^%d",
                       t_var, n, n, s_var, n+1, fact, s_var, n+1);
            snprintf(out, (size_t)out_size, "%d/%c^%d", fact, s_var, n+1);
            return true;
        }
    }

    /* L{e^(at)} = 1/(s-a) */
    {
        ce_float_t a;
        if (get_exp_coeff(f, t_var, &a)) {
            if (fabsf(a) < 1e-9f) {
                steps_add("L{e^0} = L{1} = 1/s");
                snprintf(out, (size_t)out_size, "1/%c", s_var);
            } else if (a > 0) {
                steps_addf("L{e^(%.6g*%c)} = 1/(%c-%.6g)",
                           (double)a, t_var, s_var, (double)a);
                snprintf(out, (size_t)out_size, "1/(%c-%.6g)", s_var, (double)a);
            } else {
                steps_addf("L{e^(%.6g*%c)} = 1/(%c+%.6g)",
                           (double)a, t_var, s_var, (double)(-a));
                snprintf(out, (size_t)out_size, "1/(%c+%.6g)", s_var, (double)(-a));
            }
            return true;
        }
    }

    /* L{sin(bt)} = b/(s^2+b^2), L{cos(bt)} = s/(s^2+b^2) */
    /* L{sinh(bt)} = b/(s^2-b^2), L{cosh(bt)} = s/(s^2-b^2) */
    {
        func_id_t fn; ce_float_t b;
        if (get_trig_coeff(f, t_var, &fn, &b)) {
            switch (fn) {
            case FN_SIN:
                steps_addf("L{sin(%.6g%c)} = %.6g/(%c^2+%.6g)",
                           (double)b, t_var, (double)b, s_var, (double)(b*b));
                snprintf(out, (size_t)out_size, "%.6g/(%c^2+%.6g)",
                         (double)b, s_var, (double)(b*b));
                return true;
            case FN_COS:
                steps_addf("L{cos(%.6g%c)} = %c/(%c^2+%.6g)",
                           (double)b, t_var, s_var, s_var, (double)(b*b));
                snprintf(out, (size_t)out_size, "%c/(%c^2+%.6g)",
                         s_var, s_var, (double)(b*b));
                return true;
            case FN_SINH:
                steps_addf("L{sinh(%.6g%c)} = %.6g/(%c^2-%.6g)",
                           (double)b, t_var, (double)b, s_var, (double)(b*b));
                snprintf(out, (size_t)out_size, "%.6g/(%c^2-%.6g)",
                         (double)b, s_var, (double)(b*b));
                return true;
            case FN_COSH:
                steps_addf("L{cosh(%.6g%c)} = %c/(%c^2-%.6g)",
                           (double)b, t_var, s_var, s_var, (double)(b*b));
                snprintf(out, (size_t)out_size, "%c/(%c^2-%.6g)",
                         s_var, s_var, (double)(b*b));
                return true;
            default: break;
            }
        }
    }

    /* L{e^(at)*sin(bt)} = b/((s-a)^2+b^2) */
    /* L{e^(at)*cos(bt)} = (s-a)/((s-a)^2+b^2) */
    if (f->type == NODE_BINOP && f->d.binop.op == OP_MUL) {
        const ast_node_t *left  = f->d.binop.left;
        const ast_node_t *right = f->d.binop.right;
        ce_float_t a; func_id_t fn; ce_float_t b;
        /* Try left=e^(at), right=trig or vice versa */
        bool exp_left = get_exp_coeff(left, t_var, &a);
        bool exp_right = !exp_left && get_exp_coeff(right, t_var, &a);
        const ast_node_t *trig_node = exp_left ? right : (exp_right ? left : NULL);

        if (trig_node && get_trig_coeff(trig_node, t_var, &fn, &b)) {
            if (fn == FN_SIN) {
                steps_addf("L{e^(%.6g%c)*sin(%.6g%c)} = %.6g/((%c-%.6g)^2+%.6g)",
                           (double)a, t_var, (double)b, t_var,
                           (double)b, s_var, (double)a, (double)(b*b));
                snprintf(out, (size_t)out_size, "%.6g/((%c-%.6g)^2+%.6g)",
                         (double)b, s_var, (double)a, (double)(b*b));
                return true;
            }
            if (fn == FN_COS) {
                steps_addf("L{e^(%.6g%c)*cos(%.6g%c)} = (%c-%.6g)/((%c-%.6g)^2+%.6g)",
                           (double)a, t_var, (double)b, t_var,
                           s_var, (double)a, s_var, (double)a, (double)(b*b));
                snprintf(out, (size_t)out_size, "(%c-%.6g)/((%c-%.6g)^2+%.6g)",
                         s_var, (double)a, s_var, (double)a, (double)(b*b));
                return true;
            }
        }
    }

    return false;  /* Not matched */
}

bool laplace_forward(const ast_node_t *f, char t_var, char s_var,
                     char *result_buf, int buf_size)
{
    /* eZ80 has ~4KB stack; 80 bytes per sub-buffer (vs 200) prevents overflow
     * on complex multi-term expressions. Laplace sub-results are always short
     * symbolic strings (e.g. "n!/s^(n+1)", "(s-a)/((s-a)^2+b^2)"). */
    char left_buf[80], right_buf[80];
    bool retval = false;

    if (!f || !result_buf || buf_size <= 0) return false;
    if (g_laplace_depth >= LAPLACE_MAX_DEPTH) return false;
    g_laplace_depth++;

    steps_add("Laplace transform (table lookup + linearity)");

    /* Scalar multiple: c * f(t) → c * F(s) */
    if (f->type == NODE_BINOP && f->d.binop.op == OP_MUL) {
        const ast_node_t *left  = f->d.binop.left;
        const ast_node_t *right = f->d.binop.right;
        if (is_const(left, t_var)) {
            ce_float_t c = eval_const(left);
            if (laplace_forward(right, t_var, s_var, right_buf, sizeof(right_buf))) {
                steps_addf("L{%.6g*f} = %.6g * L{f}", (double)c, (double)c);
                snprintf(result_buf, (size_t)buf_size, "%.6g*(%s)", (double)c, right_buf);
                retval = true; goto lf_done;
            }
        }
        if (is_const(right, t_var)) {
            ce_float_t c = eval_const(right);
            if (laplace_forward(left, t_var, s_var, left_buf, sizeof(left_buf))) {
                snprintf(result_buf, (size_t)buf_size, "%.6g*(%s)", (double)c, left_buf);
                retval = true; goto lf_done;
            }
        }
    }

    /* Sum: f(t) + g(t) → F(s) + G(s) */
    if (f->type == NODE_BINOP && f->d.binop.op == OP_ADD) {
        bool l_ok = laplace_forward(f->d.binop.left,  t_var, s_var, left_buf,  sizeof(left_buf));
        bool r_ok = laplace_forward(f->d.binop.right, t_var, s_var, right_buf, sizeof(right_buf));
        if (l_ok && r_ok) {
            steps_add("Linearity: L{f+g} = L{f} + L{g}");
            snprintf(result_buf, (size_t)buf_size, "%s + %s", left_buf, right_buf);
            retval = true; goto lf_done;
        }
    }

    /* Difference: f(t) - g(t) → F(s) - G(s) */
    if (f->type == NODE_BINOP && f->d.binop.op == OP_SUB) {
        bool l_ok = laplace_forward(f->d.binop.left,  t_var, s_var, left_buf,  sizeof(left_buf));
        bool r_ok = laplace_forward(f->d.binop.right, t_var, s_var, right_buf, sizeof(right_buf));
        if (l_ok && r_ok) {
            steps_add("Linearity: L{f-g} = L{f} - L{g}");
            snprintf(result_buf, (size_t)buf_size, "%s - %s", left_buf, right_buf);
            retval = true; goto lf_done;
        }
    }

    /* Negation: -f(t) → -F(s) */
    if (f->type == NODE_UNOP && f->d.unop.op == UOP_NEG) {
        if (laplace_forward(f->d.unop.operand, t_var, s_var, left_buf, sizeof(left_buf))) {
            snprintf(result_buf, (size_t)buf_size, "-(%s)", left_buf);
            retval = true; goto lf_done;
        }
    }

    /* t^n * e^(at) → n!/(s-a)^(n+1) */
    if (f->type == NODE_BINOP && f->d.binop.op == OP_MUL) {
        const ast_node_t *l = f->d.binop.left;
        const ast_node_t *r = f->d.binop.right;
        ce_float_t a; int n;
        bool exp_l = get_exp_coeff(l, t_var, &a);
        bool exp_r = !exp_l && get_exp_coeff(r, t_var, &a);
        const ast_node_t *tn = exp_l ? r : (exp_r ? l : NULL);
        if (tn) {
            n = get_power_of_t(tn, t_var);
            if (n >= 0) {
                int fact = 1, i;
                for (i = 2; i <= n; i++) fact *= i;
                steps_addf("L{%c^%d*e^(%.6g%c)} = %d!/(%c-%.6g)^%d",
                           t_var, n, (double)a, t_var, fact, s_var, (double)a, n+1);
                if (fabsf(a) < 1e-9f)
                    snprintf(result_buf, (size_t)buf_size, "%d/%c^%d", fact, s_var, n+1);
                else if (a > 0)
                    snprintf(result_buf, (size_t)buf_size, "%d/(%c-%.6g)^%d",
                             fact, s_var, (double)a, n+1);
                else
                    snprintf(result_buf, (size_t)buf_size, "%d/(%c+%.6g)^%d",
                             fact, s_var, (double)(-a), n+1);
                retval = true; goto lf_done;
            }
        }
    }

    /* Atom lookup */
    retval = laplace_forward_atom(f, t_var, s_var, result_buf, buf_size);
lf_done:
    g_laplace_depth--;
    return retval;
}

/* ── Inverse Laplace table ───────────────────────────────── */

bool laplace_inverse(const ast_node_t *F, char s_var, char t_var,
                     char *result_buf, int buf_size)
{
    /* eZ80 stack safety: 80 bytes per sub-buffer (vs 200). */
    char left_buf[80], right_buf[80];
    bool retval = false;

    if (!F || !result_buf || buf_size <= 0) return false;
    if (g_laplace_depth >= LAPLACE_MAX_DEPTH) return false;
    g_laplace_depth++;

    steps_add("Inverse Laplace (table lookup + linearity)");

    /* Linearity: A/s^n + B/(s-a) + ... */
    if (F->type == NODE_BINOP && F->d.binop.op == OP_ADD) {
        bool l = laplace_inverse(F->d.binop.left,  s_var, t_var, left_buf,  sizeof(left_buf));
        bool r = laplace_inverse(F->d.binop.right, s_var, t_var, right_buf, sizeof(right_buf));
        if (l && r) {
            steps_add("Linearity: L^-1{F+G} = L^-1{F} + L^-1{G}");
            snprintf(result_buf, (size_t)buf_size, "%s + %s", left_buf, right_buf);
            retval = true; goto li_done;
        }
    }

    if (F->type == NODE_BINOP && F->d.binop.op == OP_SUB) {
        bool l = laplace_inverse(F->d.binop.left,  s_var, t_var, left_buf,  sizeof(left_buf));
        bool r = laplace_inverse(F->d.binop.right, s_var, t_var, right_buf, sizeof(right_buf));
        if (l && r) {
            snprintf(result_buf, (size_t)buf_size, "%s - %s", left_buf, right_buf);
            retval = true; goto li_done;
        }
    }

    /* Scalar multiply: c*F(s) → c*f(t) */
    if (F->type == NODE_BINOP && F->d.binop.op == OP_MUL) {
        if (is_const(F->d.binop.left, s_var)) {
            ce_float_t c = eval_const(F->d.binop.left);
            if (laplace_inverse(F->d.binop.right, s_var, t_var, right_buf, sizeof(right_buf))) {
                snprintf(result_buf, (size_t)buf_size, "%.6g*(%s)", (double)c, right_buf);
                retval = true; goto li_done;
            }
        }
        if (is_const(F->d.binop.right, s_var)) {
            ce_float_t c = eval_const(F->d.binop.right);
            if (laplace_inverse(F->d.binop.left, s_var, t_var, left_buf, sizeof(left_buf))) {
                snprintf(result_buf, (size_t)buf_size, "%.6g*(%s)", (double)c, left_buf);
                retval = true; goto li_done;
            }
        }
    }

    /* 1/s → 1 (u(t)) */
    if (F->type == NODE_BINOP && F->d.binop.op == OP_DIV) {
        const ast_node_t *num = F->d.binop.left;
        const ast_node_t *den = F->d.binop.right;

        /* Numerator constant */
        if (!is_const(num, s_var)) goto not_simple;

        ce_float_t c_num = eval_const(num);

        /* 1/s */
        if (den->type == NODE_VARIABLE && den->d.variable == s_var) {
            steps_addf("L^-1{%.6g/%c} = %.6g*u(%c)", (double)c_num, s_var, (double)c_num, t_var);
            snprintf(result_buf, (size_t)buf_size, "%.6g", (double)c_num);
            retval = true; goto li_done;
        }

        /* c/s^n → c*t^(n-1)/(n-1)! */
        if (den->type == NODE_BINOP && den->d.binop.op == OP_POW &&
            den->d.binop.left->type == NODE_VARIABLE &&
            den->d.binop.left->d.variable == s_var) {
            ce_float_t nf = eval_const(den->d.binop.right);
            int n = (int)roundf(nf);
            if (n >= 1 && fabsf(nf - n) < 0.01f) {
                int fact = 1, i;
                for (i = 2; i <= n-1; i++) fact *= i;
                steps_addf("L^-1{%.6g/%c^%d} = %.6g*%c^%d/%d",
                           (double)c_num, s_var, n, (double)c_num, t_var, n-1, fact);
                if (n == 1) {
                    snprintf(result_buf, (size_t)buf_size, "%.6g", (double)c_num);
                } else {
                    snprintf(result_buf, (size_t)buf_size, "%.6g*%c^%d/%d",
                             (double)(c_num), t_var, n-1, fact);
                }
                retval = true; goto li_done;
            }
        }

        /* c/(s-a) → c*e^(at) */
        if (den->type == NODE_BINOP &&
            (den->d.binop.op == OP_SUB || den->d.binop.op == OP_ADD)) {
            if (den->d.binop.left->type == NODE_VARIABLE &&
                den->d.binop.left->d.variable == s_var &&
                is_const(den->d.binop.right, s_var)) {
                ce_float_t a = eval_const(den->d.binop.right);
                if (den->d.binop.op == OP_ADD) a = -a;
                steps_addf("L^-1{%.6g/(%c-%.6g)} = %.6g*e^(%.6g%c)",
                           (double)c_num, s_var, (double)a, (double)c_num, (double)a, t_var);
                snprintf(result_buf, (size_t)buf_size, "%.6g*exp(%.6g*%c)",
                         (double)c_num, (double)a, t_var);
                retval = true; goto li_done;
            }
        }

        /* c/(s^2+b^2) → (c/b)*sin(bt) */
        /* c*s/(s^2+b^2) → c*cos(bt) */
        if (den->type == NODE_BINOP && den->d.binop.op == OP_ADD) {
            const ast_node_t *dl = den->d.binop.left;
            const ast_node_t *dr = den->d.binop.right;
            /* s^2 term */
            if (dl->type == NODE_BINOP && dl->d.binop.op == OP_POW &&
                dl->d.binop.left->type == NODE_VARIABLE &&
                dl->d.binop.left->d.variable == s_var &&
                is_const(dr, s_var)) {
                ce_float_t b2 = eval_const(dr);
                if (b2 > 0) {
                    ce_float_t b = sqrtf(b2);
                    /* numerator: c → (c/b)*sin(bt) */
                    steps_addf("L^-1{%.6g/(%c^2+%.6g)} = %.6g*sin(%.6g%c)",
                               (double)c_num, s_var, (double)b2,
                               (double)(c_num/b), (double)b, t_var);
                    snprintf(result_buf, (size_t)buf_size,
                             "%.6g*sin(%.6g*%c)", (double)(c_num/b), (double)b, t_var);
                    retval = true; goto li_done;
                }
            }
        }
    }

    /* c*s/(s^2+b^2) */
    if (F->type == NODE_BINOP && F->d.binop.op == OP_DIV) {
        const ast_node_t *num = F->d.binop.left;
        const ast_node_t *den = F->d.binop.right;
        /* num = c*s or s */
        ce_float_t c_num = 1;
        const ast_node_t *s_node = num;
        if (num->type == NODE_BINOP && num->d.binop.op == OP_MUL) {
            if (is_const(num->d.binop.left, s_var)) {
                c_num = eval_const(num->d.binop.left);
                s_node = num->d.binop.right;
            } else if (is_const(num->d.binop.right, s_var)) {
                c_num = eval_const(num->d.binop.right);
                s_node = num->d.binop.left;
            }
        }
        if (s_node->type == NODE_VARIABLE && s_node->d.variable == s_var) {
            if (den->type == NODE_BINOP && den->d.binop.op == OP_ADD) {
                const ast_node_t *dl = den->d.binop.left;
                const ast_node_t *dr = den->d.binop.right;
                if (dl->type == NODE_BINOP && dl->d.binop.op == OP_POW &&
                    dl->d.binop.left->type == NODE_VARIABLE &&
                    dl->d.binop.left->d.variable == s_var &&
                    is_const(dr, s_var)) {
                    ce_float_t b2 = eval_const(dr);
                    if (b2 > 0) {
                        ce_float_t b = sqrtf(b2);
                        steps_addf("L^-1{%.6g*%c/(%c^2+%.6g)} = %.6g*cos(%.6g%c)",
                                   (double)c_num, s_var, s_var, (double)b2,
                                   (double)c_num, (double)b, t_var);
                        snprintf(result_buf, (size_t)buf_size,
                                 "%.6g*cos(%.6g*%c)", (double)c_num, (double)b, t_var);
                        retval = true; goto li_done;
                    }
                }
            }
        }
    }

not_simple:
    snprintf(result_buf, (size_t)buf_size,
             "No table match found for this F(%c).\n"
             "Try partial fractions first: pfrac(F,%c)",
             s_var, s_var);
    /* retval stays false */
li_done:
    g_laplace_depth--;
    return retval;
}
