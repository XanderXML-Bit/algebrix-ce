/*
 * main.c — CalcEngine Launcher (CALCENG)
 *
 * Entry point for the TI-84 Plus CE CalcEngine launcher program.
 * This module owns the home-screen UI: custom key scanning, history
 * display, and dispatch to the 43 worker modules via CE_REQ/CE_RESP
 * AppVar IPC.
 *
 * Phase 1 UX changes (v1.1.0):
 *   - Replaced os_GetStringInput with a custom os_GetCSC() keyscan loop.
 *     No more TI-OS home-screen handler side-effects (MODE key no longer
 *     inserts "Float"; ÷ key no longer triggers TI-OS expression eval).
 *   - Removed "Any key=continue" pause after showing results.
 *   - Screen layout: history pairs (expression + result) at top,
 *     plain-text input line with "|" cursor at bottom.
 *   - ENTRY recall: 2nd+ENTER copies last expression into input buffer.
 *   - MODE: stub mode menu ("Mode menu v1.2 coming"). 2nd+MODE = quit.
 *   - Plain UP/DOWN do not mutate the input line.
 *   - Transcript wrap: expressions and results continue on
 *     the next rows, at most 3 each; a cut field ends in the TI ellipsis
 *     (home_layout.h).  The input line marks scrolled-off text the same way.
 *   - Result viewer: UP (or ENTER on an empty input line)
 *     opens the newest result in full, wrapped at 26 columns, with its
 *     steps when the worker sent any (result_view.h).  A hint row
 *     "UP=full result (N ln)" appears under the newest result whenever
 *     the viewer holds more than the home rows show.
 *
 * Key map summary:
 *   Normal:  0-9 +−×÷ ^ ( ) . , X → sin( cos( tan( ln( log( ^2 ^(-1)
 *            ENTER=submit  CLEAR=clear  DEL=backspace  ←/→=cursor
 *            ↑=full newest result (viewer)  ↓=no-op  MODE=menu
 *            ENTER on empty input = viewer
 *   Viewer:  ↑/↓=line  ←/→=page  CLEAR/ENTER/MODE=back
 *            TRACE opens the same viewer (result, then its steps)
 *   2nd:     ENTER=recall  CLEAR=clear history  MODE=quit
 *            SIN=asin(  COS=acos(  TAN=atan(
 *            LN=e  x²=sqrt(  ^=pi  DEL=fwd-delete
 *   Alpha:   A-Z as printed on the TI-84 CE keycaps (A=MATH … M=÷,
 *            N=LOG … Z=2), 0=space; θ " : ? are no-ops
 */

#include <tice.h>
#include <ti/getcsc.h>
#include <ti/screen.h>

#include <stdbool.h>
#include <stdint.h>
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
#include "core/launcher/result_view.h"
#include "core/launcher/home_layout.h"

#if (MAX_STEPS + 2) > RV_MAX_SEGS
#error "result_view.h RV_MAX_SEGS must hold the answer, \"Steps:\" and MAX_STEPS steps"
#endif

/* ── Layout ────────────────────────────────────────────────────── */
#define HOME_ROWS       8
#define HOME_COLS       26
/* History display: up to 3 entries in rows 0-6, wrapped (home_layout.h) */
#define HIST_DISPLAY    3
/* Input row at the bottom */
#define INPUT_ROW       7
/* Prefix "> " occupies 2 columns */
#define INPUT_PREFIX    2
/* Transient message (e.g. "Syntax error") just under the input line; rows 8
 * and 9 are otherwise unused on the home screen. */
#define STATUS_ROW      (INPUT_ROW + 1)

#if HL_ROWS != INPUT_ROW || HL_MAX_ENTRIES != HIST_DISPLAY || RV_COLS != HOME_COLS
#error "home_layout.h must match the launcher layout (rows above the input row, 3 entries, 26 columns)"
#endif

/* ── Program name constants ─────────────────────────────────────── */
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

/* ── Global state ───────────────────────────────────────────────── */
static ce_shared_state_t g_shared;
static tokenizer_t       g_tokenizer;
static parser_t          g_parser;
static ce_request_t      g_request;
static ce_response_t     g_response;

int main(void);
static int launcher_reentry(void *data, int retval);
static bool              g_quit_requested;

/* Static workspace for child-failure recovery path (PR #40 fix).
 * ce_request_t (4273 B) as a stack local in main() combined with the
 * 4510-byte ce_response_t probe in ce_module_runtime_recover_child_failure
 * overflows the TI-84 CE stack on iteration-2 re-entry. */
static ce_request_t s_pending_req;

/* ── Input buffer ───────────────────────────────────────────────── */
static char     s_input[MAX_INPUT_LEN];
static uint8_t  s_cursor;   /* byte offset of cursor in s_input        */
static uint8_t  s_ilen;     /* current length of s_input (excl. NUL)   */

/* Transient home-screen message under the input line.  Set when ENTER fails
 * to parse (the typed input is kept, like ERR:SYNTAX on a TI-84) and cleared
 * by the next key ce_input_scan() processes. */
static char     s_status[HOME_COLS + 1];
/* Byte offset in s_input where the tokenizer stopped on a parse error, or
 * PARSE_NO_POS when it gives none (parser errors carry no source offset). */
#define PARSE_NO_POS 0xFFu
static uint8_t  s_parse_err_pos = PARSE_NO_POS;

/* Saved expression for UP/DOWN history navigation */
static char     s_hist_saved[MAX_INPUT_LEN];
static int8_t   s_hist_nav; /* -1 = not navigating; 0..N = nav index   */
/* Phase 2: ENTRY recall cycling index.
 * -1 = not in entry-recall mode.
 *  0 = most recent history entry, 1 = second most recent, etc.
 * Each 2nd+ENTER press while in recall mode advances one step deeper. */
static int8_t   s_entry_idx; /* initialised to -1 in main() */

/* Result viewer.  g_response keeps the newest worker answer in full until the
 * next evaluation (or until CALCENG exits).  s_view_valid says it belongs to
 * the result the home screen shows as newest: it is set right after that
 * answer is stored in the newest history slot, and it holds only while that
 * slot is still the newest one (s_view_hist_write/s_view_hist_count) and
 * still non-empty.  Dispatch, a fresh launch and CLEAR CLEAR clear it.
 * Otherwise the viewer shows the stored first line (63 characters). */
static bool     s_view_valid;
static bool     s_view_dirty = true;
static uint16_t s_view_lines;
static bool     s_view_hint;
static rv_doc_t s_view_doc;
static uint8_t  s_view_hist_write;
static uint8_t  s_view_hist_count;

/* ── Input buffer helpers ───────────────────────────────────────── */

static void input_clear(void)
{
    memset(s_input, 0, sizeof(s_input));
    s_cursor = 0;
    s_ilen   = 0;
}

/* Insert 'count' bytes from 'text' at the current cursor position.
 * Returns false if the buffer would overflow. */
static bool input_insert(const char *text, uint8_t count)
{
    if (!text || count == 0) return true;
    if ((uint8_t)(s_ilen + count) >= MAX_INPUT_LEN) return false;
    /* shift tail right */
    memmove(s_input + s_cursor + count,
            s_input + s_cursor,
            (uint8_t)(s_ilen - s_cursor + 1)); /* +1 for NUL */
    memcpy(s_input + s_cursor, text, count);
    s_cursor = (uint8_t)(s_cursor + count);
    s_ilen   = (uint8_t)(s_ilen + count);
    return true;
}

