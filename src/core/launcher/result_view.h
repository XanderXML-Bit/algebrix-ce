/*
 * result_view.h — full-text result viewer for the CALCENG home screen.
 *
 * The home screen shows one row per result, clipped to 26 columns, so any
 * multi-line worker answer (the intstrategy ranking, long by-parts or
 * partial-fraction forms, steps) used to be invisible past line 1.  The
 * viewer shows the whole answer, wrapped at 26 columns, and scrolls it.
 *
 * This header has no TI dependencies: it turns a list of text segments
 * into wrapped 26-column lines and renders a viewer page into a plain char
 * grid.  src/core/launcher/main.c prints that grid with os_PutStrLine, and
 * the desktop tests (tests/test_main.c, tests/harness/launcher_view_harness.c)
 * check the same functions.
 *
 * Wrap rule, per logical line (text up to '\n'):
 *   - a line of at most RV_COLS characters is emitted as is;
 *   - a longer line breaks at its last space within the first RV_COLS + 1
 *     characters if that space is at column RV_MIN_BREAK or later (the space
 *     is dropped), otherwise it breaks hard at RV_COLS;
 *   - "\n\n" gives an empty line; a trailing '\n' does not;
 *   - '\r', '\t' and other control bytes print as a space.  Bytes >= 0x80
 *     are TI font characters and are kept.
 * No character of the source is lost: every non-space byte appears on some
 * line, in order.
 */
#ifndef CE_RESULT_VIEW_H
#define CE_RESULT_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RV_COLS       26  /* home screen width, large font            */
#define RV_ROWS       10  /* home screen height below the status bar  */
#define RV_BODY_ROWS  (RV_ROWS - 2) /* row 0 header, row 9 footer     */
#define RV_MIN_BREAK  13  /* word-wrap only if the space is this far in */
#define RV_MAX_SEGS   (2 + 32) /* answer + "Steps:" + MAX_STEPS lines  */
#define RV_HDR_TITLE_MAX 12   /* header title bytes kept before the range */

typedef struct {
    const char *text[RV_MAX_SEGS];
    size_t      cap[RV_MAX_SEGS];  /* max bytes to read (stops at NUL) */
    uint8_t     count;
} rv_doc_t;

typedef struct {
    const rv_doc_t *doc;
    uint8_t         seg;
    size_t          pos;
} rv_iter_t;

static void rv_doc_init(rv_doc_t *doc)
{
    memset(doc, 0, sizeof(*doc));
}

/* Append a segment; empty or NULL text is skipped.  Returns false when the
 * document is full. */
static bool rv_doc_add(rv_doc_t *doc, const char *text, size_t cap)
{
    if (!text || cap == 0 || !text[0]) return true;
    if (doc->count >= RV_MAX_SEGS) return false;
    doc->text[doc->count] = text;
    doc->cap[doc->count]  = cap;
    doc->count++;
    return true;
}

static void rv_iter_init(rv_iter_t *it, const rv_doc_t *doc)
{
    it->doc = doc;
    it->seg = 0;
    it->pos = 0;
}

static char rv_at(const rv_iter_t *it, size_t i)
{
    const rv_doc_t *d = it->doc;
    if (i >= d->cap[it->seg]) return '\0';
    return d->text[it->seg][i];
}

static char rv_printable(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < 0x20u || u == 0x7Fu) return ' ';
    return c;
}

/* Emit the next wrapped line into out (RV_COLS + 1 bytes, NUL-terminated).
 * Returns false when the document is exhausted. */
