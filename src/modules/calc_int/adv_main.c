#include <stdio.h>
#include <string.h>

#include "integrate.h"
#include "split/astwire.h"
#include "split/module_runtime.h"

#define CE_PRGM_INT_PFRAC  "CEIPFR"
#define CE_PRGM_INT_TRIG   "CEITRG"
#define CE_PRGM_INT_USUB   "CEIUSB"
#define CE_PRGM_INT_PARTS  "CEIPRT"
#define CE_PRGM_INT_REPORT "CEISTR"
#define CE_SELF_PROGRAM    "CECIAD"

static const char *const k_programs[] = {
    CE_PRGM_INT_PFRAC,
    CE_PRGM_INT_TRIG,
    CE_PRGM_INT_USUB,
    CE_PRGM_INT_PARTS
};
#define CE_SEQUENCE_LEN (sizeof(k_programs) / sizeof(k_programs[0]))

/* True when the request takes the try-each-strategy path (not a direct
 * trigsub/usub/intparts/intstrategy call). */
static bool ce_is_sequence_request(const ast_node_t *root)
{
    if (!root || root->type != NODE_POLAR) return true;
    switch (root->d.polar.op) {
    case POLOP_INTSTRATEGY:
    case POLOP_TRIGSUB:
    case POLOP_USUB:
    case POLOP_INTPARTS:
        return false;
    default:
        return true;
    }
}

/* The answer when every strategy refused a try-each-strategy request.
 *
 * Each strategy worker refuses with the reason its own rule failed, and the
 * last one tried is integration by parts. Handing that reason up told the
 * user "Integration by parts requires a product-like integrand" for
 * int(int(x)), int(sum(...),n) or sin(x^2), which says nothing true about
 * the input (internal review). When the last worker answers one of its two
 * by-parts no-match reasons, all four have refused, no rule found an
 * antiderivative, and the answer becomes what the desktop processor prints
 * when integrate() fails: INTEG_NO_ANTIDERIV_MSG.
 *
 * Every other answer is handed up as it is, because it says what actually
 * went wrong: a match, a resource refusal (too deep, out of memory, eval
 * recursion limit), an IPC or request failure. Only those two reasons are
 * rewritten, so a reason added later passes through unchanged. Only the
 * error text changes: Ans, settings and the rest of the child's response
 * stay as the child wrote them. Returns true when it rewrote. */
static bool ce_sequence_refusal(ce_response_t *resp)
{
    if (resp->ok ||
        (strcmp(resp->error, INTEG_PARTS_NOT_PRODUCT_MSG) != 0 &&
         strcmp(resp->error, INTEG_PARTS_NO_SPLIT_MSG) != 0)) {
        return false;
    }
    memset(resp->error, 0, sizeof(resp->error));
    strcpy(resp->error, INTEG_NO_ANTIDERIV_MSG);
    return true;
}

static bool ce_forward_integral_sequence(ce_request_t *req,
                                         ce_shared_state_t *state,
                                         ce_response_t *resp,
                                         char *err,
                                         size_t err_size)
{
    size_t i;

    for (i = 0; i < CE_SEQUENCE_LEN; i++) {
        if (!ce_module_runtime_forward(CE_SELF_PROGRAM, k_programs[i],
                                       req, state, resp,
                                       err, err_size)) {
            return false;
        }
        if (resp->ok) {
            return true;
        }
    }

    ce_sequence_refusal(resp);
    return true;
}

static bool ce_forward_integral_request(const ast_node_t *root,
                                        ce_request_t *req,
                                        ce_shared_state_t *state,
                                        ce_response_t *resp,
                                        char *err,
                                        size_t err_size)
{
    if (!root) {
        return ce_forward_integral_sequence(req, state, resp, err, err_size);
    }

    if (root->type == NODE_POLAR) {
        switch (root->d.polar.op) {
        case POLOP_INTSTRATEGY:
            return ce_module_runtime_forward("CECIAD", CE_PRGM_INT_REPORT,
                                             req, state, resp,
                                             err, err_size);
        case POLOP_TRIGSUB:
            return ce_module_runtime_forward("CECIAD", CE_PRGM_INT_TRIG,
                                             req, state, resp,
                                             err, err_size);
        case POLOP_USUB:
            return ce_module_runtime_forward("CECIAD", CE_PRGM_INT_USUB,
                                             req, state, resp,
                                             err, err_size);
        case POLOP_INTPARTS:
            return ce_module_runtime_forward("CECIAD", CE_PRGM_INT_PARTS,
                                             req, state, resp,
                                             err, err_size);
        default:
            break;
        }
    }

    return ce_forward_integral_sequence(req, state, resp, err, err_size);
}

