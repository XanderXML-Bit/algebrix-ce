#include <tice.h>
#include <string.h>
#include <stdio.h>

#include "calcengine.h"
#include "split/ipc.h"
#include "split/module_runtime.h"
#include "split/processor.h"

static ce_request_t g_request;
static ce_response_t g_response;
static ce_shared_state_t g_state;

int main(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;
    ce_request_t *req = &g_request;
    ce_response_t *resp = &g_response;
    ce_shared_state_t *state = &g_state;

    if (!ce_module_runtime_load(req, resp, state, &root, &had_equals)) {
        ce_module_runtime_finish(req, resp, state);
        return 1;
    }

    if (root) {
        ce_processor_run_preparsed(req->expression, root, had_equals, resp);
    } else {
#if CE_PROC_PARSE_INPUT
        ce_processor_run(req->expression, resp);
#else
        ce_response_from_runtime(resp, false, false, "",
                                 "Request payload missing AST", g_last_answer);
#endif
    }

    ce_module_runtime_finish(req, resp, state);
    return 0;
}
