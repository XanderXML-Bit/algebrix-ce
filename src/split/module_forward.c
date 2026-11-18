#include "split/module_runtime.h"

#include <stdio.h>
#include <string.h>

#include "calcengine.h"

int main(void);
static int module_reentry(void *data, int retval)
{
    (void)data;
    (void)retval;
    ce_module_runtime_mark_reentry();
    return main();
}

static void ce_forward_restore_parent_state(const char *caller,
                                            ce_shared_state_t *state)
{
    if (!state) return;
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    state->has_pending_response = 0;
    ce_runtime_set_pending_module(state, caller);
    ce_state_save(state);
}

bool ce_module_runtime_forward(const char *caller,
                               const char *program,
                               ce_request_t *req,
                               ce_shared_state_t *state,
                               ce_response_t *resp,
                               char *err_buf,
                               size_t err_buf_size)
{
    int run_ret;
    bool got_response = false;

    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    if (!caller || !caller[0] || !program || !program[0] ||
        !req || !state || !resp) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Invalid module forward request");
        }
        return false;
    }

    if (state->has_pending_response ||
        (state->pending_module[0] != '\0' &&
         strncmp(state->pending_module, caller,
                 sizeof(state->pending_module) - 1) != 0)) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Module runtime busy");
        }
        return false;
    }

    req->magic = CE_IPC_MAGIC;
    req->version = CE_IPC_VERSION;
    req->settings = g_settings;
    req->last_answer = g_last_answer;
    strncpy(req->caller, caller, sizeof(req->caller) - 1);
    req->caller[sizeof(req->caller) - 1] = '\0';
    memcpy(req->return_to, req->caller, sizeof(req->return_to));

    state->settings = g_settings;
    state->last_answer = g_last_answer;
    state->has_pending_response = 0;
    ce_runtime_set_pending_module(state, program);
    strncpy(state->last_expression, req->expression, sizeof(state->last_expression) - 1);
    state->last_expression[sizeof(state->last_expression) - 1] = '\0';

    ce_response_delete();
    if (!ce_request_write(req)) {
        ce_forward_restore_parent_state(caller, state);
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "IPC request write failed");
        }
        return false;
    }
    if (!ce_state_save(state)) {
        ce_forward_restore_parent_state(caller, state);
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "IPC state save failed");
        }
        return false;
    }
    /* Existence check only. os_RunPrgm runs archived programs by copying
     * them to userMem; unarchiving first kept a second RAM copy of every
     * forwarded child, so a 3-deep chain (CECISM 49 KB -> CECIAD -> CEIPFR
     * 47 KB) ran out of RAM and os_RunPrgm returned "not enough memory"
     * ("Program launch failed" on the real ROM, 2026-11-08). */
    if (!ce_program_exists(program)) {
        ce_forward_restore_parent_state(caller, state);
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Module %s missing", program);
        }
        return false;
    }

    memset(resp, 0, sizeof(*resp));
    /* On real hardware, os_RunPrgm destroys the current program and does
       not return on success. When the child returns, the runprgm stub
       reloads this module and enters module_reentry() -> main(), where
       ce_module_runtime_resume_forward() hands the child's CE_RESP up
       unchanged. So everything below this call (and any post-processing a
       caller does after ce_module_runtime_forward() returns true) runs only
       on the desktop mock, or on hardware when the launch itself failed.
       A caller that forwards more than once in a loop therefore only ever
       reaches its first child on hardware; multi-child forwarders continue
       on re-entry instead (CECIAD from the returned child's name, CEISTR
       from its CEISTRS scratch AppVar). */
    run_ret = ce_launch_program_reentry(program, module_reentry);

    if (ce_response_read(resp)) {
        g_settings = resp->settings;
        g_last_answer = resp->last_answer;
        got_response = true;
        ce_response_delete();
    }

    ce_state_load(state);
    g_settings = state->settings;
    g_last_answer = state->last_answer;
    state->has_pending_response = 0;
    if (state->pending_module[0]) {
        ce_program_set_archived(state->pending_module, true);
    }
    ce_runtime_set_pending_module(state, caller);
    ce_state_save(state);

    if (run_ret != 0) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Program launch failed");
        }
        return false;
    }

    if (!got_response) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "No module response");
        }
        return false;
    }

    return true;
}
