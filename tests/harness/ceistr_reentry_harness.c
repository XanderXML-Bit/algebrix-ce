/*
 * ceistr_reentry_harness.c — runs CEISTR's real main() (report_main.c) the way
 * the TI-84 Plus CE runs it, on the desktop.
 *
 * On the CE, os_RunPrgm(child, ..., callback) never returns to its caller.
 * The child runs and returns from its own main(), the runprgm stub reloads
 * the parent (every static zeroed) and calls the callback, which re-enters
 * main(). When that re-entered main() returns, control goes back to the
 * parent's own caller (CECIAD), not to the line after os_RunPrgm.
 *
 * The os_RunPrgm mock below does the same: it plays the strategy child
 * (writes CE_RESP and CE_STATE as ce_module_runtime_finish() would, or
 * leaves no response), zeroes CEISTR's statics through
 * ce_istr_harness_reload(), calls the callback, and then longjmp()s back to
 * the harness. A launch failure returns non-zero, as the real call does.
 *
 * The simulated reload zeroes CEISTR's own statics, clears the re-entry flag
 * and restores g_settings/g_last_answer to their load-time values. The node
 * pool and module_runtime.c's probe buffer are not reset; every re-entry path
 * re-initialises them (mem_init, memset) before use.
 *
 * Built and run by tests/cemu/test_ceistr_reentry_harness.py. Exit status 0
 * means every check passed.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "calcengine.h"
#include "memory.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/module_runtime.h"
/* The harness uses only part of the header's static helpers. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "modules/calc_int/strategy_rank.h"

int ceistr_main(void);              /* report_main.c main(), renamed */
void ce_istr_harness_reload(void);  /* report_main.c, CE_ISTR_HARNESS */

typedef enum {
    CHILD_FAIL = 0,     /* answers ok=0 with an error message */
    CHILD_OK,           /* answers ok=1 */
    CHILD_NO_RESPONSE,  /* returns without writing CE_RESP */
    CHILD_LAUNCH_FAIL,  /* os_RunPrgm fails and returns to the caller */
    CHILD_FAIL_LONG,    /* answers ok=0 with the worker's real long message */
    CHILD_FAIL_MAX      /* answers ok=0 with a 127-char error, the CE_RESP maximum */
} child_mode_t;

/* The strategy workers' real failure messages (src/integrate.c). They are
 * longer than the old 96-byte reason slot: the CE result viewer showed them
 * cut mid-word as "or recipro" and "of the in". */
static const char k_trig_reason[] =
    "Trig substitution pattern not detected (sqrt(a^2-x^2), "
    "sqrt(x^2+a^2), sqrt(x^2-a^2), or reciprocal forms)";
static const char k_usub_reason[] =
    "u-substitution not applicable: no inner function u found"
    " whose derivative is a factor of the integrand";
/* 127 characters, the most ce_response_t.error can carry; one per child,
 * with word lengths that differ by child. */
static char k_max_reason[CE_ISTR_COUNT][128];

static child_mode_t g_mode[CE_ISTR_COUNT];
static char g_hops[16][9];
static int g_hop_count;
static int g_bad_requests;
static jmp_buf g_parent_return;
static int g_failures;
static settings_t g_settings_at_load;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", (msg), __LINE__); g_failures++; } \
    else { printf("ok: %s\n", (msg)); } \
} while (0)

static int child_index(const char *name)
{
    int i;
    for (i = 0; i < (int)CE_ISTR_COUNT; i++) {
        if (strcmp(name, ce_istr_programs[i]) == 0) return i;
    }
    return -1;
}

/* The strategy child: accepts only an indefinite int(expr,var), like
 * src/modules/calc_int/strategy_main.c. */
