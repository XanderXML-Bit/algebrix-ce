/*
 * launcher_diag.c — Diagnostic copy of src/core/launcher/main.c.
 * Adds L01–L25 keypress-gated stage markers with CEDBG AppVar logging.
 *
 * Stage map (25 launcher stages, total=25):
 *   L01 entry            — very first executed line
 *   L02 globals init     — after mem_init()
 *   L03 screen clear     — after explicit os_ClrHome()
 *   L04 banner printed   — after diagnostic banner written to row 3
 *   L05 before ti_Open   — before ce_state_load() (ti_Open inside)
 *   L06 ti_Open ok       — after ce_state_load() returns
 *   L07 state loaded     — after g_settings / g_last_answer sync
 *   L08 init complete    — after os_SetRealVar block, before event checks
 *   L09 before reentry   — entering has_pending_response check
 *   L10 resp open        — before ce_response_read() in re-entry path
 *   L11 resp parsed      — after ce_response_read() succeeds
 *   L12 result printed   — after show_response() returns
 *   L13 loop start       — top of the main input loop
 *   L14 before GetInput  — immediately before prompt_expression()
 *   L15 after GetInput   — after prompt_expression() returns with input
 *   L16 tokenize start   — before tok_init / tok_tokenize
 *   L17 tokenize done    — after tok_tokenize() succeeds
 *   L18 parse start      — before parser_init / parser_parse
 *   L19 parse done       — after parser_parse() returns non-NULL root
 *   L20 domain classify  — before ce_classify_expression()
 *   L21 domain selected  — after route_program() returns (prints module name)
 *   L22 req open         — before ce_request_write()
 *   L23 req written      — after ce_request_write() succeeds
 *   L24 pre RunPrgm      — immediately before ce_launch_program()
 *   L25 post RunPrgm     — immediately after ce_launch_program() returns
 *                          (dead on real HW — only fires on desktop mock)
 *
 * DO NOT MERGE TO MAIN. Diagnostic use only.
 */

#include <tice.h>
#include <ti/getcsc.h>
#include <ti/screen.h>

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "ast.h"
#include "calcengine.h"
#include "core/protocol/appvar_protocol.h"
#include "memory.h"
#include "parser.h"
#include "split/astwire.h"
#include "split/classify.h"
#include "split/module_runtime.h"
#include "token.h"

#include "diag_helpers.h"

/* ------------------------------------------------------------------ */
/* Module program name constants (mirrors production launcher)         */
/* ------------------------------------------------------------------ */
#define CE_HOME_COLS 26
#define CE_HOME_ROWS 8

#define CE_PRGM_BASE   "CEBASE"
#define CE_PRGM_ALGEXP "CEALGF"
#define CE_PRGM_ALGSYM "CEALGSY"
#define CE_PRGM_ALGPDV "CEAPDV"
#define CE_PRGM_ALGGEO "CEALGEO"
#define CE_PRGM_CALCLM "CECLIM"
#define CE_PRGM_CALCIN "CECINT"
#define CE_PRGM_CALCSR "CECSER"
#define CE_PRGM_CALCSQ "CECSEQ"
#define CE_PRGM_CALCPL "CECPOL"

/* ------------------------------------------------------------------ */
/* Module-level globals (mirrors production)                           */
/* ------------------------------------------------------------------ */
static ce_shared_state_t g_shared;
static tokenizer_t g_tokenizer;
static parser_t g_parser;
static ce_request_t g_request;
static ce_response_t g_response;
static uint8_t g_home_rows;
static bool g_quit_requested;

/* Static workspace for the child-failure recovery path — same fix as main.c. */
static ce_request_t s_pending_req;

/* ------------------------------------------------------------------ */
/* Home-screen display helpers (unchanged from production)             */
/* ------------------------------------------------------------------ */

static void home_reset(void)
{
    os_ClrHome();
    g_home_rows = 0;
}

static bool home_wait_page(const char *prompt)
{
    sk_key_t key;

    if (prompt && prompt[0]) {
        os_PutStrFull(prompt);
        os_NewLine();
        g_home_rows++;
    }

    do {
        key = os_GetCSC();
    } while (key == 0);

    if (key == sk_Mode) {
        g_quit_requested = true;
        return false;
    }

    home_reset();
    return true;
}

