/*
 * home_layout.h — wrapped transcript rows for the CALCENG home screen.
 *
 * The home screen used to give every expression and every result exactly
 * one row, clipped at column 26 with no marker, so a 39-character derivative
 * such as d/dX = ((cos(X)*(X^2))+(sin(X)*(2*X))) stopped at "...+(si" and a
 * long typed expression lost its tail (internal review).
 *
 * This header has no TI dependencies.  It lays out the transcript area
 * (rows 0 .. HL_ROWS-1, above the input row) into a plain char grid that
 * src/core/launcher/main.c prints row by row; the desktop tests
 * (tests/test_main.c, tests/harness/launcher_view_harness.c) check the same
 * functions.
 *
 * Layout rules:
 *   - Each entry is its expression, then line 1 of its result, then (for the
 *     newest entry only) the viewer hint "UP=full result (N ln)".
 *   - A field is wrapped with the result viewer's rule (result_view.h
 *     rv_next_line): break at the last space at column 13 or later,
 *     otherwise hard at 26.  Only the field's first logical line is used.
 *   - A field gets at most HL_EXPR_MAX_ROWS / HL_RES_MAX_ROWS rows.  When it
 *     needs more, the last row it gets ends in HL_MORE (the TI font
 *     ellipsis), so a cut field never reads as a complete one.  The full
 *     expression stays reachable with 2nd+ENTER (ENTRY), the full newest
 *     result with the viewer.
 *   - Entries are taken newest first while they fit whole; an older entry
 *     that does not fit is left out entirely rather than shown cut.  At most
 *     HL_MAX_ENTRIES entries are shown (the three pairs the home screen
 *     always showed).  The caps make the newest entry fit on its own:
 *     3 + 3 + 1 hint row = 7 = HL_ROWS.
 *   - Rows are drawn top-aligned, oldest entry first, newest lowest, exactly
 *     as before; with short fields the frame is unchanged.
 */
#ifndef CE_HOME_LAYOUT_H
#define CE_HOME_LAYOUT_H

#include "core/launcher/result_view.h"

#define HL_ROWS          7   /* transcript rows 0..6; row 7 is the input row */
#define HL_MAX_ENTRIES   3   /* entries shown at most (unchanged)            */
#define HL_EXPR_MAX_ROWS 3
#define HL_RES_MAX_ROWS  3
/* TI-OS large-font ellipsis (Lellipsis, 0xCE).  Bytes >= 0x80 are TI font
 * characters; rv_printable keeps them. */
#define HL_MORE          ((char)0xCE)

#if (HL_EXPR_MAX_ROWS + HL_RES_MAX_ROWS + 1) > HL_ROWS
#error "home_layout.h: the newest entry (expression, result, hint) must fit"
#endif

typedef struct {
    const char *expr;     /* typed expression (may be "")             */
    size_t      expr_cap; /* bytes readable at expr                   */
    const char *res;      /* stored result; only line 1 is shown      */
    size_t      res_cap;
    const char *hint;     /* hint row text, or NULL/"" for none       */
} hl_entry_t;

/* Length of the first logical line of text (stops at '\n', NUL or cap). */
static size_t hl_line1_len(const char *text, size_t cap)
{
    size_t n = 0;
    if (!text) return 0;
    while (n < cap && text[n] && text[n] != '\n') n++;
    return n;
}

/* One-segment document and scratch line shared by the helpers below.  The
 * launcher is single-threaded, and no helper calls another while it holds
 * an iterator over hl_doc (keep it that way), so static storage keeps about
 * 230 B off the eZ80 stack. */
static rv_doc_t hl_doc;
static char     hl_line[RV_COLS + 1];

static void hl_iter_line1(rv_iter_t *it, const char *text, size_t len)
{
    rv_doc_init(&hl_doc);
    rv_doc_add(&hl_doc, text, len);
    rv_iter_init(it, &hl_doc);
}

/* Wrap the first logical line of text into at most max_rows rows of out
 * (each RV_COLS + 1 bytes).  If the line needs more rows, the last row
 * ends in HL_MORE (appended, or replacing column 26).  Returns the number
 * of rows written; an empty line gives 0.  Only out[0 .. return-1] is
 * written: every line goes through hl_line first, because rv_next_line
 * clears its output before it knows whether a line follows (review round 1 P1:
 * the probe after a field ending on row 6 wrote screen[7][0]). */
