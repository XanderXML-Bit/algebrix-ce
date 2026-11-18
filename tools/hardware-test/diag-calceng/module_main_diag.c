/*
 * module_main_diag.c — Diagnostic copy of src/split/module_main.c.
 * Adds W01–W07 keypress-gated stage markers with CEDBG AppVar logging.
 *
 * Stage map (11 worker stages, total=11):
 *   W01 entry        — first line of worker main() before any init
 *   W02 globals init — after static variable setup
 *   W03 req open     — before ce_module_runtime_load() (reads request AppVar)
 *   W04 req read     — after ce_module_runtime_load() succeeds
 *   W05 req parsed   — after confirming root/had_equals from IPC payload
 *   W06 pre dispatch — before calling the computation handler
 *   W07 handler ok   — after handler returns
 *   W08 resp open    — before ce_response_write() in module_runtime_finish_diag()
 *   W09 resp written — after ce_response_write() succeeds
 *   W10 resp closed  — after ce_state_save() completes
 *   W11 pre exit     — immediately before os_RunPrgm("CALCENG") re-launch
 *
 * W08-W11 are implemented in module_runtime_diag.c (which defines
 * ce_module_runtime_finish for this diagnostic build).
 *
 * DO NOT MERGE TO MAIN. Diagnostic use only.
 */

#include <tice.h>
#include <string.h>
#include <stdio.h>

#include "calcengine.h"
#include "split/ipc.h"
#include "split/module_runtime.h"
#include "split/processor.h"

#include "diag_helpers.h"

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

    /* ---- W01: entry — first line of worker main() ---- */
    /* Note: do NOT call dbg_appvar_init() here.
     * CALCENG already wiped CEDBG at L01.  Worker appends to the same trace. */
    g_dbg_skip = 0;   /* fresh skip state for this worker load */
    dbg_stage("W01 entry", 1, 11);

    /* ---- W02: globals init ---- */
    /* (req/resp/state are static — initialised by .8xp BSS/data sections) */
    dbg_stage("W02 globals init", 2, 11);

    /* ---- W03: req open (about to call ce_module_runtime_load → ti_Open) ---- */
    dbg_stage("W03 req open", 3, 11);

    if (!ce_module_runtime_load(req, resp, state, &root, &had_equals)) {
        /* Load failed — still call finish so CALCENG gets an error response */
        dbg_appvar_append("W_load_fail");
        ce_module_runtime_finish(req, resp, state);
        return 1;
    }

    /* ---- W04: req read (ce_module_runtime_load returned success) ---- */
    dbg_stage("W04 req read", 4, 11);

    /* ---- W05: req parsed (root/had_equals extracted from IPC payload) ---- */
    dbg_appvar_append(root ? "W05 req parsed+AST" : "W05 req parsed noAST");
    if (!g_dbg_skip) {
        dbg_stage(root ? "W05 req parsed+AST" : "W05 req parsed", 5, 11);
    }

    /* ---- W06: pre dispatch (about to call computation handler) ---- */
    dbg_stage("W06 pre dispatch", 6, 11);

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

    /* ---- W07: handler ok (computation handler returned) ---- */
    dbg_stage("W07 handler ok", 7, 11);

    /* W08-W11 are inside ce_module_runtime_finish (module_runtime_diag.c) */
    ce_module_runtime_finish(req, resp, state);
    return 0;
}