static bool rv_next_line(rv_iter_t *it, char *out)
{
    const rv_doc_t *d = it->doc;

    out[0] = '\0';
    for (;;) {
        size_t start, len, brk, k;
        char c;

        if (it->seg >= d->count) return false;
        start = it->pos;
        c = rv_at(it, start);
        if (c == '\0') {            /* end of this segment */
            it->seg++;
            it->pos = 0;
            continue;
        }

        /* Measure the logical line, but never look past RV_COLS + 1. */
        len = 0;
        while (len <= RV_COLS) {
            c = rv_at(it, start + len);
            if (c == '\0' || c == '\n') break;
            len++;
        }

        if (len <= RV_COLS) {
            /* Whole logical line fits. */
            for (k = 0; k < len; k++) out[k] = rv_printable(rv_at(it, start + k));
            out[len] = '\0';
            it->pos = start + len;
            if (rv_at(it, it->pos) == '\n') it->pos++;
            return true;
        }

        /* Too long: word-break at the last space in [RV_MIN_BREAK, RV_COLS]. */
        brk = 0;
        for (k = RV_COLS; k >= RV_MIN_BREAK; k--) {
            if (rv_at(it, start + k) == ' ') { brk = k; break; }
        }
        if (brk) {
            for (k = 0; k < brk; k++) out[k] = rv_printable(rv_at(it, start + k));
            out[brk] = '\0';
            it->pos = start + brk + 1;   /* drop the space */
        } else {
            for (k = 0; k < RV_COLS; k++) out[k] = rv_printable(rv_at(it, start + k));
            out[RV_COLS] = '\0';
            it->pos = start + RV_COLS;
        }
        return true;
    }
}

/* Number of wrapped lines in the document (0 for an empty document). */
static uint16_t rv_count_lines(const rv_doc_t *doc)
{
    rv_iter_t it;
    char line[RV_COLS + 1];
    uint16_t n = 0;
    rv_iter_init(&it, doc);
    while (n < 0xFFFFu && rv_next_line(&it, line)) n++;
    return n;
}

/* Largest valid top line for a document of total lines. */
static uint16_t rv_max_top(uint16_t total)
{
    return total > RV_BODY_ROWS ? (uint16_t)(total - RV_BODY_ROWS) : 0;
}

/* Viewer keys, mapped from sk_* by the launcher. */
typedef enum {
    RV_KEY_NONE = 0,
    RV_KEY_UP,
    RV_KEY_DOWN,
    RV_KEY_PAGE_UP,    /* LEFT  */
    RV_KEY_PAGE_DOWN,  /* RIGHT */
    RV_KEY_EXIT        /* CLEAR, ENTER, MODE */
} rv_key_t;

/* Apply a key to the scroll position.  Returns true if the viewer should
 * close.  *top is clamped to [0, rv_max_top(total)]. */
static bool rv_apply_key(rv_key_t key, uint16_t total, uint16_t *top)
{
    uint16_t max_top = rv_max_top(total);
    if (*top > max_top) *top = max_top;
    switch (key) {
    case RV_KEY_UP:
        if (*top > 0) (*top)--;
        break;
    case RV_KEY_DOWN:
        if (*top < max_top) (*top)++;
        break;
    case RV_KEY_PAGE_UP:
        *top = *top > RV_BODY_ROWS ? (uint16_t)(*top - RV_BODY_ROWS) : 0;
        break;
    case RV_KEY_PAGE_DOWN:
        *top = (uint16_t)(*top + RV_BODY_ROWS);
        if (*top > max_top) *top = max_top;
        break;
    case RV_KEY_EXIT:
        return true;
    default:
        break;
    }
    return false;
}

static void rv_put(char *row, const char *s)
{
    size_t n = strlen(s);
    if (n > RV_COLS) n = RV_COLS;
    memcpy(row, s, n);
    row[n] = '\0';
}

/* Small unsigned to decimal, no printf (the launcher keeps printf out of
 * this path).  Returns the number of characters written. */