static bool home_ensure_rows(uint8_t needed)
{
    if ((uint8_t)(g_home_rows + needed) < CE_HOME_ROWS) {
        return true;
    }
    return home_wait_page("Press any key...");
}

static bool home_put_wrapped_line(const char *line)
{
    size_t len = 0;

    if (!line) line = "";
    len = strlen(line);
    if (len == 0) {
        if (!home_ensure_rows(1)) return false;
        os_NewLine();
        g_home_rows++;
        return true;
    }

    while (len > 0) {
        char chunk[CE_HOME_COLS + 1];
        size_t take = len > CE_HOME_COLS ? CE_HOME_COLS : len;

        if (!home_ensure_rows(1)) return false;
        memcpy(chunk, line, take);
        chunk[take] = '\0';
        os_PutStrFull(chunk);
        os_NewLine();
        g_home_rows++;

        line += take;
        len -= take;
    }

    return true;
}

static bool home_put_multiline(const char *text)
{
    const char *line = text;
    const char *p = text;
    char buf[MAX_RESULT_LEN];

    if (!text || !text[0]) return true;

    while (*p) {
        if (*p == '\n') {
            size_t len = (size_t)(p - line);
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, line, len);
            buf[len] = '\0';
            if (!home_put_wrapped_line(buf)) return false;
            line = p + 1;
        }
        p++;
    }

    if (p != line) {
        size_t len = (size_t)(p - line);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (!home_put_wrapped_line(buf)) return false;
    }

    return true;
}

static void state_update_last_expression(const char *expr)
{
    if (!expr) expr = "";
    strncpy(g_shared.last_expression, expr, sizeof(g_shared.last_expression) - 1);
    g_shared.last_expression[sizeof(g_shared.last_expression) - 1] = '\0';
}

static void history_push(const char *expr)
{
    uint8_t last;

    if (!expr || !expr[0]) return;

    if (g_shared.hist_count > 0) {
        last = (uint8_t)((g_shared.hist_write + CE_HISTORY_SIZE - 1) % CE_HISTORY_SIZE);
        if (strcmp(g_shared.history[last], expr) == 0) return;
    }

    strncpy(g_shared.history[g_shared.hist_write], expr, MAX_INPUT_LEN - 1);
    g_shared.history[g_shared.hist_write][MAX_INPUT_LEN - 1] = '\0';
    g_shared.hist_write = (uint8_t)((g_shared.hist_write + 1) % CE_HISTORY_SIZE);
    if (g_shared.hist_count < CE_HISTORY_SIZE) g_shared.hist_count++;
}

static bool is_finance_stat_fn(stat_fn_t fn)
{
    switch (fn) {
    case STAT_FIN_PV:
    case STAT_FIN_FV:
    case STAT_FIN_PMT:
    case STAT_FIN_NPER:
    case STAT_FIN_RATE:
    case STAT_FIN_NOM:
    case STAT_FIN_EFF:
    case STAT_FIN_BAL:
    case STAT_FIN_SUMPRN:
    case STAT_FIN_SUMINT:
        return true;
    default:
        return false;
    }
}

