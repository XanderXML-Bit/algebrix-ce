#include "split/ipc.h"

#include <fileioc.h>
#include <ti/vars.h>
#include <string.h>

#define CE_STATE_OLD "CEOLD"

/* OS lookup needs no handle or allocation. An unreadable predecessor
 * remains authoritative instead of exposing an uncommitted replacement. */
#define state_read_name() (os_GetAppVarData(CE_STATE_OLD, NULL) ? CE_STATE_OLD : CE_APPVAR_STATE)

static bool struct_appvar(const char *name, void *data, size_t size, bool writing)
{
    bool ok = false;
    uint8_t slot = ti_Open(name, writing ? "w" : "r");
    if (!slot) return false;
    if (writing) ok = ti_Write(data, size, 1, slot) == 1;
    else if (ti_GetSize(slot) >= size) ok = ti_Read(data, size, 1, slot) == 1;
    ti_Close(slot);
    return ok;
}

#define read_struct_appvar(n, p, s) struct_appvar(n, p, s, false)
#define write_struct_appvar(n, p, s) struct_appvar(n, (void *)(p), s, true)

void ce_state_defaults(ce_shared_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->magic = CE_IPC_MAGIC;
    state->version = CE_IPC_VERSION;
    state->settings.angle_mode = ANGLE_RADIANS;
    state->settings.show_steps = true;
    state->settings.precision = 6;
    state->settings.complex_mode = COMPLEX_OFF;
    state->settings.pmt_at_begin = false;
    state->settings.step_detail = STEP_ALL;
    state->settings.exact_mode = false;
    state->last_answer = 0;
    state->last_result[0] = '\0';
}

bool ce_state_load(ce_shared_state_t *state)
{
    if (!state) return false;
    if (!read_struct_appvar(state_read_name(), state, sizeof(*state)) ||
        state->magic != CE_IPC_MAGIC ||
        state->version != CE_IPC_VERSION) {
        ce_state_defaults(state);
        return false;
    }

    if (state->hist_count > CE_HISTORY_SIZE) state->hist_count = 0;
    if (state->hist_write >= CE_HISTORY_SIZE) state->hist_write = 0;
    state->pending_module[8] = '\0';
    state->last_expression[MAX_INPUT_LEN - 1] = '\0';
    state->last_result[63] = '\0';
    {
        int i;
        for (i = 0; i < CE_HISTORY_SIZE; i++) {
            state->history_results[i][63] = '\0';
        }
    }
    /* Only letters A..Z have slots: bits 24-25 (Y, Z) are the last. */
    state->user_vars.mask[3] &= 0x03u;
    return true;
}

bool ce_state_save(const ce_shared_state_t *state)
{
    /* CEdev v11.2 checks destination first: 0 moves the predecessor, 1
     * keeps an existing predecessor, and 2 means both names are absent.
     * Delete commits only after the complete replacement write. A failed
     * save leaves the predecessor authoritative, including on retry.
     * This is returned-error recovery, not reset/power-loss atomicity. */
    uint8_t moved = ti_Rename(CE_APPVAR_STATE, CE_STATE_OLD);
    if (moved <= 2 &&
        write_struct_appvar(CE_APPVAR_STATE, state, sizeof(*state)) &&
        (moved == 2 || ti_Delete(CE_STATE_OLD))) return true;
    ce_response_delete();
    return false;
}

bool ce_request_write(const ce_request_t *req)
{
    return write_struct_appvar(CE_APPVAR_REQ, req, sizeof(*req));
}

bool ce_request_read(ce_request_t *req)
{
    if (!req) return false;
    if (!read_struct_appvar(CE_APPVAR_REQ, req, sizeof(*req))) return false;
    if (req->magic != CE_IPC_MAGIC || req->version != CE_IPC_VERSION) return false;
    req->flags &= (CE_REQ_FLAG_HAD_EQUALS | CE_REQ_FLAG_HAS_AST);
    if (req->ast_size > CE_AST_WIRE_MAX) req->ast_size = 0;
    if (!(req->flags & CE_REQ_FLAG_HAS_AST)) req->ast_size = 0;
    req->caller[8] = '\0';
    req->return_to[8] = '\0';
    req->expression[MAX_INPUT_LEN - 1] = '\0';
    return true;
}

bool ce_response_write(const ce_response_t *resp)
{
    return write_struct_appvar(CE_APPVAR_RESP, resp, sizeof(*resp));
}

bool ce_response_read(ce_response_t *resp)
{
    if (!resp) return false;
    if (!read_struct_appvar(CE_APPVAR_RESP, resp, sizeof(*resp))) return false;
    if (resp->magic != CE_IPC_MAGIC || resp->version != CE_IPC_VERSION) return false;
    resp->result[MAX_RESULT_LEN - 1] = '\0';
    resp->error[127] = '\0';
    if (resp->step_count > MAX_STEPS) resp->step_count = MAX_STEPS;
    return true;
}

void ce_response_delete(void)
{
    ti_Delete(CE_APPVAR_RESP);
}

bool g_eval_refused = false;
ce_float_t g_eval_refused_ans = 0;

