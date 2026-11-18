#include "split/module_runtime.h"

#include <string.h>

#include "calcengine.h"
#include "eval.h"
#include "memory.h"
#include "split/astwire.h"

/* Static workspace for probe reads in the resume/recovery functions.
 * Placing ce_response_t on the call stack (4510 bytes) combined with the
 * caller's ce_request_t (4273 bytes) overflows the TI-84 CE stack on the
 * iteration-2 re-entry path, causing a "validating OS" watchdog reset.
 * These functions are never called recursively, so a single static is safe. */
static ce_response_t s_probe_response;

/* Set by the os_RunPrgm re-entry callback. .bss is zeroed on every program
 * load, so a fresh dispatch always starts with false. */
static bool s_reentered;

void ce_module_runtime_mark_reentry(void)
{
    s_reentered = true;
}

bool ce_module_runtime_was_reentered(void)
{
    return s_reentered;
}

#ifdef DESKTOP_BUILD
void ce_module_runtime_reset_reentry(void)
{
    s_reentered = false;
}
#endif

static void ce_runtime_sync_globals(const ce_shared_state_t *state)
{
    if (!state) return;
    g_settings = state->settings;
    g_last_answer = state->last_answer;
#if CE_EVAL_USER_VARS
    g_user_vars = state->user_vars;
#endif
}

static void ce_copy_message_part(char **cursor, size_t *remaining, const char *text)
{
    if (!cursor || !remaining || !text || *remaining == 0) {
        return;
    }

    while (*text && *remaining > 1) {
        **cursor = *text;
        (*cursor)++;
        (*remaining)--;
        text++;
    }

    **cursor = '\0';
}

static void ce_format_child_failure_message(char *err_buf,
                                            size_t err_buf_size,
                                            const char *child_program,
                                            bool has_invalid_response)
{
    char *cursor;
    size_t remaining;

    if (!err_buf || err_buf_size == 0) {
        return;
    }

    cursor = err_buf;
    remaining = err_buf_size;
    err_buf[0] = '\0';

    ce_copy_message_part(&cursor, &remaining, "Module ");
    ce_copy_message_part(&cursor, &remaining, child_program);
    ce_copy_message_part(
        &cursor,
        &remaining,
        has_invalid_response ? " returned invalid IPC data" : " returned no IPC response"
    );
}

static bool ce_runtime_read_reentry_context(ce_request_t *req,
                                            ce_shared_state_t *state)
{
    if (!req || !state) {
        return false;
    }

    mem_init();
    ce_state_load(state);
    ce_runtime_sync_globals(state);
    return ce_request_read(req);
}

static bool ce_runtime_matches_reentry(const char *self_program,
                                       const ce_request_t *req,
                                       const ce_shared_state_t *state)
{
    if (!self_program || !self_program[0] || !req || !state) {
        return false;
    }
    if (!state->pending_module[0]) {
        return false;
    }
    /* pending_module == self means a fresh dispatch to this program. */
    if (strncmp(state->pending_module, self_program,
                sizeof(state->pending_module) - 1) == 0) {
        return false;
    }
    /* Resumed through the os_RunPrgm callback: this program forwarded and the
     * child (or a deeper chain) has returned. A nested forwarder overwrites
     * req->return_to with its own name, so do not require it to match here. */
    if (s_reentered) {
        return true;
    }
    return strncmp(req->return_to, self_program, sizeof(req->return_to) - 1) == 0;
}

void ce_runtime_set_pending_module(ce_shared_state_t *state,
                                          const char *program)
{
    if (!state) return;
    if (!program) program = "";
    strncpy(state->pending_module, program, sizeof(state->pending_module) - 1);
    state->pending_module[sizeof(state->pending_module) - 1] = '\0';
}

static void ce_runtime_archive_pending_child(const ce_shared_state_t *state)
{
    if (!state || !state->pending_module[0]) return;
    ce_program_set_archived(state->pending_module, true);
}

bool ce_module_runtime_load(ce_request_t *req,
                            ce_response_t *resp,
                            ce_shared_state_t *state,
                            ast_node_t **root_out,
                            bool *had_equals_out)
{
    if (!req || !resp || !state || !root_out || !had_equals_out) {
        return false;
    }

    *root_out = NULL;
    *had_equals_out = false;

    mem_init();
    ce_state_load(state);
    ce_runtime_sync_globals(state);

    if (!ce_request_read(req)) {
        ce_response_from_runtime(resp, false, false, "",
                                 "No module request found",
                                 g_last_answer);
        return false;
    }

    *had_equals_out = (req->flags & CE_REQ_FLAG_HAD_EQUALS) != 0;
    if ((req->flags & CE_REQ_FLAG_HAS_AST) && req->ast_size > 0) {
        *root_out = ce_ast_deserialize(req->ast_payload, req->ast_size);
    }

    return true;
}

bool ce_module_runtime_resume_forward(const char *self_program,
                                      ce_request_t *req,
                                      ce_shared_state_t *state)
{
    if (!self_program || !self_program[0] || !req || !state) {
        return false;
    }
    if (!ce_runtime_read_reentry_context(req, state)) {
        return false;
    }
    if (!state->has_pending_response) {
        return false;
    }
    if (!ce_runtime_matches_reentry(self_program, req, state)) {
        return false;
    }
    memset(&s_probe_response, 0, sizeof(s_probe_response));
    if (!ce_response_read(&s_probe_response)) {
        return false;
    }

    ce_runtime_archive_pending_child(state);
    ce_runtime_set_pending_module(state, self_program);
    /* Keep has_pending_response=1 so the parent caller (launcher) can read
       the child's CE_RESP.  Clearing it here was the router-module bug that
       caused the launcher to skip response display for int/lim/solve. */
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    ce_state_save(state);
    return true;
}