static const char *route_polar_program(polar_op_t op)
{
    switch (op) {
    case POLOP_TO_RECT:
    case POLOP_FROM_RECT:
        return CE_PRGM_LISTCMPLX;

    case POLOP_CSQUARE:
    case POLOP_PFRAC:
    case POLOP_DESCARTES:
    case POLOP_NLSOLVE:
        return CE_PRGM_ALGSYM;

    case POLOP_POLYDIV:
        return CE_PRGM_ALGPDV;

    case POLOP_LINE2PT:
    case POLOP_LINEPT:
    case POLOP_PARABOLA:
    case POLOP_TOFRAC:
    case POLOP_UNITCIRC:
    case POLOP_MIDPOINT:
    case POLOP_DISTANCE:
    case POLOP_SLOPE:
        return CE_PRGM_ALGGEO;

    case POLOP_CONTINUITY:
    case POLOP_RELRATE:
        return CE_PRGM_CALCLM;

    case POLOP_RECSEQ:
    case POLOP_ARITHSUM:
    case POLOP_GEOMSUM:
        return CE_PRGM_CALCSQ;

    case POLOP_GRAPH:
    case POLOP_GRAPHEQ:
        return CE_PRGM_GRAPH;

    case POLOP_ODE_SEP:
    case POLOP_ODE_LINEAR:
    case POLOP_DIFFEQ:
    case POLOP_INTPARTS:
    case POLOP_TRIGSUB:
    case POLOP_USUB:
    case POLOP_INTSTRATEGY:
        return CE_PRGM_CALCIN;

    case POLOP_EULER:
    case POLOP_SLOPEFIELD:
    case POLOP_ARCLEN:
    case POLOP_ARCLEN_PARAM:
    case POLOP_SURFREV:
    case POLOP_DISK:
    case POLOP_WASHER:
    case POLOP_SHELL:
        return CE_PRGM_CALCPL;

    default:
        return CE_PRGM_BASE;
    }
}

static const char *route_program(const ast_node_t *root, bool input_had_equals,
                                  ce_domain_t domain)
{
    if (!root) return ce_domain_program(domain);

    switch (root->type) {
    case NODE_EXPAND:
    case NODE_FACTOR:
        return CE_PRGM_ALGEXP;

    case NODE_SOLVE:
        return CE_PRGM_ALG;

    case NODE_DERIV:
    case NODE_TANGENT:
        return CE_PRGM_CALC;

    case NODE_LIMIT:
        return CE_PRGM_CALCLM;

    case NODE_INTEGRAL:
        return CE_PRGM_CALCIN;

    case NODE_TAYLOR:
    case NODE_FOURIER:
        return CE_PRGM_CALCSR;

    case NODE_SUM:
        return CE_PRGM_CALCSQ;

    case NODE_STAT:
        return is_finance_stat_fn(root->d.stat.fn) ? CE_PRGM_FIN : CE_PRGM_STATPROB;

    case NODE_MATRIX_OP:
    case NODE_MATRIX:
        return CE_PRGM_MAT;

    case NODE_POLAR:
        return route_polar_program(root->d.polar.op);

    case NODE_LIST_LITERAL:
    case NODE_LIST_REF:
    case NODE_LIST_OP:
    case NODE_LIST_ELEM:
    case NODE_STORE:
        return CE_PRGM_LISTCMPLX;

    default:
        break;
    }

    if (input_had_equals) return CE_PRGM_ALG;

    switch (domain) {
    case CE_DOMAIN_CALC:
    case CE_DOMAIN_CALC_TANGENT:
        return CE_PRGM_CALC;
    case CE_DOMAIN_CALC_LIMIT:
        return CE_PRGM_CALCLM;
    case CE_DOMAIN_CALC_INT:
    case CE_DOMAIN_CALC_POLAR:
        return CE_PRGM_CALCIN;
    case CE_DOMAIN_CALC_SERIES:
        return CE_PRGM_CALCSR;
    case CE_DOMAIN_CALC_SEQ:
        return CE_PRGM_CALCSQ;
    case CE_DOMAIN_STAT:
    case CE_DOMAIN_PROB:
        return CE_PRGM_STATPROB;
    case CE_DOMAIN_MATRIX:
        return CE_PRGM_MAT;
    case CE_DOMAIN_FIN:
        return CE_PRGM_FIN;
    case CE_DOMAIN_GRAPH:
        return CE_PRGM_GRAPH;
    case CE_DOMAIN_CMPLX:
        return CE_PRGM_LISTCMPLX;
    default:
        return CE_PRGM_BASE;
    }
}