static uint8_t hl_wrap_field(const char *text, size_t cap, uint8_t max_rows,
                             char out[][RV_COLS + 1])
{
    rv_iter_t it;
    uint8_t   n = 0;
    size_t    len = hl_line1_len(text, cap);

    if (len == 0 || max_rows == 0) return 0;
    hl_iter_line1(&it, text, len);
    while (n < max_rows && rv_next_line(&it, hl_line)) {
        memcpy(out[n], hl_line, sizeof(hl_line));
        n++;
    }
    if (n == max_rows && rv_next_line(&it, hl_line)) {
        size_t l = strlen(out[n - 1]);
        if (l >= RV_COLS) l = RV_COLS - 1;
        out[n - 1][l]     = HL_MORE;
        out[n - 1][l + 1] = '\0';
    }
    return n;
}

/* Rows the home screen gives the first line of text (0..max_rows), and
 * whether that line is shown whole (no HL_MORE). */
static uint8_t hl_field_rows(const char *text, size_t cap, uint8_t max_rows,
                             bool *whole)
{
    rv_iter_t it;
    uint8_t   n = 0;
    size_t    len = hl_line1_len(text, cap);

    if (whole) *whole = true;
    if (len == 0) return 0;
    hl_iter_line1(&it, text, len);
    while (rv_next_line(&it, hl_line)) {
        if (n == max_rows) { if (whole) *whole = false; break; }
        n++;
    }
    return n;
}

/* Rows one entry takes on the home screen. */
static uint8_t hl_entry_rows(const hl_entry_t *e)
{
    uint8_t n = hl_field_rows(e->expr, e->expr_cap, HL_EXPR_MAX_ROWS, NULL);
    n = (uint8_t)(n + hl_field_rows(e->res, e->res_cap, HL_RES_MAX_ROWS, NULL));
    if (e->hint && e->hint[0]) n++;
    return n;
}

/* Lay out up to count entries (entries[0] = newest) into
 * screen[HL_ROWS][RV_COLS + 1], top-aligned, oldest shown first.  Rows not
 * used are "".  Returns the number of rows used. */
static uint8_t hl_layout(const hl_entry_t *entries, uint8_t count,
                         char screen[HL_ROWS][RV_COLS + 1])
{
    uint8_t shown = 0, used = 0, row = 0, r;
    int8_t  i;

    for (r = 0; r < HL_ROWS; r++) screen[r][0] = '\0';
    if (count > HL_MAX_ENTRIES) count = HL_MAX_ENTRIES;

    /* Newest first: take entries while they fit whole. */
    while (shown < count) {
        uint8_t need = hl_entry_rows(&entries[shown]);
        if ((uint16_t)used + need > HL_ROWS) break;
        used = (uint8_t)(used + need);
        shown++;
    }

    for (i = (int8_t)shown - 1; i >= 0; i--) {
        const hl_entry_t *e = &entries[i];
        row = (uint8_t)(row + hl_wrap_field(e->expr, e->expr_cap,
                                            HL_EXPR_MAX_ROWS, &screen[row]));
        row = (uint8_t)(row + hl_wrap_field(e->res, e->res_cap,
                                            HL_RES_MAX_ROWS, &screen[row]));
        if (e->hint && e->hint[0]) {
            rv_put(screen[row], e->hint);
            row++;
        }
    }
    return row;
}

/* Like rv_hidden_is_only_echo, but for the wrapped home row: true when the
 * home screen shows line 1 of text whole (it fits in the stored 63 bytes
 * and in HL_RES_MAX_ROWS rows) and the only other logical line is the
 * worker's "Parsed: ..." echo.  The launcher shows no hint then. */
static bool hl_hidden_is_only_echo(const char *text, size_t cap)
{
    static const char echo[] = "Parsed: ";
    size_t i = 0, k;
    bool   whole;

    if (!text) return false;
    i = hl_line1_len(text, cap);
    if (i > 63 || i >= cap || text[i] != '\n') return false;
    (void)hl_field_rows(text, i, HL_RES_MAX_ROWS, &whole);
    if (!whole) return false;
    i++;                                  /* start of line 2 */
    for (k = 0; k < sizeof(echo) - 1; k++) {
        if (i + k >= cap || text[i + k] != echo[k]) return false;
    }
    for (i += k; i < cap && text[i]; i++) {
        if (text[i] == '\n') {
            return i + 1 >= cap || text[i + 1] == '\0';
        }
    }
    return true;
}

#endif /* CE_HOME_LAYOUT_H */
