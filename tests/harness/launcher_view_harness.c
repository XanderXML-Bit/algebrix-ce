/*
 * launcher_view_harness.c — runs the real CALCENG launcher main()
 * (src/core/launcher/main.c) on the desktop against a recorded 10x26 home
 * screen and a scripted keypad, to check the full-text result viewer
 * and the kept input plus "Syntax error" message after a parse error.
 *
 * The os_RunPrgm mock plays the worker the way the TI-84 Plus CE does: it
 * writes CE_RESP and CE_STATE (ce_module_runtime_finish), then calls the
 * launcher's callback, which re-runs main(); main() reads the pending
 * response.  On the CE, os_RunPrgm never returns, so when the re-entered
 * main() returns the harness longjmp()s back to its own top level.  One
 * scenario instead returns 0 without calling back, which exercises the
 * desktop synchronous path in dispatch_to_program().
 *
 * The simulated reload does not zero the launcher's statics; dispatch
 * clears the viewer state before every launch and main() sets it again
 * from the pending response, which is what the CE path does after a reload.
 *
 * Built and run by tests/cemu/test_launcher_result_viewer.py.  Exit status 0
 * means every check passed.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fileioc.h>
#include <ti/getcsc.h>
#include <ti/screen.h>
#include <ti/vars.h>

#include "calcengine.h"
#include "split/ipc.h"
#include "split/module_runtime.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "core/launcher/result_view.h"

int launcher_main(void);   /* src/core/launcher/main.c main(), renamed */


/* ── Recorded home screen ──────────────────────────────────────── */
#define SCR_ROWS 10
#define SCR_COLS 26
static char g_scr[SCR_ROWS][SCR_COLS + 1];
static uint8_t g_row, g_col;
static int g_scrolls;
/* os_PutStrFull on OS 5.8 printed one character and stopped on the last
 * row (seen on the real ROM); nothing may use it there. */
static int g_full_on_last_row;
/* On OS 5.8 an os_PutStrFull that wraps from row 8 onto row 9 scrolled the
 * whole home screen up one row (real ROM); flag every write that
 * wraps onto the last row. */
static int g_full_wrap_to_last_row;

static void scr_clear(void)
{
    int r;
    for (r = 0; r < SCR_ROWS; r++) {
        memset(g_scr[r], ' ', SCR_COLS);
        g_scr[r][SCR_COLS] = '\0';
    }
    g_row = g_col = 0;
}

void os_ClrHome(void) { scr_clear(); }

void os_SetCursorPos(uint8_t r, uint8_t c)
{
    g_row = r < SCR_ROWS ? r : SCR_ROWS - 1;
    g_col = c < SCR_COLS ? c : SCR_COLS - 1;
}

/* Like the OS: wrap at column 26 and scroll when the cursor leaves row 9. */
uint24_t os_PutStrFull(const char *s)
{
    uint24_t n = 0;
    if (g_row == SCR_ROWS - 1 && *s) g_full_on_last_row++;
    for (; *s; s++, n++) {
        g_scr[g_row][g_col] = *s;
        if (++g_col >= SCR_COLS) {
            g_col = 0;
            if (g_row + 1 == SCR_ROWS - 1) g_full_wrap_to_last_row++;
            if (++g_row >= SCR_ROWS) {
                memmove(g_scr[0], g_scr[1], (SCR_ROWS - 1) * (SCR_COLS + 1));
                memset(g_scr[SCR_ROWS - 1], ' ', SCR_COLS);
                g_row = SCR_ROWS - 1;
                g_scrolls++;
            }
        }
    }
    return n;
}

/* Like the OS: write to the end of the current row only, never wrap or
 * scroll.  Returns 1 if the whole string fit on the row. */
uint24_t os_PutStrLine(const char *s)
{
    for (; *s; s++) {
        if (g_col >= SCR_COLS) return 0;
        g_scr[g_row][g_col++] = *s;
    }
    if (g_col >= SCR_COLS) g_col = SCR_COLS - 1;
    return 1;
}

/* Row text without trailing spaces. */
static const char *row_text(int r)
{
    static char buf[SCR_COLS + 1];
    int n = SCR_COLS;
    memcpy(buf, g_scr[r], SCR_COLS);
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    return buf;
}

/* ── Scripted keypad ───────────────────────────────────────────── */
#define KEY_SNAP 0xFEu   /* not a scan code: record the screen here */
#define MAX_SNAPS 16
static const uint8_t *g_keys;
static int g_key_pos, g_key_len;
static bool g_release;     /* return 0 once between keys (debounce)   */
static int g_quit_stage;
static char g_snaps[MAX_SNAPS][SCR_ROWS][SCR_COLS + 1];
static int g_snap_count;

static void snap(void)
{
    int r;
    if (g_snap_count >= MAX_SNAPS) return;
    for (r = 0; r < SCR_ROWS; r++) strcpy(g_snaps[g_snap_count][r], row_text(r));
    g_snap_count++;
}

#define KEY_FAIL_STATE_WRITE 253
#define KEY_FAIL_STATE_AND_READ 252
#define KEY_CLEAR_FAULTS 251

uint8_t os_GetCSC(void)
{
    static long polls;
    if (++polls > 1000000L) {   /* a scan loop that never ends */
        printf("FAIL: launcher stuck polling keys (key %d/%d)\n", g_key_pos, g_key_len);
        exit(2);
    }
    if (g_release) { g_release = false; return 0; }
    while (g_key_pos < g_key_len &&
           (g_keys[g_key_pos] >= KEY_CLEAR_FAULTS && g_keys[g_key_pos] <= KEY_SNAP)) {
        if (g_keys[g_key_pos] == KEY_SNAP) snap();
        else if (g_keys[g_key_pos] == KEY_CLEAR_FAULTS) fileioc_mock_clear_faults();
        else {
            fileioc_mock_fail(MOCK_WRITE, CE_APPVAR_STATE, 0, 1, 0);
            if (g_keys[g_key_pos] == KEY_FAIL_STATE_AND_READ)
                fileioc_mock_fail(MOCK_READ, "CEOLD", 0, 1, 0);
        }
        g_key_pos++;
    }
    g_release = true;
    if (g_key_pos < g_key_len) return g_keys[g_key_pos++];
    /* Script done: 2nd+MODE quits.  In the viewer or the MODE menu, 2nd is
     * ignored and MODE leaves, so the next pair quits.  (Not CLEAR: on an
     * empty input line it wipes the history a relaunch scenario needs.) */
    return (g_quit_stage++ % 2) == 0 ? sk_2nd : sk_Mode;
}

/* tice stand-in (tests/mock/tice_mock.c is not linked: it has its own
 * os_RunPrgm; src/split/runtime_globals.c supplies the cancel hooks). */
void delay(unsigned int ms) { (void)ms; }

/* ── Worker mock ───────────────────────────────────────────────── */
typedef enum {
    RUN_CE = 0,       /* CE: CE_RESP + CE_STATE, callback, never returns    */
    RUN_SYNC,         /* desktop path: write the answer, return 0           */
    RUN_LAUNCH_FAIL,  /* os_RunPrgm fails and returns -1 (as on the CE)     */
    RUN_NO_RESPONSE,  /* child returns without writing CE_RESP or CE_STATE  */
    RUN_BAD_RESPONSE, /* child flags a response but CE_RESP has a bad magic */
    RUN_SYNC_SILENT,  /* desktop path: return 0 without writing anything    */
    RUN_FORWARD_BAD   /* a forwarder (CECLIM) whose child fails; see below  */
} run_mode_t;

typedef struct {
    bool ok;
    const char *result;
    const char *error;
    const char *steps[4];
    run_mode_t mode;
    ce_float_t ans;   /* the worker's new Ans (0 when not given) */
    bool clobber;     /* worker sets Ans and settings before it answers */
    bool store_x;     /* worker writes its answer into the stored X slot */
} answer_t;

static const answer_t *g_answers;
static int g_answer_pos, g_answer_len;
static int g_launches;
static char g_last_prgm[16];   /* program the last launch named */
static jmp_buf g_top;

/* Set when the forwarder in run_forward_bad() wrongly resumed the bad response. */
static bool g_forward_resumed;

/* RUN_FORWARD_BAD plays a forwarder chain as src/modules/calc_lim/main.c
 * runs it on the CE.  CECLIM rewrites CE_REQ with its own name and the Ans
 * and settings it holds, then launches CECLMB.  CECLMB evaluates, sets a new
 * Ans and settings (a->clobber), and leaves an unreadable CE_RESP.  CECLIM is
 * re-entered, recovers through the real ce_module_runtime_recover_child_
 * failure(), and answers CALCENG with a valid error built from g_last_answer. */
static int run_forward_bad(const answer_t *a, os_runprgm_callback_t cb)
{
    static ce_request_t req;
    static ce_response_t resp;
    static ce_shared_state_t state;
    static char err[128];

    /* CECLIM: ce_module_runtime_forward() up to os_RunPrgm. */
    ce_request_read(&req);
    ce_state_load(&state);
    g_settings = state.settings;
    g_last_answer = state.last_answer;
    req.settings = g_settings;
    req.last_answer = g_last_answer;
    strcpy(req.caller, "CECLIM");
    strcpy(req.return_to, "CECLIM");
    state.has_pending_response = 0;
    strcpy(state.pending_module, "CECLMB");
    ce_response_delete();
    ce_request_write(&req);
    ce_state_save(&state);

    /* CECLMB: evaluates, keeps the new Ans, but its CE_RESP is unreadable. */
    ce_state_load(&state);
    if (a->clobber) {
        g_last_answer = a->ans;
        g_settings.precision = 9;
        g_settings.angle_mode = ANGLE_RADIANS;
    }
    ce_response_from_runtime(&resp, true, false, a->result, "", g_last_answer);
    resp.magic = 0;
    ce_module_runtime_finish(&req, &resp, &state);

    /* CECLIM re-entered: like its main(), try to resume the child's answer
     * first (it must refuse the unreadable CE_RESP), then recover and send
     * CALCENG a valid error. */
    ce_module_runtime_reset_reentry();
    ce_module_runtime_mark_reentry();
    g_forward_resumed = ce_module_runtime_resume_forward("CECLIM", &req, &state);
    if (!g_forward_resumed &&
        ce_module_runtime_recover_child_failure("CECLIM", "CECLIM", &req, &state,
                                                err, sizeof(err))) {
        ce_response_from_runtime(&resp, false, false, "", err, g_last_answer);
        ce_module_runtime_finish(&req, &resp, &state);
    }

    ce_module_runtime_reset_reentry();
    if (cb) cb(NULL, 0);
    longjmp(g_top, 1);
}