static void play_child(int idx)
{
    static ce_request_t req;
    static ce_response_t resp;
    static ce_shared_state_t state;
    ast_node_t *root = NULL;
    bool accepts;

    memset(&req, 0, sizeof(req));
    mem_init();
    ce_state_load(&state);
    if (ce_request_read(&req) && (req.flags & CE_REQ_FLAG_HAS_AST) && req.ast_size) {
        root = ce_ast_deserialize(req.ast_payload, req.ast_size);
    }
    accepts = root && root->type == NODE_INTEGRAL &&
              !root->d.integral.lo && !root->d.integral.hi;
    if (!accepts) g_bad_requests++;
    ast_free(root);

    if (g_mode[idx] == CHILD_NO_RESPONSE) return;
    if (g_mode[idx] == CHILD_OK && accepts) {
        ce_response_from_runtime(&resp, true, false, "int = ... + C", "", 0.0f);
    } else if (g_mode[idx] == CHILD_FAIL_LONG && accepts) {
        const char *msg = idx == 1 ? k_trig_reason :
                          idx == 2 ? k_usub_reason : k_max_reason[idx];
        ce_response_from_runtime(&resp, false, false, "", msg, 0.0f);
    } else if (g_mode[idx] == CHILD_FAIL_MAX && accepts) {
        ce_response_from_runtime(&resp, false, false, "", k_max_reason[idx], 0.0f);
    } else {
        ce_response_from_runtime(&resp, false, false, "",
                                 accepts ? "child says no" :
                                 "Unsupported advanced integral request", 0.0f);
    }
    ce_module_runtime_finish(&req, &resp, &state);
}

int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t callback)
{
    int idx = child_index(prgm);
    (void)data;
    (void)size;
    if (g_hop_count < 16) {
        strncpy(g_hops[g_hop_count], prgm, 8);
        g_hops[g_hop_count][8] = '\0';
    }
    g_hop_count++;
    if (idx < 0 || g_hop_count > 12) return -1;      /* unknown child / loop guard */
    if (g_mode[idx] == CHILD_LAUNCH_FAIL) return -1;  /* returns, like the CE */

    play_child(idx);

    /* Program reload, then the stub's callback re-enters main(). */
    ce_istr_harness_reload();
    ce_module_runtime_reset_reentry();
    g_settings = g_settings_at_load;   /* initialised data reloads too */
    g_last_answer = 0;
    if (callback) callback(NULL, 0);
    /* The re-entered main() returned: control goes to CEISTR's caller. */
    longjmp(g_parent_return, 1);
}

/* CECIAD dispatching intstrategy(x*sin(x),x) to CEISTR. */
static void dispatch(const char *const *programs_present)
{
    static ce_request_t req;
    ce_shared_state_t state;
    ast_node_t *x1, *x2, *sinx, *prod, *root;
    uint16_t size = 0;

    mem_init();
    memset(&req, 0, sizeof(req));
    req.magic = CE_IPC_MAGIC;
    req.version = CE_IPC_VERSION;
    strncpy(req.expression, "intstrategy(x*sin(x),x)", MAX_INPUT_LEN - 1);
    strncpy(req.caller, "CECIAD", sizeof(req.caller) - 1);
    strncpy(req.return_to, "CECIAD", sizeof(req.return_to) - 1);
    x1 = ast_variable('x');
    x2 = ast_variable('x');
    sinx = ast_func(FN_SIN, x2);
    prod = ast_binop(OP_MUL, x1, sinx);
    root = mem_alloc_node();
    root->type = NODE_POLAR;
    root->d.polar.op = POLOP_INTSTRATEGY;
    root->d.polar.var = 'x';
    root->d.polar.expr1 = prod;
    ce_ast_serialize(root, req.ast_payload, sizeof(req.ast_payload), &size);
    req.ast_size = size;
    req.flags = CE_REQ_FLAG_HAS_AST;
    ast_free(root);
    ce_request_write(&req);

    ce_state_defaults(&state);
    strncpy(state.pending_module, "CEISTR", sizeof(state.pending_module) - 1);
    ce_state_save(&state);

    for (; *programs_present; programs_present++) {
        fileioc_mock_add_program(*programs_present, 1);
    }
}