static bool input_insert_char(char c)
{
    return input_insert(&c, 1);
}

static bool input_insert_str(const char *s)
{
    return input_insert(s, (uint8_t)strlen(s));
}

/* Backspace: delete char before cursor */
static void input_backspace(void)
{
    if (s_cursor == 0) return;
    memmove(s_input + s_cursor - 1,
            s_input + s_cursor,
            (uint8_t)(s_ilen - s_cursor + 1));
    s_cursor--;
    s_ilen--;
}

/* Forward delete: delete char at cursor */
static void input_delete(void)
{
    if (s_cursor >= s_ilen) return;
    memmove(s_input + s_cursor,
            s_input + s_cursor + 1,
            (uint8_t)(s_ilen - s_cursor));
    s_ilen--;
    s_input[s_ilen] = '\0';
}

static void input_cursor_left(void)
{
    if (s_cursor > 0) s_cursor--;
}

static void input_cursor_right(void)
{
    if (s_cursor < s_ilen) s_cursor++;
}

/* Load an expression from the history ring buffer at nav index.
 * index 0 = most recent, 1 = second most recent, etc. */
static bool input_load_history(uint8_t index)
{
    uint8_t ring_idx;
    if (index >= g_shared.hist_count) return false;
    ring_idx = (uint8_t)((g_shared.hist_write + CE_HISTORY_SIZE - 1 - index)
                         % CE_HISTORY_SIZE);
    input_clear();
    strncpy(s_input, g_shared.history[ring_idx], MAX_INPUT_LEN - 1);
    s_input[MAX_INPUT_LEN - 1] = '\0';
    s_ilen   = (uint8_t)strnlen(s_input, MAX_INPUT_LEN - 1);
    s_cursor = s_ilen; /* cursor at end */
    return true;
}

/* ── Result viewer source ───────────────────────────────────────── */

static uint8_t view_newest_slot(void)
{
    return (uint8_t)((g_shared.hist_write + CE_HISTORY_SIZE - 1) % CE_HISTORY_SIZE);
}

/* The newest result as shown on the home screen (same fallback as home_draw).
 * With no history the home screen shows no result at all, so neither does
 * the viewer, whatever last_result still holds. */
static const char *view_newest_stored(void)
{
    uint8_t slot;
    if (g_shared.hist_count == 0) return "";
    slot = view_newest_slot();
    if (g_shared.history_results[slot][0]) return g_shared.history_results[slot];
    return g_shared.last_result;
}

/* Does g_response still belong to the newest result on the home screen? */
static bool view_response_current(void)
{
    return s_view_valid &&
           g_shared.hist_count > 0 &&
           g_shared.hist_count == s_view_hist_count &&
           g_shared.hist_write == s_view_hist_write &&
           g_shared.history_results[view_newest_slot()][0] != '\0';
}

/* Build the viewer document: the full newest answer (result or error, then
 * its steps) when g_response holds it, otherwise the stored first line. */
static void view_build(rv_doc_t *doc)
{
    rv_doc_init(doc);
    if (view_response_current()) {
        if (g_response.ok) {
            rv_doc_add(doc, g_response.result, sizeof(g_response.result));
        } else {
            rv_doc_add(doc, g_response.error, sizeof(g_response.error));
        }
        /* Steps follow MODE "Show work" (settings_t.show_steps). */
        if (doc->count && g_settings.show_steps &&
            g_response.show_steps && g_response.step_count) {
            uint8_t k, n = g_response.step_count;
            if (n > MAX_STEPS) n = MAX_STEPS;
            rv_doc_add(doc, "Steps:", 7);
            for (k = 0; k < n; k++) {
                rv_doc_add(doc, g_response.steps[k], sizeof(g_response.steps[k]));
            }
        }
    }
    if (doc->count == 0) {
        rv_doc_add(doc, view_newest_stored(), sizeof(g_shared.last_result));
    }
}

/* Line count of the viewer document and whether the home screen should
 * point at it (cached; s_view_dirty forces a rebuild). */
static uint16_t view_line_count(void)
{
    if (s_view_dirty) {
        view_build(&s_view_doc);
        s_view_lines = rv_count_lines(&s_view_doc);
        /* No hint when the only hidden part is the worker's "Parsed:" echo. */
        /* The home screen wraps the stored first line over up to
         * HL_RES_MAX_ROWS rows (home_layout.h); hint only when the viewer
         * holds more than that, and not when the only hidden part is the
         * worker's "Parsed:" echo. */
        {
            const char *stored = view_newest_stored();
            size_t  slen  = strnlen(stored, sizeof(g_shared.last_result));
            uint8_t shown = hl_field_rows(stored, sizeof(g_shared.last_result),
                                          HL_RES_MAX_ROWS, NULL);
            /* The stored copy ends in the ellipsis when the first line was
             * over 63 bytes: then part of it is hidden even if the viewer
             * needs no more rows than the home screen shows.  That counts
             * only while the viewer holds the full text (after a relaunch it
             * shows the same stored copy). */
            /* (A worker line whose 63rd byte really is 0xCE would also
             * read as cut; the TI font has no reason to send that.) */
            bool cut = slen == 63 && stored[62] == HL_MORE &&
                       view_response_current();
            s_view_hint = (cut || s_view_lines > shown) &&
                          !(!cut && s_view_doc.count == 1 &&
                            hl_hidden_is_only_echo(s_view_doc.text[0], s_view_doc.cap[0]));
        }
        s_view_dirty = false;
    }
    return s_view_lines;
}

static bool view_hint_wanted(void)
{
    (void)view_line_count();
    return s_view_hint;
}

/* g_response now holds the full text of the answer just stored in the
 * newest history slot. */
static void view_mark_response(void)
{
    s_view_valid      = true;
    s_view_hist_write = g_shared.hist_write;
    s_view_hist_count = g_shared.hist_count;
    s_view_dirty      = true;
}

/* ── Screen rendering ───────────────────────────────────────────── */

/* TI-OS large-font codes differ from ASCII for '[': 0x5B draws theta,
 * and the left bracket glyph is 0xC1 (']' at 0x5D is the same in both;
 * both checked on the real OS 5.8 ROM in CEmu, 2026-11-09). Steps,
 * matrix and interval text all use ASCII '[', so map it at print time. */
#define TI_CHAR_LBRACKET '\xC1'

static void ti_fix_chars(char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len && buf[i]; i++) {
        if (buf[i] == '[') buf[i] = TI_CHAR_LBRACKET;
    }
}

/* Print exactly one home-screen row (26 chars, NUL-padded if shorter,
 * clipped if longer). Caller must position cursor first. */
static void home_print_row(const char *text)
{
    char buf[HOME_COLS + 1];
    size_t len;
    if (!text) text = "";
    len = strnlen(text, HOME_COLS);
    memcpy(buf, text, len);
    ti_fix_chars(buf, len);
    /* Pad remainder with spaces so previous content is overwritten */
    while (len < HOME_COLS) buf[len++] = ' ';
    buf[HOME_COLS] = '\0';
    os_PutStrFull(buf);
}

