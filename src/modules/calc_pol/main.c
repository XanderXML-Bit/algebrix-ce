#include <stdio.h>

#include "split/module_runtime.h"

#define CE_PRGM_POL_NUM "CECPNM"
#define CE_PRGM_INT_ADV "CECIAD"
#define CE_PRGM_ODE     "CECODE"
#define CE_SELF_PROGRAM "CECPOL"

static const char *ce_calc_pol_program_for(const ast_node_t *root)
{
    if (!root || root->type != NODE_POLAR) return CE_PRGM_POL_NUM;

    switch (root->d.polar.op) {
    case POLOP_INTPARTS:
    case POLOP_TRIGSUB:
    case POLOP_USUB:
    case POLOP_INTSTRATEGY:
        return CE_PRGM_INT_ADV;
    case POLOP_ODE_SEP:
    case POLOP_ODE_LINEAR:
    case POLOP_DIFFEQ:
        return CE_PRGM_ODE;
    default:
        return CE_PRGM_POL_NUM;
    }
}

/* Each program launch enters main once. Keep IPC storage off the OS stack. */
static ce_request_t req;
static ce_response_t resp;
static ce_shared_state_t state;

static char err[128];

int main(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;

    const char *program;

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

    (void)had_equals;
    program = ce_calc_pol_program_for(root);
    if (!ce_module_runtime_forward("CECPOL", program, &req, &state, &resp,
                                   err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
