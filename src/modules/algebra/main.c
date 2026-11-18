#include <stdio.h>

#include "split/module_runtime.h"

#define CE_PRGM_ALGSOLVE "CEASLV"
#define CE_PRGM_ALGEXP   "CEALGF"
#define CE_PRGM_ALGSYM   "CEALGSY"
#define CE_PRGM_ALGPDV   "CEAPDV"
#define CE_PRGM_ALGGEO   "CEALGEO"
#define CE_SELF_PROGRAM  "CEALG"

static const char *ce_algebra_program_for(const ast_node_t *root, bool had_equals)
{
    if (!root) return CE_PRGM_ALGSOLVE;

    switch (root->type) {
    case NODE_EXPAND:
    case NODE_FACTOR:
        return CE_PRGM_ALGEXP;
    case NODE_SOLVE:
        return CE_PRGM_ALGSOLVE;
    case NODE_POLAR:
        switch (root->d.polar.op) {
        case POLOP_POLYDIV:
            return CE_PRGM_ALGPDV;
        case POLOP_CSQUARE:
        case POLOP_PFRAC:
        case POLOP_DESCARTES:
        case POLOP_NLSOLVE:
            return CE_PRGM_ALGSYM;
        case POLOP_LINE2PT:
        case POLOP_LINEPT:
        case POLOP_PARABOLA:
        case POLOP_TOFRAC:
        case POLOP_UNITCIRC:
        case POLOP_MIDPOINT:
        case POLOP_DISTANCE:
        case POLOP_SLOPE:
            return CE_PRGM_ALGGEO;
        default:
            break;
        }
        break;
    default:
        break;
    }

    if (had_equals) return CE_PRGM_ALGSOLVE;
    return CE_PRGM_ALGSOLVE;
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

    program = ce_algebra_program_for(root, had_equals);
    if (!ce_module_runtime_forward("CEALG", program, &req, &state, &resp,
                                   err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