void ce_response_from_runtime(ce_response_t *resp, bool ok, bool show_steps,
                              const char *result, const char *error,
                              ce_float_t last_answer)
{
    int i;
    if (!resp) return;
    /* An evaluation in this run hit eval's depth cap or stack backstop. Its
     * caller may have treated the error as a value (a zero coefficient, a
     * divergent integral, a converged root), so no answer from this run is
     * trusted, and Ans goes back to what it was before the refusal. Only
     * callers that never make a value from an eval error clear the flag
     * (calcengine.h); a probe elsewhere whose error was handled correctly
     * also fails the request. */
    if (ok && g_eval_refused) {
        ok = false;
        show_steps = false;
        result = "";
        error = "Eval recursion limit";
        last_answer = g_eval_refused_ans;
        g_last_answer = g_eval_refused_ans;
    }
    memset(resp, 0, sizeof(*resp));
    resp->magic = CE_IPC_MAGIC;
    resp->version = CE_IPC_VERSION;
    resp->ok = ok ? 1 : 0;
    resp->show_steps = show_steps ? 1 : 0;
    resp->settings = g_settings;
    resp->last_answer = last_answer;

    if (result) {
        strncpy(resp->result, result, sizeof(resp->result) - 1);
    }
    if (error) {
        strncpy(resp->error, error, sizeof(resp->error) - 1);
    }

#ifdef CE_DISABLE_STEP_RECORDING
    resp->step_count = 0;
    for (i = 0; i < MAX_STEPS; i++) {
        resp->steps[i][0] = '\0';
    }
#else
    resp->step_count = (uint8_t)((steps_count() < MAX_STEPS) ? steps_count() : MAX_STEPS);
    for (i = 0; i < resp->step_count; i++) {
        const char *line = steps_get(i);
        strncpy(resp->steps[i], line ? line : "", MAX_STEP_LEN - 1);
        resp->steps[i][MAX_STEP_LEN - 1] = '\0';
    }
#endif
}

const char *ce_domain_program(ce_domain_t domain)
{
    switch (domain) {
    case CE_DOMAIN_CORE: return CE_PRGM_ALG;
    case CE_DOMAIN_ALG: return CE_PRGM_ALG;
    case CE_DOMAIN_ALG_EXPFAC: return CE_PRGM_ALG;
    case CE_DOMAIN_ALG_ADV: return CE_PRGM_ALG;
    case CE_DOMAIN_ALG_GEO: return CE_PRGM_ALG;
    case CE_DOMAIN_CALC: return CE_PRGM_CALC;
    case CE_DOMAIN_CALC_TANGENT: return CE_PRGM_CALC;
    case CE_DOMAIN_CALC_LIMIT: return CE_PRGM_LIMSER;
    case CE_DOMAIN_CALC_INT: return CE_PRGM_CALC;
    case CE_DOMAIN_CALC_POLAR: return CE_PRGM_CALC;
    case CE_DOMAIN_CALC_SERIES: return CE_PRGM_LIMSER;
    case CE_DOMAIN_CALC_SEQ: return CE_PRGM_LIMSER;
    case CE_DOMAIN_STAT: return CE_PRGM_STATPROB;
    case CE_DOMAIN_PROB: return CE_PRGM_STATPROB;
    case CE_DOMAIN_MATRIX: return CE_PRGM_MAT;
    case CE_DOMAIN_GRAPH: return CE_PRGM_GRAPH;
    case CE_DOMAIN_FIN: return CE_PRGM_FIN;
    case CE_DOMAIN_CMPLX: return CE_PRGM_LISTCMPLX;
    default: return CE_PRGM_ALG;
    }
}

bool ce_program_set_archived(const char *name, bool archived)
{
    uint8_t slot;
    int is_archived;

    if (!name || !name[0]) return false;

    slot = ti_OpenVar(name, "r", OS_TYPE_PRGM);
    if (!slot) {
        slot = ti_OpenVar(name, "r", OS_TYPE_PROT_PRGM);
        if (!slot) return false;
    }

    is_archived = ti_IsArchived(slot);
    if ((is_archived != 0) == archived) {
        ti_Close(slot);
        return true;
    }

    if (!ti_SetArchiveStatus(archived, slot)) {
        ti_Close(slot);
        return false;
    }

    ti_Close(slot);
    return true;
}

bool ce_program_ensure_ram(const char *name)
{
    return ce_program_set_archived(name, false);
}

bool ce_program_exists(const char *name)
{
    uint8_t slot;
    if (!name || !name[0]) return false;
    slot = ti_OpenVar(name, "r", OS_TYPE_PRGM);
    if (!slot) {
        slot = ti_OpenVar(name, "r", OS_TYPE_PROT_PRGM);
        if (!slot) return false;
    }
    ti_Close(slot);
    return true;
}

int ce_launch_program(const char *name)
{
    return os_RunPrgm(name, NULL, 0, NULL);
}

int ce_launch_program_reentry(const char *name, os_runprgm_callback_t cb)
{
    return os_RunPrgm(name, NULL, 0, cb);
}
