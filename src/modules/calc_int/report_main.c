#include <stdio.h>
#include <string.h>

#include "split/module_runtime.h"
#include "modules/calc_int/strategy_rank.h"

#define CE_SELF_PROGRAM   "CEISTR"

static bool ce_is_report_request(const ast_node_t *root)
{
    if (!root) return false;
    /* The strategy children rank indefinite integrals only. */
    if (root->type == NODE_INTEGRAL) {
        return !root->d.integral.lo && !root->d.integral.hi;
    }
    return root->type == NODE_POLAR && root->d.polar.op == POLOP_INTSTRATEGY;
}

/* Each program launch enters main once. Keep IPC storage off the OS stack. */
static ce_request_t req;
static ce_response_t resp;
static ce_shared_state_t state;
static ce_istr_scratch_t scratch;

static char out[MAX_RESULT_LEN];
static char err[128];

#ifdef CE_ISTR_HARNESS
/* Desktop harness hook (tests/harness/ceistr_reentry_harness.c): a CE
 * program reload zeroes every static before the callback re-enters main(). */
void ce_istr_harness_reload(void)
{
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    memset(&state, 0, sizeof(state));
    memset(&scratch, 0, sizeof(scratch));
    memset(out, 0, sizeof(out));
    memset(err, 0, sizeof(err));
}
#endif

static void ce_istr_fail(const char *msg)
{
    /* Name CEISTR as the answering child even when CE_STATE was lost and
     * reloaded from defaults (empty pending_module): CECIAD's resume_peek
     * only recognises a resumed child by that name, and would otherwise
     * treat CEISTR's own forwarded int(expr,var) as a fresh request. */
    strncpy(state.pending_module, CE_SELF_PROGRAM, sizeof(state.pending_module) - 1);
    state.pending_module[sizeof(state.pending_module) - 1] = '\0';
    ce_istr_scratch_delete();
    ce_response_from_runtime(&resp, false, false, "", msg, g_last_answer);
    ce_module_runtime_finish(&req, &resp, &state);
}

/* Runs the remaining strategies, then answers with the ranking.
 *
 * On hardware a successful ce_module_runtime_forward() never returns: the
 * child runs, returns, and CEISTR is reloaded and re-entered through the
 * forward callback, where main() records the child's result and calls this
 * again. That is why the scratch is saved before every forward. The loop body
 * after a successful forward runs only on the synchronous desktop mock. A
 * failed forward (launch failure, missing module) returns on hardware too and
 * is recorded as that strategy's reason. */
static void ce_istr_run(void)
{
    while (ce_istr_next(&scratch) < CE_ISTR_COUNT) {
        size_t i = ce_istr_next(&scratch);
        if (!ce_istr_scratch_save(&scratch)) {
            ce_istr_fail("Strategy report could not save its progress");
            return;
        }
        if (!ce_module_runtime_forward(CE_SELF_PROGRAM, ce_istr_programs[i],
                                       &req, &state, &resp,
                                       err, sizeof(err))) {
            ce_istr_record(&scratch, false, err);
            continue;
        }
        ce_istr_record(&scratch, resp.ok != 0, resp.error);
    }

    ce_istr_format(&scratch, out, sizeof(out));
    ce_istr_scratch_delete();
    ce_response_from_runtime(&resp, true, false, out, "", g_last_answer);
    ce_module_runtime_finish(&req, &resp, &state);
}

int main(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;
    char child[9];

    /* Re-entered after a strategy child answered. CE_REQ still holds this
     * module's request: the strategy children never forward (guarded by
     * tests/cemu/test_module_handoff_contract.py). */
    if (ce_module_runtime_resume_peek(CE_SELF_PROGRAM, &req, &state, &resp,
                                      child, sizeof(child))) {
        ce_module_runtime_reclaim_forward(CE_SELF_PROGRAM, &state);
        if (!ce_istr_scratch_load(&scratch) ||
            !ce_istr_record_child(&scratch, child, resp.ok != 0, resp.error)) {
            ce_istr_fail("Strategy report lost its progress");
            return 0;
        }
        ce_istr_run();
        return 0;
    }

    /* Re-entered, but the child left no readable response. resume_peek has
     * already loaded CE_STATE, so pending_module still names that child. */
    strncpy(child, state.pending_module, sizeof(child) - 1);
    child[sizeof(child) - 1] = '\0';
    if (ce_module_runtime_recover_child_failure(CE_SELF_PROGRAM, CE_SELF_PROGRAM,
                                                &req, &state, err, sizeof(err))) {
        if (!ce_istr_scratch_load(&scratch) ||
            !ce_istr_record_child(&scratch, child, false, err)) {
            ce_istr_fail(err);
            return 0;
        }
        ce_istr_run();
        return 0;
    }

    /* Re-entered, yet neither branch above recognised a returned strategy
     * child (for example CE_STATE could not be read back). CE_REQ now holds
     * this module's own forwarded int(expr,var), so loading it as a fresh
     * request would silently restart all four strategies. Fail instead. */
    if (ce_module_runtime_was_reentered()) {
        ce_istr_fail("Strategy report lost its progress");
        return 0;
    }

    if (!ce_module_runtime_load(&req, &resp, &state, &root, &had_equals)) {
        ce_module_runtime_finish(&req, &resp, &state);
        return 1;
    }

    (void)had_equals;
    if (!ce_is_report_request(root)) {
        ce_response_from_runtime(&resp, false, false, "",
                                 "Unsupported strategy report request",
                                 g_last_answer);
        ast_free(root);
        ce_module_runtime_finish(&req, &resp, &state);
        return 0;
    }
    if (!ce_istr_integral_request(root, &req)) {
        ce_response_from_runtime(&resp, false, false, "",
                                 "Strategy report could not forward int(expr,dx)",
                                 g_last_answer);
        ast_free(root);
        ce_module_runtime_finish(&req, &resp, &state);
        return 0;
    }
    ast_free(root);

    /* Fresh dispatch: drop any progress an aborted earlier run left behind. */
    ce_istr_scratch_reset(&scratch);
    ce_istr_run();
    return 0;
}
