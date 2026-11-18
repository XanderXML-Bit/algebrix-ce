#include <tice.h>
#include <string.h>
#include <stdio.h>

#include "calcengine.h"
#include "memory.h"
#include "eval.h"
#include "modules/common/module_entry.h"
#include "core/protocol/appvar_protocol.h"
#include "split/astwire.h"
#include "split/processor.h"

static ce_request_t g_request;
static ce_response_t g_response;
static ce_shared_state_t g_state;

int ce_module_entry(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;
#if CE_EVAL_USER_VARS
    bool response_written;
#endif
    ce_request_t *req = &g_request;
    ce_response_t *resp = &g_response;
    ce_shared_state_t *state = &g_state;

    mem_init();
    ce_state_load(state);
    g_settings = state->settings;
    g_last_answer = state->last_answer;
#if CE_EVAL_USER_VARS
    g_user_vars = state->user_vars;
#endif

    if (!ce_request_read(req)) {
        ce_response_from_runtime(resp, false, false, "", "No module request found", g_last_answer);
        ce_response_write(resp);
        return 1;
    }

    had_equals = (req->flags & CE_REQ_FLAG_HAD_EQUALS) != 0;
    if ((req->flags & CE_REQ_FLAG_HAS_AST) && req->ast_size > 0) {
        root = ce_ast_deserialize(req->ast_payload, req->ast_size);
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

#if CE_EVAL_USER_VARS
    response_written = ce_response_write(resp);
#else
    ce_response_write(resp);
#endif

    state->settings = g_settings;
    state->last_answer = g_last_answer;
#if CE_EVAL_USER_VARS
    /* A failed evaluation or response delivery must not commit a store. */
    if (response_written && resp->ok) state->user_vars = g_user_vars;
    else g_user_vars = state->user_vars;
#endif
    state->has_pending_response = 1;
    strncpy(state->last_expression, req->expression, sizeof(state->last_expression) - 1);
    state->last_expression[sizeof(state->last_expression) - 1] = '\0';
#if CE_EVAL_USER_VARS
    /* Without durable state, the launcher must not accept a store success. */
    if (!ce_state_save(state)) ce_response_delete();
#else
    ce_state_save(state);
#endif
    return 0;
}