static void reset_world(void)
{
    fileioc_mock_reset();
    fileioc_mock_enable();
    memset(g_mode, 0, sizeof(g_mode));
    memset(g_hops, 0, sizeof(g_hops));
    g_hop_count = 0;
    g_bad_requests = 0;
    ce_istr_harness_reload();
    ce_module_runtime_reset_reentry();
}

/* Runs CEISTR from a fresh dispatch until control returns to CECIAD. */
static void run_ceistr(void)
{
    if (setjmp(g_parent_return) == 0) {
        ceistr_main();
    }
}

static bool final_answer(ce_response_t *resp, ce_shared_state_t *state)
{
    memset(resp, 0, sizeof(*resp));
    memset(state, 0, sizeof(*state));
    return ce_response_read(resp) && ce_state_load(state);
}

static bool hops_are(const char *const *want, int n)
{
    int i;
    if (g_hop_count != n) return false;
    for (i = 0; i < n; i++) {
        if (strcmp(g_hops[i], want[i]) != 0) return false;
    }
    return true;
}

static const char *const k_all[] = { "CEIPFR", "CEITRG", "CEIUSB", "CEIPRT", NULL };
static const char *const k_no_trig[] = { "CEIPFR", "CEIUSB", "CEIPRT", NULL };

int main(void)
{
    ce_response_t resp;
    ce_shared_state_t state;
    ce_istr_scratch_t s;

    g_settings_at_load = g_settings;

    /* 1. x*sin(x): only integration by parts matches. */
    reset_world();
    dispatch(k_all);
    g_mode[3] = CHILD_OK;
    run_ceistr();
    CHECK(hops_are(k_all, 4), "1: all four strategies launched in order");
    CHECK(g_bad_requests == 0, "1: every child got an indefinite int(expr,var)");
    CHECK(final_answer(&resp, &state), "1: final CE_RESP and CE_STATE readable");
    CHECK(resp.ok == 1, "1: ranking answered ok");
    CHECK(strncmp(resp.result, "Best: integration by parts\n1) integration by parts (score 80): matched by CEIPRT\n",
                  strlen("Best: integration by parts\n1) integration by parts (score 80): matched by CEIPRT\n")) == 0,
          "1: by parts ranked first and summarised");
    CHECK(strstr(resp.result, "2) partial fractions (score 48): child says no") != NULL,
          "1: failed strategy keeps the child's reason");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "1: CE_STATE hands the answer up from CEISTR (CECIAD resume_peek sees CEISTR)");
    CHECK(!ce_istr_scratch_load(&s), "1: CEISTRS deleted after the ranking");

    /* 2. A child that returns without writing CE_RESP. */
    reset_world();
    dispatch(k_all);
    g_mode[1] = CHILD_NO_RESPONSE;
    g_mode[3] = CHILD_OK;
    run_ceistr();
    CHECK(hops_are(k_all, 4), "2: run continues past a silent child");
    CHECK(final_answer(&resp, &state) && resp.ok == 1, "2: ranking answered ok");
    CHECK(strstr(resp.result, "trig substitution (score 44): Module CEITRG returned no IPC response") != NULL,
          "2: silent child recorded as its strategy's failure");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "2: CE_STATE hands the answer up");

    /* 3. os_RunPrgm fails (returns on the CE too). */
    reset_world();
    dispatch(k_all);
    g_mode[1] = CHILD_LAUNCH_FAIL;
    g_mode[3] = CHILD_OK;
    run_ceistr();
    CHECK(hops_are(k_all, 4), "3: run continues past a launch failure");
    CHECK(final_answer(&resp, &state) && resp.ok == 1, "3: ranking answered ok");
    CHECK(strstr(resp.result, "trig substitution (score 44): Program launch failed") != NULL,
          "3: launch failure recorded");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "3: CE_STATE hands the answer up");

    /* 4. A strategy program is missing. */
    reset_world();
    dispatch(k_no_trig);
    g_mode[3] = CHILD_OK;
    run_ceistr();
    {
        static const char *const want[] = { "CEIPFR", "CEIUSB", "CEIPRT" };
        CHECK(hops_are(want, 3), "4: missing child skipped, the others launched");
    }
    CHECK(final_answer(&resp, &state) && resp.ok == 1, "4: ranking answered ok");
    CHECK(strstr(resp.result, "trig substitution (score 44): Module CEITRG missing") != NULL,
          "4: missing child recorded");

    /* 5. A stale CEISTRS from an aborted run is discarded on fresh dispatch. */
    reset_world();
    dispatch(k_all);
    ce_istr_scratch_reset(&s);
    ce_istr_record(&s, true, "");
    ce_istr_record(&s, true, "");
    ce_istr_scratch_save(&s);
    g_mode[3] = CHILD_OK;
    run_ceistr();
    CHECK(hops_are(k_all, 4), "5: stale progress ignored, all four launched");
    CHECK(final_answer(&resp, &state) && strncmp(resp.result, "Best: integration by parts\n", 27) == 0,
          "5: ranking built from this run only");

    /* 6. Re-entered for a child that is not the pending strategy. */
    reset_world();
    dispatch(k_all);
    ce_istr_scratch_reset(&s);
    ce_istr_scratch_save(&s);                      /* CEIPFR pending */
    ce_state_load(&state);
    strncpy(state.pending_module, "CEIUSB", sizeof(state.pending_module) - 1);
    ce_state_save(&state);
    ce_response_from_runtime(&resp, true, false, "int = u + C", "", 0.0f);
    ce_module_runtime_finish(NULL, &resp, &state);
    ce_module_runtime_mark_reentry();
    run_ceistr();
    CHECK(g_hop_count == 0, "6: no strategy relaunched");
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Strategy report lost its progress") == 0,
          "6: unexpected child reported, not trusted");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "6: CE_STATE hands the error up");
    CHECK(!ce_istr_scratch_load(&s), "6: CEISTRS deleted");

    /* 7. Re-entered with nothing recognisable pending: must not restart. */
    reset_world();
    dispatch(k_all);
    ce_module_runtime_mark_reentry();               /* pending_module == CEISTR */
    run_ceistr();
    CHECK(g_hop_count == 0, "7: re-entry without a pending child does not restart");
    CHECK(final_answer(&resp, &state) && resp.ok == 0, "7: reported as an error");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "7: CE_STATE hands the error up");

    /* 8. Re-entered after CE_STATE was lost: defaults have no pending child,
     * yet the error must still name CEISTR so CECIAD recognises it. */
    reset_world();
    dispatch(k_all);
    ce_istr_scratch_reset(&s);
    ce_istr_scratch_save(&s);
    ti_Delete(CE_APPVAR_STATE);
    ce_module_runtime_mark_reentry();
    run_ceistr();
    CHECK(g_hop_count == 0, "8: lost CE_STATE does not restart the strategies");
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Strategy report lost its progress") == 0,
          "8: reported as lost progress, CE_STATE readable again");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "8: error names CEISTR as the answering child");
    CHECK(!ce_istr_scratch_load(&s), "8: CEISTRS deleted");

    /* 9. A definite integral is not a strategy request. */
    reset_world();
    {
        static ce_request_t rq;
        ast_node_t *integral;
        uint16_t size = 0;
        dispatch(k_all);
        ce_request_read(&rq);
        mem_init();
        integral = mem_alloc_node();
        integral->type = NODE_INTEGRAL;
        integral->d.integral.var = 'x';
        integral->d.integral.expr = ast_variable('x');
        integral->d.integral.lo = ast_number(0.0f);
        integral->d.integral.hi = ast_number(1.0f);
        ce_ast_serialize(integral, rq.ast_payload, sizeof(rq.ast_payload), &size);
        rq.ast_size = size;
        ast_free(integral);
        ce_request_write(&rq);
    }
    run_ceistr();
    CHECK(g_hop_count == 0, "9: definite integral not forwarded");
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Unsupported strategy report request") == 0,
          "9: definite integral reported as unsupported");

    /* 10. Long reasons through the real re-entry path: the trig and u-sub
     * workers' own messages arrive whole in the ranking. */
    {
        size_t c, w;
        for (c = 0; c < CE_ISTR_COUNT; c++) {
            for (w = 0; w + 1 < sizeof(k_max_reason[c]); w++) {
                k_max_reason[c][w] = ((w + c) % (7 + c) == 6) ? ' ' : (char)('a' + (w % 26));
            }
            k_max_reason[c][sizeof(k_max_reason[c]) - 1] = '\0';
        }
    }
    reset_world();
    dispatch(k_all);
    g_mode[1] = CHILD_FAIL_LONG;
    g_mode[2] = CHILD_FAIL_LONG;
    g_mode[3] = CHILD_OK;
    run_ceistr();
    CHECK(hops_are(k_all, 4), "10: all four strategies launched");
    CHECK(final_answer(&resp, &state) && resp.ok == 1, "10: ranking answered ok");
    {
        char want[200];
        snprintf(want, sizeof(want), "\n3) trig substitution (score 44): %s\n", k_trig_reason);
        CHECK(strstr(resp.result, want) != NULL, "10: trig reason survives re-entry whole");
        snprintf(want, sizeof(want), "\n4) u-substitution (score 42): %s", k_usub_reason);
        CHECK(strstr(resp.result, want) != NULL &&
              strcmp(strstr(resp.result, want), want) == 0,
              "10: u-sub reason whole at the end of the ranking");
    }
    CHECK(strstr(resp.result, "...") == NULL, "10: nothing clipped when the ranking fits");
    CHECK(strcmp(state.pending_module, "CEISTR") == 0 && state.has_pending_response == 1,
          "10: CE_STATE hands the answer up");

    /* 11. Three children fail with 127-char errors (the CE_RESP maximum):
     * the ranking still fits CE_RESP, keeps all four lines, and every failed
     * strategy's reason is a word-boundary prefix of its child's error
     * followed by "...". */
    reset_world();
    dispatch(k_all);
    g_mode[0] = CHILD_FAIL_MAX;
    g_mode[1] = CHILD_FAIL_MAX;
    g_mode[2] = CHILD_FAIL_MAX;
    g_mode[3] = CHILD_OK;
    run_ceistr();
    CHECK(hops_are(k_all, 4), "11: all four strategies launched");
    CHECK(final_answer(&resp, &state) && resp.ok == 1, "11: ranking answered ok");
    CHECK(strlen(resp.result) < sizeof(resp.result), "11: ranking fits CE_RESP");
    CHECK(strstr(resp.result, "\n1) integration by parts (score 80): matched by CEIPRT\n") != NULL,
          "11: the short winner reason is whole");
    {
        static const char *const prefixes[3] = {
            "\n2) partial fractions (score 48): ",
            "\n3) trig substitution (score 44): ",
            "\n4) u-substitution (score 42): "
        };
        int c;
        for (c = 0; c < 3; c++) {
            const char *p2 = strstr(resp.result, prefixes[c]);
            char msg[64];
            snprintf(msg, sizeof(msg), "11: child %d reason clipped at a word with ...", c);
            if (!p2) {
                CHECK(0, msg);
                continue;
            }
            {
                const char *body = p2 + strlen(prefixes[c]);
                const char *end = strchr(body, '\n');
                size_t blen = end ? (size_t)(end - body) : strlen(body);
                CHECK(blen > 3 && blen < strlen(k_max_reason[c]) &&
                      strncmp(body + blen - 3, "...", 3) == 0 &&
                      strncmp(body, k_max_reason[c], blen - 3) == 0 &&
                      k_max_reason[c][blen - 3] == ' ',
                      msg);
                CHECK(blen >= 80, "11: clipped reason keeps most of its share");
            }
        }
    }

    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