static uint8_t rv_utoa(uint16_t v, char *out)
{
    char tmp[6];
    uint8_t n = 0, i;
    do { tmp[n++] = (char)('0' + v % 10u); v = (uint16_t)(v / 10u); } while (v && n < 5);
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

/* Render one viewer page into screen[RV_ROWS][RV_COLS + 1]:
 *   row 0      "<title> 1-8/12"  (or "<title> 1/1")
 *   rows 1..8  wrapped lines top .. top + 7
 *   row 9      key help
 * Rows are NUL-terminated and not padded. */
static void rv_render(const rv_doc_t *doc, const char *title, uint16_t total,
                      uint16_t top, char screen[RV_ROWS][RV_COLS + 1])
{
    rv_iter_t it;
    char line[RV_COLS + 1];
    /* Scratch for the header before rv_put clips it to 26 columns: a 12-byte
     * title, a space and "65535-65535/65535" need 31 bytes with the NUL. */
    char hdr[RV_HDR_TITLE_MAX + 1 + 17 + 1];
    uint16_t i, last;
    uint8_t r, p;

    for (r = 0; r < RV_ROWS; r++) screen[r][0] = '\0';
    if (top > rv_max_top(total)) top = rv_max_top(total);

    /* Header: title, then the visible line range out of the total. */
    p = 0;
    if (title) {
        while (title[p] && p < RV_HDR_TITLE_MAX) { hdr[p] = title[p]; p++; }
    }
    hdr[p++] = ' ';
    last = (uint16_t)(top + RV_BODY_ROWS);
    if (last > total) last = total;
    if (total == 0) {
        hdr[p] = '\0';
        rv_put(screen[0], hdr);
        rv_put(screen[1], "(empty)");
    } else {
        p = (uint8_t)(p + rv_utoa((uint16_t)(top + 1), hdr + p));
        if (last != top + 1) {
            hdr[p++] = '-';
            p = (uint8_t)(p + rv_utoa(last, hdr + p));
        }
        hdr[p++] = '/';
        p = (uint8_t)(p + rv_utoa(total, hdr + p));
        hdr[p] = '\0';
        rv_put(screen[0], hdr);

        rv_iter_init(&it, doc);
        for (i = 0; i < last && rv_next_line(&it, line); i++) {
            if (i >= top) rv_put(screen[1 + (i - top)], line);
        }
    }
    rv_put(screen[RV_ROWS - 1],
           total > RV_BODY_ROWS ? "UP/DN L/R=page CLEAR=back" : "CLEAR=back");
}

/* Not used by CALCENG since an internal review: the wrapped home row uses
 * hl_hidden_is_only_echo (home_layout.h).  Kept for tests/test_main.c.
 * True when the home row already shows everything in text except a single
 * trailing "Parsed: ..." echo line (the worker repeats the parsed input
 * there): line 1 fits in 26 columns and the only other logical line starts
 * with "Parsed: ".  The launcher shows no hint then; UP still opens it.
 * cap bounds the read like rv_doc_add. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static bool rv_hidden_is_only_echo(const char *text, size_t cap)
{
    static const char echo[] = "Parsed: ";
    size_t i = 0, k;
    if (!text) return false;
    while (i < cap && text[i] && text[i] != '\n') i++;
    if (i > RV_COLS || i >= cap || text[i] != '\n') return false;
    i++;                                  /* start of line 2 */
    for (k = 0; k < sizeof(echo) - 1; k++) {
        if (i + k >= cap || text[i + k] != echo[k]) return false;
    }
    for (i += k; i < cap && text[i]; i++) {
        if (text[i] == '\n') {
            /* Only a trailing newline may follow the echo line. */
            return i + 1 >= cap || text[i + 1] == '\0';
        }
    }
    return true;
}

/* Home-screen hint shown under the newest result when the viewer has more
 * than the one row the home screen shows.  out is RV_COLS + 1 bytes.
 * Returns false (out empty) when nothing is hidden. */
static bool rv_home_hint(uint16_t total, char *out)
{
    uint8_t p;
    out[0] = '\0';
    if (total <= 1) return false;
    memcpy(out, "UP=full result (", 16);
    p = 16;
    p = (uint8_t)(p + rv_utoa(total, out + p));
    memcpy(out + p, " ln)", 5);   /* includes NUL; at most 16+5+4 = 25 */
    return true;
}

#endif /* CE_RESULT_VIEW_H */
