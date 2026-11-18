/*
 * module_runtime_launcher_diag.c
 *
 * Instrumented replacement for src/split/module_runtime.c in the CALCENG
 * diagnostic build.  Only ce_module_runtime_recover_child_failure has been
 * changed: it now emits L08a-L08e sub-stage markers so we can pinpoint
 * exactly which line inside the function causes the "validating OS" crash
 * on hardware iteration 2.
 *
 * All other public symbols are identical to the production module_runtime.c.
 *
 * Used by Makefile.launcher.diag via PROGRAM_RUNTIME_SOURCES.
 * DO NOT use in the CEBASE diagnostic build — that build uses
 * module_runtime_diag.c which instruments ce_module_runtime_finish (W08-W11).
 *
 * Sub-stage marker plan (all stamped at stage_num=8 so the gate fires, and
 * all logged to CEDBG even in skip-all mode):
 *
 *   L08a pend=1 / L08a pend=0
 *       Fires right after the state->has_pending_response check.
 *       "pend=1" means we are on the crash path (iter 2 re-entry with a
 *       worker's CE_RESP in the AppVar store).
 *       "pend=0" means no pending response — normal first-run path.
 *
 *   L08b pre-read
 *       Fires immediately before ce_response_read(&probe).
 *       If CEDBG shows L08b but not L08c, the crash happened INSIDE
 *       ce_response_read — i.e., while writing 4510 bytes into the
 *       stack-allocated probe struct.
 *
 *   L08c read-ok / L08c read-bad
 *       Fires after ce_response_read returns.
 *       "read-ok" = response was readable (not a child failure, early return).
 *       "read-bad" = response missing/corrupt (confirmed child failure).
 *
 *   L08d pre-format
 *       Fires before ce_format_child_failure_message.
 *       If we reach here, we survived the probe read (crash was not there).
 *
 *   L08e post-format
 *       Fires after the error message is formatted and state is saved.
 *       If CEDBG shows this, the entire recover path completed.
 *
 * CRASH DEDUCTION TABLE
 * ---------------------
 * Last CEDBG entry  | Hardware interpretation
 * ----------------- | -------------------------------------------------------
 * L08b pre-read     | Crash inside ce_response_read — stack overflow writing
 *                   | probe (4510 B) while pending_req (4273 B) is on the
 *                   | caller's stack.  THIS IS THE EXPECTED ROOT CAUSE.
 * L08c read-ok      | Survived probe write; early return (not a child failure)
 * L08c read-bad     | Survived probe write; confirmed child failure
 * L08d pre-format   | Survived up to error formatting
 * L08e post-format  | Entire recover path completed; crash is elsewhere
 * L08a pend=0       | First-run path; crash is NOT in recover_child_failure
 */

#include "split/module_runtime.h"

#include <string.h>

#include "calcengine.h"
#include "memory.h"
#include "split/astwire.h"
#include "diag_helpers.h"

/* Static probe workspace — same fix as production module_runtime.c.
 * Avoids a 4510-byte stack local in both ce_module_runtime_resume_forward
 * and ce_module_runtime_recover_child_failure. */
static ce_response_t s_probe_response_ld;

/* ------------------------------------------------------------------ */
/* Static helpers (copies of production statics from module_runtime.c) */
/* ------------------------------------------------------------------ */

static void ce_runtime_sync_globals_ld(const ce_shared_state_t *state)
{
    if (!state) return;
    g_settings = state->settings;
    g_last_answer = state->last_answer;
}

static void ce_copy_message_part_ld(char **cursor,
                                     size_t *remaining,
                                     const char *text)
{
    if (!cursor || !remaining || !text || *remaining == 0) return;
    while (*text && *remaining > 1) {
        **cursor = *text;
        (*cursor)++;
        (*remaining)--;
        text++;
    }
    **cursor = '\0';
}

static void ce_format_child_failure_message_ld(char *err_buf,
                                                size_t err_buf_size,
                                                const char *child_program,
                                                bool has_invalid_response)
{
    char *cursor;
    size_t remaining;

    if (!err_buf || err_buf_size == 0) return;

    cursor = err_buf;
    remaining = err_buf_size;
    err_buf[0] = '\0';

    ce_copy_message_part_ld(&cursor, &remaining, "Module ");
    ce_copy_message_part_ld(&cursor, &remaining, child_program);
    ce_copy_message_part_ld(
        &cursor, &remaining,
        has_invalid_response ? " returned invalid IPC data"
                             : " returned no IPC response"
    );
}

static bool ce_runtime_read_reentry_context_ld(ce_request_t *req,
                                                ce_shared_state_t *state)
{
    if (!req || !state) return false;
    mem_init();
    ce_state_load(state);
    ce_runtime_sync_globals_ld(state);
    return ce_request_read(req);
}

static bool ce_runtime_matches_reentry_ld(const char *self_program,
                                           const ce_request_t *req,
                                           const ce_shared_state_t *state)
{
    if (!self_program || !self_program[0] || !req || !state) return false;
    if (!state->pending_module[0]) return false;
    if (strncmp(req->return_to, self_program, sizeof(req->return_to) - 1) != 0)
        return false;
    return strncmp(state->pending_module, self_program,
                   sizeof(state->pending_module) - 1) != 0;
}

