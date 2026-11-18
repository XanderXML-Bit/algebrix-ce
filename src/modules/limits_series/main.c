#include <stdio.h>
#include <string.h>
#include <math.h>

#include "calcengine.h"
#include "ast.h"
#include "eval.h"
#include "limits.h"
#include "split/module_runtime.h"
#include "split/ipc.h"

#define CE_SELF_PROGRAM   "CELIMS"
#define CE_PRGM_LIMIT_ADV "CECLIA"

static void celims_format_decimal(char *buf, size_t size, ce_float_t value)
{
    double v;
    bool neg = false;
    unsigned long scale = 1000000UL;
    unsigned long scaled;
    unsigned long whole;
    unsigned long frac;
    char frac_buf[8];
    int frac_len = 6;

    if (!buf || size == 0) return;
    if (ce_isnan(value)) { snprintf(buf, size, "nan"); return; }
    if (ce_isinf(value)) { snprintf(buf, size, "%sinf", value < 0 ? "-" : ""); return; }

    v = (double)value;
    if (v < 0.0) { neg = true; v = -v; }
    if (v > 4294.0) {
        if (v > 4294967294.0) { snprintf(buf, size, "%s>4e9", neg ? "-" : ""); return; }
        /* 32-bit float: no + 0.5 at >= 2^23 (it rounded 16777215 up). */
        whole = v >= 8388608.0 ? (unsigned long)v : (unsigned long)(v + 0.5);
        snprintf(buf, size, "%s%lu", neg ? "-" : "", whole);
        return;
    }

    if (v >= 100000.0) {
        scale = 1UL;
        frac_len = 0;
    } else if (v >= 10000.0) {
        scale = 10UL;
        frac_len = 1;
    } else if (v >= 1000.0) {
        scale = 100UL;
        frac_len = 2;
    } else if (v >= 100.0) {
        scale = 1000UL;
        frac_len = 3;
    } else if (v >= 10.0) {
        scale = 10000UL;
        frac_len = 4;
    } else if (v >= 1.0) {
        scale = 100000UL;
        frac_len = 5;
    }

    scaled = (unsigned long)(v * (double)scale + 0.5);
    whole = scaled / scale;
    frac = scaled % scale;
    if (frac == 0UL || frac_len == 0) { snprintf(buf, size, "%s%lu", neg ? "-" : "", whole); return; }
    snprintf(frac_buf, sizeof(frac_buf), "%06lu", frac);
    if (frac_len < 6) memmove(frac_buf, frac_buf + (6 - frac_len), (size_t)frac_len + 1U);
    while (frac_len > 0 && frac_buf[frac_len - 1] == '0') frac_buf[--frac_len] = '\0';
    snprintf(buf, size, "%s%lu.%s", neg ? "-" : "", whole, frac_buf);
}

static void celims_format_result(char *buf, size_t size, ce_float_t value)
{
    char number[32];
    celims_format_decimal(number, sizeof(number), value);
    snprintf(buf, size, "= %s", number);
}

/* Inline pattern-match helpers — identical logic to try_special in limits.c
   but placed here to avoid any CE soft-float exception propagation that may
   occur when the check is buried in a deep call chain (compute_limit →
   limit_inner → try_special).  Returns true and writes *out on match. */
static bool ce_is_var(const ast_node_t *n, char v)
{
    return n && n->type == NODE_VARIABLE && n->d.variable == v;
}

static bool celims_try_special_zero(const ast_node_t *expr, char var,
                                    ce_float_t *out)
{
    const ast_node_t *num, *den;
    if (!expr || expr->type != NODE_BINOP || expr->d.binop.op != OP_DIV)
        return false;
    num = expr->d.binop.left;
    den = expr->d.binop.right;

    /* sin(x)/x → 1 */
    if (num->type == NODE_FUNC && num->d.func.id == FN_SIN &&
        ce_is_var(num->d.func.arg, var) && ce_is_var(den, var)) {
        *out = 1.0f; return true;
    }
    /* x/sin(x) → 1 */
    if (ce_is_var(num, var) &&
        den->type == NODE_FUNC && den->d.func.id == FN_SIN &&
        ce_is_var(den->d.func.arg, var)) {
        *out = 1.0f; return true;
    }
    /* tan(x)/x → 1 */
    if (num->type == NODE_FUNC && num->d.func.id == FN_TAN &&
        ce_is_var(num->d.func.arg, var) && ce_is_var(den, var)) {
        *out = 1.0f; return true;
    }
    /* (1 - cos(x))/x → 0 */
    if (ce_is_var(den, var) &&
        num->type == NODE_BINOP && num->d.binop.op == OP_SUB &&
        num->d.binop.left->type == NODE_NUMBER &&
        fabsf(num->d.binop.left->d.number - 1.0f) < 1e-6f &&
        num->d.binop.right->type == NODE_FUNC &&
        num->d.binop.right->d.func.id == FN_COS &&
        ce_is_var(num->d.binop.right->d.func.arg, var)) {
        *out = 0.0f; return true;
    }
    /* (e^x - 1)/x → 1 */
    if (ce_is_var(den, var) &&
        num->type == NODE_BINOP && num->d.binop.op == OP_SUB &&
        num->d.binop.right->type == NODE_NUMBER &&
        fabsf(num->d.binop.right->d.number - 1.0f) < 1e-6f) {
        const ast_node_t *left = num->d.binop.left;
        /* e^x form */
        if (left->type == NODE_FUNC && left->d.func.id == FN_EXP &&
            ce_is_var(left->d.func.arg, var)) {
            *out = 1.0f; return true;
        }
        /* E^x form (constant E) */
        if (left->type == NODE_BINOP && left->d.binop.op == OP_POW &&
            left->d.binop.left->type == NODE_CONSTANT &&
            left->d.binop.left->d.constant == CONST_E &&
            ce_is_var(left->d.binop.right, var)) {
            *out = 1.0f; return true;
        }
    }
    return false;
}

