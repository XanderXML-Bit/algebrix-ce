#ifndef SPLIT_IPC_H
#define SPLIT_IPC_H

#include "calcengine.h"
#include "steps.h"

#define CE_IPC_MAGIC 0xCE84u
/* Version 6 adds user_vars to CE_STATE to persist values for A..Z.
 * Every module and the launcher must come from the same build. */
#define CE_IPC_VERSION 6u

#define CE_APPVAR_REQ   "CE_REQ"
#define CE_APPVAR_RESP  "CE_RESP"
#define CE_APPVAR_STATE "CE_STATE"

#define CE_PRGM_CORE    "CALCENG"
#define CE_PRGM_ALG     "CEALG"
#define CE_PRGM_CALC    "CECALC"
#define CE_PRGM_LIMSER  "CELIMS"
#define CE_PRGM_MAT     "CEMAT"
#define CE_PRGM_STATPROB "CESTPRB"
#define CE_PRGM_GRAPH   "CEGRPH"
#define CE_PRGM_FIN     "CEFIN"
#define CE_PRGM_LISTCMPLX "CELSCX"

/* Legacy aliases retained for old split target names. */
#define CE_PRGM_BASIC      CE_PRGM_ALG
#define CE_PRGM_ALG_EXPFAC CE_PRGM_ALG
#define CE_PRGM_ALG_ADV    CE_PRGM_ALG
#define CE_PRGM_ALG_GEO    CE_PRGM_ALG
#define CE_PRGM_CALC_TAN   CE_PRGM_CALC
#define CE_PRGM_CALC_LIM   CE_PRGM_LIMSER
#define CE_PRGM_CALC_INT   CE_PRGM_CALC
#define CE_PRGM_CALC_POL   CE_PRGM_CALC
#define CE_PRGM_CALC_SER   CE_PRGM_LIMSER
#define CE_PRGM_CALC_SEQ   CE_PRGM_LIMSER
#define CE_PRGM_STAT       CE_PRGM_STATPROB
#define CE_PRGM_PROB       CE_PRGM_STATPROB
#define CE_PRGM_CMPLX      CE_PRGM_LISTCMPLX

#define CE_HISTORY_SIZE 16
#define CE_AST_WIRE_MAX 4096

#define CE_REQ_FLAG_HAD_EQUALS 0x01u
#define CE_REQ_FLAG_HAS_AST    0x02u

typedef enum {
    CE_DOMAIN_CORE = 0,
    CE_DOMAIN_ALG,
    CE_DOMAIN_ALG_EXPFAC,
    CE_DOMAIN_ALG_ADV,
    CE_DOMAIN_ALG_GEO,
    CE_DOMAIN_CALC,
    CE_DOMAIN_CALC_TANGENT,
    CE_DOMAIN_CALC_LIMIT,
    CE_DOMAIN_CALC_INT,
    CE_DOMAIN_CALC_POLAR,
    CE_DOMAIN_CALC_SERIES,
    CE_DOMAIN_CALC_SEQ,
    CE_DOMAIN_STAT,
    CE_DOMAIN_PROB,
    CE_DOMAIN_MATRIX,
    CE_DOMAIN_GRAPH,
    CE_DOMAIN_FIN,
    CE_DOMAIN_CMPLX,
    CE_DOMAIN_UNKNOWN
} ce_domain_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint8_t version;
    uint8_t domain;
    uint8_t flags;
    uint16_t ast_size;
    settings_t settings;
    ce_float_t last_answer;
    char caller[9];
    char return_to[9];
    char expression[MAX_INPUT_LEN];
    uint8_t ast_payload[CE_AST_WIRE_MAX];
} ce_request_t;

typedef struct {
    uint16_t magic;
    uint8_t version;
    uint8_t ok;
    uint8_t show_steps;
    settings_t settings;
    ce_float_t last_answer;
    char result[MAX_RESULT_LEN];
    char error[128];
    uint8_t step_count;
    char steps[MAX_STEPS][MAX_STEP_LEN];
} ce_response_t;

typedef struct {
    uint16_t magic;
    uint8_t version;
    uint8_t has_pending_response;
    settings_t settings;
    ce_float_t last_answer;
    char pending_module[9];
    char last_expression[MAX_INPUT_LEN];
    uint8_t hist_count;
    uint8_t hist_write;
    char history[CE_HISTORY_SIZE][MAX_INPUT_LEN];
    /* Phase 1: last result string from the most recent response.
     * Persisted in CE_STATE so it survives the worker re-launch cycle
     * (CALCENG is destroyed by os_RunPrgm and reloaded fresh from archive). */
    char last_result[64];
    /* Phase 2: per-entry result strings, indexed in parallel with history[].
     * history_results[i] holds the result string for history[i].
     * Allows every history entry to show its own result on the home screen,
     * not just the most-recent entry. */
    char history_results[CE_HISTORY_SIZE][64];
    /* Values stored with `expr->X` (see calcengine.h). Modules
     * built with CE_EVAL_USER_VARS=1 (CEBASE, CELSCX) read and write it;
     * every other module and the launcher save it back unchanged.  The
     * launcher's MODE menu can clear it. */
    ce_user_vars_t user_vars;
} ce_shared_state_t;
#pragma pack(pop)

void ce_state_defaults(ce_shared_state_t *state);
bool ce_state_load(ce_shared_state_t *state);
bool ce_state_save(const ce_shared_state_t *state);

bool ce_request_write(const ce_request_t *req);
bool ce_request_read(ce_request_t *req);

bool ce_response_write(const ce_response_t *resp);
bool ce_response_read(ce_response_t *resp);
void ce_response_delete(void);

void ce_response_from_runtime(ce_response_t *resp, bool ok, bool show_steps,
                              const char *result, const char *error,
                              ce_float_t last_answer);

const char *ce_domain_program(ce_domain_t domain);

bool ce_program_set_archived(const char *name, bool archived);
bool ce_program_ensure_ram(const char *name);
/* Check whether a worker program exists (in RAM or archive).
 * Returns true if found; false if absent.  Does NOT unarchive.
 * Use this before os_RunPrgm() to surface a friendly error instead of
 * letting TI-OS crash on a missing program. */
bool ce_program_exists(const char *name);

/* Launch another program. On real hardware this does NOT return — the current
   program is destroyed by the OS. Use ce_launch_program_reentry when the
   caller needs control back after the child exits. */
int ce_launch_program(const char *name);

/* Launch another program with a re-entry callback. On real hardware, when the
   child program exits, the OS reloads this program and invokes the callback
   instead of main(). The callback should re-enter the main loop or re-start
   main(). On desktop (mock) this returns immediately like ce_launch_program.
   @param name   Program name to launch.
   @param cb     Re-entry callback. */
#include <ti/vars.h>
int ce_launch_program_reentry(const char *name, os_runprgm_callback_t cb);

#endif /* SPLIT_IPC_H */