/* ------------------------------------------------------------------ */
/* parse_for_domain — with L16/L17/L18/L19/L20 stage markers          */
/* ------------------------------------------------------------------ */
static bool parse_for_domain(const char *input,
                              ast_node_t **out_root,
                              bool *out_had_equals,
                              ce_domain_t *out_domain)
{
    ast_node_t *root = NULL;
    const char *p;

    if (!input || !input[0] || !out_root || !out_had_equals || !out_domain) return false;

    *out_root = NULL;
    *out_had_equals = false;
    *out_domain = CE_DOMAIN_UNKNOWN;

    for (p = input; *p; p++) {
        if (*p == '=') {
            *out_had_equals = true;
            break;
        }
    }

    mem_free_all();

    /* L16: tokenize start */
    dbg_stage("L16 tokenize start", 16, 25);
    tok_init(&g_tokenizer, input);
    if (tok_tokenize(&g_tokenizer) != CE_OK) return false;
    /* L17: tokenize done */
    dbg_stage("L17 tokenize done", 17, 25);

    /* L18: parse start */
    dbg_stage("L18 parse start", 18, 25);
    parser_init(&g_parser, &g_tokenizer);
    root = parser_parse(&g_parser);
    if (!root) return false;
    /* L19: parse done */
    dbg_stage("L19 parse done", 19, 25);

    /* L20: domain classify */
    dbg_stage("L20 domain classify", 20, 25);
    *out_domain = ce_classify_expression(root, *out_had_equals);
    *out_root = root;
    return true;
}

/* ------------------------------------------------------------------ */
/* show_response / show_runtime_error (unchanged from production)      */
/* ------------------------------------------------------------------ */
static void show_response(const ce_response_t *resp)
{
    uint8_t i;

    home_reset();
    if (!home_put_wrapped_line("CalcEngine-DIAG")) return;
    if (!home_put_wrapped_line(resp->ok ? "OK" : "ERROR")) return;

    if (resp->ok) {
        if (!home_put_multiline(resp->result)) return;
    } else {
        if (!home_put_multiline(resp->error)) return;
    }

    if (resp->show_steps && resp->step_count > 0) {
        if (!home_put_wrapped_line("")) return;
        if (!home_put_wrapped_line("Steps:")) return;
        for (i = 0; i < resp->step_count; i++) {
            if (!home_put_multiline(resp->steps[i])) return;
        }
    }

    if (!g_quit_requested) {
        (void)home_wait_page("Any key=continue");
    }
}

static void show_runtime_error(const char *msg)
{
    memset(&g_response, 0, sizeof(g_response));
    g_response.ok = 0;
    strncpy(g_response.error, msg ? msg : "Unknown error", sizeof(g_response.error) - 1);
    show_response(&g_response);
}

static void clear_failed_dispatch(void)
{
    g_shared.settings = g_settings;
    g_shared.last_answer = g_last_answer;
    g_shared.has_pending_response = 0;
    g_shared.pending_module[0] = '\0';
    ce_state_save(&g_shared);
}