int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t cb)
{
    static ce_request_t req;
    static ce_response_t resp;
    static ce_shared_state_t state;
    const answer_t *a;
    int k;
    (void)data; (void)size;

    g_launches++;
    strncpy(g_last_prgm, prgm ? prgm : "", sizeof(g_last_prgm) - 1);
    g_last_prgm[sizeof(g_last_prgm) - 1] = '\0';
    if (g_answer_pos >= g_answer_len) return -1;
    a = &g_answers[g_answer_pos++];
    if (a->mode == RUN_LAUNCH_FAIL) return -1;
    if (a->mode == RUN_SYNC_SILENT) return 0;
    if (a->mode == RUN_FORWARD_BAD) return run_forward_bad(a, cb);
    if (a->mode == RUN_NO_RESPONSE) {
        ce_module_runtime_reset_reentry();
        if (cb) cb(NULL, 0);
        longjmp(g_top, 1);
    }

    memset(&req, 0, sizeof(req));
    ce_request_read(&req);
    ce_state_load(&state);
    if (a->clobber) {   /* like finalize_response(): Ans first, then CE_RESP */
        g_last_answer = a->ans;
        g_settings.precision = 9;
        g_settings.angle_mode = ANGLE_RADIANS;
    }
    if (a->store_x) {
        state.user_vars.mask[2] |= 0x80u;
        state.user_vars.value['X' - 'A'] = a->ans;
    }
    ce_response_from_runtime(&resp, a->ok, a->steps[0] != NULL,
                             a->result, a->error, a->ans);
    if (a->mode == RUN_BAD_RESPONSE) resp.magic = 0;  /* launcher must reject it */
    for (k = 0; k < 4 && a->steps[k]; k++) {
        strncpy(resp.steps[k], a->steps[k], MAX_STEP_LEN - 1);
        resp.step_count = (uint8_t)(k + 1);
    }
    ce_module_runtime_finish(&req, &resp, &state);

    if (a->mode == RUN_SYNC) return 0;
    ce_module_runtime_reset_reentry();
    if (cb) cb(NULL, 0);
    longjmp(g_top, 1);   /* the re-entered launcher returned: back to the OS */
}

/* ── Scenario driver ───────────────────────────────────────────── */
static int g_failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", (msg), __LINE__); g_failures++; } \
    else { printf("ok: %s\n", (msg)); } \
} while (0)

/* tests/mock/fileioc_mock.c holds 8 variables: CE_STATE, CE_REQ, CE_RESP
 * and the programs below (the digit inputs route to CEBASE). */
static const char *const k_programs[] = { "CALCENG", "CEBASE", NULL };

/* Launch CALCENG with the current AppVars (CE_STATE kept: a relaunch). */
static void launch(const uint8_t *keys, int nkeys, const answer_t *answers, int nanswers)
{
    scr_clear();
    g_scrolls = 0;
    g_full_on_last_row = 0;
    g_full_wrap_to_last_row = 0;
    g_keys = keys; g_key_pos = 0; g_key_len = nkeys;
    g_release = false; g_quit_stage = 0;
    g_snap_count = 0;
    g_answers = answers; g_answer_pos = 0; g_answer_len = nanswers;
    g_launches = 0;
    ce_module_runtime_reset_reentry();
    if (setjmp(g_top) == 0) launcher_main();
    g_release = false;
    if (getenv("LVH_DEBUG")) {
        int r;
        printf("  [run] keys %d/%d snaps %d launches %d\n",
               g_key_pos, g_key_len, g_snap_count, g_launches);
        for (r = 0; r < SCR_ROWS; r++) printf("   |%s|\n", row_text(r));
    }
}

static void reset_world(bool with_programs)
{
    int i;
    fileioc_mock_reset();
    fileioc_mock_enable();
    if (with_programs) {
        for (i = 0; k_programs[i]; i++) fileioc_mock_add_program(k_programs[i], 1);
    }
}

/* Fresh calculator: no CE_STATE, all programs present, then launch. */
static void run(const uint8_t *keys, int nkeys, const answer_t *answers, int nanswers)
{
    reset_world(true);
    launch(keys, nkeys, answers, nanswers);
}

static bool snap_has(int s, int row, const char *text)
{
    return s < g_snap_count && strcmp(g_snaps[s][row], text) == 0;
}

static bool snap_contains(int s, const char *text)
{
    int r;
    if (s >= g_snap_count) return false;
    for (r = 0; r < SCR_ROWS; r++) if (strstr(g_snaps[s][r], text)) return true;
    return false;
}

static void dump(int s)
{
    int r;
    if (s >= g_snap_count) { printf("   (no snapshot %d)\n", s); return; }
    for (r = 0; r < SCR_ROWS; r++) printf("   %d|%s|\n", r, g_snaps[s][r]);
}

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define S KEY_SNAP

/* Snapshot row that starts with prefix (e.g. a viewer title "Error"). */
static bool snap_starts(int s, int row, const char *prefix)
{
    return s < g_snap_count && strncmp(g_snaps[s][row], prefix, strlen(prefix)) == 0;
}

/* Fresh calculator whose CE_STATE already holds non-default settings, so
 * the failure scenarios can prove a failed dispatch leaves them alone. */
static void seed_settings(ce_shared_state_t *st)
{
    st->settings.precision  = 4;
    st->settings.angle_mode = ANGLE_DEGREES;
}

static bool settings_seeded(const ce_shared_state_t *st)
{
    return st->settings.precision == 4 && st->settings.angle_mode == ANGLE_DEGREES;
}

/* Launcher-side failures, in the order scenario 29 runs them. */
enum { FK_IPC_WRITE, FK_STATE_SAVE, FK_MISSING, FK_LAUNCH_FAIL, FK_SILENT, FK_BAD,
       FK_STATE_ZERO, FK_STATE_SHORT, FK_STATE_RENAME, FK_STATE_COMMIT, FK_COUNT };
static const char *const k_fk_tag[FK_COUNT] = {
    "IPC write", "state save", "Module missing", "launch failure",
    "silent child", "invalid response", "state zero write", "state short write",
    "state rename", "state commit"
};
static const char *const k_fk_err[FK_COUNT] = {
    "IPC write failed", "IPC state save failed", "Module missing",
    "Program launch failed", "Module CEBASE returned no IPC response",
    "Module CEBASE returned invalid IPC data", "IPC state save failed", "IPC state save failed",
    "IPC state save failed", "IPC state save failed"
};
/* How the home screen wraps each error (second row NULL: one row). */
static const char *const k_fk_row1[FK_COUNT] = {
    "IPC write failed", "IPC state save failed", "Module missing",
    "Program launch failed", "Module CEBASE returned no", "Module CEBASE returned",
    "IPC state save failed", "IPC state save failed", "IPC state save failed", "IPC state save failed"
};
static const char *const k_fk_row2[FK_COUNT] = {
    NULL, NULL, NULL, NULL, "IPC response", "invalid IPC data", NULL, NULL, NULL, NULL
};

/* Arrange for the next dispatch to fail in way fk.  Returns the answer the
 * worker mock should play for it. */
static answer_t fk_arm(int fk)
{
    answer_t a;
    memset(&a, 0, sizeof(a));
    a.result = "";
    a.error = "";
    a.mode = RUN_CE;
    switch (fk) {
    case FK_IPC_WRITE:   fileioc_mock_fail_writes(CE_APPVAR_REQ, 0, 1);   break;
    case FK_STATE_SAVE:  fileioc_mock_fail_writes(CE_APPVAR_STATE, 0, 1); break;
    case FK_STATE_ZERO:  fileioc_mock_fail(MOCK_WRITE, CE_APPVAR_STATE, 0, 1, 0); break;
    case FK_STATE_SHORT: fileioc_mock_fail(MOCK_WRITE, CE_APPVAR_STATE, 0, 1, 1706); break;
    case FK_STATE_RENAME: fileioc_mock_fail(MOCK_RENAME, CE_APPVAR_STATE, 0, 1, 0); break;
    case FK_STATE_COMMIT: fileioc_mock_fail(MOCK_DELETE, "CEOLD", 0, 1, 0); break;
    case FK_MISSING:     ti_Delete("CEBASE");                              break;
    case FK_LAUNCH_FAIL: a.mode = RUN_LAUNCH_FAIL;                         break;
    case FK_SILENT:      a.mode = RUN_NO_RESPONSE;                         break;
    default:             a.mode = RUN_BAD_RESPONSE;                        break;
    }
    return a;
}

/* Check that rows r and r+1 show failure fk's error.  Returns the next row. */
static int fk_rows(int s, int r, int fk, bool *ok)
{
    if (!snap_has(s, r, k_fk_row1[fk])) *ok = false;
    r++;
    if (k_fk_row2[fk]) {
        if (!snap_has(s, r, k_fk_row2[fk])) *ok = false;
        r++;
    }
    return r;
}

static const char k_rank[] =
    "Best: integration by parts\n"
    "1) integration by parts (score 80): matched by CEIPRT\n"
    "2) partial fractions (score 48): Unsupported advanced integral request\n"
    "3) trig substitution (score 44): no match\n"
    "4) u-substitution (score 42): no match";