/* Render the full home screen (called after every state change). */
static void home_draw(void)
{
    uint8_t row = 0;
    uint8_t show_count;
    int8_t  i;

    os_ClrHome();

    /* ── History pairs ────────────────────────────────────────── */
    /* Expressions and results wrap over several rows (see home_layout.h).
     * Rows 0..HL_ROWS-1 sit above the input row. */
    {
        static char rows[HL_ROWS][RV_COLS + 1];
        hl_entry_t  ent[HIST_DISPLAY];
        char        hint[RV_COLS + 1];
        uint8_t     r;

        show_count = g_shared.hist_count;
        if (show_count > HIST_DISPLAY) show_count = HIST_DISPLAY;

        hint[0] = '\0';
        for (i = 0; i < (int8_t)show_count; i++) {
            /* Entry: 0 = most recent */
            uint8_t ring_idx = (uint8_t)((g_shared.hist_write + CE_HISTORY_SIZE - 1
                                          - (uint8_t)i) % CE_HISTORY_SIZE);
            /* Result: per-entry stored result (Phase 2).  Falls back to
             * last_result for the most-recent entry if the per-entry slot is
             * empty (e.g. loading state from older firmware). */
            const char *res = g_shared.history_results[ring_idx];
            if (!res[0] && i == 0) res = g_shared.last_result;
            ent[i].expr     = g_shared.history[ring_idx];
            ent[i].expr_cap = MAX_INPUT_LEN;
            ent[i].res      = res;
            ent[i].res_cap  = sizeof(g_shared.last_result);
            ent[i].hint     = NULL;
            /* Under the newest result, say when part of it is hidden. */
            if (i == 0 && res[0] && view_hint_wanted() &&
                rv_home_hint(view_line_count(), hint)) {
                ent[i].hint = hint;
            }
        }
        row = hl_layout(ent, show_count, rows);
        for (r = 0; r < row; r++) {
            os_SetCursorPos(r, 0);
            home_print_row(rows[r]);
        }
    }

    /* ── Input line at row INPUT_ROW ──────────────────────────── */
    {
        /* Build "> " + visible_window_of_input with "|" cursor.
         * Window is HOME_COLS - INPUT_PREFIX chars wide. */
        uint8_t avail   = HOME_COLS - INPUT_PREFIX;
        uint8_t win_start = 0;
        uint8_t pos;
        char    ibuf[HOME_COLS + 2]; /* +2 for cursor marker */
        uint8_t k;

        /* Scroll window so cursor is always visible.  With text right of
         * the cursor, keep the last column free for that text (or for the
         * ellipsis that says it continues). */
        {
            uint8_t reserve = (s_cursor < s_ilen) ? 1u : 0u;
            if ((uint16_t)s_cursor + reserve >= avail) {
                win_start = (uint8_t)(s_cursor - avail + 1 + reserve);
            }
        }

        ibuf[0] = '>';
        ibuf[1] = ' ';
        pos = 2;

        for (k = win_start; k <= s_ilen && pos < HOME_COLS; k++) {
            if (k == s_cursor && pos < HOME_COLS) {
                ibuf[pos++] = '|'; /* cursor marker */
                if (pos >= HOME_COLS) break;
            }
            if (k < s_ilen) {
                ibuf[pos++] = s_input[k];
            }
        }
        /* If cursor is at end and we haven't placed the marker yet */
        if (s_cursor == s_ilen && pos < HOME_COLS) {
            /* marker may already be placed by the loop above */
            /* check: if cursor == s_ilen, the loop exited with k==s_ilen
             * and placed '|' then skipped the char branch. Fine. */
        }
        ibuf[pos] = '\0';

        /* Mark hidden parts of a long input line: the TI
         * ellipsis replaces the prompt's space when the start has scrolled
         * off, and the last column when text runs past the right edge
         * (unless the cursor sits there). */
        if (win_start > 0) ibuf[1] = HL_MORE;
        if (pos >= HOME_COLS && ibuf[HOME_COLS - 1] != '|' &&
            k < s_ilen) {
            ibuf[HOME_COLS - 1] = HL_MORE;
        }

        os_SetCursorPos(INPUT_ROW, 0);
        home_print_row(ibuf);
    }

    /* ── Transient message (parse error) under the input line ─── */
    /* os_PutStrLine, not home_print_row: on OS 5.8 a 26-column
     * os_PutStrFull on row 8 wraps the cursor onto row 9 and scrolls the
     * whole home screen up one row. This was observed on the real ROM.
     * os_ClrHome above already blanked the row. */
    if (s_status[0]) {
        os_SetCursorPos(STATUS_ROW, 0);
        os_PutStrLine(s_status);
    }
}

/* Show a one-line status message on the input row (for errors/info)
 * and redraw on next keypress. */
static void home_show_status(const char *msg)
{
    char line[HOME_COLS + 1];
    snprintf(line, sizeof(line), "%-26s", msg ? msg : "");
    os_SetCursorPos(INPUT_ROW, 0);
    os_PutStrFull(line);
}

/* ── Routing helpers ────────────────────────────────────────────── */

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

    /* CEMAT handles both (processor.c handle_eigen); classify.c agrees. */
    case POLOP_EIGENVAL:
    case POLOP_EIGENVEC:
        return CE_PRGM_MAT;

    case POLOP_ODE_SEP:
    case POLOP_ODE_LINEAR:
    case POLOP_DIFFEQ:
    case POLOP_INTPARTS:
    case POLOP_TRIGSUB:
    case POLOP_USUB:
    case POLOP_INTSTRATEGY:
    /* CECINT forwards these two to CECIDF, the definite-integral worker.
     * CEBASE has no integrator and previously printed "Polar/advanced op". */
    case POLOP_IMPROPER:
    case POLOP_AREABETWEEN:
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

static const char *route_program(const ast_node_t *root,
                                  bool input_had_equals,
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
        return is_finance_stat_fn(root->d.stat.fn) ? CE_PRGM_FIN
                                                    : CE_PRGM_STATPROB;

    case NODE_MATRIX_OP:
    case NODE_MATRIX:
        return CE_PRGM_MAT;

    case NODE_POLAR:
        return route_polar_program(root->d.polar.op);

    case NODE_STORE:
        /* classify.c: a scalar store of a plain real value is CORE and is
         * evaluated (and refused when tiny, #140) by CEBASE. */
        if (domain == CE_DOMAIN_CORE) return CE_PRGM_BASE;
        return CE_PRGM_LISTCMPLX;

    case NODE_LIST_LITERAL:
    case NODE_LIST_REF:
    case NODE_LIST_OP:
    case NODE_LIST_ELEM:
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

static bool parse_for_domain(const char *input,
                              ast_node_t **out_root,
                              bool *out_had_equals,
                              ce_domain_t *out_domain)
{
    ast_node_t *root = NULL;
    const char *p;

    if (!input || !input[0] || !out_root || !out_had_equals || !out_domain)
        return false;

    *out_root     = NULL;
    *out_had_equals = false;
    *out_domain   = CE_DOMAIN_UNKNOWN;
    s_parse_err_pos = PARSE_NO_POS;

    for (p = input; *p; p++) {
        if (*p == '=') { *out_had_equals = true; break; }
    }

    mem_free_all();
    tok_init(&g_tokenizer, input);
    if (tok_tokenize(&g_tokenizer) != CE_OK) {
        /* The tokenizer says where it stopped: the unknown character or
         * the start of the unknown word (token.c err_pos). */
        if ((g_tokenizer.error == CE_ERR_SYNTAX ||
             g_tokenizer.error == CE_ERR_PRECISION ||
             g_tokenizer.error == CE_ERR_OVERFLOW) &&
            g_tokenizer.err_pos < g_tokenizer.len &&
            g_tokenizer.err_pos < MAX_INPUT_LEN - 1)
            s_parse_err_pos = (uint8_t)g_tokenizer.err_pos;
        return false;
    }

    parser_init(&g_parser, &g_tokenizer);
    root = parser_parse(&g_parser);
    if (!root) return false;

    *out_domain = ce_classify_expression(root, *out_had_equals);
    *out_root   = root;
    return true;
}

/* ── State helpers ──────────────────────────────────────────────── */

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
        last = (uint8_t)((g_shared.hist_write + CE_HISTORY_SIZE - 1)
                          % CE_HISTORY_SIZE);
        if (strncmp(g_shared.history[last], expr, MAX_INPUT_LEN - 1) == 0)
            return;
    }
    strncpy(g_shared.history[g_shared.hist_write], expr, MAX_INPUT_LEN - 1);
    g_shared.history[g_shared.hist_write][MAX_INPUT_LEN - 1] = '\0';
    /* The ring reuses slots: drop the result of the entry 16 back.  Every
     * answer and every local error is then written into this slot (never a
     * generic copy of last_result, which may belong to an older entry). */
    g_shared.history_results[g_shared.hist_write][0] = '\0';
    s_view_dirty = true;
    g_shared.hist_write = (uint8_t)((g_shared.hist_write + 1)
                                     % CE_HISTORY_SIZE);
    if (g_shared.hist_count < CE_HISTORY_SIZE) g_shared.hist_count++;
}