/* ------------------------------------------------------------------ */
/* dispatch_to_program — with L22..L25 stage markers                  */
/* ------------------------------------------------------------------ */
static bool dispatch_to_program(const char *program,
                                 ce_domain_t domain,
                                 const char *expression,
                                 const ast_node_t *root,
                                 bool had_equals)
{
    ce_request_t *req = &g_request;
    ce_response_t *resp = &g_response;
    int run_ret;

    memset(req, 0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));

    req->magic = CE_IPC_MAGIC;
    req->version = CE_IPC_VERSION;
    req->domain = (uint8_t)domain;
    req->flags = had_equals ? CE_REQ_FLAG_HAD_EQUALS : 0;
    req->settings = g_settings;
    req->last_answer = g_last_answer;
    strncpy(req->caller, CE_PRGM_CORE, sizeof(req->caller) - 1);
    strncpy(req->return_to, CE_PRGM_CORE, sizeof(req->return_to) - 1);
    strncpy(req->expression, expression, sizeof(req->expression) - 1);
    if (root && ce_ast_serialize(root, req->ast_payload, sizeof(req->ast_payload),
                                 &req->ast_size)) {
        req->flags |= CE_REQ_FLAG_HAS_AST;
    }

    g_shared.settings = g_settings;
    g_shared.last_answer = g_last_answer;
    g_shared.has_pending_response = 0;
    strncpy(g_shared.pending_module, program, sizeof(g_shared.pending_module) - 1);
    history_push(expression);
    state_update_last_expression(expression);

    ce_response_delete();

    /* L22: req open (about to write request AppVar) */
    dbg_stage("L22 req open", 22, 25);
    if (!ce_request_write(req)) {
        clear_failed_dispatch();
        show_runtime_error("IPC request write failed");
        return false;
    }
    /* L23: req written */
    dbg_stage("L23 req written", 23, 25);

    if (!ce_state_save(&g_shared)) {
        clear_failed_dispatch();
        show_runtime_error("IPC state save failed");
        return false;
    }
    /* (state saved — req closed internally) */

    if (!ce_program_exists(program)) {
        clear_failed_dispatch();
        show_runtime_error("Module missing");
        return false;
    }

    /* L24: pre RunPrgm — last stage before os_RunPrgm fires.
     * On real hardware this is the last stage CALCENG executes until
     * the worker calls os_RunPrgm("CALCENG",...) and main() re-enters. */
    dbg_stage("L24 pre RunPrgm", 24, 25);

    run_ret = ce_launch_program(program);

    /* L25: post RunPrgm — only reachable on desktop mock (synchronous path).
     * On real hardware, os_RunPrgm destroys CALCENG; this line never runs. */
    dbg_stage("L25 post RunPrgm", 25, 25);

    /* --- Desktop/mock synchronous fallback (dead code on real CE) --- */
    if (ce_response_read(resp)) {
        g_settings = resp->settings;
        g_last_answer = resp->last_answer;
        show_response(resp);
        ce_response_delete();
    } else if (run_ret == 0) {
        show_runtime_error("No module response");
    }

    ce_state_load(&g_shared);
    g_settings = g_shared.settings;
    g_last_answer = g_shared.last_answer;
    g_shared.has_pending_response = 0;
    if (g_shared.pending_module[0]) {
        ce_program_set_archived(g_shared.pending_module, true);
        g_shared.pending_module[0] = '\0';
    }
    ce_state_save(&g_shared);

    if (run_ret != 0) {
        show_runtime_error("Program launch failed");
        return false;
    }

    return !g_quit_requested;
}

/* ------------------------------------------------------------------ */
/* prompt_expression — with L14/L15 stage markers                     */
/* ------------------------------------------------------------------ */
static bool prompt_expression(char *input, size_t input_size)
{
    /* L14: before GetInput */
    dbg_stage("L14 before GetInput", 14, 25);

    do {
        home_reset();
        if (!home_put_wrapped_line("CalcEngine-DIAG")) return false;
        if (!home_put_wrapped_line("Expr (MODE exits):")) return false;
        if (g_shared.last_expression[0]) {
            if (!home_put_wrapped_line("Last:")) return false;
            if (!home_put_multiline(g_shared.last_expression)) return false;
        }
        os_GetStringInput("> ", input, (uint8_t)input_size);
        /* Normalize TI-OS token bytes to ASCII (mirrors production fix) */
        {
            uint8_t *p = (uint8_t *)input;
            while (*p) {
                if (*p == 0x83u) *p = '/';
                p++;
            }
        }
    } while (!input[0]);

    /* L15: after GetInput */
    dbg_stage("L15 after GetInput", 15, 25);
    return true;
}