/* Each program launch enters main once. Keep IPC storage off the OS stack. */
static ce_request_t req;
static ce_response_t resp;
static ce_shared_state_t state;

static char err[128];

#ifdef CE_IADV_HARNESS
/* Desktop harness hook (tests/harness/ceciad_reentry_harness.c): a CE
 * program reload zeroes every static before the callback re-enters main(). */
void ce_iadv_harness_reload(void)
{
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    memset(&state, 0, sizeof(state));
    memset(err, 0, sizeof(err));
}
#endif

int main(void)
{
    ast_node_t *root = NULL;
    bool had_equals = false;

    /* Hardware resume after a strategy child returned. os_RunPrgm never
     * returns on the CE, so the in-process loop in
     * ce_forward_integral_sequence() only reaches its first child there; the
     * next strategy is chosen here from the child that just answered.
     * Assumes the four strategy children never forward themselves, so
     * CE_REQ still holds this module's request on resume (guarded by
     * tests/cemu/test_module_handoff_contract.py). */
    {
        char child[9];
        if (ce_module_runtime_resume_peek(CE_SELF_PROGRAM, &req, &state, &resp,
                                          child, sizeof(child))) {
            /* CE_SEQUENCE_LEN + 1: hand the child's answer up unchanged. */
            size_t next = CE_SEQUENCE_LEN + 1;
            if (!resp.ok) {
                size_t i;
                for (i = 0; i < CE_SEQUENCE_LEN; i++) {
                    if (strcmp(child, k_programs[i]) == 0) {
                        next = i + 1;
                        break;
                    }
                }
                if (next <= CE_SEQUENCE_LEN) {
                    if ((req.flags & CE_REQ_FLAG_HAS_AST) && req.ast_size > 0) {
                        root = ce_ast_deserialize(req.ast_payload, req.ast_size);
                    }
                    if (!ce_is_sequence_request(root)) {
                        next = CE_SEQUENCE_LEN + 1;
                    }
                    ast_free(root);
                    root = NULL;
                }
            }
            if (next == CE_SEQUENCE_LEN && ce_sequence_refusal(&resp)) {
                /* The last strategy refused a try-each-strategy request:
                 * every strategy has now refused (internal review). Write
                 * the rewritten answer, then hand it up exactly as below.
                 * If the write fails, resume_forward cannot read CE_RESP
                 * and the child-failure path below reports it. */
                (void)ce_response_write(&resp);
            }
            if (next < CE_SEQUENCE_LEN) {
                ce_module_runtime_reclaim_forward(CE_SELF_PROGRAM, &state);
                for (; next < CE_SEQUENCE_LEN; next++) {
                    if (!ce_module_runtime_forward(CE_SELF_PROGRAM, k_programs[next],
                                                   &req, &state, &resp,
                                                   err, sizeof(err))) {
                        ce_response_from_runtime(&resp, false, false, "", err,
                                                 g_last_answer);
                        break;
                    }
                    if (resp.ok) break;
                }
                /* Reached only when a launch returned (a failed launch, or
                 * the desktop mock): all strategies after the child ran. */
                if (next == CE_SEQUENCE_LEN) ce_sequence_refusal(&resp);
                ce_module_runtime_finish(&req, &resp, &state);
                return 0;
            }
        }
    }
    if (ce_module_runtime_resume_forward(CE_SELF_PROGRAM, &req, &state)) {
        return 0;
    }
    if (ce_module_runtime_recover_child_failure(CE_SELF_PROGRAM, CE_SELF_PROGRAM,
                                                &req, &state, err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
        ce_module_runtime_finish(&req, &resp, &state);
        return 0;
    }

    if (!ce_module_runtime_load(&req, &resp, &state, &root, &had_equals)) {
        ce_module_runtime_finish(&req, &resp, &state);
        return 1;
    }

    (void)had_equals;
    if (!ce_forward_integral_request(root, &req, &state, &resp,
                                     err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
    }

    ast_free(root);
    ce_module_runtime_finish(&req, &resp, &state);
    return 0;
}