/* Store the first line of a result string into g_shared.last_result.
 * Truncates to 63 chars. */
static void state_store_last_result(const char *result, bool ok)
{
    char buf[64];
    const char *p;
    size_t len;

    /* Rebuild the viewer's stored-line fallback. Callers that replace a
     * worker response with a local error must also clear s_view_valid:
     * ring indices alone cannot distinguish a full history wrap. */
    s_view_dirty = true;

    if (!result || !result[0]) {
        if (!ok) {
            strncpy(g_shared.last_result, "ERROR", 63);
        } else {
            g_shared.last_result[0] = '\0';
        }
        g_shared.last_result[63] = '\0';
        return;
    }

    /* Take first line only (up to '\n') */
    p   = result;
    len = 0;
    while (*p && *p != '\n' && len < 63) {
        buf[len++] = *p++;
    }
    /* A first line longer than 63 bytes ends in the TI ellipsis, so the
     * stored row still shows that the result was clipped after a relaunch. */
    if (*p && *p != '\n') buf[62] = HL_MORE;
    buf[len] = '\0';

    strncpy(g_shared.last_result, buf, 63);
    g_shared.last_result[63] = '\0';
}

/* Phase 2: write result into the per-entry slot for the most-recently-pushed
 * history entry.  Called immediately after state_store_last_result() whenever
 * a result is known.  Safe to call even when hist_count == 0 (no-op). */
static void history_update_result(const char *result_str)
{
    uint8_t slot;
    const char *src;
    if (g_shared.hist_count == 0) return;
    slot = (uint8_t)((g_shared.hist_write + CE_HISTORY_SIZE - 1) % CE_HISTORY_SIZE);
    src  = result_str ? result_str : "";
    s_view_dirty = true;
    strncpy(g_shared.history_results[slot], src, 63);
    g_shared.history_results[slot][63] = '\0';
}

/* Readable errors the launcher itself produces when a dispatch or a child
 * fails (the child-recovery text is formatted by module_runtime.c as
 * "Module <name> returned ..."). */
#define LE_IPC_WRITE   "IPC write failed"
#define LE_STATE_SAVE  "IPC state save failed"
#define LE_MISSING     "Module missing"
#define LE_NO_RESPONSE "No module response"
#define LE_LAUNCH_FAIL "Program launch failed"

/* Is a stored line one of the launcher's own dispatch/recovery errors?
 * The viewer titles those Error even when it reads them from a slot. */
static bool is_launcher_error(const char *s)
{
    return strcmp(s, LE_IPC_WRITE) == 0 || strcmp(s, LE_STATE_SAVE) == 0 ||
           strcmp(s, LE_MISSING) == 0 || strcmp(s, LE_NO_RESPONSE) == 0 ||
           strcmp(s, LE_LAUNCH_FAIL) == 0 ||
           (strncmp(s, "Module ", 7) == 0 && strstr(s, " returned ") != NULL);
}

/* When the newest submission fails in the launcher, bind its
 * readable error to that entry's own history slot, so it survives later
 * submissions and relaunches, and mark g_response stale, because ring
 * indices alone cannot tell a full wrap from the answer it held.  The
 * caller saves CE_STATE.  Ans and settings are not touched. */
static void record_local_error(const char *message)
{
    state_store_last_result(message, false);
    history_update_result(g_shared.last_result);
    s_view_valid = false;
}

static void clear_failed_dispatch(void)
{
    g_shared.settings         = g_settings;
    g_shared.last_answer      = g_last_answer;
    g_shared.has_pending_response = 0;
    g_shared.pending_module[0] = '\0';
    ce_state_save(&g_shared);
}

/* ── Mode menu (Phase 3) ────────────────────────────────────────── */
/*
 * Full settings overlay opened by the MODE key.
 * Layout (9 rows × 26 cols):
 *   Row 0  "CalcEngine Settings"
 *   Row 1  "UP/DN=sel L/R=change"
 *   Row 2  ">Show work    [ON ]"
 *   Row 3  " Show steps   [ALL]"
 *   Row 4  " Show segs    [OFF]"
 *   Row 5  " Exact/Dec    [DEC]"
 *   Row 6  " Precision    [  6]"
 *   Row 7  " Stored vars  [  2]"   (count of A..Z holding a value)
 *   Row 8  "ENTER=save CLEAR=exit"
 *
 * UP/DOWN move the cursor between items 0-5.
 * LEFT/RIGHT change the selected item's value; on "Stored vars" they
 * toggle between the count and CLR (clear every stored value).
 * ENTER saves settings into g_shared.settings + g_settings + CE_STATE, and
 * clears the stored values when CLR is shown.
 * CLEAR or 2nd+MODE exits without saving.
 *
 * The home screen is redrawn after returning so the UI is always
 * consistent — no partial-redraw required.
 */