static void ce_runtime_set_pending_module_ld(ce_shared_state_t *state,
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

static void ce_runtime_archive_pending_child_ld(const ce_shared_state_t *state)
{
    if (!state || !state->pending_module[0]) return;
    ce_program_set_archived(state->pending_module, true);
}


/* ------------------------------------------------------------------ */
/* Public API — non-instrumented symbols (identical to production)    */
/* ------------------------------------------------------------------ */

bool ce_module_runtime_load(ce_request_t *req,
                             ce_response_t *resp,
                             ce_shared_state_t *state,
                             ast_node_t **root_out,
                             bool *had_equals_out)
{
    if (!req || !resp || !state || !root_out || !had_equals_out) return false;

    *root_out = NULL;
    *had_equals_out = false;

    mem_init();
    ce_state_load(state);
    ce_runtime_sync_globals_ld(state);

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
    if (!self_program || !self_program[0] || !req || !state) return false;
    if (!ce_runtime_read_reentry_context_ld(req, state)) return false;
    if (!state->has_pending_response) return false;
    if (!ce_runtime_matches_reentry_ld(self_program, req, state)) return false;
    memset(&s_probe_response_ld, 0, sizeof(s_probe_response_ld));
    if (!ce_response_read(&s_probe_response_ld)) return false;

    ce_runtime_archive_pending_child_ld(state);
    ce_runtime_set_pending_module_ld(state, self_program);
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    ce_state_save(state);
    return true;
}

void ce_module_runtime_finish(const ce_request_t *req,
                               const ce_response_t *resp,
                               ce_shared_state_t *state)
{
    const char *expr = "";

    if (!resp || !state) return;
    if (req) expr = req->expression;

    ce_response_write(resp);

    state->settings = g_settings;
    state->last_answer = g_last_answer;
    state->has_pending_response = 1;
    strncpy(state->last_expression, expr, sizeof(state->last_expression) - 1);
    state->last_expression[sizeof(state->last_expression) - 1] = '\0';
    ce_state_save(state);

#ifndef DESKTOP_BUILD
    /* Re-launch the parent launcher (CALCENG) from archive.  See the same
     * block in src/split/module_runtime.c for the full UAF fix rationale. */
    if (req && req->return_to[0]) {
        os_RunPrgm(req->return_to, NULL, 0, NULL);
    }
#endif
}

bool ce_module_runtime_forward(const char *caller,
                                const char *program,
                                ce_request_t *req,
                                ce_shared_state_t *state,
                                ce_response_t *resp,
                                char *err_buf,
                                size_t err_buf_size)
{
    (void)caller; (void)program; (void)req;
    (void)state;  (void)resp;
    (void)err_buf; (void)err_buf_size;
    /* forward routing not used in launcher context */
    return false;
}


/* ================================================================== */
/* INSTRUMENTED: ce_module_runtime_recover_child_failure              */
/* L08a-L08e sub-stage markers pinpoint the crash location.           */
/* ================================================================== */

bool ce_module_runtime_recover_child_failure(const char *self_program,
                                             const char *pending_target,
                                             ce_request_t *req,
                                             ce_shared_state_t *state,
                                             char *err_buf,
                                             size_t err_buf_size)
{
    bool has_invalid_response = false;
    /* probe is now s_probe_response_ld (static) — no longer on the stack.
     * The L08a/b/c markers are still here to confirm the fix works on hw. */
    char child_program[sizeof(state->pending_module)];

    if (err_buf && err_buf_size > 0) {
        err_buf[0] = '\0';
    }
    if (!self_program || !self_program[0] || !req || !state) {
        return false;
    }

    /* Read CE_REQ and reload CE_STATE (mem_init happens inside) */
    if (!ce_runtime_read_reentry_context_ld(req, state)) {
        return false;
    }

    /* Check whether this CALCENG instance is the intended return target */
    if (!ce_runtime_matches_reentry_ld(self_program, req, state)) {
        return false;
    }

    /* ------ L08a: right after has_pending_response check --------- */
    /* We are on the re-entry path.  Log whether a pending response   */
    /* exists — pend=1 means we are about to enter the crash code.    */
    if (state->has_pending_response) {

        dbg_stage("L08a pend=1", 8, 25);

        /* ------ L08b: before ce_response_read -------------------- */
        /* This fires immediately before writing 4510 bytes (the      */
        /* ce_response_t probe struct) onto the stack.  On hardware   */
        /* iteration 2, this is the last marker before the crash.     */
        dbg_stage("L08b pre-read", 8, 25);

        memset(&s_probe_response_ld, 0, sizeof(s_probe_response_ld));
        if (ce_response_read(&s_probe_response_ld)) {
            /* ------ L08c read-ok: ce_response_read returned true -- */
            /* With the fix applied, this should fire on iteration 2: */
            /* the worker wrote CE_RESP normally, CALCENG re-entered, */
            /* and the probe read succeeded (not a child failure).     */
            dbg_stage("L08c read-ok", 8, 25);
            return false;
        }

        /* ------ L08c read-bad: ce_response_read returned false ---- */
        /* Survived the write but response is missing/corrupt —       */
        /* confirmed child failure path.                              */
        dbg_stage("L08c read-bad", 8, 25);
        has_invalid_response = true;

    } else {

        /* ------ L08a pend=0: first-run path, no crash risk -------- */
        /* has_pending_response is 0; probe is never written.         */
        /* If the crash is here, it must be in a different code path. */
        dbg_stage("L08a pend=0", 8, 25);

    }

    strncpy(child_program, state->pending_module, sizeof(child_program) - 1);
    child_program[sizeof(child_program) - 1] = '\0';

    /* ------ L08d: before error message formatting ----------------- */
    dbg_stage("L08d pre-format", 8, 25);

    ce_format_child_failure_message_ld(err_buf, err_buf_size,
                                       child_program, has_invalid_response);
    ce_response_delete();
    ce_runtime_archive_pending_child_ld(state);
    ce_runtime_set_pending_module_ld(state, pending_target);
    state->has_pending_response = 0;
    state->settings = g_settings;
    state->last_answer = g_last_answer;
    ce_state_save(state);

    /* ------ L08e: after format + state save ----------------------- */
    dbg_stage("L08e post-format", 8, 25);

    return true;
}
