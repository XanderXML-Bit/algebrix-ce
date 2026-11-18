/*
 * module_runtime_diag.c — Diagnostic copy of src/split/module_runtime.c.
 * Replaces ce_module_runtime_finish() with an instrumented version that
 * fires W08-W11 stage markers.  All other functions (load, resume, recover)
 * are identical to production.
 *
 * Stage map (continued from module_main_diag.c, total=11):
 *   W08 resp open    — before ce_response_write() opens the response AppVar
 *   W09 resp written — after ce_response_write() returns
 *   W10 resp closed  — after ce_state_save() completes (AppVars closed)
 *   W11 pre exit     — immediately before os_RunPrgm("CALCENG") re-launch
 *
 * DO NOT MERGE TO MAIN. Diagnostic use only.
 */

#include "split/module_runtime.h"

#include <string.h>

#include "calcengine.h"
#include "memory.h"
#include "split/astwire.h"

#include "diag_helpers.h"

/* ------------------------------------------------------------------ */
/* Internal helpers (identical to production module_runtime.c)         */
/* ------------------------------------------------------------------ */

static void ce_runtime_sync_globals(const ce_shared_state_t *state)
{
    if (!state) return;
    g_settings = state->settings;
    g_last_answer = state->last_answer;
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
    if (strncmp(req->return_to, self_program, sizeof(req->return_to) - 1) != 0) {
        return false;
    }
    return strncmp(state->pending_module, self_program,
                   sizeof(state->pending_module) - 1) != 0;
}

static void ce_runtime_set_pending_module(ce_shared_state_t *state,
                                          const char *program)
{
    if (!state) return;
    if (!program || !program[0]) {
        state->pending_module[0] = '\0';
        return;
    }

    strncpy(state->pending_module, program, sizeof(state->pending_module) - 1);
    state->pending_module[sizeof(state->pending_module) - 1] = '\0';
}

static void ce_runtime_archive_pending_child(const ce_shared_state_t *state)
{
    if (!state || !state->pending_module[0]) return;
    ce_program_set_archived(state->pending_module, true);
}

/* ------------------------------------------------------------------ */
/* Public API (identical to production)                                */
/* ------------------------------------------------------------------ */

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
    ce_response_t probe;

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
    if (!ce_response_read(&probe)) {
        return false;
    }

    ce_runtime_archive_pending_child(state);
    ce_runtime_set_pending_module(state, self_program);
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    ce_state_save(state);
    return true;
}

bool ce_module_runtime_recover_child_failure(const char *self_program,
                                             const char *pending_target,
                                             ce_request_t *req,
                                             ce_shared_state_t *state,
                                             char *err_buf,
                                             size_t err_buf_size)
{
    bool has_invalid_response = false;
    ce_response_t probe;
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
        if (ce_response_read(&probe)) {
            return false;
        }
        has_invalid_response = true;
    }

    strncpy(child_program, state->pending_module, sizeof(child_program) - 1);
    child_program[sizeof(child_program) - 1] = '\0';
    ce_format_child_failure_message(err_buf, err_buf_size, child_program,
                                    has_invalid_response);
    ce_response_delete();
    ce_runtime_archive_pending_child(state);
    ce_runtime_set_pending_module(state, pending_target);
    state->has_pending_response = 0;
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    ce_state_save(state);
    return true;
}

/* ------------------------------------------------------------------ */
/* ce_module_runtime_finish — INSTRUMENTED with W08-W11               */
/* ------------------------------------------------------------------ */
void ce_module_runtime_finish(const ce_request_t *req,
                              const ce_response_t *resp,
                              ce_shared_state_t *state)
{
    const char *expr = "";

    if (!resp || !state) return;
    if (req) expr = req->expression;

    /* ---- W08: resp open (about to write response AppVar) ---- */
    dbg_stage("W08 resp open", 8, 11);

    ce_response_write(resp);

    /* ---- W09: resp written ---- */
    dbg_stage("W09 resp written", 9, 11);

    state->settings = g_settings;
    state->last_answer = g_last_answer;
    state->has_pending_response = 1;
    strncpy(state->last_expression, expr, sizeof(state->last_expression) - 1);
    state->last_expression[sizeof(state->last_expression) - 1] = '\0';
    ce_state_save(state);

    /* ---- W10: resp closed (state saved, AppVars closed) ---- */
    dbg_stage("W10 resp closed", 10, 11);

#ifndef DESKTOP_BUILD
    /* ---- W11: pre exit (about to hand control back to CALCENG) ---- */
    dbg_stage("W11 pre exit", 11, 11);

    /* Re-launch CALCENG (UAF fix: no callback pointer into freed RAM).
     * CALCENG's main() re-enters, reads has_pending_response=1, shows result. */
    if (req && req->return_to[0]) {
        os_RunPrgm(req->return_to, NULL, 0, NULL);
    }
#endif
}
