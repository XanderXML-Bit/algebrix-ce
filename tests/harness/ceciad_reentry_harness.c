/*
 * ceciad_reentry_harness.c — runs CECIAD's real main() (adv_main.c) the way
 * the TI-84 Plus CE runs it, on the desktop.
 *
 * CECIAD is the advanced-integral forwarder: CECISM hands it every
 * indefinite int() its basic rules could not solve (CEIADVREQ), and CECIAD
 * tries the strategy workers CEIPFR, CEITRG, CEIUSB and CEIPRT in turn.
 * On the CE, os_RunPrgm(child, ..., callback) never returns to its caller:
 * the child returns from its own main(), the runprgm stub reloads CECIAD
 * (every static zeroed) and the callback re-enters main(), which picks the
 * next strategy from the child that just answered.
 *
 * The os_RunPrgm mock below does the same. It plays the strategy child
 * (writes CE_RESP and CE_STATE as ce_module_runtime_finish() would, or
 * leaves no response), zeroes CECIAD's statics through
 * ce_iadv_harness_reload(), calls the callback, and then longjmp()s back to
 * the harness, i.e. to CECIAD's caller (CECISM). A launch failure returns
 * non-zero, as the real call does. With g_inprocess set the mock returns 0
 * after the child instead, like the desktop module mock: that drives the
 * in-process loop in ce_forward_integral_sequence().
 *
 * Previously, when every strategy refused int(int(x)), the CE showed
 * the last worker's reason, "Integration by parts requires a product-like
 * integrand", while the desktop CLI says "Cannot find symbolic
 * antiderivative".
 *
 * Usage:
 *   ceciad_harness                 run the built-in checks; exit 0 = pass
 *   ceciad_harness --chain KIND OK0 MSG0 OK1 MSG1 OK2 MSG2 OK3 MSG3
 *       run one CE-style chain where strategy child i answers OKi (0/1)
 *       with MSGi (its result when OKi is 1, else its error), and print
 *       "<ok>\t<result or error>" of the answer CECIAD hands up.
 *       KIND is "int" (an indefinite int(x,x) request) or "intparts".
 *       tests/cemu/test_ceciad_reentry_harness.py feeds it the real
 *       workers' answers (tests/module_parity/strategy_driver.c).
 *
 * Built and run by tests/cemu/test_ceciad_reentry_harness.py.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fileioc.h>

#include "calcengine.h"
#include "integrate.h"
#include "memory.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/module_runtime.h"

int ceciad_main(void);              /* adv_main.c main(), renamed */
void ce_iadv_harness_reload(void);  /* adv_main.c, CE_IADV_HARNESS */

#define N_CHILD 4
static const char *const k_children[N_CHILD] = {
    "CEIPFR", "CEITRG", "CEIUSB", "CEIPRT"
};

typedef enum {
    CHILD_FAIL = 0,     /* answers ok=0 with g_msg[i] */
    CHILD_OK,           /* answers ok=1 with g_msg[i] as the result */
    CHILD_NO_RESPONSE,  /* returns without writing CE_RESP */
    CHILD_LAUNCH_FAIL   /* os_RunPrgm fails and returns to the caller */
} child_mode_t;

/* The strategy workers' real refusals of int(int(x,x),x) (src/integrate.c,
 * integrate_with_strategy). The python test checks that the real workers
 * still answer these. */
static const char k_pfr_reason[] =
    "Partial fractions requires a rational form that decomposes cleanly";
static const char k_trg_reason[] =
    "Trig substitution pattern not detected (sqrt(a^2-x^2), "
    "sqrt(x^2+a^2), sqrt(x^2-a^2), or reciprocal forms)";
static const char k_usb_reason[] =
    "u-substitution not applicable: no inner function u found"
    " whose derivative is a factor of the integrand";
static const char k_prt_reason[] = INTEG_PARTS_NOT_PRODUCT_MSG;

static child_mode_t g_mode[N_CHILD];
static char g_msg[N_CHILD][128];
static char g_hops[16][9];
static int g_hop_count;
static int g_bad_requests;
static bool g_inprocess;
static jmp_buf g_parent_return;
static int g_failures;
static settings_t g_settings_at_load;
static ce_float_t g_ans_at_dispatch;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", (msg), __LINE__); g_failures++; } \
    else { printf("ok: %s\n", (msg)); } \
} while (0)

static int child_index(const char *name)
{
    int i;
    for (i = 0; i < N_CHILD; i++) {
        if (strcmp(name, k_children[i]) == 0) return i;
    }
    return -1;
}

