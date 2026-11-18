#include <stdio.h>
#include <string.h>

#include "split/module_runtime.h"
#include "split/processor.h"

#define CE_LIMIT_ADV_FALLBACK "CELADVREQ"
#define CE_PRGM_LIMIT_ADV     "CECLIA"
#define CE_SELF_PROGRAM       "CECLMB"

/* Each program launch enters main once. Keep IPC storage off the OS stack. */
static ce_request_t req;
static ce_response_t resp;
static ce_shared_state_t state;

static char err[128];

int main(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;

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

    if (root) {
        ce_processor_run_preparsed(req.expression, root, had_equals, &resp);
    } else {
        ce_response_from_runtime(&resp, false, false, "",
                                 "Request payload missing AST",
                                 g_last_answer);
    }

    if (!resp.ok && strcmp(resp.error, CE_LIMIT_ADV_FALLBACK) == 0) {
        if (!ce_module_runtime_forward("CECLMB", CE_PRGM_LIMIT_ADV,
                                       &req, &state, &resp,
                                       err, sizeof(err))) {
            ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
        }
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