static void show_mode_menu(void)
{
    /* Working copy — user edits this; only committed on ENTER. */
    settings_t s;
    settings_t previous_settings;
    static ce_user_vars_t previous_vars;
    int8_t  sel = 0;   /* selected row index, 0-5 */
    bool    clear_vars = false;   /* "Stored vars" shows CLR */
    uint8_t var_count = 0;
    uint8_t vi;
    bool    redraw = true;
    sk_key_t key;
    sk_key_t prev_key = 0;

    /* Start with current live settings */
    s = g_shared.settings;
    previous_settings = s;
    previous_vars = g_shared.user_vars;
    for (vi = 0; vi < CE_USER_VAR_COUNT; vi++) {
        if (g_shared.user_vars.mask[vi >> 3] & (uint8_t)(1u << (vi & 7))) var_count++;
    }

    for (;;) {
        /* ── Render ───────────────────────────────────────────── */
        if (redraw) {
            char line[HOME_COLS + 1];
            static const char * const labels[6] = {
                "Show work",
                "Show steps",
                "Show segs",
                "Exact/Dec",
                "Precision",
                "Stored vars"
            };
            uint8_t i;

            os_ClrHome();
            os_SetCursorPos(0, 0);
            os_PutStrFull("CalcEngine Settings");
            os_SetCursorPos(1, 0);
            os_PutStrFull("UP/DN=sel L/R=change");

            for (i = 0; i < 6; i++) {
                char valstr[5];
                switch (i) {
                case 0: /* Show work: show_steps */
                    snprintf(valstr, 5, "%-3s", s.show_steps ? "ON" : "OFF");
                    break;
                case 1: /* Show steps: step_detail */
                    snprintf(valstr, 5, "%-3s",
                             s.step_detail == STEP_ALL ? "ALL" : "CMP");
                    break;
                case 2: /* Show segments: segment_pause */
                    snprintf(valstr, 5, "%-3s", s.segment_pause ? "ON" : "OFF");
                    break;
                case 3: /* Exact/Decimal: exact_mode */
                    snprintf(valstr, 5, "%-3s", s.exact_mode ? "EXC" : "DEC");
                    break;
                case 4: /* Precision: 1-10 */
                    snprintf(valstr, 5, "%3d", (int)s.precision);
                    break;
                case 5: /* Stored vars: count, or CLR = clear on ENTER */
                    if (clear_vars) snprintf(valstr, 5, "CLR");
                    else snprintf(valstr, 5, "%3d", (int)var_count);
                    break;
                default:
                    valstr[0] = '\0';
                    break;
                }
                /* Format: [>] or [ ] + label (padded to 10) + [VVV] */
                snprintf(line, HOME_COLS + 1, "%c%-12s[%s]",
                         (i == (uint8_t)sel) ? '>' : ' ',
                         labels[i], valstr);
                os_SetCursorPos((uint8_t)(2 + i), 0);
                home_print_row(line);
            }

            os_SetCursorPos(8, 0);
            os_PutStrFull("ENTER=save CLEAR=exit");

            redraw = false;
        }

        /* ── Input ────────────────────────────────────────────── */
        key = os_GetCSC();
        if (key == 0) {
            prev_key = 0;
            delay(10);
            continue;
        }
        if (key == prev_key) {
            /* same key still held — skip (debounce) */
            delay(10);
            continue;
        }
        prev_key = key;

        switch (key) {
        /* ── Navigation ──────────────────────────────────────── */
        case sk_Up:
            if (sel > 0) { sel--; redraw = true; }
            break;

        case sk_Down:
            if (sel < 5) { sel++; redraw = true; }
            break;

        /* ── Value change ─────────────────────────────────────── */
        case sk_Left:
        case sk_Right:
            switch (sel) {
            case 0: /* Show work toggle */
                s.show_steps = !s.show_steps;
                break;
            case 1: /* Show steps: ALL ↔ COMPACT */
                s.step_detail = (s.step_detail == STEP_ALL)
                                ? STEP_COMPACT : STEP_ALL;
                break;
            case 2: /* Show segments toggle */
                s.segment_pause = !s.segment_pause;
                break;
            case 3: /* Exact/Decimal toggle */
                s.exact_mode = !s.exact_mode;
                break;
            case 4: /* Precision: 1-10; RIGHT=+1, LEFT=-1 */
                if (key == sk_Right) {
                    if (s.precision < 10) s.precision++;
                } else {
                    if (s.precision > 1) s.precision--;
                }
                break;
            case 5: /* Stored vars: count <-> CLR (nothing to clear: stay) */
                if (var_count > 0) clear_vars = !clear_vars;
                break;
            default:
                break;
            }
            redraw = true;
            break;

        /* ── Confirm ──────────────────────────────────────────── */
        case sk_Enter:
            /* Commit edited settings to shared state and live globals */
            g_shared.settings = s;
            g_settings        = s;
            s_view_dirty      = true; /* Show work changes the viewer */
            if (clear_vars) {
                memset(&g_shared.user_vars, 0, sizeof(g_shared.user_vars));
            }
            if (!ce_state_save(&g_shared)) {
                /* Restore the pre-edit snapshot without another fallible read. */
                g_shared.settings = previous_settings;
                g_shared.user_vars = previous_vars;
                g_settings = previous_settings;
                strcpy(s_status, "Settings save failed");
            }
            return;

        /* ── Cancel ───────────────────────────────────────────── */
        case sk_Clear:
        case sk_Mode:   /* MODE pressed again = cancel */
            /* Discard edits, return without saving */
            return;

        default:
            break;
        }
    }
}

/* ── Keyscan ────────────────────────────────────────────────────── */

static void home_clear_all(void)
{
    input_clear();
    g_shared.hist_count = 0;
    g_shared.hist_write = 0;
    memset(g_shared.history,         0, sizeof(g_shared.history));
    memset(g_shared.history_results, 0, sizeof(g_shared.history_results));
    g_shared.last_result[0] = '\0';
    s_hist_nav  = -1;
    s_entry_idx = -1;
    s_view_valid = false;
    s_view_dirty = true;
    ce_state_save(&g_shared);
}

/* ── Result viewer ───────────────────────────────────────────────────── */
/*
 * Full-screen view of the newest result, wrapped at 26 columns:
 *   Row 0     "Result 1-8/12" (or "Error ...")
 *   Rows 1-8  the wrapped text
 *   Row 9     key help
 * UP/DOWN scroll one line, LEFT/RIGHT one page; CLEAR, ENTER or MODE return
 * to the home screen, which the caller redraws.
 */
static void show_result_view(void)
{
    static char screen[RV_ROWS][RV_COLS + 1];
    uint16_t total;
    uint16_t top = 0;
    bool     redraw = true;
    sk_key_t key;
    sk_key_t prev_key = 0;
    const char *title;

    view_build(&s_view_doc);
    total = rv_count_lines(&s_view_doc);
    if (total == 0) return;
    if (view_response_current()) {
        title = g_response.ok ? "Result" : "Error";
    } else {
        /* Stored line.  Every worker answer, ok or not, and every
         * launcher-side failure (IPC write/state save, Module missing,
         * launch failure, a child that left no or an invalid response) is
         * copied into its history slot.  The launcher's own errors are
         * recognised by their text and titled Error.  So is an empty slot
         * that falls back to last_result (an older CE_STATE).  Known
         * limit, since no per-slot ok bit is stored: a worker error or a
         * Syntax error shown from its slot is titled Result. */
        {
            const char *stored = view_newest_stored();
            title = (stored == g_shared.last_result || is_launcher_error(stored))
                        ? "Error" : "Result";
        }
    }

    for (;;) {
        if (redraw) {
            uint8_t r;
            rv_render(&s_view_doc, title, total, top, screen);
            os_ClrHome();
            for (r = 0; r < RV_ROWS; r++) {
                /* os_PutStrLine, not os_PutStrFull: on OS 5.8 os_PutStrFull
                 * printed only the first character on row 9 (it stops when
                 * the string would run past the end of the screen).  No
                 * padding: os_ClrHome already blanked every row. */
                if (!screen[r][0]) continue;
                ti_fix_chars(screen[r], RV_COLS);
                os_SetCursorPos(r, 0);
                os_PutStrLine(screen[r]);
            }
            redraw = false;
        }

        key = os_GetCSC();
        if (key == 0) {
            prev_key = 0;
            delay(10);
            continue;
        }
        if (key == prev_key) {
            delay(10);
            continue;
        }
        prev_key = key;

        {
            rv_key_t vk = RV_KEY_NONE;
            uint16_t before = top;
            switch (key) {
            case sk_Up:    vk = RV_KEY_UP;        break;
            case sk_Down:  vk = RV_KEY_DOWN;      break;
            case sk_Left:  vk = RV_KEY_PAGE_UP;   break;
            case sk_Right: vk = RV_KEY_PAGE_DOWN; break;
            case sk_Clear:
            case sk_Enter:
            case sk_Mode:  vk = RV_KEY_EXIT;      break;
            default:       break;
            }
            if (rv_apply_key(vk, total, &top)) return;
            if (top != before) redraw = true;
        }
    }
}