bool ce_module_runtime_resume_peek(const char *self_program,
                                   ce_request_t *req,
                                   ce_shared_state_t *state,
                                   ce_response_t *resp,
                                   char *child_out,
                                   size_t child_out_size)
{
    if (!self_program || !self_program[0] || !req || !state || !resp ||
        !child_out || child_out_size == 0) {
        return false;
    }
    child_out[0] = '\0';
    if (!ce_runtime_read_reentry_context(req, state)) {
        return false;
    }
    if (!state->has_pending_response) {
        return false;
    }
    if (!ce_runtime_matches_reentry(self_program, req, state)) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!ce_response_read(resp)) {
        return false;
    }
    strncpy(child_out, state->pending_module, child_out_size - 1);
    child_out[child_out_size - 1] = '\0';
    return true;
}

void ce_module_runtime_reclaim_forward(const char *self_program,
                                       ce_shared_state_t *state)
{
    if (!state) return;
    ce_runtime_archive_pending_child(state);
    ce_runtime_set_pending_module(state, self_program);
    state->has_pending_response = 0;
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    ce_response_delete();
    ce_state_save(state);
}

bool ce_module_runtime_recover_child_failure(const char *self_program,
                                             const char *pending_target,
                                             ce_request_t *req,
                                             ce_shared_state_t *state,
                                             char *err_buf,
                                             size_t err_buf_size)
{
    bool has_invalid_response = false;
    char child_program[sizeof(state->pending_module)];

    if (err_buf && err_buf_size > 0) {
        err_buf[0] = '\0';
    }
    if (!self_program || !self_program[0] || !req || !state) {
        return false;
    }
    if (!ce_runtime_read_reentry_context(req, state)) {
        return false;
    }
    if (!ce_runtime_matches_reentry(self_program, req, state)) {
        return false;
    }
    if (state->has_pending_response) {
        memset(&s_probe_response, 0, sizeof(s_probe_response));
        if (ce_response_read(&s_probe_response)) {
            return false;
        }
        has_invalid_response = true;
    }

    strncpy(child_program, state->pending_module, sizeof(child_program) - 1);
    child_program[sizeof(child_program) - 1] = '\0';
    ce_format_child_failure_message(err_buf, err_buf_size, child_program, has_invalid_response);
    ce_response_delete();
    ce_runtime_archive_pending_child(state);
    ce_runtime_set_pending_module(state, pending_target);
    state->has_pending_response = 0;
    /* The failed child may already have saved a new Ans or settings in
     * CE_STATE: ce_module_runtime_finish() saves them even when its CE_RESP
     * write fails.  A failure must not change them, so go back to what this
     * caller sent in its request.  The globals feed the caller's error
     * response too (forwarders), not only the saved state (internal review). */
    state->settings = req->settings;
    state->last_answer = req->last_answer;
    ce_runtime_sync_globals(state);
    ce_state_save(state);
    return true;
}

void ce_module_runtime_finish(const ce_request_t *req,
                              const ce_response_t *resp,
                              ce_shared_state_t *state)
{
    const char *expr = "";
#if CE_EVAL_USER_VARS
    bool response_written;
#endif

    if (!resp || !state) return;
    if (req) expr = req->expression;

#if CE_EVAL_USER_VARS
    response_written = ce_response_write(resp);
#else
    ce_response_write(resp);
#endif

    state->settings = g_settings;
    state->last_answer = g_last_answer;
#if CE_EVAL_USER_VARS
    /* A store (`5->X`) in this run is kept for later entries (internal review).
     * Only finish() writes the table: the forwarding paths above belong to
     * router modules, which are built without CE_EVAL_USER_VARS and carry
     * the loaded table back unchanged. Failed evaluations and unreadable
     * responses keep the previous values. */
    if (response_written && resp->ok) state->user_vars = g_user_vars;
    else g_user_vars = state->user_vars;
#endif
    state->has_pending_response = 1;
    strncpy(state->last_expression, expr, sizeof(state->last_expression) - 1);
    state->last_expression[sizeof(state->last_expression) - 1] = '\0';
#if CE_EVAL_USER_VARS
    if (!ce_state_save(state)) ce_response_delete();
#else
    ce_state_save(state);
#endif

    /* Return normally. The parent launched this program with
     * os_RunPrgm(..., callback): when main() returns, the CEdev runprgm stub
     * reloads the parent from its variable, restores the stack to where the
     * parent called os_RunPrgm, and enters the parent's callback, which
     * re-runs the parent's main() and reads has_pending_response=1.
     *
     * Do NOT call os_RunPrgm(req->return_to, NULL, 0, NULL) here. Every
     * os_RunPrgm call pushes a ~209-byte stub frame onto the 4 KB OS stack
     * that is only released when the launched program returns. A
     * fire-and-forget relaunch never returns, so each hop leaked 209 bytes
     * and the chain overflowed the stack limit (NMI -> "RAM Cleared") after
     * about 18 hops, and a forwarder whose return_to named itself relaunched
     * itself until it crashed (int/solve, 2026-11-08 real-ROM trace). */
    (void)req;
}
