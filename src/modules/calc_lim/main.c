#include <stdio.h>

#include "split/module_runtime.h"

#define CE_PRGM_LIMIT_BASE "CECLMB"
#define CE_PRGM_LIMIT_ADV  "CECLIA"
#define CE_PRGM_LIMIT_CON  "CECLIC"
#define CE_PRGM_LIMIT_REL  "CECLIR"
#define CE_SELF_PROGRAM    "CECLIM"

static const char *ce_limit_program_for(const ast_node_t *root)
{
    if (!root) return CE_PRGM_LIMIT_BASE;

    if (root->type == NODE_POLAR) {
        switch (root->d.polar.op) {
        case POLOP_CONTINUITY:
            return CE_PRGM_LIMIT_CON;
        case POLOP_RELRATE:
            return CE_PRGM_LIMIT_REL;
        default:
            break;
        }
    }

    return CE_PRGM_LIMIT_BASE;
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

    program = ce_limit_program_for(root);
    if (!ce_module_runtime_forward("CECLIM", program, &req, &state, &resp,
                                   err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