/* Process a key press (with resolved modifier) and update input buffer.
 * Returns:
 *   1  = submitted (ENTER pressed with non-empty input)
 *   0  = continue looping
 *  -1  = quit requested
 */
static int8_t process_key(sk_key_t key, bool mod_2nd, bool mod_alpha)
{
    /* ── 2nd modifier ─────────────────────────────────────────── */
    if (mod_2nd) {
        switch (key) {
        case sk_Mode:   /* 2nd+MODE = quit */
            g_quit_requested = true;
            return -1;

        case sk_Enter:  /* 2nd+ENTER = ENTRY recall (cycles through history) */
            if (g_shared.hist_count == 0) return 0;
            if (s_entry_idx < 0) {
                /* Enter recall mode starting at most-recent entry */
                s_entry_idx = 0;
            } else {
                /* Advance to older entry, wrapping around */
                s_entry_idx = (int8_t)((s_entry_idx + 1) % g_shared.hist_count);
            }
            input_load_history((uint8_t)s_entry_idx);
            s_hist_nav = -1; /* entry recall replaces history nav */
            return 0;

        case sk_Clear:  /* 2nd+CLEAR = clear everything */
            home_clear_all();
            return 0;

        case sk_Del:    /* 2nd+DEL = forward delete */
            input_delete();
            return 0;

        case sk_Sin:    input_insert_str("asin(");  return 0;
        case sk_Cos:    input_insert_str("acos(");  return 0;
        case sk_Tan:    input_insert_str("atan(");  return 0;
        case sk_Ln:     input_insert_str("e");      return 0;
        case sk_Log:    input_insert_str("log10("); return 0;
        case sk_Square: input_insert_str("sqrt(");  return 0;
        case sk_Power:  input_insert_str("pi");     return 0;
        case sk_Mul:    input_insert_str("ans");    return 0;

        /* 2nd + number keys: no special meaning in Phase 1 */
        default:
            return 0;
        }
    }

    /* ── Alpha modifier ───────────────────────────────────────── */
    if (mod_alpha) {
        char c = 0;
        switch (key) {
        /* TI-84 Plus CE ALPHA layer, exactly as printed in green on the
         * keycaps.  Source: TI "Keyboard Mapping for the TI-84 Plus CE online
         * calculator" (education.ti.com eGuide, letter a-z = alpha letter on
         * that key), cross-checked on OS 5.8.0.0022 in CEmu: ALPHA-lock + the
         * keys math..div types A..M, log..2 types N..Z, 3 + 0 . (-) types
         * θ " space : ?.  Row by row:
         *   math apps prgm            A B C
         *   x^-1 sin cos tan ^        D E F G H
         *   x^2  ,   (   )   ÷        I J K L M
         *   log  7   8   9   ×        N O P Q R
         *   ln   4   5   6   −        S T U V W
         *   sto  1   2   3   +        X Y Z θ "
         *        0   .   (-)            space : ?
         * Do not "simplify" this into a digit-row pattern: until 2026-11 the
         * table was shifted one key from M onward, so lim/solve/sum/mean
         * typed from the printed keycaps came out as LI(/S8)1E/... */
        case sk_Math:     c = 'A'; break;
        case sk_Apps:     c = 'B'; break;
        case sk_Prgm:     c = 'C'; break;
        case sk_Recip:    c = 'D'; break;
        case sk_Sin:      c = 'E'; break;
        case sk_Cos:      c = 'F'; break;
        case sk_Tan:      c = 'G'; break;
        case sk_Power:    c = 'H'; break;
        case sk_Square:   c = 'I'; break;
        case sk_Comma:    c = 'J'; break;
        case sk_LParen:   c = 'K'; break;
        case sk_RParen:   c = 'L'; break;
        case sk_Div:      c = 'M'; break;
        case sk_Log:      c = 'N'; break;
        case sk_7:        c = 'O'; break;
        case sk_8:        c = 'P'; break;
        case sk_9:        c = 'Q'; break;
        case sk_Mul:      c = 'R'; break;
        case sk_Ln:       c = 'S'; break;
        case sk_4:        c = 'T'; break;
        case sk_5:        c = 'U'; break;
        case sk_6:        c = 'V'; break;
        case sk_Sub:      c = 'W'; break;
        case sk_Store:    c = 'X'; break;
        case sk_1:        c = 'Y'; break;
        case sk_2:        c = 'Z'; break;
        case sk_0:        c = ' '; break; /* printed: space (tokenizer skips it) */
        case sk_GraphVar: c = 'X'; break; /* X,T,θ,n */
        /* θ (alpha+3), " (alpha++), : (alpha+.) and ? (alpha+(-)) are
         * printed on the keycaps but the Algebrix tokenizer has no token for
         * them, so they stay explicit no-ops instead of inserting a
         * different character. */
        case sk_3:
        case sk_Add:
        case sk_DecPnt:
        case sk_Chs:
        default:
            return 0; /* unmapped alpha key: no-op */
        }
        if (c) input_insert_char(c);
        return 0;
    }

    /* ── Normal (no modifier) ─────────────────────────────────── */
    switch (key) {
    /* Digits */
    case sk_0:  input_insert_char('0'); return 0;
    case sk_1:  input_insert_char('1'); return 0;
    case sk_2:  input_insert_char('2'); return 0;
    case sk_3:  input_insert_char('3'); return 0;
    case sk_4:  input_insert_char('4'); return 0;
    case sk_5:  input_insert_char('5'); return 0;
    case sk_6:  input_insert_char('6'); return 0;
    case sk_7:  input_insert_char('7'); return 0;
    case sk_8:  input_insert_char('8'); return 0;
    case sk_9:  input_insert_char('9'); return 0;

    /* Operators */
    case sk_Add:   input_insert_char('+'); return 0;
    case sk_Sub:   input_insert_char('-'); return 0;
    case sk_Mul:   input_insert_char('*'); return 0;
    case sk_Div:   input_insert_char('/'); return 0;
    case sk_Power: input_insert_char('^'); return 0;

    /* Delimiters */
    case sk_LParen:  input_insert_char('('); return 0;
    case sk_RParen:  input_insert_char(')'); return 0;
    case sk_DecPnt:  input_insert_char('.'); return 0;
    case sk_Comma:   input_insert_char(','); return 0;
    /* Matrix row separator, mat(1,2;3,4).  The keycaps have no ';', so APPS
     * types it, as in the desktop keymap (src/ui.c).  Without it no matrix
     * literal could be typed on the CE. */
    case sk_Apps:    input_insert_char(';'); return 0;

    /* Variable */
    case sk_GraphVar: input_insert_char('X'); return 0;

    /* Negation: insert "(-" so user can type "(-5)" */
    case sk_Chs: input_insert_str("(-"); return 0;

    /* Store arrow */
    case sk_Store: input_insert_str("->"); return 0;

    /* Functions — insert name + opening paren */
    case sk_Sin:    input_insert_str("sin(");   return 0;
    case sk_Cos:    input_insert_str("cos(");   return 0;
    case sk_Tan:    input_insert_str("tan(");   return 0;
    case sk_Ln:     input_insert_str("ln(");    return 0;
    case sk_Log:    input_insert_str("log(");   return 0;
    case sk_Square: input_insert_str("^2");     return 0;
    case sk_Recip:  input_insert_str("^(-1)");  return 0;

    /* Navigation */
    case sk_Left:  input_cursor_left();  return 0;
    case sk_Right: input_cursor_right(); return 0;

    case sk_Up:
        /* TI-OS home screen keeps UP/DOWN from mutating the edit line.
         * ENTRY recall remains on 2nd+ENTER; UP opens the newest result in
         * the full-text viewer and leaves the edit line as it was. */
        s_hist_nav = -1;
        show_result_view();
        return 0;

    case sk_Down:
        /* TI-OS home screen keeps UP/DOWN from mutating the edit line. */
        s_hist_nav = -1;
        return 0;

    case sk_Del:
        /* Backspace */
        if (s_ilen > 0 || s_cursor > 0) {
            input_backspace();
        }
        s_hist_nav  = -1; /* editing breaks history nav */
        s_entry_idx = -1; /* editing breaks entry recall */
        return 0;

    case sk_Clear:
        if (s_ilen > 0) {
            /* First CLEAR: clear the current input line */
            input_clear();
            s_hist_nav  = -1;
            s_entry_idx = -1;
        } else {
            /* Second CLEAR (already empty): wipe full history display.
             * Phase 2: clears all entries, results, and last_result so the
             * screen is blank again.  Expression history is not archived
             * separately — this is a full screen-clear. */
            home_clear_all();
        }
        return 0;

    case sk_Enter:
        /* Submit if non-empty */
        if (s_ilen > 0) {
            s_hist_nav  = -1;
            s_entry_idx = -1;
            return 1; /* submitted */
        }
        /* Empty input line: show the newest result in full. */
        show_result_view();
        return 0;

    case sk_Mode:
        show_mode_menu();
        return 0;

    case sk_Trace:
        /* TRACE = "show the work": the same full-result viewer as UP, whose
         * document is the newest result followed by its steps. */
        show_result_view();
        return 0;

    /* Unknown/unhandled keys: no-op (never pass to OS) */
    default:
        return 0;
    }
}

