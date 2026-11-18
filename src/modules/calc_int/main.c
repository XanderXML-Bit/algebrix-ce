#include <stdio.h>

#include "split/module_runtime.h"

#define CE_PRGM_INT_DEF "CECIDF"
#define CE_PRGM_INT_SIM "CECISM"
#define CE_PRGM_INT_ADV "CECIAD"
#define CE_PRGM_ODE     "CECODE"
#define CE_SELF_PROGRAM "CECINT"

static const char *ce_integral_program_for(const ast_node_t *root)
{
    if (!root) return CE_PRGM_INT_SIM;

    switch (root->type) {
    case NODE_INTEGRAL:
        if (root->d.integral.lo && root->d.integral.hi) {
            return CE_PRGM_INT_DEF;
        }
        return CE_PRGM_INT_SIM;

    case NODE_POLAR:
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
        case POLOP_IMPROPER:
        case POLOP_AREABETWEEN:
            /* Answered with handle_integral()'s checks (internal review). */
            return CE_PRGM_INT_DEF;
        default:
            break;
        }
        break;

    default:
        break;
    }

    return CE_PRGM_INT_SIM;
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
    program = ce_integral_program_for(root);
    if (!ce_module_runtime_forward("CECINT", program, &req, &state, &resp,
                                   err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