/* The strategy child, like src/modules/calc_int/strategy_main.c: it loads
 * Ans from CE_STATE, reads the request AST and answers. */
static void play_child(int idx)
{
    static ce_request_t req;
    static ce_response_t resp;
    static ce_shared_state_t state;
    ast_node_t *root = NULL;

    memset(&req, 0, sizeof(req));
    mem_init();
    ce_state_load(&state);
    g_settings = state.settings;
    g_last_answer = state.last_answer;
    if (ce_request_read(&req) && (req.flags & CE_REQ_FLAG_HAS_AST) && req.ast_size) {
        root = ce_ast_deserialize(req.ast_payload, req.ast_size);
    }
    if (!root || strcmp(req.caller, "CECIAD") != 0) g_bad_requests++;
    ast_free(root);

    if (g_mode[idx] == CHILD_NO_RESPONSE) return;
    if (g_mode[idx] == CHILD_OK) {
        ce_response_from_runtime(&resp, true, true, g_msg[idx], "", g_last_answer);
    } else {
        ce_response_from_runtime(&resp, false, false, "", g_msg[idx], g_last_answer);
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
    if (g_inprocess) return 0;                        /* desktop module mock */

    /* Program reload, then the stub's callback re-enters main(). */
    ce_iadv_harness_reload();
    ce_module_runtime_reset_reentry();
    g_settings = g_settings_at_load;   /* initialised data reloads too */
    g_last_answer = 0;
    if (callback) callback(NULL, 0);
    /* The re-entered main() returned: control goes to CECIAD's caller. */
    longjmp(g_parent_return, 1);
}

/* CECISM forwarding a request to CECIAD. kind "int": int(x,x) whose
 * integrand is int(x,x) (a nested indefinite integral). kind "intparts":
 * the direct intparts(x*x,x) op, which goes to CEIPRT alone. */
static void dispatch(const char *kind)
{
    static ce_request_t req;
    ce_shared_state_t state;
    ast_node_t *root;
    uint16_t size = 0;
    int i;

    mem_init();
    memset(&req, 0, sizeof(req));
    req.magic = CE_IPC_MAGIC;
    req.version = CE_IPC_VERSION;
    req.last_answer = g_ans_at_dispatch;
    strncpy(req.caller, "CECISM", sizeof(req.caller) - 1);
    strncpy(req.return_to, "CECISM", sizeof(req.return_to) - 1);
    root = mem_alloc_node();
    if (strcmp(kind, "intparts") == 0) {
        strncpy(req.expression, "intparts(x*x,x)", MAX_INPUT_LEN - 1);
        root->type = NODE_POLAR;
        root->d.polar.op = POLOP_INTPARTS;
        root->d.polar.var = 'x';
        root->d.polar.expr1 = ast_binop(OP_MUL, ast_variable('x'), ast_variable('x'));
    } else {
        ast_node_t *inner = mem_alloc_node();
        strncpy(req.expression, "int(int(x,x),x)", MAX_INPUT_LEN - 1);
        inner->type = NODE_INTEGRAL;
        inner->d.integral.var = 'x';
        inner->d.integral.expr = ast_variable('x');
        root->type = NODE_INTEGRAL;
        root->d.integral.var = 'x';
        root->d.integral.expr = inner;
    }
    ce_ast_serialize(root, req.ast_payload, sizeof(req.ast_payload), &size);
    req.ast_size = size;
    req.flags = CE_REQ_FLAG_HAS_AST;
    ast_free(root);
    ce_request_write(&req);

    ce_state_defaults(&state);
    state.last_answer = g_ans_at_dispatch;
    strncpy(state.pending_module, "CECIAD", sizeof(state.pending_module) - 1);
    ce_state_save(&state);
    g_last_answer = 0;

    /* Unarchived, so the test can see that CECIAD archives each child it
     * ran (ce_runtime_archive_pending_child). */
    for (i = 0; i < N_CHILD; i++) fileioc_mock_add_program(k_children[i], 0);
}

static void reset_world(void)
{
    int i;
    fileioc_mock_reset();
    fileioc_mock_enable();
    memset(g_mode, 0, sizeof(g_mode));
    memset(g_hops, 0, sizeof(g_hops));
    g_hop_count = 0;
    g_bad_requests = 0;
    g_inprocess = false;
    g_ans_at_dispatch = 42.0f;
    strcpy(g_msg[0], k_pfr_reason);
    strcpy(g_msg[1], k_trg_reason);
    strcpy(g_msg[2], k_usb_reason);
    strcpy(g_msg[3], k_prt_reason);
    for (i = 0; i < N_CHILD; i++) g_mode[i] = CHILD_FAIL;
    ce_iadv_harness_reload();
    ce_module_runtime_reset_reentry();
}

/* Runs CECIAD from a fresh dispatch until control returns to CECISM. */
static void run_ceciad(void)
{
    if (setjmp(g_parent_return) == 0) {
        ceciad_main();
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

static bool handed_up(const ce_shared_state_t *state)
{
    return strcmp(state->pending_module, "CECIAD") == 0 &&
           state->has_pending_response == 1;
}

static int run_chain(int argc, char **argv)
{
    ce_response_t resp;
    ce_shared_state_t state;
    int i;

    if (argc != 11) {
        fprintf(stderr, "usage: --chain int|intparts OK0 MSG0 ... OK3 MSG3\n");
        return 2;
    }
    reset_world();
    for (i = 0; i < N_CHILD; i++) {
        g_mode[i] = atoi(argv[3 + 2 * i]) ? CHILD_OK : CHILD_FAIL;
        strncpy(g_msg[i], argv[4 + 2 * i], sizeof(g_msg[i]) - 1);
        g_msg[i][sizeof(g_msg[i]) - 1] = '\0';
    }
    dispatch(argv[2]);
    run_ceciad();
    if (!final_answer(&resp, &state) || !handed_up(&state) || g_bad_requests) {
        fprintf(stderr, "chain did not hand an answer up\n");
        return 1;
    }
    printf("%d\t%s\n", resp.ok, resp.ok ? resp.result : resp.error);
    return 0;
}

int main(int argc, char **argv)
{
    ce_response_t resp;
    ce_shared_state_t state;
    static const char *const all[] = { "CEIPFR", "CEITRG", "CEIUSB", "CEIPRT" };

    g_settings_at_load = g_settings;
    if (argc > 1 && strcmp(argv[1], "--chain") == 0) return run_chain(argc, argv);

    /* 1. int(int(x,x),x): every strategy refuses with its own reason. The
     * answer is the desktop CLI's, not CEIPRT's "product-like" reason. */
    reset_world();
    dispatch("int");
    run_ceciad();
    CHECK(hops_are(all, 4), "1: all four strategies launched in order");
    CHECK(g_bad_requests == 0, "1: every child got CECIAD's request");
    CHECK(final_answer(&resp, &state), "1: final CE_RESP and CE_STATE readable");
    CHECK(resp.ok == 0 && strcmp(resp.error, INTEG_NO_ANTIDERIV_MSG) == 0,
          "1: refusal says Cannot find symbolic antiderivative");
    CHECK(strstr(resp.error, "product-like") == NULL, "1: no by-parts reason");
    CHECK(resp.last_answer == 42.0f && state.last_answer == 42.0f,
          "1: Ans unchanged by the refusal");
    CHECK(handed_up(&state), "1: CE_STATE hands the answer up from CECIAD");
    CHECK(fileioc_mock_is_archived("CEIPFR", OS_TYPE_PRGM) &&
          fileioc_mock_is_archived("CEITRG", OS_TYPE_PRGM) &&
          fileioc_mock_is_archived("CEIUSB", OS_TYPE_PRGM) &&
          fileioc_mock_is_archived("CEIPRT", OS_TYPE_PRGM),
          "1: every child archived again, CEIPRT on the refusal path too");

    /* 2. The last strategy hit the nesting cap: that reason is kept. */
    reset_world();
    strcpy(g_msg[3], INTEG_TOO_DEEP_MSG);
    dispatch("int");
    run_ceciad();
    CHECK(hops_are(all, 4), "2: all four strategies launched");
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, INTEG_TOO_DEEP_MSG) == 0,
          "2: Expression too deep is kept");
    CHECK(handed_up(&state), "2: CE_STATE hands the answer up");

    /* 3. The last strategy ran out of nodes: that reason is kept. */
    reset_world();
    strcpy(g_msg[3], INTEG_OOM_MSG);
    dispatch("int");
    run_ceciad();
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, INTEG_OOM_MSG) == 0,
          "3: Out of memory is kept");

    /* 4. The workers did not accept the request at all: kept. */
    reset_world();
    strcpy(g_msg[3], "Unsupported advanced integral request");
    dispatch("int");
    run_ceciad();
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Unsupported advanced integral request") == 0,
          "4: Unsupported advanced integral request is kept");

    /* 5. u-substitution matches: the chain stops there, answer unchanged. */
    reset_world();
    g_mode[2] = CHILD_OK;
    strcpy(g_msg[2], "int = sin((x^2)) + C");
    dispatch("int");
    run_ceciad();
    {
        static const char *const want[] = { "CEIPFR", "CEITRG", "CEIUSB" };
        CHECK(hops_are(want, 3), "5: stops at the matching strategy");
    }
    CHECK(final_answer(&resp, &state) && resp.ok == 1 &&
          strcmp(resp.result, "int = sin((x^2)) + C") == 0 && resp.error[0] == '\0',
          "5: the worker's answer is handed up unchanged");
    CHECK(handed_up(&state), "5: CE_STATE hands the answer up");

    /* 6. The last strategy matches: answer unchanged. */
    reset_world();
    g_mode[3] = CHILD_OK;
    strcpy(g_msg[3], "int = ((x*(-cos(x)))+sin(x)) + C");
    dispatch("int");
    run_ceciad();
    CHECK(hops_are(all, 4), "6: all four launched");
    CHECK(final_answer(&resp, &state) && resp.ok == 1 &&
          strcmp(resp.result, "int = ((x*(-cos(x)))+sin(x)) + C") == 0,
          "6: by-parts answer handed up unchanged");

    /* 7. The last strategy cannot be launched: that failure is the answer. */
    reset_world();
    g_mode[3] = CHILD_LAUNCH_FAIL;
    dispatch("int");
    run_ceciad();
    CHECK(hops_are(all, 4), "7: all four attempted");
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Program launch failed") == 0,
          "7: launch failure is reported, not rewritten");

    /* 8. The last strategy returns without a response. */
    reset_world();
    g_mode[3] = CHILD_NO_RESPONSE;
    dispatch("int");
    run_ceciad();
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Module CEIPRT returned no IPC response") == 0,
          "8: silent child is reported, not rewritten");

    /* 9. A direct intparts() request goes to CEIPRT only, and CEIPRT's own
     * reason is the true answer to it: kept. */
    reset_world();
    dispatch("intparts");
    run_ceciad();
    {
        static const char *const want[] = { "CEIPRT" };
        CHECK(hops_are(want, 1), "9: intparts goes to CEIPRT alone");
    }
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, k_prt_reason) == 0,
          "9: intparts keeps the by-parts reason");
    CHECK(handed_up(&state), "9: CE_STATE hands the answer up");

    /* 10. The in-process loop (a launch that returns, as on the desktop
     * module mock) gives the same refusal. */
    reset_world();
    g_inprocess = true;
    dispatch("int");
    run_ceciad();
    CHECK(hops_are(all, 4), "10: in-process loop launches all four");
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, INTEG_NO_ANTIDERIV_MSG) == 0,
          "10: in-process refusal matches the CE path");

    /* 11. A silent child in the middle ends the chain (unchanged
     * behaviour): its failure is the answer, not the sequence refusal. */
    reset_world();
    g_mode[1] = CHILD_NO_RESPONSE;
    dispatch("int");
    run_ceciad();
    {
        static const char *const want[] = { "CEIPFR", "CEITRG" };
        CHECK(hops_are(want, 2), "11: chain ends at the silent child");
    }
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, "Module CEITRG returned no IPC response") == 0,
          "11: silent middle child is reported, not rewritten");

    /* 12. By parts found no reducible split: also a no-match, rewritten. */
    reset_world();
    strcpy(g_msg[3], INTEG_PARTS_NO_SPLIT_MSG);
    dispatch("int");
    run_ceciad();
    CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
          strcmp(resp.error, INTEG_NO_ANTIDERIV_MSG) == 0,
          "12: no reducible split becomes Cannot find symbolic antiderivative");

    /* 13. Any other refusal from the last strategy says what went wrong and
     * is kept: eval's stack backstop (ipc.c), a lost request, or a reason
     * added later (review round 1). */
    {
        static const char *const kept[] = {
            "Eval recursion limit", "No module request found",
            "Integral input was null", "Some future reason"
        };
        size_t k;
        for (k = 0; k < sizeof(kept) / sizeof(kept[0]); k++) {
            char msg[96];
            reset_world();
            strcpy(g_msg[3], kept[k]);
            dispatch("int");
            run_ceciad();
            snprintf(msg, sizeof(msg), "13: '%s' is kept", kept[k]);
            CHECK(final_answer(&resp, &state) && resp.ok == 0 &&
                  strcmp(resp.error, kept[k]) == 0 && handed_up(&state), msg);
        }
    }

    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