/*
 * ce_input_scan — block until the user submits an expression or quits.
 *
 * Fills s_input (already reset by caller) with the submitted expression.
 * Returns true if an expression was submitted, false if quit was requested.
 *
 * Implementation: simple press-and-release debounce (Phase 1).
 * Key repeat (hold-to-repeat) is Phase 2.
 */
static bool ce_input_scan(void)
{
    bool   mod_2nd   = false;
    bool   mod_alpha = false;
    sk_key_t key     = 0;
    sk_key_t prev_key = 0;
    int8_t   result;

    home_draw();

    for (;;) {
        key = os_GetCSC();

        if (key == 0) {
            /* No key pressed: clear transient modifiers that were pressed
             * and released without a following key. */
            if (prev_key == sk_2nd || prev_key == sk_Alpha) {
                /* Modifier was pressed then released alone — keep state
                 * until a non-modifier key resolves it. */
            }
            prev_key = 0;
            delay(10); /* ~10 ms poll interval; avoids CPU-spin on hardware */
            continue;
        }

        /* Wait for release before processing (debounce).
         * This prevents key-repeat for Phase 1. */
        if (key == prev_key) {
            /* same key still held — skip until released */
            delay(10);
            continue;
        }
        prev_key = key;

        /* Modifier tracking: 2nd and Alpha are sticky until the next
         * non-modifier key resolves them. Pressing 2nd twice cancels it. */
        if (key == sk_2nd) {
            mod_2nd   = !mod_2nd;
            mod_alpha = false; /* 2nd cancels alpha */
            continue;
        }
        if (key == sk_Alpha) {
            mod_alpha = !mod_alpha;
            mod_2nd   = false; /* alpha cancels 2nd */
            continue;
        }

        /* The parse-error message lasts until the next key. */
        s_status[0] = '\0';

        /* Process the key */
        result = process_key(key, mod_2nd, mod_alpha);

        /* Consume modifiers after a non-modifier key */
        mod_2nd   = false;
        mod_alpha = false;

        if (result == 1) {
            /* ENTER submitted — expression is in s_input */
            return true;
        }
        if (result == -1) {
            /* Quit */
            return false;
        }

        /* Redraw after every key */
        home_draw();
    }
}

/* ── Dispatch ───────────────────────────────────────────────────── */

static bool dispatch_to_program(const char *program,
                                 ce_domain_t domain,
                                 const char *expression,
                                 const ast_node_t *root,
                                 bool had_equals)
{
    ce_request_t  *req  = &g_request;
    ce_response_t *resp = &g_response;
    int run_ret;

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    s_view_valid = false;   /* g_response no longer holds the newest answer */
    s_view_dirty = true;

    req->magic      = CE_IPC_MAGIC;
    req->version    = CE_IPC_VERSION;
    req->domain     = (uint8_t)domain;
    req->flags      = had_equals ? CE_REQ_FLAG_HAD_EQUALS : 0;
    req->settings   = g_settings;
    req->last_answer = g_last_answer;
    strncpy(req->caller,    CE_PRGM_CORE, sizeof(req->caller) - 1);
    strncpy(req->return_to, CE_PRGM_CORE, sizeof(req->return_to) - 1);
    strncpy(req->expression, expression, sizeof(req->expression) - 1);
    if (root && ce_ast_serialize(root, req->ast_payload,
                                  sizeof(req->ast_payload), &req->ast_size)) {
        req->flags |= CE_REQ_FLAG_HAS_AST;
    }

    g_shared.settings          = g_settings;
    g_shared.last_answer       = g_last_answer;
    g_shared.has_pending_response = 0;
    strncpy(g_shared.pending_module, program, sizeof(g_shared.pending_module) - 1);
    history_push(expression);
    state_update_last_expression(expression);

    /* Each failure below records its error in the entry just pushed
     * before clear_failed_dispatch() saves CE_STATE, so the launcher exits
     * with the error saved beside its expression. */
    ce_response_delete();
    if (!ce_request_write(req)) {
        record_local_error(LE_IPC_WRITE);
        clear_failed_dispatch();
        home_draw();
        return false;
    }
    if (!ce_state_save(&g_shared)) {
        record_local_error(LE_STATE_SAVE);
        clear_failed_dispatch();   /* retries the save: keeps it if it works */
        home_draw();
        return false;
    }

    /* Verify the worker module exists before calling os_RunPrgm.
     * Without this check, os_RunPrgm on a missing program causes TI-OS
     * to crash ("RAM Cleared") rather than returning an error. */
    if (!ce_program_exists(program)) {
        record_local_error(LE_MISSING);
        clear_failed_dispatch();
        home_draw();
        return false;
    }

    /* On real hardware, os_RunPrgm destroys this program and never returns
     * on success. When the worker's main() returns, the CEdev runprgm stub
     * reloads CALCENG from its variable (the name crt0 saved in
     * ___prgm_caller), unwinds the OS stack to this call site, and enters
     * launcher_reentry(), which re-runs main(); main() reads
     * has_pending_response=1 and displays the result. The callback address
     * is valid because CALCENG is reloaded at the same userMem address
     * before the stub jumps to it.
     *
     * The old fire-and-forget scheme (worker relaunching CALCENG with a NULL
     * callback) leaked a 209-byte stub frame of the 4 KB OS stack per hop
     * and reset the calculator after a few module calls (2026-11-08).
     * On desktop, the mock returns 0 immediately and we fall through to
     * the synchronous fallback below. */
    run_ret = ce_launch_program_reentry(program, launcher_reentry);

    /* ── Desktop/mock synchronous fallback (dead code on real CE) ── */
    if (ce_response_read(resp)) {
        const char *res_str = resp->ok ? resp->result : resp->error;
        /* Adopt worker-written values before saving the display fields. */
        ce_state_load(&g_shared);
        g_settings   = resp->settings;
        g_last_answer = resp->last_answer;
        /* Store result for display */
        state_store_last_result(res_str, resp->ok != 0);
        /* Phase 2: persist result in per-entry slot */
        history_update_result(g_shared.last_result);
        view_mark_response();
        g_shared.settings   = g_settings;
        g_shared.last_answer = g_last_answer;
        g_shared.last_result[63] = '\0'; /* guaranteed NUL */
        ce_state_save(&g_shared);
        ce_response_delete();
    } else if (run_ret == 0) {
        record_local_error(LE_NO_RESPONSE);
        ce_state_save(&g_shared);
    }

    ce_state_load(&g_shared);
    g_settings   = g_shared.settings;
    g_last_answer = g_shared.last_answer;
    g_shared.has_pending_response = 0;
    if (g_shared.pending_module[0]) {
        ce_program_set_archived(g_shared.pending_module, true);
        g_shared.pending_module[0] = '\0';
    }
    ce_state_save(&g_shared);

    if (run_ret != 0) {
        record_local_error(LE_LAUNCH_FAIL);
        ce_state_save(&g_shared);
        return false;
    }

    return !g_quit_requested;
}