/* Each program launch enters main once. Keep IPC storage off the OS stack. */
static ce_request_t req;
static ce_response_t resp;
static ce_shared_state_t state;

static char buf[MAX_RESULT_LEN];
static char err[128];

int main(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;

    lim_status_t ls;
    ce_float_t lim_val = 0;

    if (ce_module_runtime_resume_forward(CE_SELF_PROGRAM, &req, &state)) {
        return 0;
    }
    if (ce_module_runtime_recover_child_failure(CE_SELF_PROGRAM, CE_SELF_PROGRAM,
                                                &req, &state, err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
        ce_module_runtime_finish(&req, &resp, &state);
        return 0;
    }

    if (!ce_module_runtime_load(&req, &resp, &state, &root, &had_equals)) {
        ce_module_runtime_finish(&req, &resp, &state);
        return 1;
    }

    if (!root) {
        ce_response_from_runtime(&resp, false, false, "",
                                 "Request payload missing AST",
                                 g_last_answer);
        ce_module_runtime_finish(&req, &resp, &state);
        return 0;
    }

    if (root->type != NODE_LIMIT || !root->d.limit.point) {
        ce_response_from_runtime(&resp, false, false, "",
                                 "Not a limit expression",
                                 g_last_answer);
        ast_free(root);
        ce_module_runtime_finish(&req, &resp, &state);
        return 0;
    }

    /* Fast-path: inline special-limit pattern matching for x→0 forms.
       Identical logic to limits.c try_special(), but called directly here
       to avoid any CE soft-float exception that could arise deep in the
       compute_limit → limit_inner call chain before try_special fires. */
    if (root->d.limit.point->type == NODE_NUMBER &&
        fabsf(root->d.limit.point->d.number) < 1e-4f) {
        if (celims_try_special_zero(root->d.limit.expr, root->d.limit.var,
                                    &lim_val)) {
            celims_format_result(buf, sizeof(buf), lim_val);
            g_last_answer = lim_val;
            ce_response_from_runtime(&resp, true, false, buf, "", lim_val);
            ast_free(root);
            ce_module_runtime_finish(&req, &resp, &state);
            return 0;
        }
    }

    /* General limit computation — try_direct_sub + L'Hopital + etc. */
    ls = compute_limit(root->d.limit.expr, root->d.limit.var,
                       root->d.limit.point, root->d.limit.direction,
                       &lim_val);

    switch (ls) {
    case LIM_OK:
        celims_format_result(buf, sizeof(buf), lim_val);
        g_last_answer = lim_val;
        ce_response_from_runtime(&resp, true, false, buf, "", lim_val);
        break;
    case LIM_POS_INF:
        ce_response_from_runtime(&resp, true, false, "= +inf", "",
                                 g_last_answer);
        break;
    case LIM_NEG_INF:
        ce_response_from_runtime(&resp, true, false, "= -inf", "",
                                 g_last_answer);
        break;
    case LIM_DNE:
        ce_response_from_runtime(&resp, true, false,
                                 "= DNE (does not exist)", "",
                                 g_last_answer);
        break;
    default:
        /* Escalate to advanced limit worker */
        if (!ce_module_runtime_forward(CE_SELF_PROGRAM, CE_PRGM_LIMIT_ADV,
                                       &req, &state, &resp,
                                       err, sizeof(err))) {
            ce_response_from_runtime(&resp, false, false, "",
                                     "Could not evaluate limit",
                                     g_last_answer);
        }
        break;
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