/* ================================================================== */
/* main()                                                               */
/* ================================================================== */
int main(void)
{
    char input[MAX_INPUT_LEN];
    bool running = true;

    /* ---- L01: entry — very first line before any code ---- */
    /* Wipe CEDBG for this session (CALCENG entry = new trace) */
    dbg_appvar_init();
    g_dbg_skip = 0;  /* fresh skip state each CALCENG load */
    dbg_stage("L01 entry", 1, 25);

    /* ---- L02: globals init ---- */
    mem_init();
    g_quit_requested = false;
    dbg_stage("L02 globals init", 2, 25);

    /* ---- L03: screen clear (explicit for diagnostic) ---- */
    os_ClrHome();
    g_home_rows = 0;
    dbg_stage("L03 screen clear", 3, 25);

    /* ---- L04: banner printed ---- */
    /* Print a persistent diagnostic badge on row 5 so it's visible
     * below the stage-gate rows (0-2) during all stepping. */
    os_SetCursorPos(5, 0);
    os_PutStrFull("** CALCENG-DIAG **        ");
    dbg_stage("L04 banner printed", 4, 25);

    /* ---- L05: before ti_Open (ce_state_load uses ti_Open internally) ---- */
    dbg_stage("L05 before ti_Open", 5, 25);
    ce_state_load(&g_shared);
    /* ---- L06: ti_Open ok (state loaded from AppVar) ---- */
    dbg_stage("L06 ti_Open ok", 6, 25);

    g_settings = g_shared.settings;
    g_last_answer = g_shared.last_answer;
    /* ---- L07: state loaded (globals synced) ---- */
    dbg_stage("L07 state loaded", 7, 25);

    /* Pre-set TI-OS real variables to prevent 0/0 in os_GetStringInput */
    {
        real_t one = os_FloatToReal(1.0f);
        os_SetRealVar("D", &one);
        os_SetRealVar("X", &one);
        os_SetRealVar("L", &one);
        os_SetRealVar("I", &one);
        os_SetRealVar("M", &one);
    }
    /* ---- L08: init complete (os_SetRealVar done, before event checks) ---- */
    dbg_stage("L08 init complete", 8, 25);

    /* Recover child failure if any — use s_pending_req (static) instead of
     * a stack local to avoid the 4273+4510-byte overflow on iter-2 re-entry. */
    {
        char pending_err[128];
        memset(&s_pending_req, 0, sizeof(s_pending_req));
        if (ce_module_runtime_recover_child_failure(CE_PRGM_CORE, "",
                                                    &s_pending_req, &g_shared,
                                                    pending_err, sizeof(pending_err))) {
            show_runtime_error(pending_err);
        }
    }

    /* ---- L09: before re-entry check ---- */
    dbg_stage("L09 before reentry", 9, 25);

    /* Re-entry path: worker set has_pending_response=1 via os_RunPrgm.
     * Reuse g_response (already static) — no stack-local ce_response_t. */
    if (g_shared.has_pending_response) {
        memset(&g_response, 0, sizeof(g_response));

        /* ---- L10: resp open (about to read response AppVar) ---- */
        dbg_stage("L10 resp open", 10, 25);

        if (ce_response_read(&g_response)) {
            /* ---- L11: resp parsed (response deserialized OK) ---- */
            dbg_stage("L11 resp parsed", 11, 25);

            g_settings = g_response.settings;
            g_last_answer = g_response.last_answer;
            show_response(&g_response);
            ce_response_delete();

            /* ---- L12: result printed (show_response returned) ---- */
            dbg_stage("L12 result printed", 12, 25);
        }

        g_shared.has_pending_response = 0;
        g_shared.settings = g_settings;
        g_shared.last_answer = g_last_answer;
        if (g_shared.pending_module[0]) {
            ce_program_set_archived(g_shared.pending_module, true);
            g_shared.pending_module[0] = '\0';
        }
        ce_state_save(&g_shared);
    }

    /* ---- L13: loop start (entering main expression loop) ---- */
    dbg_stage("L13 loop start", 13, 25);

    while (running && !g_quit_requested) {
        ast_node_t *root = NULL;
        ce_domain_t domain = CE_DOMAIN_UNKNOWN;
        bool had_equals = false;
        const char *program;
        char l21buf[21];

        memset(input, 0, sizeof(input));

        /* prompt_expression fires L14 and L15 internally */
        if (!prompt_expression(input, sizeof(input))) break;

        /* parse_for_domain fires L16-L20 internally */
        if (!parse_for_domain(input, &root, &had_equals, &domain)) {
            show_runtime_error("Could not parse input");
            continue;
        }

        program = route_program(root, had_equals, domain);

        /* ---- L21: domain selected (show chosen module) ---- */
        snprintf(l21buf, sizeof(l21buf), "L21->%.13s", program);
        dbg_stage(l21buf, 21, 25);

        /* dispatch_to_program fires L22-L25 internally */
        running = dispatch_to_program(program, domain, input, root, had_equals);
        ast_free(root);
    }

    home_reset();
    return 0;
}
