#include <stdio.h>

#include "integrate.h"
#include "steps.h"
#include "split/module_runtime.h"

#define CE_INT_WORKER_PARTS  1
#define CE_INT_WORKER_TRIG   2
#define CE_INT_WORKER_USUB   3
#define CE_INT_WORKER_PFRAC  4
#define CE_INT_WORKER_REPORT 5

#ifndef CE_INT_WORKER_KIND
#define CE_INT_WORKER_KIND 0
#endif

static bool ce_extract_integral_request(const ast_node_t *root,
                                        const ast_node_t **expr_out,
                                        const ast_node_t **u_hint_out,
                                        char *var_out)
{
    if (!root || !expr_out || !u_hint_out || !var_out) return false;

    *expr_out = NULL;
    *u_hint_out = NULL;
    *var_out = 'x';

    if (root->type == NODE_INTEGRAL) {
        if (root->d.integral.lo || root->d.integral.hi) return false;
        *expr_out = root->d.integral.expr;
        *var_out = root->d.integral.var ? root->d.integral.var : 'x';
        return true;
    }

    if (root->type != NODE_POLAR) return false;
    *expr_out = root->d.polar.expr1;
    *var_out = root->d.polar.var ? root->d.polar.var : 'x';

#if CE_INT_WORKER_KIND == CE_INT_WORKER_REPORT
    return root->d.polar.op == POLOP_INTSTRATEGY;
#elif CE_INT_WORKER_KIND == CE_INT_WORKER_PARTS
    return root->d.polar.op == POLOP_INTPARTS;
#elif CE_INT_WORKER_KIND == CE_INT_WORKER_TRIG
    return root->d.polar.op == POLOP_TRIGSUB;
#elif CE_INT_WORKER_KIND == CE_INT_WORKER_USUB
    *u_hint_out = root->d.polar.expr2;
    return root->d.polar.op == POLOP_USUB;
#elif CE_INT_WORKER_KIND == CE_INT_WORKER_PFRAC
    return false;
#else
    return false;
#endif
}

static integ_strategy_t ce_integral_strategy(void)
{
#if CE_INT_WORKER_KIND == CE_INT_WORKER_PARTS
    return INTEG_STRATEGY_PARTS;
#elif CE_INT_WORKER_KIND == CE_INT_WORKER_TRIG
    return INTEG_STRATEGY_TRIGSUB;
#elif CE_INT_WORKER_KIND == CE_INT_WORKER_USUB
    return INTEG_STRATEGY_USUB;
#else
    return INTEG_STRATEGY_PARTIAL_FRAC;
#endif
}

/* Each program launch enters main once. Keep IPC storage off the OS stack. */
static ce_request_t req;
static ce_response_t resp;
static ce_shared_state_t state;

static char ast_buf[128];
static char out[MAX_RESULT_LEN];

int main(void)
{
    ast_node_t *root = NULL;
    const ast_node_t *expr = NULL;
    const ast_node_t *u_hint = NULL;
    ast_node_t *result = NULL;
    bool had_equals = false;
    char var = 'x';

    if (!ce_module_runtime_load(&req, &resp, &state, &root, &had_equals)) {
        ce_module_runtime_finish(&req, &resp, &state);
        return 1;
    }

    (void)had_equals;
    steps_clear();

    if (!ce_extract_integral_request(root, &expr, &u_hint, &var)) {
        ce_response_from_runtime(&resp, false, false, "",
                                 "Unsupported advanced integral request",
                                 g_last_answer);
    } else {
#if CE_INT_WORKER_KIND == CE_INT_WORKER_REPORT
        integrate_strategy_report(expr, var, out, sizeof(out));
        ce_response_from_runtime(&resp, true, true, out, "", g_last_answer);
#else
        result = integrate_with_strategy(expr, var, ce_integral_strategy(), u_hint);
        if (!result) {
            ce_response_from_runtime(&resp, false, false, "",
                                     integrate_last_failure_reason(),
                                     g_last_answer);
        } else {
            ast_to_display_str(result, ast_buf, sizeof(ast_buf));
            snprintf(out, sizeof(out), "int = %s + C", ast_buf);
            ce_response_from_runtime(&resp, true, true, out, "", g_last_answer);
            ast_free(result);
        }
#endif
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