/* ── Entry point ────────────────────────────────────────────────── */

static int launcher_reentry(void *data, int retval)
{
    (void)data;
    (void)retval;
    ce_module_runtime_mark_reentry();
    return main();
}

int main(void)
{
    bool running = true;

    mem_init();
    ce_state_load(&g_shared);
    g_settings   = g_shared.settings;
    g_last_answer = g_shared.last_answer;
    g_quit_requested = false;

    /* Reset input buffer */
    input_clear();
    s_status[0] = '\0';
    s_hist_nav  = -1;
    s_entry_idx = -1;
    /* g_response holds the full text of the newest answer only after the
     * pending-response read below; a fresh launch has nothing in it. */
    s_view_valid = false;
    s_view_dirty = true;

    /* ── Child-failure recovery (PR #40 stack overflow fix) ─────── */
    {
        char pending_err[128];
        memset(&s_pending_req, 0, sizeof(s_pending_req));
        if (ce_module_runtime_recover_child_failure(CE_PRGM_CORE, "",
                                                    &s_pending_req, &g_shared,
                                                    pending_err,
                                                    sizeof(pending_err))) {
            /* Bind the child's error to the failed entry's own slot, never
             * only to last_result, which the next submission hides.
             * dispatch_to_program() pushed the expression and saved
             * CE_STATE before launching, so it is the newest entry and
             * history_push() is a no-op.  The push is defensive: if a saved
             * state ever lacks the entry, the error still gets its own row. */
            /* Recovery has already put back the Ans and settings this
             * dispatch sent (s_pending_req), so a child that saved new ones
             * before failing changes neither. */
            if (s_pending_req.expression[0]) {
                history_push(s_pending_req.expression);
                record_local_error(pending_err);
            } else {
                /* Unreachable: dispatch never sends an empty expression, and
                 * history_push() would reject one.  Keep the old fallback
                 * rather than overwrite another entry's slot. */
                state_store_last_result(pending_err, false);
            }
            ce_state_save(&g_shared);
        }
    }

    /* ── Pending response from dispatched worker (re-entry path) ── */
    /* Reuse g_response (static global) rather than allocating another
     * 4510-byte ce_response_t on main()'s stack (PR #40 fix). */
    if (g_shared.has_pending_response) {
        memset(&g_response, 0, sizeof(g_response));
        if (ce_response_read(&g_response)) {
            g_settings   = g_response.settings;
            g_last_answer = g_response.last_answer;
            /* Store result into persistent shared state for display */
            state_store_last_result(g_response.ok
                                        ? g_response.result
                                        : g_response.error,
                                    g_response.ok != 0);
            /* Phase 2: persist result in per-entry slot */
            history_update_result(g_shared.last_result);
            view_mark_response();   /* g_response keeps the full text */
            ce_response_delete();
        }
        g_shared.has_pending_response = 0;
        g_shared.settings   = g_settings;
        g_shared.last_answer = g_last_answer;
        if (g_shared.pending_module[0]) {
            ce_program_set_archived(g_shared.pending_module, true);
            g_shared.pending_module[0] = '\0';
        }
        ce_state_save(&g_shared);
    }

    /* ── Main input loop ─────────────────────────────────────────── */
    while (running && !g_quit_requested) {
        ast_node_t *root      = NULL;
        ce_domain_t domain    = CE_DOMAIN_UNKNOWN;
        bool        had_equals = false;
        const char *program;

        /* Block for user input. Draws the screen internally. */
        if (!ce_input_scan()) break;

        /* s_input now has a non-empty expression */
        if (!parse_for_domain(s_input, &root, &had_equals, &domain)) {
            /* Keep failed input editable, but record a complete pair just
             * like a worker error.  Never let the previous worker's full text
             * masquerade as this entry (including after a ring wrap). */
            const char *message = "Syntax error";
            const char *p = s_input;
            while (*p == ' ') p++;
            /* A nonzero literal that would read as 0 or a tiny value
             * (token.c number_lost; same text as CE_FOPS_TINY_MSG). */
            if (g_tokenizer.error == CE_ERR_PRECISION)
                message = "Tiny value unsupported";
            /* A literal too long to read (token.c scan_number). */
            else if (g_tokenizer.error == CE_ERR_OVERFLOW)
                message = "Number too long";
            else if (*p == 'D' || *p == 'd') {
                p++;
                while (*p == ' ') p++;
                if (*p == '(') message = "Syntax error: d/dx(expr)";
            }
            strncpy(s_status, message, sizeof(s_status) - 1);
            s_status[sizeof(s_status) - 1] = '\0';
            history_push(s_input);
            history_update_result(message);
            state_store_last_result(message, false);
            state_update_last_expression(s_input);
            s_view_valid = false;
            g_shared.settings = g_settings;
            g_shared.last_answer = g_last_answer; /* syntax errors never change Ans */
            ce_state_save(&g_shared);
            /* Move to a tokenizer error; parser errors have no source offset. */
            if (s_parse_err_pos != PARSE_NO_POS && s_parse_err_pos <= s_ilen)
                s_cursor = s_parse_err_pos;
            s_hist_nav = -1;
            continue;
        }

        program = route_program(root, had_equals, domain);

        /* Copy expression before clearing input */
        {
            char expr_copy[MAX_INPUT_LEN];
            strncpy(expr_copy, s_input, MAX_INPUT_LEN - 1);
            expr_copy[MAX_INPUT_LEN - 1] = '\0';

            /* Clear input buffer for next entry */
            input_clear();
            s_hist_nav = -1;

            running = dispatch_to_program(program, domain, expr_copy,
                                          root, had_equals);
        }
        ast_free(root);
    }

    /* Clean exit */
    os_ClrHome();
    return 0;
}