static const char k_int[] =
    "int = ((X*(-cos(X)))-(1*(-sin(X))))+C";

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 1. The intstrategy ranking: the home screen shows line 1 and a hint;
     * UP opens every line, wrapped; DOWN/RIGHT scroll; CLEAR goes back and
     * the half-typed input line is untouched. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Enter, S,           /* 0: home after the answer   */
            sk_2, sk_Up, S,              /* 1: viewer, top             */
            sk_Down, S,                  /* 2: one line down           */
            sk_Right, S,                 /* 3: last page               */
            sk_Left, S,                  /* 4: back to the top         */
            sk_Clear, S                  /* 5: home, input "2" kept    */
        };
        static const answer_t ans[] = { { true, k_rank, "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(g_launches == 1, "1: one worker launch");
        CHECK(snap_has(0, 0, "1") && snap_has(0, 1, "Best: integration by parts"),
              "1: home shows the expression and line 1");
        CHECK(snap_has(0, 2, "UP=full result (11 ln)"), "1: home hint counts the hidden lines");
        CHECK(snap_has(1, 0, "Result 1-8/11"), "1: viewer header shows the range");
        CHECK(snap_has(1, 1, "Best: integration by parts") &&
              snap_has(1, 2, "1) integration by parts") &&
              snap_has(1, 3, "(score 80): matched by") &&
              snap_has(1, 4, "CEIPRT"), "1: viewer word-wraps line 2 at 26 columns");
        CHECK(snap_has(1, 9, "UP/DN L/R=page CLEAR=back"), "1: viewer key help on row 9");
        CHECK(snap_has(2, 0, "Result 2-9/11") && snap_has(2, 1, "1) integration by parts"),
              "1: DOWN scrolls one line");
        CHECK(snap_has(3, 0, "Result 4-11/11") && snap_has(3, 7, "4) u-substitution (score") &&
              snap_has(3, 8, "42): no match"),
              "1: RIGHT pages to the last line and stops");
        CHECK(snap_has(4, 0, "Result 1-8/11"), "1: LEFT pages back to the top");
        CHECK(snap_has(5, 7, "> 2|") && snap_has(5, 1, "Best: integration by parts"),
              "1: CLEAR returns home and keeps the input line");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
        CHECK(g_scrolls == 0, "1: no write ever scrolled the screen");
        CHECK(g_full_on_last_row == 0, "1: row 9 is written with os_PutStrLine");
        CHECK(g_full_wrap_to_last_row == 0, "1: no os_PutStrFull wraps onto row 9");
    }

    /* 2. A single long line (the int by-parts form): the home screen wraps
     * it over two rows, so nothing is hidden and no hint shows;
     * ENTER on an empty line still opens the viewer and ENTER closes it. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Enter, S,           /* 0: home     */
            sk_Enter, S,                 /* 1: viewer   */
            sk_Enter, S                  /* 2: home     */
        };
        static const answer_t ans[] = { { true, k_int, "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 1, "int = ((X*(-cos(X)))-(1*(-") &&
              snap_has(0, 2, "sin(X))))+C") && snap_has(0, 3, ""),
              "2: long line wraps onto the next row, no hint");
        CHECK(snap_has(1, 0, "Result 1-2/2") &&
              snap_has(1, 1, "int = ((X*(-cos(X)))-(1*(-") &&
              snap_has(1, 2, "sin(X))))+C"), "2: ENTER on empty input shows the whole form");
        CHECK(snap_has(1, 9, "CLEAR=back"), "2: one-page help");
        CHECK(snap_has(2, 1, "int = ((X*(-cos(X)))-(1*(-") && snap_has(2, 2, "sin(X))))+C") &&
              snap_has(2, 7, "> |"),
              "2: ENTER closes the viewer without submitting");
        CHECK(g_launches == 1, "2: no second evaluation");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 3. A short answer: no hint, the viewer still opens (1/1). */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S, sk_Up, S, sk_Mode, S };
        static const answer_t ans[] = { { true, "= 5", "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 1, "= 5") && snap_has(0, 2, ""), "3: no hint for a one-line answer");
        CHECK(snap_has(1, 0, "Result 1/1") && snap_has(1, 1, "= 5"), "3: viewer shows 1/1");
        CHECK(snap_has(2, 1, "= 5"), "3: MODE closes the viewer");
    }

    /* 4. Error answer: titled Error, full 128-byte error text. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S, sk_Up, S };
        static const answer_t ans[] = { { false, "",
            "Unsupported advanced integral request for this form", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 1, "Unsupported advanced") &&
              snap_has(0, 2, "integral request for this") && snap_has(0, 3, "form") &&
              snap_has(0, 4, ""), "4: error wraps whole on the home screen, no hint");
        CHECK(snap_has(1, 0, "Error 1-3/3") && snap_has(1, 1, "Unsupported advanced") &&
              snap_has(1, 2, "integral request for this") && snap_has(1, 3, "form"),
              "4: viewer shows the whole error");
    }

    /* 5. Steps sent by the worker follow the answer. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S, sk_Up, S };
        static const answer_t ans[] = { { true, "d/dX = 2*X", "",
            { "Power rule: d/dX X^n = n*X^(n-1)", "Result: 2*X", NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 2, "UP=full result (5 ln)"), "5: hint counts the step lines");
        CHECK(snap_has(1, 1, "d/dX = 2*X") && snap_has(1, 2, "Steps:") &&
              snap_has(1, 3, "Power rule: d/dX X^n =") &&
              snap_has(1, 4, "n*X^(n-1)") && snap_has(1, 5, "Result: 2*X"),
              "5: steps listed after the answer");
    }

    /* 6. A parse error after an answer. The input stays editable and the
     * failed submission gets its own history/result pair.
     * The viewer must show that error, never the previous full answer. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Enter, S,           /* 0: ranking answered            */
            sk_Add, sk_Enter, S,         /* 1: "+" -> Syntax error, kept   */
            sk_Up, S,                    /* 2: viewer                      */
            sk_Clear, S,                 /* 3: home, "+" kept, no message  */
            sk_Enter, S,                 /* 4: same error again            */
            sk_2, S,                     /* 5: next key: message gone      */
            sk_Clear, sk_3, sk_Enter, S  /* 6: fixed input evaluates       */
        };
        static const answer_t ans[] = {
            { true, k_rank, "", { NULL }, RUN_CE },
            { true, "= 3", "", { NULL }, RUN_CE }
        };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(1, 7, "> +|"), "6: the typed input is kept, cursor at the end");
        CHECK(snap_has(1, 8, "Syntax error"), "6: Syntax error shows under the input");
        CHECK(snap_has(1, 0, "1") && snap_has(1, 1, "Best: integration by parts") &&
              snap_has(1, 2, "+") && snap_has(1, 3, "Syntax error"),
              "6: ranking kept and failed submission recorded");
        CHECK(snap_has(2, 0, "Result 1/1") && snap_has(2, 1, "Syntax error"),
              "6: viewer shows the new error, not the old ranking");
        CHECK(snap_has(3, 7, "> +|") && snap_has(3, 8, "") &&
              snap_has(3, 1, "Best: integration by parts"),
              "6: back home the input is still there and the message is gone");
        CHECK(snap_has(4, 7, "> +|") && snap_has(4, 8, "Syntax error"),
              "6: ENTER on the same input reports it again");
        CHECK(snap_has(5, 7, "> +2|") && snap_has(5, 8, ""),
              "6: the next key edits the kept input and clears the message");
        CHECK(g_launches == 2, "6: the parse errors launched nothing");
        CHECK(snap_has(6, 0, "1") && snap_has(6, 1, "Best: integration by parts") &&
              snap_has(6, 2, "+") && snap_has(6, 3, "Syntax error") &&
              snap_has(6, 4, "3") && snap_has(6, 5, "= 3") && snap_has(6, 7, "> |") &&
              snap_has(6, 8, ""), "6: history keeps success, error, then corrected success");
        CHECK(g_scrolls == 0 && g_full_on_last_row == 0 && g_full_wrap_to_last_row == 0,
              "6: the message row never wraps onto row 9 or scrolls");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 6b. The tokenizer stops on an unknown word ("AB" is not implicit
     * multiplication): the cursor moves to its first letter. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Add, sk_Alpha, sk_Math, sk_Alpha, sk_Apps, sk_Enter, S, /* 0 */
            sk_Right, S                                                      /* 1 */
        };
        run(keys, N(keys), NULL, 0);
        CHECK(snap_has(0, 7, "> 1+|AB") && snap_has(0, 8, "Syntax error"),
              "6b: cursor on the unknown word, message shown");
        CHECK(snap_has(1, 7, "> 1+A|B") && snap_has(1, 8, ""), "6b: arrows move from there");
        CHECK(g_launches == 0, "6b: no launch");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 6c. APPS types the matrix row separator ';' (the keycaps have none),
     * while ALPHA+APPS stays 'B'. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Comma, sk_2, sk_Apps, sk_3, sk_Comma, sk_4, S,  /* 0 */
            sk_Alpha, sk_Apps, S                                     /* 1 */
        };
        run(keys, N(keys), NULL, 0);
        CHECK(snap_has(0, 7, "> 1,2;3,4|"), "6c: APPS types ';'");
        CHECK(snap_has(1, 7, "> 1,2;3,4B|"), "6c: ALPHA+APPS still types B");
        CHECK(g_launches == 0, "6c: no launch");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 6d. Message lifetime and cursor sources (round-1 review P3s).  A
     * tokenizer error never leaves its offset behind for a later parser
     * error; 2nd alone keeps the message and a 2nd-layer key clears it; the
     * MODE menu and the stray-"." and d/dX cases. */
    {
        static const uint8_t keys[] = {
            sk_Alpha, sk_Math, sk_Alpha, sk_Apps, sk_Enter, S,     /* 0: "AB"            */
            sk_Clear, sk_2, sk_Mul, sk_LParen, sk_Left, sk_Left,
            sk_Enter, S,                                           /* 1: "2*(" parser    */
            sk_2nd, S,                                             /* 2: 2nd alone       */
            sk_Del, S,                                             /* 3: 2nd+DEL         */
            sk_Enter, S,                                           /* 4: "2(" error      */
            sk_Mode, S,                                            /* 5: MODE menu       */
            sk_Mode, S,                                            /* 6: back home       */
            sk_Clear, sk_GraphVar, sk_DecPnt, sk_Enter, S,         /* 7: "X."            */
            sk_Clear, sk_Alpha, sk_Recip, sk_Div, sk_Alpha, sk_Recip, sk_GraphVar,
            sk_Alpha, sk_Math, sk_Alpha, sk_Apps, sk_Enter, S      /* 8: "D/DXAB"        */
        };
        run(keys, N(keys), NULL, 0);
        CHECK(snap_has(0, 7, "> |AB") && snap_has(0, 8, "Syntax error"), "6d: unknown word at the start");
        CHECK(snap_has(1, 7, "> 2|*(") && snap_has(1, 8, "Syntax error"),
              "6d: a parser error keeps the cursor where it was, not the old offset");
        CHECK(snap_has(2, 8, "Syntax error"), "6d: 2nd alone keeps the message");
        CHECK(snap_has(3, 7, "> 2|(") && snap_has(3, 8, ""), "6d: a 2nd-layer key clears it and acts");
        CHECK(snap_has(4, 7, "> 2|(") && snap_has(4, 8, "Syntax error"), "6d: \"2(\" reports again");
        CHECK(!snap_contains(5, "Syntax error"), "6d: the MODE menu does not show the message");
        CHECK(snap_has(6, 7, "> 2|(") && snap_has(6, 8, ""), "6d: back from MODE the message is gone");
        CHECK(snap_has(7, 7, "> X|.") && snap_has(7, 8, "Syntax error"), "6d: cursor on a stray \".\"");
        CHECK(snap_has(8, 7, "> D/DX|AB") && snap_has(8, 8, "Syntax error"),
              "6d: the unknown word after d/dX, not inside the d/dX token");
        CHECK(g_launches == 0, "6d: no launch");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 6f. A nonzero literal that would read as 0 or a tiny value stops in
     * the launcher's tokenizer (on the ROM, 1.3E-38 read as 0):
     * "Tiny value unsupported", cursor on the literal, its own
     * history pair, nothing launched. A normal small literal still routes
     * to CEBASE. (Host atof is IEEE, so 1.3E-38 itself is checked on the
     * ROM, tests/cemu/test_realrom_tiny.py.) */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Alpha, sk_Sin, sk_Sub, sk_4, sk_0, sk_Enter, S,           /* 0: 1E-40   */
            sk_Clear, sk_2, sk_Add, sk_1, sk_Alpha, sk_Sin, sk_Sub, sk_3, sk_9,
            sk_Enter, S,                                                       /* 1: 2+1E-39 */
            sk_2, S,                                                           /* 2: edit    */
            sk_Clear, sk_1, sk_Alpha, sk_Sin, sk_Sub, sk_3, sk_0, sk_Enter, S  /* 3: 1E-30   */
        };
        static const answer_t ans[] = { { true, "= 1e-30", "", { NULL }, RUN_CE } };
        g_last_prgm[0] = '\0';
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 7, "> |1E-40") && snap_has(0, 8, "Tiny value unsupported"),
              "6f: tiny literal refused, cursor on it");
        CHECK(snap_has(0, 0, "1E-40") && snap_has(0, 1, "Tiny value unsupported"),
              "6f: the refusal gets its own history pair");
        CHECK(snap_has(1, 7, "> 2+|1E-39") && snap_has(1, 8, "Tiny value unsupported"),
              "6f: cursor on the tiny literal after 2+");
        CHECK(snap_has(2, 7, "> 2+2|1E-39") && snap_has(2, 8, ""),
              "6f: the next key edits the kept input and clears the message");
        CHECK(g_launches == 1 && strcmp(g_last_prgm, "CEBASE") == 0,
              "6f: only the normal literal launched, and it routed to CEBASE");
        CHECK(snap_contains(3, "= 1e-30"), "6f: the small normal answer shows");
        CHECK(g_scrolls == 0 && g_full_on_last_row == 0 && g_full_wrap_to_last_row == 0,
              "6f: the message row never wraps onto row 9 or scrolls");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 6e. Entry recall now includes failed submissions. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Enter, sk_Add, sk_Enter, S,   /* 0: error after an answer */
            sk_2nd, sk_Enter, S,                   /* 1: recall failed "+"     */
            sk_Enter, sk_2nd, sk_Enter, S,         /* 2: resubmit, recall "+"  */
            sk_2nd, sk_Enter, S                    /* 3: recall "1"            */
        };
        static const answer_t ans[] = { { true, "= 1", "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 7, "> +|") && snap_has(0, 8, "Syntax error"), "6e: error shown");
        CHECK(snap_has(1, 7, "> +|") && snap_has(1, 8, ""), "6e: entry recall includes the failed input");
        CHECK(snap_has(2, 7, "> +|"), "6e: resubmitting resets the entry-recall cycle");
        CHECK(snap_has(3, 7, "> 1|"), "6e: recall still reaches the earlier successful input");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 6c. A parse error after an empty worker result must have its own
     * slot, never attach its message to the older empty-result entry. */
    {
        static const uint8_t keys1[] = { sk_1, sk_Enter, S, sk_Add, sk_Enter, S };
        static const uint8_t keys2[] = { S };
        static const answer_t ans[] = { { true, "", "", { NULL }, RUN_CE } };
        run(keys1, N(keys1), ans, N(ans));
        CHECK(snap_has(1, 0, "1") && snap_has(1, 1, "+") && snap_has(1, 2, "Syntax error") &&
              snap_has(1, 8, "Syntax error"), "6c: error belongs to its own pair after an empty answer");
        launch(keys2, N(keys2), NULL, 0);
        CHECK(snap_has(0, 0, "1") && snap_has(0, 1, "+") && snap_has(0, 2, "Syntax error") &&
              snap_has(0, 7, "> |") && snap_has(0, 8, ""),
              "6c: relaunch preserves the error pair, not the transient message");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 7. Two evaluations: the viewer always shows the newest. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Enter, sk_2, sk_Enter, S, sk_Up, S
        };
        static const answer_t ans[] = {
            { true, k_rank, "", { NULL }, RUN_CE },
            { true, k_int, "", { NULL }, RUN_CE }
        };
        run(keys, N(keys), ans, N(ans));
        CHECK(g_launches == 2, "7: two launches");
        CHECK(snap_contains(0, "Best: integration by parts") &&
              snap_contains(0, "sin(X))))+C") &&
              !snap_contains(0, "UP=full result"), "7: only the newest result could have a hint");
        CHECK(snap_has(1, 0, "Result 1-2/2") && snap_has(1, 2, "sin(X))))+C"),
              "7: viewer shows the newest answer");
    }

    /* 8. Desktop synchronous path (os_RunPrgm returns 0). */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S, sk_Up, S };
        static const answer_t ans[] = { { true, k_rank, "", { NULL }, RUN_SYNC } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 2, "UP=full result (11 ln)") && snap_has(1, 0, "Result 1-8/11"),
              "8: synchronous path keeps the full text too");
    }

    /* 9. Nothing evaluated yet: UP and ENTER do nothing. */
    {
        static const uint8_t keys[] = { S, sk_Up, S, sk_Enter, S };
        run(keys, N(keys), NULL, 0);
        CHECK(snap_has(1, 7, "> |") && snap_has(2, 7, "> |") && snap_has(2, 0, ""),
              "9: empty history: no viewer");
        CHECK(g_launches == 0, "9: no launch");
    }

    /* 10. CLEAR CLEAR wipes the history; the viewer has nothing to show. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, sk_Clear, S, sk_Up, S };
        static const answer_t ans[] = { { true, k_rank, "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 0, "") && snap_has(1, 7, "> |") && snap_has(1, 0, ""),
              "10: cleared history opens no stale viewer");
    }

    /* 10b. Empty history, then a parse error: the new pair is viewable. */
    {
        static const uint8_t keys[] = { sk_Add, sk_Enter, S, sk_Up, S, sk_Clear,
                                        sk_Clear, S, sk_Enter, S };
        run(keys, N(keys), NULL, 0);
        CHECK(snap_has(0, 0, "+") && snap_has(0, 1, "Syntax error") &&
              snap_has(0, 7, "> +|") && snap_has(0, 8, "Syntax error"),
              "10b: parse error creates the first history pair");
        CHECK(snap_has(1, 0, "Result 1/1") && snap_has(1, 1, "Syntax error"),
              "10b: UP opens the error text");
        CHECK(snap_has(3, 0, "Result 1/1") && snap_has(3, 1, "Syntax error"),
              "10b: ENTER on the cleared line opens the recorded error");
        CHECK(g_launches == 0, "10b: no launch");
    }

    /* 11. The worker's "Parsed:" echo alone gives no hint; UP still shows it. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S, sk_Up, S };
        static const answer_t ans[] = { { true, "= 5\nParsed: (2+3)", "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 1, "= 5") && snap_has(0, 2, ""), "11: no hint for the Parsed: echo");
        CHECK(snap_has(1, 0, "Result 1-2/2") && snap_has(1, 2, "Parsed: (2+3)"),
              "11: viewer still shows the echo");
    }

    /* 12. A full answer, then a launch failure (the launcher exits, as it
     * did before), then a relaunch: the newest row and the viewer show the
     * failure, titled Error, never the earlier ranking. */
    {
        static const uint8_t keys1[] = { sk_1, sk_Enter, sk_2, sk_Enter };
        static const uint8_t keys2[] = { S, sk_Up, S };
        static const answer_t ans[] = {
            { true, k_rank, "", { NULL }, RUN_CE },
            { false, "", "", { NULL }, RUN_LAUNCH_FAIL }
        };
        run(keys1, N(keys1), ans, N(ans));
        CHECK(g_launches == 2, "12: second launch attempted");
        launch(keys2, N(keys2), NULL, 0);
        CHECK(snap_has(0, 2, "2") && snap_has(0, 3, "Program launch failed") &&
              !snap_contains(0, "UP=full result"), "12: relaunch shows the failure, no hint");
        CHECK(snap_has(1, 0, "Error 1/1") && snap_has(1, 1, "Program launch failed") &&
              !snap_contains(1, "score"), "12: viewer shows the failure as an Error");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 13. A child that returns without any response, after a full answer. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, sk_2, sk_Enter, S, sk_Up, S };
        static const answer_t ans[] = {
            { true, k_rank, "", { NULL }, RUN_CE },
            { false, "", "", { NULL }, RUN_NO_RESPONSE }
        };
        run(keys, N(keys), ans, N(ans));
        CHECK(g_launches == 2, "13: two launches");
        CHECK(snap_has(0, 2, "2") && snap_has(0, 3, "Module CEBASE returned no") &&
              snap_has(0, 4, "IPC response") && !snap_contains(0, "UP=full result"),
              "13: home shows the silent child's whole error");
        CHECK(snap_has(1, 0, "Error 1-2/2") && snap_has(1, 2, "IPC response") &&
              !snap_contains(1, "score 80"), "13: viewer shows that error, not the ranking");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 14. Module missing (CEBASE absent), then a relaunch. */
    {
        static const uint8_t keys1[] = { sk_1, sk_Enter };
        static const uint8_t keys2[] = { S, sk_Up, S };
        reset_world(false);
        fileioc_mock_add_program("CALCENG", 1);
        launch(keys1, N(keys1), NULL, 0);
        CHECK(g_launches == 0, "14: nothing launched");
        launch(keys2, N(keys2), NULL, 0);
        CHECK(snap_has(0, 0, "1") && snap_has(0, 1, "Module missing"), "14: home shows Module missing");
        CHECK(snap_has(1, 0, "Error 1/1"), "14: viewer titles it Error");
    }

    /* 15. Relaunch with a saved history: only the stored first line is left,
     * so the viewer shows that and no stale full text. */
    {
        static const uint8_t keys1[] = { sk_1, sk_Enter };
        static const uint8_t keys2[] = { S, sk_Up, S };
        static const answer_t ans[] = { { true, k_rank, "", { NULL }, RUN_CE } };
        run(keys1, N(keys1), ans, N(ans));
        launch(keys2, N(keys2), NULL, 0);
        CHECK(snap_has(0, 1, "Best: integration by parts") && !snap_contains(0, "UP=full result"),
              "15: relaunch shows the stored line, no hint");
        CHECK(snap_has(1, 0, "Result 1/1") && !snap_contains(1, "score 80"),
              "15: viewer shows the stored line only");
    }

    /* 16. History ring wrap: 17 answers, then a launch failure.  The reused
     * slot must not show the answer from 16 entries earlier (round-1 P3). */
    {
        static uint8_t keys1[64];
        static answer_t ans[18];
        static const uint8_t keys2[] = { S };
        static const char *const res[17] = {
            "= r1", "= r2", "= r3", "= r4", "= r5", "= r6", "= r7", "= r8", "= r9",
            "= r10", "= r11", "= r12", "= r13", "= r14", "= r15", "= r16", "= r17"
        };
        static const uint8_t digit[10] = { sk_0, sk_1, sk_2, sk_3, sk_4, sk_5, sk_6, sk_7, sk_8, sk_9 };
        int n = 0, k;
        for (k = 1; k <= 18; k++) {          /* expressions "1".."18", all distinct */
            if (k >= 10) keys1[n++] = digit[k / 10];
            keys1[n++] = digit[k % 10];
            keys1[n++] = sk_Enter;
            ans[k - 1].ok = true;
            ans[k - 1].error = "";
            ans[k - 1].result = k <= 17 ? res[k - 1] : "";
            ans[k - 1].mode = k <= 17 ? RUN_CE : RUN_LAUNCH_FAIL;
        }
        run(keys1, n, ans, 18);
        CHECK(g_launches == 18, "16: 18 launches");
        launch(keys2, N(keys2), NULL, 0);
        CHECK(snap_has(0, 4, "18") && snap_has(0, 5, "Program launch failed"),
              "16: newest row shows the failure, not the answer 16 entries back");
        CHECK(!snap_contains(0, "= r2"), "16: the reused slot's old result is gone");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 17. The 39-character derivative wraps onto the next row;
     * nothing is cut, so no hint. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S };
        static const answer_t ans[] = { { true,
            "d/dX = ((cos(X)*(X^2))+(sin(X)*(2*X)))", "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 0, "1") && snap_has(0, 1, "d/dX = ((cos(X)*(X^2))+(si") &&
              snap_has(0, 2, "n(X)*(2*X)))") && snap_has(0, 3, ""),
              "17: long derivative wraps whole over two rows");
        CHECK(!snap_contains(0, "UP=full result"), "17: no hint when the wrap shows it all");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 18. A long typed expression: the input line marks the hidden start,
     * the transcript wraps it, and past three rows it ends in the ellipsis. */
    {
        static uint8_t keys[160];   /* 144 keys below */
        int n = 0, k;
        for (k = 0; k < 60; k++) keys[n++] = (k % 2) ? sk_2 : sk_1;   /* "1212..." */
        keys[n++] = S;                                                /* 0: typing      */
        for (k = 0; k < 30; k++) keys[n++] = sk_Left;
        keys[n++] = S;                                                /* 1: cursor mid  */
        for (k = 0; k < 30; k++) keys[n++] = sk_Right;
        for (k = 0; k < 20; k++) keys[n++] = (k % 2) ? sk_2 : sk_1;   /* 80 chars total */
        keys[n++] = sk_Enter; keys[n++] = S;                          /* 2: answered    */
        {
            static const answer_t ans[] = { { true, "= 5", "", { NULL }, RUN_CE } };
            run(keys, n, ans, N(ans));
        }
        /* 60 typed, cursor at the end: chars 37..59 then the cursor. */
        CHECK(snap_has(0, 7, ">\xCE" "21212121212121212121212|"),
              "18: scrolled input line marks its hidden start");
        /* Cursor at 30: window from 8 (one column kept for the text right
         * of the cursor), the cursor at column 24, the ellipsis at 25. */
        CHECK(snap_has(1, 7, ">\xCE" "1212121212121212121212|\xCE"),
              "18: text right of the cursor is marked too");
        CHECK(snap_has(2, 0, "12121212121212121212121212") &&
              snap_has(2, 1, "12121212121212121212121212") &&
              snap_has(2, 2, "1212121212121212121212121\xCE") &&
              snap_has(2, 3, "= 5") && snap_has(2, 7, "> |"),
              "18: 80-char expression: three rows, the last ends in the ellipsis");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 19. Older entries that no longer fit are left out whole, never cut:
     * two 4-row entries, then a 2-row one, then a short one. */
    {
        static uint8_t keys[80];    /* 66 keys below */
        int n = 0, k, e;
        static const answer_t ans[] = {
            { true, "d/dX = ((cos(X)*(X^2))+(sin(X)*(2*X)))", "", { NULL }, RUN_CE },
            { true, "d/dX = ((cos(X)*(X^2))+(sin(X)*(2*X)))", "", { NULL }, RUN_CE },
            { true, "= 7", "", { NULL }, RUN_CE },
        };
        for (e = 0; e < 2; e++) {
            for (k = 0; k < 30; k++) keys[n++] = e ? sk_2 : sk_1;
            keys[n++] = sk_Enter;
        }
        keys[n++] = S;                                                /* 0 */
        keys[n++] = sk_3; keys[n++] = sk_Enter; keys[n++] = S;        /* 1 */
        run(keys, n, ans, N(ans));
        CHECK(snap_has(0, 0, "22222222222222222222222222") && snap_has(0, 1, "2222") &&
              snap_has(0, 2, "d/dX = ((cos(X)*(X^2))+(si") && snap_has(0, 3, "n(X)*(2*X)))") &&
              !snap_contains(0, "1111"), "19: the older 4-row entry is left out whole");
        CHECK(snap_has(1, 0, "22222222222222222222222222") && snap_has(1, 4, "3") &&
              snap_has(1, 5, "= 7") && snap_has(1, 6, "") && !snap_contains(1, "1111"),
              "19: newest entry lowest, oldest that fits on top");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 20. A first line over 63 bytes: the stored copy ends in the ellipsis,
     * the hint points at the viewer, and after a relaunch (stored copy only)
     * the ellipsis stays but the hint goes. */
    {
        static const uint8_t keys1[] = { sk_1, sk_Enter, S, sk_Up, S, sk_Clear };
        static const uint8_t keys2[] = { S };
        static const answer_t ans[] = { { true,
            "= 1234567890123456789012345678901234567890123456789012345678901234567890",
            "", { NULL }, RUN_CE } };
        run(keys1, N(keys1), ans, N(ans));
        CHECK(snap_has(0, 1, "= 123456789012345678901234") &&
              snap_has(0, 2, "56789012345678901234567890") &&
              snap_has(0, 3, "1234567890\xCE") &&
              snap_has(0, 4, "UP=full result (3 ln)"), "20: over-63 line ends in the ellipsis, hint");
        CHECK(snap_has(1, 0, "Result 1-3/3") && snap_has(1, 3, "12345678901234567890"),
              "20: viewer shows the full line");
        launch(keys2, N(keys2), NULL, 0);
        CHECK(snap_has(0, 3, "1234567890\xCE") && !snap_contains(0, "UP=full result"),
              "20: relaunch keeps the ellipsis, drops the hint");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 21. A result that word-wraps past three rows: row 3 ends in the
     * ellipsis and the hint counts the viewer lines. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S };
        static const answer_t ans[] = { { true,
            "aaaaaaaaaaaaa bbbbbbbbbbbbb ccccccccccccc ddddddddddddd", "", { NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 1, "aaaaaaaaaaaaa") && snap_has(0, 2, "bbbbbbbbbbbbb") &&
              snap_has(0, 3, "ccccccccccccc\xCE") && !snap_contains(0, "ddd"),
              "21: three word-wrapped rows, the last ends in the ellipsis");
        CHECK(snap_has(0, 4, "UP=full result (4 ln)"), "21: hint counts the viewer lines");
        CHECK(snap_has(0, 7, "> |") && g_scrolls == 0, "21: input row untouched, no scroll");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 22. Stored-line boundary (review round 1 P3): a 63-byte first line stays
     * unmarked, a 64-byte one keeps 62 bytes and the ellipsis. */
    {
        static char l63[64], l64[65];
        static answer_t ans[1];
        static const uint8_t keys[] = { sk_1, sk_Enter, S };
        int k;
        for (k = 0; k < 63; k++) l63[k] = (char)('A' + k % 26);
        l63[63] = '\0';
        for (k = 0; k < 64; k++) l64[k] = (char)('A' + k % 26);
        l64[64] = '\0';
        ans[0].ok = true; ans[0].error = ""; ans[0].mode = RUN_CE;
        ans[0].result = l63;
        run(keys, N(keys), ans, 1);
        CHECK(snap_has(0, 3, "ABCDEFGHIJK") && !snap_contains(0, "\xCE") &&
              !snap_contains(0, "UP=full result"), "22: 63-byte line whole, no mark, no hint");
        ans[0].result = l64;
        run(keys, N(keys), ans, 1);
        CHECK(snap_has(0, 3, "ABCDEFGHIJ\xCE") && snap_has(0, 4, "UP=full result (3 ln)"),
              "22: 64-byte line keeps 62 bytes, ellipsis, hint");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 23. Input-line marker edges (review round 1 P3). */
    {
        static uint8_t keys[128];   /* 107 keys below */
        int n = 0, k;
        for (k = 0; k < 24; k++) keys[n++] = sk_1;
        keys[n++] = S;                              /* 0: 24 chars, cursor at end   */
        keys[n++] = sk_Left; keys[n++] = S;         /* 1: cursor 23, one char right */
        for (k = 0; k < 2; k++) keys[n++] = sk_2;   /* 26 chars, cursor 25          */
        for (k = 0; k < 25; k++) keys[n++] = sk_Left;
        keys[n++] = S;                              /* 2: cursor 0 on 26 chars      */
        for (k = 0; k < 26; k++) keys[n++] = sk_Right;
        for (k = 0; k < 20; k++) keys[n++] = sk_Del; /* backspace: 6 chars left     */
        keys[n++] = S;                              /* 3: short, cursor at end      */
        run(keys, n, NULL, 0);
        CHECK(snap_has(0, 7, ">\xCE" "11111111111111111111111|"),
              "23: 24 chars, cursor at end: left mark only");
        CHECK(snap_has(1, 7, ">\xCE" "1111111111111111111111|1"),
              "23: cursor 23, one char right of it: shown, no right mark");
        CHECK(snap_has(2, 7, "> |1111111111111111111111\xCE"),
              "23: cursor 0 on a long line: right mark only");
        CHECK(snap_has(3, 7, "> 111111|"), "23: short line after DEL: no marks");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 24. Echo exception end to end (review round 1 P3): a wrapped line 1 plus
     * the Parsed: echo gives no hint; a cut line 1 plus the echo does. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S };
        static const answer_t a1[] = { { true,
            "d/dX = ((cos(X)*(X^2))+(sin(X)*(2*X)))\nParsed: x", "", { NULL }, RUN_CE } };
        static const answer_t a2[] = { { true,
            "aaaaaaaaaaaaa bbbbbbbbbbbbb ccccccccccccc ddddddddddddd\nParsed: x", "", { NULL }, RUN_CE } };
        run(keys, N(keys), a1, 1);
        CHECK(snap_has(0, 2, "n(X)*(2*X)))") && !snap_contains(0, "UP=full result"),
              "24: wrapped line 1 + echo: no hint");
        run(keys, N(keys), a2, 1);
        CHECK(snap_has(0, 3, "ccccccccccccc\xCE") && snap_has(0, 4, "UP=full result (5 ln)"),
              "24: cut line 1 + echo: hint");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 25. TRACE opens the same viewer as UP: the answer, then its steps.
     * ASCII '[' is printed as the TI-font bracket
     * 0xC1, because the TI large font draws 0x5B as theta. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S, sk_Trace, S, sk_Clear, S };
        static const answer_t ans[] = { { true, "int = ((X^(2+1))/(2+1)) + C", "",
            { "int((X^2)) dX", "Power rule: int(X^n) = X^(n+1)/(n+1)", "on [0, 1]", NULL },
            RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_contains(0, "UP=full result ("), "25: home offers the viewer");
        CHECK(snap_has(1, 0, "Result 1-7/7") && snap_has(1, 3, "Steps:") &&
              snap_has(1, 4, "int((X^2)) dX") && snap_has(1, 5, "Power rule: int(X^n) ="),
              "25: TRACE shows the answer, then the steps");
        CHECK(snap_has(1, 7, "on \xC1" "0, 1]") && !snap_contains(1, "["),
              "25: '[' is printed as the TI bracket glyph 0xC1");
        CHECK(snap_has(2, 7, "> |"), "25: CLEAR returns home");
        CHECK(g_launches == 1, "25: TRACE does not evaluate again");
    }

    /* 26. MODE "Show work" OFF hides the steps in the viewer and drops the
     * hint they would cause; toggling it marks the viewer cache dirty. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, S,
                                        sk_Mode, sk_Right, sk_Enter, S, sk_Trace, S, sk_Clear,
                                        sk_Mode, sk_Right, sk_Enter, S, sk_Trace, S };
        static const answer_t ans[] = { { true, "= 4", "",
            { "lim X->2 of (X^2)", "Direct sub: f(2) = 4", NULL }, RUN_CE } };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_contains(0, "UP=full result (4 ln)"), "26: steps on: hint counts them");
        CHECK(!snap_contains(1, "UP=full result"), "26: Show work OFF: no hint");
        CHECK(snap_has(2, 0, "Result 1/1") && !snap_contains(2, "Steps:"),
              "26: Show work OFF: viewer shows only the answer");
        CHECK(snap_contains(3, "UP=full result (4 ln)"), "26: Show work ON again: hint is back");
        CHECK(snap_has(4, 2, "Steps:") && snap_has(4, 3, "lim X->2 of (X^2)"),
              "26: Show work ON again: steps are back");
    }

    /* 27. The reported D(X^3,X) submission is a local parse failure, not
     * a worker request. Keep it editable, but persist its readable error
     * beside it in history, including after a fresh launch. */
    {
        static const uint8_t keys[] = {
            sk_1, sk_Enter,
            sk_Alpha, sk_Recip, sk_LParen, sk_GraphVar, sk_Power, sk_3,
            sk_Comma, sk_GraphVar, sk_RParen, sk_Enter, S,
            sk_Enter, S, sk_Up, S, sk_Clear, sk_Clear, S
        };
        static const answer_t ans[] = { { true, "= 1", "", { NULL }, RUN_CE } };
        static ce_shared_state_t saved;
        static const uint8_t again[] = { S, sk_Up, S };
        run(keys, N(keys), ans, N(ans));
        CHECK(snap_has(0, 0, "1") && snap_has(0, 1, "= 1") &&
              snap_has(0, 2, "D(X^3,X)") &&
              snap_has(0, 3, "Syntax error: d/dx(expr)"),
              "27: failed submission has a history/result pair, old pair kept");
        CHECK(snap_has(0, 7, "> D(X^3,X)|"), "27: input stays editable");
        CHECK(snap_has(0, 8, "Syntax error: d/dx(expr)"), "27: short derivative syntax hint");
        CHECK(snap_has(2, 1, "Syntax error: d/dx(expr)") && !snap_contains(2, "= 1"),
              "27: viewer never shows the old answer for the failed submission");
        CHECK(snap_has(3, 7, "> |") && snap_has(3, 3, "Syntax error: d/dx(expr)"),
              "27: clearing input does not erase the history error");
        CHECK(ce_state_load(&saved) && saved.hist_count == 2 &&
              strcmp(saved.history[1], "D(X^3,X)") == 0 &&
              strcmp(saved.history_results[1], "Syntax error: d/dx(expr)") == 0,
              "27: repeated ENTER leaves a persisted error pair without duplicate spam");
        CHECK(g_launches == 1, "27: parse failures never dispatch");
        launch(again, N(again), NULL, 0);
        CHECK(snap_has(0, 2, "D(X^3,X)") && snap_has(0, 3, "Syntax error: d/dx(expr)") &&
              snap_has(1, 1, "Syntax error: d/dx(expr)"),
              "27: error pair and viewer survive quitting and relaunching");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 28. Sixteen distinct local failures wrap the ring back to the old
     * worker response's write index. That response must stay invalid. */
    {
        uint8_t keys[6 * CE_HISTORY_SIZE + 4];
        int n = 0, k;
        static const answer_t ans[] = { { true, "OLD FULL ANSWER", "", { NULL }, RUN_CE } };
        static ce_shared_state_t saved;
        static const uint8_t digits[] = { sk_0, sk_1, sk_2, sk_3, sk_4,
                                          sk_5, sk_6, sk_7, sk_8, sk_9 };
        keys[n++] = sk_1;
        keys[n++] = sk_Enter;
        for (k = 0; k < CE_HISTORY_SIZE; k++) {
            keys[n++] = sk_Add;
            if (k >= 10) keys[n++] = sk_1;
            keys[n++] = digits[k % 10];
            keys[n++] = sk_LParen; /* distinct incomplete expression */
            keys[n++] = sk_Enter;
            keys[n++] = sk_Clear;
        }
        keys[n++] = sk_Up;
        keys[n++] = S;
        reset_world(true);
        ce_state_defaults(&saved);
        for (k = 0; k < CE_HISTORY_SIZE - 1; k++) {
            snprintf(saved.history[k], MAX_INPUT_LEN, "old%d", k);
            strcpy(saved.history_results[k], "old result");
        }
        saved.hist_count = CE_HISTORY_SIZE - 1;
        saved.hist_write = CE_HISTORY_SIZE - 1;
        CHECK(ce_state_save(&saved), "28: seed an almost-full history ring");
        launch(keys, n, ans, N(ans));
        CHECK(ce_state_load(&saved) && saved.hist_count == CE_HISTORY_SIZE && saved.hist_write == 0,
              "28: sixteen failed submissions wrap the ring");
        CHECK(snap_has(0, 1, "Syntax error") && !snap_contains(0, "OLD FULL ANSWER"),
              "28: ring wrap cannot resurrect the old worker response");
        CHECK(g_launches == 1, "28: local errors launch no workers");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 29. Launcher-side dispatch and recovery failures: IPC
     * write and state-save failures, Module missing, a launch failure, a
     * child that returns without a response, and one that leaves an
     * invalid response.  Each failure's readable error belongs to its own
     * history slot and is saved before the launcher exits, so it survives
     * a following submission and quitting/relaunching, never shows the
     * earlier answer under the new row, and never opens the earlier full
     * answer in the viewer.  Ans and settings are unchanged. */
    {
        int fk;
        for (fk = 0; fk < FK_COUNT; fk++) {
            static const uint8_t keys_a[] = { sk_1, sk_Enter };
            static const uint8_t keys_b[] = { sk_2, sk_Enter, S, sk_Up, S, sk_Clear };
            static const uint8_t keys_c[] = { S, sk_Up, S, sk_Clear, sk_3, sk_Enter, S, sk_Up, S };
            static const uint8_t keys_d[] = { S };
            static answer_t ans_a[1], ans_b[1], ans_c[1];
            static ce_shared_state_t st;
            bool in_loop = fk == FK_SILENT || fk == FK_BAD;
            bool ok;
            int r;
            char msg[160];
            const char *tag = k_fk_tag[fk];

            memset(ans_a, 0, sizeof(ans_a));
            ans_a[0].ok = true; ans_a[0].result = k_rank; ans_a[0].error = "";
            ans_a[0].mode = RUN_CE; ans_a[0].ans = 7.0f;
            memset(ans_c, 0, sizeof(ans_c));
            ans_c[0].ok = true; ans_c[0].result = "= 3"; ans_c[0].error = "";
            ans_c[0].mode = RUN_CE; ans_c[0].ans = 3.0f;

            reset_world(true);
            ce_state_defaults(&st);
            seed_settings(&st);
            st.user_vars.mask[0] = 1;
            st.user_vars.mask[2] = 128;
            st.user_vars.value[0] = -2.5f;
            st.user_vars.value[23] = 6;
            ce_state_save(&st);
            launch(keys_a, N(keys_a), ans_a, N(ans_a));   /* "1": full ranking, Ans 7 */

            ans_b[0] = fk_arm(fk);
            launch(keys_b, N(keys_b), ans_b, N(ans_b));   /* "2" fails */
            snprintf(msg, sizeof(msg), "29 %s: the launcher %s after the failure",
                     tag, in_loop ? "stays" : "exits");
            CHECK(in_loop ? g_snap_count == 2 : g_snap_count == 0, msg);
            if (in_loop) {
                ok = snap_has(0, 0, "1") && snap_has(0, 1, "Best: integration by parts") &&
                     snap_has(0, 2, "2");
                r = fk_rows(0, 3, fk, &ok);
                ok = ok && snap_has(0, r, "") && !snap_contains(0, "UP=full result");
                snprintf(msg, sizeof(msg), "29 %s: the error shows under its own entry", tag);
                CHECK(ok, msg);
                snprintf(msg, sizeof(msg), "29 %s: the viewer shows the error, not the ranking", tag);
                CHECK(snap_starts(1, 0, "Error") && snap_has(1, 1, k_fk_row1[fk]) &&
                      !snap_contains(1, "score") && !snap_contains(1, "Best:"), msg);
            }
            snprintf(msg, sizeof(msg), "29 %s: the error is saved in the new entry's slot", tag);
            CHECK(ce_state_load(&st) && st.hist_count == 2 && strcmp(st.history[1], "2") == 0 &&
                  strcmp(st.history_results[1], k_fk_err[fk]) == 0 &&
                  strcmp(st.history_results[0], "Best: integration by parts") == 0, msg);
            snprintf(msg, sizeof(msg), "29 %s: Ans and settings are unchanged", tag);
            CHECK(st.last_answer == 7.0f && settings_seeded(&st) &&
                  !st.has_pending_response && !st.pending_module[0], msg);
            CHECK(st.user_vars.value[0] == -2.5f && st.user_vars.value[23] == 6,
                  "29: launcher failure preserves both scalar values");

            if (fk == FK_MISSING) fileioc_mock_add_program("CEBASE", 1);
            launch(keys_c, N(keys_c), ans_c, N(ans_c));   /* relaunch, then "3" */
            ok = snap_has(0, 0, "1") && snap_has(0, 1, "Best: integration by parts") &&
                 snap_has(0, 2, "2");
            r = fk_rows(0, 3, fk, &ok);
            ok = ok && snap_has(0, r, "") && snap_has(0, 7, "> |") &&
                 !snap_contains(0, "UP=full result");
            snprintf(msg, sizeof(msg), "29 %s: relaunch shows the error, not the earlier answer", tag);
            CHECK(ok, msg);
            snprintf(msg, sizeof(msg), "29 %s: after relaunch the viewer shows the error", tag);
            CHECK(snap_starts(1, 0, "Error") && snap_has(1, 1, k_fk_row1[fk]) &&
                  !snap_contains(1, "score") && !snap_contains(1, "Best:"), msg);
            ok = snap_has(2, 2, "2");
            r = fk_rows(2, 3, fk, &ok);
            ok = ok && snap_has(2, r, "3") && snap_has(2, r + 1, "= 3");
            snprintf(msg, sizeof(msg), "29 %s: a following answer keeps the error above it", tag);
            CHECK(ok, msg);
            snprintf(msg, sizeof(msg), "29 %s: the viewer moves on to the new answer", tag);
            CHECK(snap_has(3, 0, "Result 1/1") && snap_has(3, 1, "= 3"), msg);

            launch(keys_d, N(keys_d), NULL, 0);           /* quit and relaunch again */
            ok = snap_has(0, 2, "2");
            r = fk_rows(0, 3, fk, &ok);
            ok = ok && snap_has(0, r, "3") && snap_has(0, r + 1, "= 3");
            snprintf(msg, sizeof(msg), "29 %s: the error pair survives a second relaunch", tag);
            CHECK(ok, msg);
            snprintf(msg, sizeof(msg), "29 %s: every pair is saved in its own slot", tag);
            CHECK(ce_state_load(&st) && st.hist_count == 3 &&
                  strcmp(st.history_results[1], k_fk_err[fk]) == 0 &&
                  strcmp(st.history[2], "3") == 0 && strcmp(st.history_results[2], "= 3") == 0 &&
                  st.last_answer == 3.0f && settings_seeded(&st), msg);
            if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
        }
    }

    /* 30. Full ring, recovery failures (scenario-28 style): 15 seeded
     * entries plus a full worker answer fill the ring.  Then 16 distinct
     * children fail (alternately silent and invalid), returning both ring
     * indices to the full answer's values.  Every reused slot holds its
     * own error, never the old result, and the viewer never resurrects the
     * old full answer.  A following answer after a relaunch keeps them. */
    {
        static uint8_t keys[4 * CE_HISTORY_SIZE + 8];
        static answer_t ans[CE_HISTORY_SIZE + 1];
        static const uint8_t keys2[] = { S, sk_Up, S, sk_Clear, sk_1, sk_8, sk_Enter, S };
        static const answer_t ans2[] = { { true, "= 18", "", { NULL }, RUN_CE, 18.0f } };
        static const uint8_t digits[] = { sk_0, sk_1, sk_2, sk_3, sk_4,
                                          sk_5, sk_6, sk_7, sk_8, sk_9 };
        static ce_shared_state_t st;
        int n = 0, k, j;
        bool ok = true;

        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        for (k = 0; k < CE_HISTORY_SIZE - 1; k++) {
            snprintf(st.history[k], MAX_INPUT_LEN, "old%d", k);
            strcpy(st.history_results[k], "old result");
        }
        st.hist_count = CE_HISTORY_SIZE - 1;
        st.hist_write = CE_HISTORY_SIZE - 1;
        CHECK(ce_state_save(&st), "30: seed an almost-full history ring");

        memset(ans, 0, sizeof(ans));
        keys[n++] = sk_1; keys[n++] = sk_Enter;                 /* "1" fills the ring */
        ans[0].ok = true; ans[0].result = k_rank; ans[0].error = "";
        ans[0].mode = RUN_CE; ans[0].ans = 7.0f;
        for (j = 0; j < CE_HISTORY_SIZE; j++) {                 /* "2".."17" fail     */
            int e = j + 2;
            if (e >= 10) keys[n++] = digits[e / 10];
            keys[n++] = digits[e % 10];
            keys[n++] = sk_Enter;
            ans[j + 1].result = ""; ans[j + 1].error = "";
            ans[j + 1].mode = (j % 2) ? RUN_BAD_RESPONSE : RUN_NO_RESPONSE;
        }
        keys[n++] = S;                                          /* 0: home   */
        keys[n++] = sk_Up; keys[n++] = S;                       /* 1: viewer */
        launch(keys, n, ans, N(ans));
        CHECK(g_launches == CE_HISTORY_SIZE + 1, "30: every submission launched its worker");
        CHECK(ce_state_load(&st) && st.hist_count == CE_HISTORY_SIZE && st.hist_write == 0,
              "30: sixteen failed children wrap the ring back to the answer's indices");
        for (j = 0; j < CE_HISTORY_SIZE; j++) {
            char want[8];
            snprintf(want, sizeof(want), "%d", j + 2);
            if (strcmp(st.history[j], want) != 0 ||
                strcmp(st.history_results[j], k_fk_err[(j % 2) ? FK_BAD : FK_SILENT]) != 0) {
                printf("  slot %d: '%s' -> '%s'\n", j, st.history[j], st.history_results[j]);
                ok = false;
            }
        }
        CHECK(ok, "30: every reused slot holds its own child's error");
        CHECK(st.last_answer == 7.0f && settings_seeded(&st), "30: Ans and settings unchanged");
        CHECK(snap_has(0, 0, "16") && snap_has(0, 2, "IPC response") && snap_has(0, 3, "17") &&
              snap_has(0, 4, "Module CEBASE returned") && snap_has(0, 5, "invalid IPC data") &&
              !snap_contains(0, "old result") && !snap_contains(0, "Best:"),
              "30: the home screen shows the errors, never an old result");
        CHECK(snap_starts(1, 0, "Error") && snap_has(1, 1, "Module CEBASE returned") &&
              !snap_contains(1, "score") && !snap_contains(1, "Best:"),
              "30: ring wrap cannot resurrect the old full answer in the viewer");

        launch(keys2, N(keys2), ans2, N(ans2));
        CHECK(snap_has(0, 3, "17") && snap_has(0, 4, "Module CEBASE returned") &&
              snap_has(0, 5, "invalid IPC data") && snap_starts(1, 0, "Error") &&
              snap_has(1, 1, "Module CEBASE returned"),
              "30: relaunch keeps the newest error and its viewer");
        CHECK(snap_has(2, 0, "17") && snap_has(2, 1, "Module CEBASE returned") &&
              snap_has(2, 2, "invalid IPC data") && snap_has(2, 3, "18") && snap_has(2, 4, "= 18"),
              "30: the following answer leaves the earlier error in place");
        CHECK(ce_state_load(&st) && strcmp(st.history[0], "18") == 0 &&
              strcmp(st.history_results[0], "= 18") == 0 &&
              strcmp(st.history_results[15], k_fk_err[FK_BAD]) == 0 &&
              strcmp(st.history_results[14], k_fk_err[FK_SILENT]) == 0,
              "30: the new answer evicts only the oldest pair");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 30b. The same full-ring wrap on the desktop synchronous path, where
     * no re-entry resets the viewer: a full answer marks g_response valid,
     * then sixteen launches return with no response ("No module
     * response").  The answer's ring indices come back.  Three guards each
     * keep the old full answer out of the viewer: the g_response memset
     * and the s_view_valid clear at the top of dispatch_to_program(), and
     * the clear in record_local_error().  This scenario fails only when
     * all three are removed (mutation M7b). */
    {
        static uint8_t keys[4 * CE_HISTORY_SIZE + 8];
        static answer_t ans[CE_HISTORY_SIZE + 1];
        static const uint8_t digits[] = { sk_0, sk_1, sk_2, sk_3, sk_4,
                                          sk_5, sk_6, sk_7, sk_8, sk_9 };
        static ce_shared_state_t st;
        int n = 0, k, j;
        bool ok = true;

        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        for (k = 0; k < CE_HISTORY_SIZE - 1; k++) {
            snprintf(st.history[k], MAX_INPUT_LEN, "old%d", k);
            strcpy(st.history_results[k], "old result");
        }
        st.hist_count = CE_HISTORY_SIZE - 1;
        st.hist_write = CE_HISTORY_SIZE - 1;
        CHECK(ce_state_save(&st), "30b: seed an almost-full history ring");
        memset(ans, 0, sizeof(ans));
        keys[n++] = sk_1; keys[n++] = sk_Enter;
        ans[0].ok = true; ans[0].result = k_rank; ans[0].error = "";
        ans[0].mode = RUN_SYNC; ans[0].ans = 7.0f;
        for (j = 0; j < CE_HISTORY_SIZE; j++) {
            int e = j + 2;
            if (e >= 10) keys[n++] = digits[e / 10];
            keys[n++] = digits[e % 10];
            keys[n++] = sk_Enter;
            ans[j + 1].result = ""; ans[j + 1].error = "";
            ans[j + 1].mode = RUN_SYNC_SILENT;
        }
        keys[n++] = S;
        keys[n++] = sk_Up; keys[n++] = S;
        launch(keys, n, ans, N(ans));
        CHECK(ce_state_load(&st) && st.hist_count == CE_HISTORY_SIZE && st.hist_write == 0,
              "30b: sixteen silent launches wrap the ring back to the answer's indices");
        for (j = 0; j < CE_HISTORY_SIZE; j++) {
            if (strcmp(st.history_results[j], "No module response") != 0) ok = false;
        }
        CHECK(ok && st.last_answer == 7.0f && settings_seeded(&st),
              "30b: every slot holds its own error; Ans and settings unchanged");
        CHECK(snap_has(0, 4, "17") && snap_has(0, 5, "No module response") &&
              !snap_contains(0, "Best:"), "30b: the newest row shows its error");
        CHECK(snap_starts(1, 0, "Error") && snap_has(1, 1, "No module response") &&
              !snap_contains(1, "score") && !snap_contains(1, "Best:"),
              "30b: the viewer never resurrects the old full answer");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 31. Full ring, exiting failures: a full seeded ring whose saved
     * last_result is a stale answer, then four launches whose dispatch
     * fails in the four ways that make the launcher exit.  Each is a
     * following submission for the one before.  All four errors stay in
     * their own reused slots, and none of those rows shows the stale line. */
    {
        static const uint8_t k21[] = { sk_2, sk_1, sk_Enter };
        static const uint8_t k22[] = { sk_2, sk_2, sk_Enter };
        static const uint8_t k23[] = { sk_2, sk_3, sk_Enter };
        static const uint8_t k24[] = { sk_2, sk_4, sk_Enter };
        static const uint8_t *const kk[4] = { k21, k22, k23, k24 };
        static const uint8_t again[] = { S, sk_Up, S };
        static const int order[4] = { FK_IPC_WRITE, FK_STATE_SAVE, FK_MISSING, FK_LAUNCH_FAIL };
        static ce_shared_state_t st;
        static answer_t a[1];
        int k, r = 0;
        bool ok = true;

        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        st.last_answer = 5.0f;
        for (k = 0; k < CE_HISTORY_SIZE; k++) {
            snprintf(st.history[k], MAX_INPUT_LEN, "old%d", k);
            strcpy(st.history_results[k], "old result");
        }
        strcpy(st.last_result, "STALE LAST");
        st.hist_count = CE_HISTORY_SIZE;
        st.hist_write = 3;
        CHECK(ce_state_save(&st), "31: seed a full history ring");
        for (k = 0; k < 4; k++) {
            a[0] = fk_arm(order[k]);
            launch(kk[k], 3, a, 1);
            if (order[k] == FK_MISSING) fileioc_mock_add_program("CEBASE", 1);
        }
        CHECK(ce_state_load(&st) && st.hist_count == CE_HISTORY_SIZE && st.hist_write == 7,
              "31: four failures reuse four slots");
        for (k = 0; k < 4; k++) {
            char want[4];
            snprintf(want, sizeof(want), "2%d", k + 1);
            if (strcmp(st.history[3 + k], want) != 0 ||
                strcmp(st.history_results[3 + k], k_fk_err[order[k]]) != 0) {
                printf("  slot %d: '%s' -> '%s'\n", 3 + k, st.history[3 + k],
                       st.history_results[3 + k]);
                ok = false;
            }
        }
        CHECK(ok, "31: every exiting failure saved its own error");
        CHECK(st.last_answer == 5.0f && settings_seeded(&st), "31: Ans and settings unchanged");
        launch(again, N(again), NULL, 0);
        /* Three two-row pairs fill the transcript.  The state check above covers "21". */
        ok = snap_has(0, r, "22"); r = fk_rows(0, r + 1, FK_STATE_SAVE, &ok);
        ok = ok && snap_has(0, r, "23"); r = fk_rows(0, r + 1, FK_MISSING, &ok);
        ok = ok && snap_has(0, r, "24"); r = fk_rows(0, r + 1, FK_LAUNCH_FAIL, &ok);
        CHECK(ok && !snap_contains(0, "STALE LAST") && !snap_contains(0, "old result"),
              "31: relaunch shows the newest errors in order, no stale line");
        CHECK(snap_starts(1, 0, "Error") && snap_has(1, 1, "Program launch failed"),
              "31: the viewer shows the newest failure");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 32. Recovery binding edges (review round 1 P3).  (a) Resubmitting the
     * newest expression shares its entry, so a failing child's error
     * replaces that entry's result and adds no row.  (b) A saved state
     * that lacks the failed request's entry (the defensive history_push in
     * main()): the error gets a new row of its own, and the older pair is
     * untouched. */
    {
        static const uint8_t keys_a[] = { sk_5, sk_Enter, sk_5, sk_Enter, S, sk_Up, S };
        static const answer_t ans_a[] = {
            { true, "= 5", "", { NULL }, RUN_CE, 5.0f },
            { false, "", "", { NULL }, RUN_NO_RESPONSE, 0.0f }
        };
        static const uint8_t keys_b[] = { S, sk_Up, S };
        static ce_shared_state_t st;
        static ce_request_t rq;

        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        ce_state_save(&st);
        launch(keys_a, N(keys_a), ans_a, N(ans_a));
        CHECK(g_launches == 2 && ce_state_load(&st) && st.hist_count == 1 &&
              strcmp(st.history[0], "5") == 0 &&
              strcmp(st.history_results[0], k_fk_err[FK_SILENT]) == 0,
              "32a: a resubmitted expression's failing child replaces its result, no new row");
        CHECK(st.last_answer == 5.0f && settings_seeded(&st), "32a: Ans and settings unchanged");
        CHECK(snap_has(0, 0, "5") && snap_has(0, 1, "Module CEBASE returned no") &&
              snap_has(0, 2, "IPC response") && snap_starts(1, 0, "Error"),
              "32a: the row and the viewer show the error");

        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        st.last_answer = 8.0f;
        strcpy(st.history[0], "8");
        strcpy(st.history_results[0], "= 8");
        strcpy(st.last_result, "= 8");
        st.hist_count = 1;
        st.hist_write = 1;
        strcpy(st.pending_module, "CEBASE");      /* a dispatch to CEBASE was in flight */
        ce_state_save(&st);
        memset(&rq, 0, sizeof(rq));
        rq.magic = CE_IPC_MAGIC;
        rq.version = CE_IPC_VERSION;
        strcpy(rq.caller, "CALCENG");
        strcpy(rq.return_to, "CALCENG");
        strcpy(rq.expression, "9");               /* ...for an entry the state lacks */
        rq.settings = st.settings;                /* as dispatch_to_program() sends them */
        rq.last_answer = st.last_answer;
        CHECK(ce_request_write(&rq), "32b: seed a request the saved history lacks");
        launch(keys_b, N(keys_b), NULL, 0);
        CHECK(ce_state_load(&st) && st.hist_count == 2 && strcmp(st.history[0], "8") == 0 &&
              strcmp(st.history_results[0], "= 8") == 0 && strcmp(st.history[1], "9") == 0 &&
              strcmp(st.history_results[1], k_fk_err[FK_SILENT]) == 0,
              "32b: recovery gives the error its own new row, the older pair is untouched");
        CHECK(st.last_answer == 8.0f && settings_seeded(&st) && !st.pending_module[0],
              "32b: Ans and settings unchanged, pending module cleared");
        CHECK(snap_has(0, 0, "8") && snap_has(0, 1, "= 8") && snap_has(0, 2, "9") &&
              snap_has(0, 3, "Module CEBASE returned no") && snap_starts(1, 0, "Error"),
              "32b: the home screen and the viewer show it");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 33. review round 1 P2: a worker evaluates, updates Ans and settings, then
     * leaves an unreadable CE_RESP.  ce_module_runtime_finish() has saved
     * the new values in CE_STATE, but the failed submission must leave Ans
     * and settings as they were, also after a relaunch. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, sk_2, sk_Enter, S };
        static const uint8_t again[] = { S };
        static answer_t ans[2];
        static ce_shared_state_t st;

        memset(ans, 0, sizeof(ans));
        ans[0].ok = true; ans[0].result = "= 7"; ans[0].error = "";
        ans[0].mode = RUN_CE; ans[0].ans = 7.0f;
        ans[1].ok = true; ans[1].result = "= 2"; ans[1].error = "";
        ans[1].mode = RUN_BAD_RESPONSE; ans[1].ans = 2.0f; ans[1].clobber = true;
        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        ce_state_save(&st);
        launch(keys, N(keys), ans, N(ans));
        CHECK(ce_state_load(&st) && st.hist_count == 2 &&
              strcmp(st.history_results[1], k_fk_err[FK_BAD]) == 0,
              "33: the invalid response is recorded in its own slot");
        CHECK(st.last_answer == 7.0f && settings_seeded(&st) &&
              g_last_answer == 7.0f && g_settings.precision == 4,
              "33: the failed worker's new Ans and settings are discarded");
        CHECK(snap_has(0, 2, "2") && snap_has(0, 3, "Module CEBASE returned") &&
              !snap_contains(0, "= 2"), "33: the screen shows the error, not the lost answer");
        launch(again, N(again), NULL, 0);
        CHECK(ce_state_load(&st) && st.last_answer == 7.0f && settings_seeded(&st) &&
              g_last_answer == 7.0f && g_settings.precision == 4,
              "33: Ans and settings stay restored after a relaunch");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 34. review round 2 P2: the failing child sits behind a forwarder.  The
     * forwarder recovers and sends a valid error response, so CALCENG's own
     * recovery never runs.  The shared recovery must already have put back
     * the forwarder's request Ans and settings, which CALCENG then adopts
     * from that response.  The error is a worker answer in its own slot. */
    {
        static const uint8_t keys[] = { sk_1, sk_Enter, sk_2, sk_Enter, S, sk_Up, S };
        static const uint8_t again[] = { S };
        static answer_t ans[2];
        static ce_shared_state_t st;

        memset(ans, 0, sizeof(ans));
        ans[0].ok = true; ans[0].result = "= 7"; ans[0].error = "";
        ans[0].mode = RUN_CE; ans[0].ans = 7.0f;
        ans[1].ok = true; ans[1].result = "= 2"; ans[1].error = "";
        ans[1].mode = RUN_FORWARD_BAD; ans[1].ans = 2.0f; ans[1].clobber = true;
        reset_world(true);
        ce_state_defaults(&st);
        seed_settings(&st);
        ce_state_save(&st);
        g_forward_resumed = true;
        launch(keys, N(keys), ans, N(ans));
        CHECK(!g_forward_resumed, "34: the forwarder refuses to resume the unreadable response");
        CHECK(ce_state_load(&st) && st.hist_count == 2 &&
              strcmp(st.history_results[1], "Module CECLMB returned invalid IPC data") == 0,
              "34: the forwarder's error is stored in its own slot");
        CHECK(st.last_answer == 7.0f && settings_seeded(&st) &&
              g_last_answer == 7.0f && g_settings.precision == 4,
              "34: a failed grandchild's new Ans and settings are discarded");
        CHECK(snap_has(0, 2, "2") && snap_has(0, 3, "Module CECLMB returned") &&
              snap_has(0, 4, "invalid IPC data") && snap_starts(1, 0, "Error"),
              "34: the screen and the viewer show the error");
        launch(again, N(again), NULL, 0);
        CHECK(ce_state_load(&st) && st.last_answer == 7.0f && settings_seeded(&st),
              "34: Ans and settings stay unchanged after a relaunch");
        if (g_failures) { int s; for (s = 0; s < g_snap_count; s++) { printf("  snap %d\n", s); dump(s); } }
    }

    /* 35. Stored variables.  The launcher and a module built
     * without CE_EVAL_USER_VARS (this worker mock) carry the table in
     * CE_STATE through unchanged; it survives a relaunch.  MODE shows the
     * count on "Stored vars"; RIGHT shows CLR; ENTER clears the table,
     * CLEAR leaves it.  With nothing stored, RIGHT stays at 0. */
    {
        static const uint8_t keys1[] = { sk_1, sk_Enter, S };
        static const uint8_t keys_menu[] = {
            sk_Mode, S,                                          /* 0: menu        */
            sk_Down, sk_Down, sk_Down, sk_Down, sk_Down, S,      /* 1: Stored vars */
            sk_Right, S,                                         /* 2: CLR         */
            sk_Clear                                             /* cancel         */
        };
        static const uint8_t keys_clear[] = {
            sk_Mode, sk_Down, sk_Down, sk_Down, sk_Down, sk_Down,
            sk_Right, S,                                         /* 0: CLR         */
            sk_Enter, S                                          /* 1: home        */
        };
        static const uint8_t keys_empty[] = {
            sk_Mode, sk_Down, sk_Down, sk_Down, sk_Down, sk_Down,
            sk_Right, S                                          /* 0: still 0     */
        };
        static const answer_t ans[] = { { true, "= 1", "", { NULL }, RUN_CE } };
        static ce_shared_state_t st;
        bool loaded;

        reset_world(true);
        ce_state_defaults(&st);
        st.user_vars.value['X' - 'A'] = 5.0f;
        st.user_vars.value['A' - 'A'] = -2.5f;
        st.user_vars.mask[('X' - 'A') >> 3] |= (uint8_t)(1u << (('X' - 'A') & 7));
        st.user_vars.mask[0] |= 1u;
        ce_state_save(&st);

        launch(keys1, N(keys1), ans, N(ans));
        memset(&st, 0, sizeof(st));
        loaded = ce_state_load(&st);
        CHECK(g_launches == 1 && loaded &&
              (st.user_vars.mask[2] & 0x80u) && st.user_vars.value['X' - 'A'] == 5.0f &&
              (st.user_vars.mask[0] & 1u) && st.user_vars.value[0] == -2.5f,
              "35: a worker round trip keeps the stored values");

        launch(keys_menu, N(keys_menu), NULL, 0);   /* a relaunch */
        CHECK(snap_contains(0, "Stored vars") && snap_has(0, 8, "ENTER=save CLEAR=exit"),
              "35: MODE lists Stored vars, footer on row 8");
        CHECK(snap_has(1, 7, ">Stored vars \xC1  2]"), "35: after a relaunch the count is 2");
        CHECK(snap_has(2, 7, ">Stored vars \xC1" "CLR]"), "35: RIGHT shows CLR");
        memset(&st, 0, sizeof(st));
        CHECK(ce_state_load(&st) && (st.user_vars.mask[2] & 0x80u) &&
              st.user_vars.value['X' - 'A'] == 5.0f, "35: CLEAR cancels: values kept");
        CHECK(g_full_on_last_row == 0 && g_full_wrap_to_last_row == 0 && g_scrolls == 0,
              "35: the 6-row menu never writes the last row or scrolls");

        launch(keys_clear, N(keys_clear), NULL, 0);
        memset(&st, 0, sizeof(st));
        CHECK(ce_state_load(&st) && st.user_vars.mask[0] == 0 && st.user_vars.mask[1] == 0 &&
              st.user_vars.mask[2] == 0 && st.user_vars.mask[3] == 0,
              "35: ENTER on CLR clears every stored value");
        CHECK(snap_has(0, 7, ">Stored vars \xC1" "CLR]") && !snap_contains(1, "Stored vars"),
              "35: ENTER saves and returns home");

        launch(keys_empty, N(keys_empty), NULL, 0);
        CHECK(snap_has(0, 7, ">Stored vars \xC1  0]"), "35: nothing stored: RIGHT stays at 0");
        if (g_failures) { int s2; for (s2 = 0; s2 < g_snap_count; s2++) { printf("  snap %d\n", s2); dump(s2); } }
    }

    /* 36. The desktop synchronous path adopts worker-written scalar state. */
    {
        static const uint8_t keys[] = { sk_5, sk_Enter, S };
        static const uint8_t again[] = { S };
        static const answer_t ans[] = {
            { true, "= 5", "", { NULL }, RUN_SYNC, 5.0f, false, true }
        };
        static ce_shared_state_t st;
        reset_world(true);
        launch(keys, N(keys), ans, N(ans));
        CHECK(ce_state_load(&st) && (st.user_vars.mask[2] & 0x80u) &&
              st.user_vars.value['X' - 'A'] == 5.0f,
              "36: synchronous worker store survives the response save");
        launch(again, N(again), NULL, 0);
        CHECK(ce_state_load(&st) && (st.user_vars.mask[2] & 0x80u) &&
              st.user_vars.value['X' - 'A'] == 5.0f,
              "36: synchronous worker store survives relaunch");
    }

    /* 37. Failed MODE edits restore memory without a fallible reload. */
    {
        uint8_t keys[] = {
            sk_Mode, sk_Right, sk_Down, sk_Down, sk_Down, sk_Down, sk_Down,
            sk_Right, KEY_FAIL_STATE_WRITE, sk_Enter, S, KEY_CLEAR_FAULTS,
            sk_Mode, sk_Down, sk_Down, sk_Down, sk_Down, sk_Down, S,
            sk_Enter, sk_1, sk_Enter, S
        };
        static const answer_t ans[] = { { true, "= 1", "", { NULL }, RUN_CE } };
        static ce_shared_state_t st;
        int fault_case;
        for (fault_case = 0; fault_case < 2; fault_case++) {
            reset_world(true);
            ce_state_defaults(&st);
            st.user_vars.mask[2] = 0x80u;
            st.user_vars.value['X' - 'A'] = 5.0f;
            CHECK(ce_state_save(&st), "37: seed X=5");
            keys[8] = fault_case ? KEY_FAIL_STATE_AND_READ : KEY_FAIL_STATE_WRITE;
            launch(keys, N(keys), ans, N(ans));
            CHECK(snap_contains(0, "Settings save failed"), "37: failed CLR reports save failure");
            CHECK(snap_has(1, 7, ">Stored vars \xC1  1]"), "37: same-session menu restores count");
            CHECK(ce_state_load(&st) && st.user_vars.mask[2] == 0x80u &&
                  st.user_vars.value['X' - 'A'] == 5.0f,
                  "37: following settings save and worker preserve X");
            CHECK(st.settings.show_steps && g_settings.show_steps,
                  "37: failed setting edit restores shared and live settings");
            CHECK(g_launches == 1, "37: following worker runs");
        }
    }

    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
