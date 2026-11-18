/*
 * ui.c — Display rendering & keypad input for the TI-84 Plus CE
 *
 * Uses graphx for 320×240 8-bit color rendering and
 * keypadc for direct keyboard scanning with debounce.
 */
#include "ui.h"
#include "mathprint.h"
#include "steps.h"
#include "bookmark.h"
#include "userfunc.h"
#include <tice.h>
#include <graphx.h>
#include <keypadc.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifndef CE_UI_MINIMAL_PRETTY
#define CE_UI_MINIMAL_PRETTY 0
#endif

/* ── Layout constants (pixel coordinates) ────────────────── */
#define TEXT_MARGIN_X     4    /* left margin for header/status text     */
#define CONTENT_MARGIN_X  6    /* left margin for work area content      */
#define INDENT_X          10   /* indented text (steps, menu items)      */
#define LINE_H            12   /* standard text line height              */
#define LINE_H_SMALL      11   /* compact line height (menus/help)       */
#define LINE_H_HEADER     14   /* header/section title line height       */
#define LINE_H_SECTION    13   /* section divider line height            */
#define LINE_H_LARGE      16   /* large gap (settings title)             */
#define GAP_SMALL         2    /* small vertical/horizontal padding      */
#define GAP_MEDIUM        4    /* medium padding (content top offset)    */
#define INPUT_STRIP_H     14   /* height of input area strip             */
#define ERROR_STRIP_H     14   /* height of error banner strip           */
#define CURSOR_W          2    /* block cursor width                     */
#define CURSOR_H          10   /* block cursor height                    */
#define TEXT_CHARS_MAX     39   /* fallback cap when width metrics fail   */
#define WRAP_CHUNK_MAX    120   /* hard cap per wrapped render chunk      */
#define RESULT_FALLBACK_Y  40   /* fallback offset for steps below result */
#define SCROLL_ARROW_X    (SCREEN_W - 14) /* x-pos for scroll indicators */
#define STATUS_RIGHT_X    204  /* right-aligned status text x position   */

/* ── Previous key state for edge detection ───────────────── */
static uint16_t prev_kb[8];   /* kb_Data is uint16_t* */
static bool     second_held;  /* 2nd modifier active  */
static bool     alpha_held;   /* ALPHA modifier active */
static int      result_bottom_y; /* Y below last result draw */

typedef struct {
    int start;
    int end;
} span_t;

/* ── Helpers ─────────────────────────────────────────────── */

/* Detect rising edge (new press) for a key group. */
static uint16_t pressed(uint8_t group)
{
    return kb_Data[group] & ~prev_kb[group];
}

/* Pixel-width wrapping with safe fallback for mock/no-width environments. */
static int wrapped_chunk_len(const char *text, int max_px)
{
    char probe[WRAP_CHUNK_MAX + 1];
    int len = 0;
    int last_space = -1;

    if (!text || !*text) return 0;

    if (max_px <= 0) {
        while (text[len] && text[len] != '\n' && len < TEXT_CHARS_MAX) len++;
        return len > 0 ? len : 1;
    }

    while (text[len] && text[len] != '\n' && len < WRAP_CHUNK_MAX) {
        probe[len] = text[len];
        probe[len + 1] = '\0';
        if (gfx_GetStringWidth(probe) > max_px) break;
        if (text[len] == ' ') last_space = len;
        len++;
    }

    if (len == 0) return 1;
    if (text[len] == '\0' || text[len] == '\n') return len;
    if (last_space > 0) return last_space;
    return len;
}

static void pretty_append(char *out, int out_size, int *pos, const char *s)
{
    while (*s && *pos < out_size - 1) {
        out[*pos] = *s;
        (*pos)++;
        s++;
    }
    out[*pos] = '\0';
}

static void pretty_append_ch(char *out, int out_size, int *pos, char c)
{
    if (*pos < out_size - 1) {
        out[*pos] = c;
        (*pos)++;
    }
    out[*pos] = '\0';
}

static bool is_token_boundary_char(char c)
{
    return !(isalnum((unsigned char)c) || c == '_');
}

static int find_matching_paren(const char *s, int open_idx)
{
    int depth = 0;
    int i;

    if (!s || open_idx < 0 || s[open_idx] != '(') return -1;

    for (i = open_idx; s[i] != '\0'; i++) {
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static void trim_span(const char *s, int *start, int *end)
{
    while (*start < *end && isspace((unsigned char)s[*start])) (*start)++;
    while (*end > *start && isspace((unsigned char)s[*end - 1])) (*end)--;
}

static int split_top_level_args(const char *s, int start, int end,
                                span_t *out, int max_args)
{
    int depth = 0;
    int arg_start = start;
    int i;
    int count = 0;

    for (i = start; i < end; i++) {
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            if (depth > 0) depth--;
        } else if (s[i] == ',' && depth == 0) {
            int a = arg_start;
            int b = i;
            if (count >= max_args) return -1;
            trim_span(s, &a, &b);
            out[count].start = a;
            out[count].end = b;
            count++;
            arg_start = i + 1;
        }
    }

    if (count >= max_args) return -1;
    {
        int a = arg_start;
        int b = end;
        trim_span(s, &a, &b);
        out[count].start = a;
        out[count].end = b;
        count++;
    }

    return count;
}

static void pretty_append_formatted_span(const char *input, span_t span,
                                         char *out, int out_size, int *pos)
{
    char raw[MAX_RESULT_LEN];
    char pretty[MAX_RESULT_LEN * 2];
    int len = span.end - span.start;

    if (len <= 0) return;
    if (len >= (int)sizeof(raw)) len = (int)sizeof(raw) - 1;

    memcpy(raw, &input[span.start], (size_t)len);
    raw[len] = '\0';
    ui_format_math_pretty(raw, pretty, (int)sizeof(pretty));
    pretty_append(out, out_size, pos, pretty);
}

static bool pretty_try_bound_operator(const char *input, int idx,
                                      const char *name, const char *symbol,
                                      char *out, int out_size, int *pos,
                                      int *consumed)
{
    int name_len = (int)strlen(name);
    int open_idx = idx + name_len;
    int close_idx;
    int argc;
    span_t args[4];
    char prev = (idx > 0) ? input[idx - 1] : '\0';

    if (strncmp(&input[idx], name, (size_t)name_len) != 0 ||
        input[open_idx] != '(' ||
        !(idx == 0 || is_token_boundary_char(prev))) {
        return false;
    }

    close_idx = find_matching_paren(input, open_idx);
    if (close_idx < 0) return false;

    argc = split_top_level_args(input, open_idx + 1, close_idx, args, 4);
    if (argc < 0) return false;

    if (strcmp(name, "int") == 0 && (argc == 2 || argc == 4)) {
        pretty_append(out, out_size, pos, symbol);
        if (argc == 4) {
            pretty_append(out, out_size, pos, "_{");
            pretty_append_formatted_span(input, args[2], out, out_size, pos);
            pretty_append(out, out_size, pos, "}^{");
            pretty_append_formatted_span(input, args[3], out, out_size, pos);
            pretty_append_ch(out, out_size, pos, '}');
        }
        pretty_append_ch(out, out_size, pos, ' ');
        pretty_append_formatted_span(input, args[0], out, out_size, pos);
        pretty_append(out, out_size, pos, " d");
        pretty_append_formatted_span(input, args[1], out, out_size, pos);
        *consumed = close_idx - idx + 1;
        return true;
    }

    if ((strcmp(name, "sum") == 0 || strcmp(name, "prod") == 0) && argc == 4) {
        pretty_append(out, out_size, pos, symbol);
        pretty_append(out, out_size, pos, "_{");
        pretty_append_formatted_span(input, args[1], out, out_size, pos);
        pretty_append_ch(out, out_size, pos, '=');
        pretty_append_formatted_span(input, args[2], out, out_size, pos);
        pretty_append(out, out_size, pos, "}^{");
        pretty_append_formatted_span(input, args[3], out, out_size, pos);
        pretty_append(out, out_size, pos, "} ");
        pretty_append_formatted_span(input, args[0], out, out_size, pos);
        *consumed = close_idx - idx + 1;
        return true;
    }

    return false;
}

static const char *superscript_for(char c)
{
    switch (c) {
        case '0': return "⁰";
        case '1': return "¹";
        case '2': return "²";
        case '3': return "³";
        case '4': return "⁴";
        case '5': return "⁵";
        case '6': return "⁶";
        case '7': return "⁷";
        case '8': return "⁸";
        case '9': return "⁹";
        case '+': return "⁺";
        case '-': return "⁻";
        case '=': return "⁼";
        case '(': return "⁽";
        case ')': return "⁾";
        case 'n': return "ⁿ";
        case 'i': return "ⁱ";
        default:  return NULL;
    }
}

static const char *subscript_for(char c)
{
    switch (c) {
        case '0': return "₀";
        case '1': return "₁";
        case '2': return "₂";
        case '3': return "₃";
        case '4': return "₄";
        case '5': return "₅";
        case '6': return "₆";
        case '7': return "₇";
        case '8': return "₈";
        case '9': return "₉";
        case '+': return "₊";
        case '-': return "₋";
        case '=': return "₌";
        case '(': return "₍";
        case ')': return "₎";
        case 'a': return "ₐ";
        case 'e': return "ₑ";
        case 'h': return "ₕ";
        case 'i': return "ᵢ";
        case 'j': return "ⱼ";
        case 'k': return "ₖ";
        case 'l': return "ₗ";
        case 'm': return "ₘ";
        case 'n': return "ₙ";
        case 'o': return "ₒ";
        case 'p': return "ₚ";
        case 'r': return "ᵣ";
        case 's': return "ₛ";
        case 't': return "ₜ";
        case 'u': return "ᵤ";
        case 'v': return "ᵥ";
        case 'x': return "ₓ";
        default:  return NULL;
    }
}

void ui_format_math_pretty(const char *input, char *out, int out_size)
{
#if CE_UI_MINIMAL_PRETTY
    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (!input) return;
    strncpy(out, input, (size_t)(out_size - 1));
    out[out_size - 1] = '\0';
#else
    int i = 0;
    int pos = 0;

    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (!input) return;

    while (input[i] != '\0' && pos < out_size - 1) {
        char prev = (i > 0) ? input[i - 1] : '\0';
        int consumed = 0;

        if (pretty_try_bound_operator(input, i, "int", "\xE2\x88\xAB",
                                      out, out_size, &pos, &consumed) ||
            pretty_try_bound_operator(input, i, "sum", "\xE2\x88\x91",
                                      out, out_size, &pos, &consumed) ||
            pretty_try_bound_operator(input, i, "prod", "\xE2\x88\x8F",
                                      out, out_size, &pos, &consumed)) {
            i += consumed;
            continue;
        }

        if (strncmp(&input[i], "sqrt(", 5) == 0) {
            pretty_append(out, out_size, &pos, "√(");
            i += 5;
            continue;
        }

        if (strncmp(&input[i], "int(", 4) == 0 &&
            (i == 0 || is_token_boundary_char(prev))) {
            pretty_append(out, out_size, &pos, "∫(");
            i += 4;
            continue;
        }

        if (strncmp(&input[i], "sum(", 4) == 0 &&
            (i == 0 || is_token_boundary_char(prev))) {
            pretty_append(out, out_size, &pos, "∑(");
            i += 4;
            continue;
        }

        if (strncmp(&input[i], "prod(", 5) == 0 &&
            (i == 0 || is_token_boundary_char(prev))) {
            pretty_append(out, out_size, &pos, "∏(");
            i += 5;
            continue;
        }

        if (input[i] == '^') {
            int j = i + 1;

            if (input[j] == '(') {
                int k = j + 1;
                int converted = 0;
                bool ok = true;
                while (input[k] != '\0' && input[k] != ')') {
                    if (!superscript_for(input[k])) {
                        ok = false;
                        break;
                    }
                    converted++;
                    k++;
                }
                if (ok && converted > 0 && input[k] == ')') {
                    while (j + 1 < k) {
                        const char *rep = superscript_for(input[j + 1]);
                        pretty_append(out, out_size, &pos, rep);
                        j++;
                    }
                    i = k + 1;
                    continue;
                }
            } else {
                int k = j;
                int converted = 0;
                while (input[k] != '\0') {
                    const char *rep = superscript_for(input[k]);
                    if (!rep) break;
                    pretty_append(out, out_size, &pos, rep);
                    converted++;
                    k++;
                }
                if (converted > 0) {
                    i = k;
                    continue;
                }
            }

            pretty_append_ch(out, out_size, &pos, '^');
            i++;
            continue;
        }

        if (input[i] == '_') {
            int k = i + 1;
            int converted = 0;
            while (input[k] != '\0') {
                const char *rep = subscript_for(input[k]);
                if (!rep) break;
                pretty_append(out, out_size, &pos, rep);
                converted++;
                k++;
            }
            if (converted > 0) {
                i = k;
                continue;
            }
            pretty_append_ch(out, out_size, &pos, '_');
            i++;
            continue;
        }

        if (input[i] == '/') {
            char next = input[i + 1];
            if (prev != '\0' && prev != ' ' && prev != '\n' &&
                next != '\0' && next != ' ' && next != '\n') {
                pretty_append(out, out_size, &pos, "⁄");
            } else {
                pretty_append_ch(out, out_size, &pos, '/');
            }
            i++;
            continue;
        }

        pretty_append_ch(out, out_size, &pos, input[i]);
        i++;
    }
#endif
}

/* ── UI lifecycle ────────────────────────────────────────── */

void ui_init(void)
{
    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_SetTextTransparentColor(COL_WHITE);
    gfx_FillScreen(COL_WHITE);
    gfx_SwapDraw();
    gfx_FillScreen(COL_WHITE);
    memset(prev_kb, 0, sizeof(prev_kb));
    second_held = false;
    alpha_held  = false;
}

void ui_shutdown(void)
{
    gfx_End();
}

/* ── Drawing primitives ──────────────────────────────────── */

void ui_draw_header(void)
{
    gfx_SetColor(COL_DKBLUE);
    gfx_FillRectangle(0, 0, SCREEN_W, HEADER_H);
    gfx_SetTextFGColor(COL_WHITE);
    gfx_SetTextBGColor(COL_DKBLUE);
    gfx_PrintStringXY("CalcEngine " CE_VERSION_STR, TEXT_MARGIN_X, 3);
}

void ui_draw_status(uint16_t nodes_used, uint16_t nodes_free)
{
    char buf[48];
    int y = SCREEN_H - STATUS_H;
    const char *mode_str = g_settings.angle_mode == ANGLE_DEGREES ? "DEG" : "RAD";

    gfx_SetColor(COL_LTGRAY);
    gfx_FillRectangle(0, y, SCREEN_W, STATUS_H);
    gfx_SetTextBGColor(COL_LTGRAY);

    /* Show 2nd/ALPHA modifier indicators */
    if (second_held) {
        gfx_SetTextFGColor(COL_BLUE);
        gfx_PrintStringXY("[2ND]", TEXT_MARGIN_X, y + GAP_SMALL);
        gfx_SetTextFGColor(COL_DKGRAY);
        snprintf(buf, sizeof(buf), " %s N:%u/%u", mode_str,
                 (unsigned)nodes_used, (unsigned)(nodes_used + nodes_free));
        gfx_PrintStringXY(buf, 44, y + GAP_SMALL);
    } else if (alpha_held) {
        gfx_SetTextFGColor(COL_BLUE);
        gfx_PrintStringXY("[A]", TEXT_MARGIN_X, y + GAP_SMALL);
        gfx_SetTextFGColor(COL_DKGRAY);
        snprintf(buf, sizeof(buf), " %s N:%u/%u", mode_str,
                 (unsigned)nodes_used, (unsigned)(nodes_used + nodes_free));
        gfx_PrintStringXY(buf, 28, y + GAP_SMALL);
    } else {
        gfx_SetTextFGColor(COL_DKGRAY);
        snprintf(buf, sizeof(buf), "%s  N:%u/%u", mode_str,
                 (unsigned)nodes_used, (unsigned)(nodes_used + nodes_free));
        gfx_PrintStringXY(buf, TEXT_MARGIN_X, y + GAP_SMALL);
    }

    /* Right side: help hint and battery */
    {
        char rbuf[32];
        uint8_t batt = boot_GetBatteryStatus(); /* 0-4 */
        const char *bstr = batt >= 4 ? "||||" :
                           batt >= 3 ? "||| " :
                           batt >= 2 ? "||  " :
                           batt >= 1 ? "|   " : "LOW!";
        snprintf(rbuf, sizeof(rbuf), "2G=help [%s]", bstr);
        gfx_SetTextFGColor(batt <= 1 ? COL_RED : COL_DKGRAY);
        gfx_PrintStringXY(rbuf, STATUS_RIGHT_X, y + GAP_SMALL);
    }
}

void ui_draw_input(const input_buf_t *ib)
{
    char line[MAX_INPUT_LEN + 2];
    int  y = INPUT_Y;

    /* Try MathPrint 2D rendering first */
    if (ib->len > 0 && mp_try_draw_input(ib))
        return;

    /* Fallback: text-based rendering */

    /* background strip with subtle highlight */
    gfx_SetColor(COL_WHITE);
    gfx_FillRectangle(0, y, SCREEN_W, INPUT_STRIP_H);
    gfx_SetColor(COL_BLUE);
    gfx_HorizLine(0, y, SCREEN_W);

    /* input text in dark blue for distinction from output */
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);

    memcpy(line, ib->buf, ib->len);
    line[ib->len] = '\0';
    gfx_PrintStringXY(line, TEXT_MARGIN_X, y + 3);

    /* block cursor in blue */
    {
        char before[MAX_INPUT_LEN + 1];
        int cx;
        memcpy(before, ib->buf, ib->cursor);
        before[ib->cursor] = '\0';
        cx = TEXT_MARGIN_X + gfx_GetStringWidth(before);
        gfx_SetColor(COL_BLUE);
        gfx_FillRectangle(cx, y + GAP_SMALL, CURSOR_W, CURSOR_H);
    }
}

void ui_clear_work_area(void)
{
    gfx_SetColor(COL_WHITE);
    gfx_FillRectangle(0, WORK_Y, SCREEN_W, WORK_H);
    result_bottom_y = 0;  /* reset for next draw cycle */
}

void ui_draw_result(const char *text)
{
    char pretty[MAX_RESULT_LEN * 4];
    int y = WORK_Y + GAP_MEDIUM;
    int max_px = SCREEN_W - CONTENT_MARGIN_X - CONTENT_MARGIN_X;

    if (!text) text = "";
    ui_format_math_pretty(text, pretty, (int)sizeof(pretty));
    text = pretty;

    gfx_SetTextFGColor(COL_BLACK);
    gfx_SetTextBGColor(COL_WHITE);

    /* Pixel-width wrapped output */
    while (*text && y + CURSOR_H < INPUT_Y) {
        char line[WRAP_CHUNK_MAX + 1];
        int n;
        if (*text == '\n') { text++; y += LINE_H; continue; }
        n = wrapped_chunk_len(text, max_px);
        if (n > WRAP_CHUNK_MAX) n = WRAP_CHUNK_MAX;
        if (n > 0) { memcpy(line, text, n); line[n] = '\0'; }
        else       { line[0] = '\0'; }
        while (n > 0 && line[n - 1] == ' ') line[--n] = '\0';
        gfx_PrintStringXY(line, CONTENT_MARGIN_X, y);
        text += n;
        while (*text == ' ') text++;
        if (*text == '\n') text++;
        y += LINE_H;
    }
    result_bottom_y = y + GAP_MEDIUM;
}

void ui_draw_result_colored(const char *text)
{
    char pretty[MAX_RESULT_LEN * 4];
    int y = WORK_Y + GAP_MEDIUM;
    int max_px = SCREEN_W - CONTENT_MARGIN_X - CONTENT_MARGIN_X;
    bool first_line = true;

    if (!text) text = "";
    ui_format_math_pretty(text, pretty, (int)sizeof(pretty));
    text = pretty;

    gfx_SetTextBGColor(COL_WHITE);

    /* Pixel-width wrapped output with color: first line = answer */
    while (*text && y + CURSOR_H < INPUT_Y) {
        char line[WRAP_CHUNK_MAX + 1];
        int n;
        if (*text == '\n') { text++; y += LINE_H; first_line = false; continue; }
        n = wrapped_chunk_len(text, max_px);
        if (n > WRAP_CHUNK_MAX) n = WRAP_CHUNK_MAX;
        if (n > 0) { memcpy(line, text, n); line[n] = '\0'; }
        else       { line[0] = '\0'; }
        while (n > 0 && line[n - 1] == ' ') line[--n] = '\0';
        gfx_SetTextFGColor(first_line ? COL_DKBLUE : COL_DKGRAY);
        gfx_PrintStringXY(line, CONTENT_MARGIN_X, y);
        text += n;
        while (*text == ' ') text++;
        if (*text == '\n') text++;
        y += LINE_H;
        first_line = false;
    }
    result_bottom_y = y + GAP_MEDIUM;
}

void ui_draw_result_ast(const ast_node_t *node)
{
    if (!node) return;
    mp_draw_result(node);
    {
        mp_bbox_t bbox = mp_measure(node);
        result_bottom_y = WORK_Y + 4 + MP_BBOX_H(bbox) + GAP_MEDIUM;
    }
}

void ui_draw_steps(int scroll_offset)
{
    int count = steps_count();
    int i;
    /* Start below result, with fallback if result hasn't been drawn */
    int y = (result_bottom_y > WORK_Y) ? result_bottom_y : (WORK_Y + RESULT_FALLBACK_Y);
    int max_visible;

    if (count == 0) return;

    /* How many lines fit between y and INPUT_Y? */
    max_visible = (INPUT_Y - y) / LINE_H;
    if (max_visible == 0) return;

    /* Draw "Steps:" label in green */
    gfx_SetTextFGColor(COL_GREEN);
    gfx_SetTextBGColor(COL_WHITE);
    {
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "Steps: (%d total)", (int)count);
        gfx_PrintStringXY(hdr, CONTENT_MARGIN_X, y);
    }
    y += LINE_H_HEADER;
    if (max_visible > 1) max_visible--;

    /* Clamp scroll offset */
    if (scroll_offset > 0 && scroll_offset >= count)
        scroll_offset = count - 1;

    for (i = scroll_offset; i < count && (i - scroll_offset) < max_visible; i++) {
        const char *line = steps_get(i);
        char pretty_line[MAX_RESULT_LEN * 2];
        /* Alternate colors for readability: odd=dark gray, even=black */
        gfx_SetTextFGColor((i % 2 == 0) ? COL_BLACK : COL_DKGRAY);
        if (line) {
            ui_format_math_pretty(line, pretty_line, (int)sizeof(pretty_line));
            gfx_PrintStringXY(pretty_line, INDENT_X, y);
        }
        y += LINE_H;
    }

    /* Scroll indicators in orange for visibility */
    if (scroll_offset > 0) {
        int top_y = (result_bottom_y > WORK_Y) ? result_bottom_y + GAP_SMALL : (WORK_Y + RESULT_FALLBACK_Y + GAP_SMALL);
        gfx_SetTextFGColor(COL_ORANGE);
        gfx_PrintStringXY("^", SCROLL_ARROW_X, top_y);
    }
    if (scroll_offset + max_visible < count) {
        gfx_SetTextFGColor(COL_ORANGE);
        gfx_PrintStringXY("v", SCROLL_ARROW_X, INPUT_Y - LINE_H_HEADER);
    }
}

void ui_draw_error(const char *msg)
{
    /* Red background strip for error label */
    gfx_SetColor(COL_RED);
    gfx_FillRectangle(0, WORK_Y + GAP_SMALL, SCREEN_W, ERROR_STRIP_H);
    gfx_SetTextFGColor(COL_WHITE);
    gfx_SetTextBGColor(COL_RED);
    gfx_PrintStringXY("Error:", CONTENT_MARGIN_X, WORK_Y + GAP_MEDIUM);

    /* Error message in red on white below */
    gfx_SetTextFGColor(COL_RED);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_PrintStringXY(msg, CONTENT_MARGIN_X, WORK_Y + ERROR_STRIP_H + CONTENT_MARGIN_X + GAP_SMALL);
}

void ui_draw_settings(uint8_t cursor)
{
    char buf[48];
    int y = WORK_Y + LINE_H_HEADER + GAP_MEDIUM + GAP_SMALL;

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_PrintStringXY("SETTINGS  (Up/Down, Enter=toggle)", CONTENT_MARGIN_X, y);
    y += LINE_H_LARGE;

    /* 0: Show Work */
    gfx_SetTextFGColor(cursor == 0 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Show Work: %s",
             cursor == 0 ? ">" : " ",
             g_settings.show_steps ? "ON" : "OFF");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 1: Solve Mode */
    gfx_SetTextFGColor(cursor == 1 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Solve Mode: %s",
             cursor == 1 ? ">" : " ",
             g_settings.solve_mode == SOLVE_FULL_AUTO ? "Full Auto" : "Step-by-Step");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 2: Method Selection */
    gfx_SetTextFGColor(cursor == 2 ? COL_BLUE : COL_BLACK);
    {
        const char *mstr = "Auto";
        switch (g_settings.integ_method) {
        case METHOD_USUB:         mstr = "U-Sub"; break;
        case METHOD_PARTS:        mstr = "By Parts"; break;
        case METHOD_TRIGSUB:      mstr = "Trig Sub"; break;
        case METHOD_PARTIAL_FRAC: mstr = "Partial Frac"; break;
        default: break;
        }
        snprintf(buf, sizeof(buf), "%s Method: %s",
                 cursor == 2 ? ">" : " ", mstr);
    }
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_SECTION;

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("Standard Modes", INDENT_X, y);
    y += LINE_H_HEADER;

    /* 3: Angle mode */
    gfx_SetTextFGColor(cursor == 3 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Angle: %s",
             cursor == 3 ? ">" : " ",
             g_settings.angle_mode == ANGLE_RADIANS ? "Radians" : "Degrees");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 4: Precision */
    gfx_SetTextFGColor(cursor == 4 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Precision: %d digits",
             cursor == 4 ? ">" : " ",
             (int)g_settings.precision);
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 5: Complex mode */
    gfx_SetTextFGColor(cursor == 5 ? COL_BLUE : COL_BLACK);
    {
        const char *cstr = "OFF";
        if (g_settings.complex_mode == COMPLEX_RECT) cstr = "Rect";
        else if (g_settings.complex_mode == COMPLEX_POLAR) cstr = "Polar";
        snprintf(buf, sizeof(buf), "%s Complex: %s",
                 cursor == 5 ? ">" : " ", cstr);
    }
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 6: Finance payment timing */
    gfx_SetTextFGColor(cursor == 6 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Finance PMT: %s",
             cursor == 6 ? ">" : " ",
             g_settings.pmt_at_begin ? "BEGIN" : "END");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 7: Step detail */
    gfx_SetTextFGColor(cursor == 7 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Step detail: %s",
             cursor == 7 ? ">" : " ",
             g_settings.step_detail == STEP_COMPACT ? "Compact" : "All");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 8: Show method choices (disambiguation setting) */
    gfx_SetTextFGColor(cursor == 8 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Method choices: %s",
             cursor == 8 ? ">" : " ",
             g_settings.show_method_choices ? "ON" : "OFF");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_HEADER;

    /* 9: Segment pause */
    gfx_SetTextFGColor(cursor == 9 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Segment pause: %s",
             cursor == 9 ? ">" : " ",
             g_settings.segment_pause ? "ON" : "OFF");
    gfx_PrintStringXY(buf, INDENT_X, y);
    y += LINE_H_LARGE + GAP_MEDIUM;

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("MODE/CLEAR = close settings", INDENT_X, y);
}

void ui_draw_help(uint8_t page)
{
    int y = WORK_Y + GAP_MEDIUM;

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);

    if (page == 0) {
        gfx_PrintStringXY("QUICK REFERENCE  (1/3)", CONTENT_MARGIN_X, y); y += LINE_H_HEADER;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Calculus:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" d/dx(expr)      derivative", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" int(expr,x)     indefinite", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" int(expr,x,a,b) definite", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" lim(expr,x,pt)  limit", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" lim(expr,x,pt,+) one-sided", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" sum(expr,n,1,inf) series", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" taylor(f,x,0,5) Taylor poly", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" tangent(f,x,a)  tangent line", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("Left/Right = more pages", CONTENT_MARGIN_X, y);
    } else if (page == 1) {
        gfx_PrintStringXY("QUICK REFERENCE  (2/3)", CONTENT_MARGIN_X, y); y += LINE_H_HEADER;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Algebra:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" solve(x^2-4=0,x) equation", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" expand((x+1)^3) polynomial", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" factor(x^2-1)   factor", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Stats / Matrix:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" mean(1,2,3) median mode stdev", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" linreg(x0,y0,x1,y1,...)", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" ncr(n,r)  npr(n,r)", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" mat(1,2;3,4) det inv rref", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("Left/Right = more pages", CONTENT_MARGIN_X, y);
    } else {
        gfx_PrintStringXY("QUICK REFERENCE  (3/3)", CONTENT_MARGIN_X, y); y += LINE_H_HEADER;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Polar / Parametric:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" polar(r,theta)  to rect", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" rect(x,y)       to polar", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" arclen(f,x,a,b) arc length", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" surfarea(f,x,a,b) surface", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" disk/washer/shell volumes", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Keys:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" Y= eqn  WINDOW viewport  ZOOM", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" TRACE trace  GRAPH draw", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" 2nd+CLEAR or 2nd+0 = catalog", CONTENT_MARGIN_X, y); y += LINE_H_SMALL;
        gfx_PrintStringXY(" 2nd: .=pi (-)=e ,=ans EN=-> APPS=;", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("CLEAR = close help", CONTENT_MARGIN_X, y);
    }
}

/* ── Recents tracking ──────────────────────────────────────── */

#define RECENTS_MAX 10
static menu_item_t recents_items[RECENTS_MAX];
static uint8_t recents_count = 0;

void ui_menu_add_recent(const menu_item_t *item)
{
    int i;
    if (!item || !item->insert_str) return;

    /* Check if already in recents — move to front */
    for (i = 0; i < recents_count; i++) {
        if (strcmp(recents_items[i].insert_str, item->insert_str) == 0) {
            menu_item_t tmp = recents_items[i];
            memmove(&recents_items[1], &recents_items[0],
                    (size_t)i * sizeof(menu_item_t));
            recents_items[0] = tmp;
            return;
        }
    }

    /* Add to front, shift others down */
    if (recents_count < RECENTS_MAX) recents_count++;
    memmove(&recents_items[1], &recents_items[0],
            (size_t)(recents_count - 1) * sizeof(menu_item_t));
    recents_items[0] = *item;
}

uint8_t ui_menu_recents_count(void)
{
    return recents_count;
}

/* ── Main menu categories ──────────────────────────────────── */

/*
 * Page 0  = Recents (dynamic)
 * Pages 1-11 = categories
 *   1 CALCULUS         — derivatives, integrals, limits, series, templates
 *   2 INT METHODS      — forced integration strategies + vector calc ops
 *   3 ODEs & GRAPHS    — differential equations, slope fields, graphing
 *   4 ALGEBRA          — solve, factor, expand, roots, geometry
 *   5 STATISTICS       — descriptive stats, regression, hypothesis tests
 *   6 LINEAR ALGEBRA   — matrix operations
 *   7 PROBABILITY      — all distribution functions
 *   8 FINANCE          — TVM, amortization, interest conversion
 *   9 FUNCTIONS        — built-in math functions (trig, log, etc.)
 *  10 SEQUENCES        — series sums, recursive sequences, convergence
 *  11 SYMBOLS          — constants, Greek letters, special symbols
 */
static const char * const menu_titles[] = {
    "RECENTS",
    "CALCULUS",
    "INT METHODS",
    "ODEs & GRAPHS",
    "ALGEBRA",
    "STATISTICS",
    "LINEAR ALGEBRA",
    "PROBABILITY",
    "FINANCE",
    "FUNCTIONS",
    "SEQUENCES",
    "SYMBOLS"
};
static const char * const menu_tab_titles[MENU_TAB_COUNT] = {
    "Symbols & Functions",
    "Recently Used",
    "Settings",
    "Bookmarks",
    "My Functions"
};

/* ── Page 1: CALCULUS ─────────────────────────────────────────
 * Template types:  1=INTEGRAL_INDEF  2=INTEGRAL_DEF  3=DERIV
 *                  4=LIMIT  5=SUM  6=PRODUCT  7=FRACTION
 *                  8=TAYLOR  9=TANGENT  10=NDERIV               */
static const menu_item_t menu_calc[] = {
    { "d/dx(       derivative",     "d/dx(",    3  },
    { "int(        indefinite",     "int(",     1  },
    { "int(f,x,a,b)definite",       "int(",     2  },
    { "lim(        limit",          "lim(",     4  },
    { "sum(        summation",      "sum(",     5  },
    { "product(    product",        "product(", 6  },
    { "taylor(     Taylor series",  "taylor(",  8  },
    { "tangent(    tangent line",   "tangent(", 9  },
    { "nderiv(     nth derivative", "nderiv(",  10 },
    { "fourier(    Fourier series", "fourier(", 0  },
    { "converge(   convergence",    "converge(",0  },
    { "arclen(     arc length",     "arclen(",  0  },
    { "arclen_param( param arclen", "arclen_param(", 0 },
    { "critical(   critical pts",   "critical(", 0 },
    /* ODE solver */
    { "dsolve(eq,y,x) ODE",        "dsolve(",  0  },
    { "dsolve2(a,b,c,f,x) 2ndODE", "dsolve2(", 0  },
    /* Polar calculus */
    { "polararea(r,t,a,b) area",   "polararea(",  0 },
    { "polararclen(  arc len",     "polararclen(", 0 },
    /* Vector calculus */
    { "dot(         dot product",  "dot(",     0  },
    { "cross(       cross prod",   "cross(",   0  },
    { "grad(f,x,y)  gradient",     "grad(",    0  },
    { "div(Fx,Fy,x,y) diverge",   "div(",     0  },
    { "curl(Fx,Fy,Fz,x,y,z)",     "curl(",    0  },
    /* Laplace transforms */
    { "laplace(f,t,s) L{f}",      "laplace(", 0  },
    { "ilaplace(F,s,t) L^-1",     "ilaplace(",0  },
};

/* ── Page 2: INT METHODS & VECTOR CALC ───────────────────────── */
static const menu_item_t menu_intmethods[] = {
    { "intparts(   by parts",       "intparts(",    0 },
    { "trigsub(    trig sub",       "trigsub(",     0 },
    { "usub(       u-sub",          "usub(",        0 },
    { "intstrategy(rank methods",   "intstrategy(", 0 },
    { "pdiff(      partial deriv",  "pdiff(",       0 },
    { "gradient(   gradient",       "gradient(",    0 },
    { "dblint(     double int",     "dblint(",      0 },
    { "surfarea(   surface area",   "surfarea(",    0 },
    { "disk(       disk method",    "disk(",        0 },
    { "washer(     washer method",  "washer(",      0 },
    { "shell(      shell method",   "shell(",       0 },
    { "relrate(    related rates",  "relrate(",     0 },
};

/* ── Page 3: ODEs & GRAPHS ────────────────────────────────────── */
static const menu_item_t menu_odes[] = {
    { "ode_sep(    separable ODE",  "ode_sep(",    0 },
    { "ode_linear( 1st order lin",  "ode_linear(", 0 },
    { "diffeq(     auto ODE",       "diffeq(",     0 },
    { "euler(      Euler method",   "euler(",      0 },
    { "slopefield( slope field",    "slopefield(", 0 },
    { "graph(      plot function",  "graph(",      0 },
    { "grapheq(    plot equation",  "grapheq(",    0 },
    { "graphimpl(F,x,y) implicit",  "graphimpl(",  0 },
    { "newton(     Newton method",  "newton(",     0 },
    { "bisect(     bisection",      "bisect(",     0 },
    { "secant(     secant method",  "secant(",     0 },
};

/* ── Page 4: ALGEBRA ──────────────────────────────────────────── */
static const menu_item_t menu_alg[] = {
    { "solve(      equation",       "solve(",       0 },
    { "factor(     factor",         "factor(",      0 },
    { "expand(     expand",         "expand(",      0 },
    { "csquare(    compl. sq.",     "csquare(",     0 },
    { "pfrac(      partial frac",   "pfrac(",       0 },
    { "tofrac(     to fraction",    "tofrac(",      0 },
    { "nlsolve(    nonlin system",  "nlsolve(",     0 },
    { "descartes(  sign rule",      "descartes(",   0 },
    { "polydiv(    poly division",  "polydiv(",     0 },
    { "continuity( continuity",     "continuity(",  0 },
    { "line(       line 2 pts",     "line(",        0 },
    { "linept(     slope + pt",     "linept(",      0 },
    { "parabola(   vertex + pt",    "parabola(",    0 },
    { "midpoint(   midpoint",       "midpoint(",    0 },
    { "distance(   distance",       "distance(",    0 },
    { "slope(      slope formula",  "slope(",       0 },
    { "unitcirc(   unit circle",    "unitcirc(",    0 },
};

/* ── Page 5: STATISTICS ───────────────────────────────────────── */
static const menu_item_t menu_stat[] = {
    { "mean(       mean",           "mean(",        0 },
    { "stdev(      std dev",        "stdev(",       0 },
    { "var(        variance",       "var(",         0 },
    { "median(     median",         "median(",      0 },
    { "mode(       mode",           "mode(",        0 },
    { "min(        minimum",        "min(",         0 },
    { "max(        maximum",        "max(",         0 },
    { "linreg(     lin. reg.",      "linreg(",      0 },
    { "quadreg(    quadratic reg",  "quadreg(",     0 },
    { "expreg(     exponential reg","expreg(",      0 },
    { "ncr(        nCr",            "ncr(",         0 },
    { "npr(        nPr",            "npr(",         0 },
    { "gcd(        GCD",            "gcd(",         0 },
    { "lcm(        LCM",            "lcm(",         0 },
    { "randint(    random int",     "randint(",     0 },
    { "ztest(      z-test",         "ztest(",       0 },
    { "ttest(      t-test",         "ttest(",       0 },
    { "chi2test(   chi2 test",      "chi2test(",    0 },
    { "2sampztest( 2-samp z-test",  "2sampztest(",  0 },
    { "2sampttest( 2-samp t-test",  "2sampttest(",  0 },
    { "1propztest( 1-prop z-test",  "1propztest(",  0 },
    { "zinterval(  z interval",     "zinterval(",   0 },
    { "tinterval(  t interval",     "tinterval(",   0 },
    { "2varstats(  2-var stats",    "2varstats(",   0 },
};

/* ── Page 6: LINEAR ALGEBRA ───────────────────────────────────── */
static const menu_item_t menu_linalg[] = {
    { "mat(        matrix literal", "mat(",         0 },
    { "det(        determinant",    "det(",         0 },
    { "inv(        inverse",        "inv(",         0 },
    { "rref(       row reduce",     "rref(",        0 },
    { "transpose(  transpose",      "transpose(",   0 },
    { "eigenval(   eigenvalues",    "eigenval(",    0 },
    { "eigenvec(   eigenvectors",   "eigenvec(",    0 },
};

/* ── Page 7: PROBABILITY ──────────────────────────────────────── */
static const menu_item_t menu_prob[] = {
    { "normalcdf(  normal CDF",     "normalcdf(",   0 },
    { "normalpdf(  normal PDF",     "normalpdf(",   0 },
    { "invnorm(    inv normal",     "invnorm(",     0 },
    { "binompdf(   binom PDF",      "binompdf(",    0 },
    { "binomcdf(   binom CDF",      "binomcdf(",    0 },
    { "poissonpdf( Poisson PDF",    "poissonpdf(",  0 },
    { "poissoncdf( Poisson CDF",    "poissoncdf(",  0 },
    { "geometpdf(  geom PDF",       "geometpdf(",   0 },
    { "geometcdf(  geom CDF",       "geometcdf(",   0 },
    { "tcdf(       t CDF",          "tcdf(",        0 },
    { "invt(       inv-t",          "invt(",        0 },
    { "chi2cdf(    chi2 CDF",       "chi2cdf(",     0 },
    { "invchi2(    inv chi2",       "invchi2(",     0 },
    { "fcdf(       F CDF",          "fcdf(",        0 },
    { "invf(       inv F",          "invf(",        0 },
};

/* ── Page 8: FINANCE ──────────────────────────────────────────── */
static const menu_item_t menu_fin[] = {
    { "pv(         present val",    "pv(",          0 },
    { "fv(         future val",     "fv(",          0 },
    { "pmt(        payment",        "pmt(",         0 },
    { "nper(       periods",        "nper(",        0 },
    { "rate(       interest rate",  "rate(",        0 },
    { "nom(        nominal rate",   "nom(",         0 },
    { "eff(        effective rate", "eff(",         0 },
    { "bal(        balance",        "bal(",         0 },
    { "sumprn(     sum principal",  "sumprn(",      0 },
    { "sumint(     sum interest",   "sumint(",      0 },
};

/* ── Page 9: FUNCTIONS ────────────────────────────────────────── */
static const menu_item_t menu_fns[] = {
    { "sin(        sine",           "sin(",         0 },
    { "cos(        cosine",         "cos(",         0 },
    { "tan(        tangent",        "tan(",         0 },
    { "asin(       arcsine",        "asin(",        0 },
    { "acos(       arccosine",      "acos(",        0 },
    { "atan(       arctangent",     "atan(",        0 },
    { "sec(        secant",         "sec(",         0 },
    { "csc(        cosecant",       "csc(",         0 },
    { "cot(        cotangent",      "cot(",         0 },
    { "sinh(       hyp. sine",      "sinh(",        0 },
    { "cosh(       hyp. cosine",    "cosh(",        0 },
    { "tanh(       hyp. tangent",   "tanh(",        0 },
    { "asinh(      inv hyp sin",    "asinh(",       0 },
    { "acosh(      inv hyp cos",    "acosh(",       0 },
    { "atanh(      inv hyp tan",    "atanh(",       0 },
    { "ln(         natural log",    "ln(",          0 },
    { "log(        base-10 log",    "log(",         0 },
    { "exp(        e^x",            "exp(",         0 },
    { "sqrt(       square root",    "sqrt(",        0 },
    { "abs(        absolute val",   "abs(",         0 },
    { "floor(      floor",          "floor(",       0 },
    { "ceil(       ceiling",        "ceil(",        0 },
    { "round(      round",          "round(",       0 },
    { "sign(       sign",           "sign(",        0 },
    { "real(       real part",      "real(",        0 },
    { "imag(       imag part",      "imag(",        0 },
    { "conj(       conjugate",      "conj(",        0 },
    { "heaviside(  Heaviside",      "heaviside(",   0 },
};

/* ── Page 10: SEQUENCES & SERIES ─────────────────────────────── */
static const menu_item_t menu_seq[] = {
    { "sum(n,lo,hi) numeric sum",   "sum(",         5  },
    { "product(    numeric prod",   "product(",     6  },
    { "recseq(     recursive seq",  "recseq(",      0  },
    { "arithsum(   arithmetic sum", "arithsum(",    0  },
    { "geomsum(    geometric sum",  "geomsum(",     0  },
    { "converge(   convergence",    "converge(",    0  },
    { "fourier(    Fourier series", "fourier(",     0  },
    { "polar(      polar->rect",    "polar(",       0  },
    { "rect(       rect->polar",    "rect(",        0  },
};

/* ── Page 11: SYMBOLS ─────────────────────────────────────────── */
static const menu_item_t menu_sym[] = {
    /* Constants */
    { "inf     infinity",           "inf",          0 },
    { "pi      pi (3.14159...)",    "pi",           0 },
    { "e       Euler's e",          "e",            0 },
    { "i       imaginary unit",     "i",            0 },
    { "ans     previous answer",    "ans",          0 },
    /* Special math functions */
    { "sqrt(   square root",        "sqrt(",        0 },
    { "^(1/2)  square root",        "^(1/2)",       0 },
    { "^(1/3)  cube root",          "^(1/3)",       0 },
    { "^(1/    nth root  ^(1/n)",   "^(1/",         0 },
    { "abs(    absolute value",     "abs(",         0 },
    { "floor(  floor  |_x_|",       "floor(",       0 },
    { "ceil(   ceiling  |^x^|",     "ceil(",        0 },
    { "round(  round",              "round(",       0 },
    { "piecewise( piecewise fn",    "piecewise(",   0 },
    /* Variables / Greek equivalents */
    { "x       variable x",         "x",            0 },
    { "y       variable y",         "y",            0 },
    { "z       variable z",         "z",            0 },
    { "t  (theta / time)",          "t",            0 },
    { "n  (index / integer)",       "n",            0 },
    { "k  (index / constant)",      "k",            0 },
    { "a  (alpha / coeff)",         "a",            0 },
    { "b  (beta  / coeff)",         "b",            0 },
    { "c  (gamma / constant)",      "c",            0 },
    { "m  (mu / slope)",            "m",            0 },
    { "s  (sigma / arc-len)",       "s",            0 },
    { "r  (radius / corr)",         "r",            0 },
    { "p  (phi / probability)",     "p",            0 },
    { "w  (omega / weight)",        "w",            0 },
    /* Fraction template shortcut */
    { "/  fraction [num]/[den]",    "/",            7  },
    /* Factorial */
    { "!       factorial",           "!",            0  },
};

/* Category pages 1-11 */
static const menu_item_t * const menu_cat_pages[] = {
    menu_calc,      /* 1 CALCULUS          */
    menu_intmethods,/* 2 INT METHODS       */
    menu_odes,      /* 3 ODEs & GRAPHS     */
    menu_alg,       /* 4 ALGEBRA           */
    menu_stat,      /* 5 STATISTICS        */
    menu_linalg,    /* 6 LINEAR ALGEBRA    */
    menu_prob,      /* 7 PROBABILITY       */
    menu_fin,       /* 8 FINANCE           */
    menu_fns,       /* 9 FUNCTIONS         */
    menu_seq,       /* 10 SEQUENCES        */
    menu_sym        /* 11 SYMBOLS          */
};
static const uint8_t menu_cat_count[] = {
    25,  /* CALCULUS (14 orig + 11 new)  */
    12,  /* INT METHODS                  */
    11,  /* ODEs & GRAPHS (+graphimpl)   */
    17,  /* ALGEBRA                      */
    24,  /* STATISTICS                   */
    7,   /* LINEAR ALGEBRA (+eigenval/vec) */
    15,  /* PROBABILITY                  */
    10,  /* FINANCE                      */
    28,  /* FUNCTIONS                    */
    9,   /* SEQUENCES                    */
    30   /* SYMBOLS                      */
};
#define MENU_CAT_PAGES 11

/* Accessor: get page items (page 0 = recents, 1-7 = categories) */
const menu_item_t *ui_menu_get_page(uint8_t page)
{
    if (page == 0) return recents_items;
    if (page >= 1 && page <= MENU_CAT_PAGES) return menu_cat_pages[page - 1];
    return NULL;
}

/* Accessor: get item count for a page */
uint8_t ui_menu_get_count(uint8_t page)
{
    if (page == 0) return recents_count;
    if (page >= 1 && page <= MENU_CAT_PAGES) return menu_cat_count[page - 1];
    return 0;
}

const char *ui_menu_tab_title(menu_tab_t tab)
{
    if (tab >= 0 && tab < MENU_TAB_COUNT) return menu_tab_titles[tab];
    return "";
}

void ui_draw_menu(uint8_t cursor, uint8_t page)
{
    int y = WORK_Y + GAP_MEDIUM;
    uint8_t i, count;
    char buf[42];
    const menu_item_t *items;

    if (page >= MENU_PAGES) page = 0;
    count = ui_menu_get_count(page);
    items = ui_menu_get_page(page);

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%s  (%d/%d)", menu_titles[page],
             (int)page + 1, MENU_PAGES);
    gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    if (page == 0 && count == 0) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("No recent functions yet.", CONTENT_MARGIN_X, y);
        y += LINE_H;
        gfx_PrintStringXY("Use R arrow for categories.", CONTENT_MARGIN_X, y);
    } else if (items) {
        for (i = 0; i < count && y + LINE_H_SMALL < INPUT_Y; i++) {
            gfx_SetTextFGColor(i == cursor ? COL_BLUE : COL_BLACK);
            snprintf(buf, sizeof(buf), "%s%s",
                     i == cursor ? ">" : " ", items[i].label);
            gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
            y += LINE_H_SMALL;
        }
    }

    y += GAP_MEDIUM;
    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("L/R=page Up/Dn ENTER=insert", CONTENT_MARGIN_X, y);
}

static void draw_main_menu_tabs(menu_tab_t active_tab)
{
    int x = CONTENT_MARGIN_X;
    int i;

    for (i = 0; i < MENU_TAB_COUNT; i++) {
        int width = gfx_GetStringWidth(menu_tab_titles[i]) + 12;
        if (x + 6 >= SCREEN_W) break;  /* text start would be off-screen */
        gfx_SetColor(i == (int)active_tab ? COL_BLUE : COL_LTGRAY);
        gfx_FillRectangle(x, WORK_Y + GAP_SMALL, width, LINE_H_HEADER);
        gfx_SetTextFGColor(i == (int)active_tab ? COL_WHITE : COL_DKBLUE);
        gfx_SetTextBGColor(i == (int)active_tab ? COL_BLUE : COL_LTGRAY);
        gfx_PrintStringXY(menu_tab_titles[i], x + 6, WORK_Y + GAP_SMALL + 2);
        x += width + GAP_SMALL + 2;
    }
    gfx_SetTextBGColor(COL_WHITE);
}

void ui_draw_main_menu(menu_tab_t tab, uint8_t symbols_page,
                       uint8_t cursor, uint8_t scroll_offset)
{
    int y = WORK_Y + LINE_H_HEADER + GAP_MEDIUM + GAP_SMALL;
    uint8_t count;
    const menu_item_t *items;
    char buf[42];
    int visible_rows;
    int start;
    int i;

    if (tab >= MENU_TAB_COUNT) tab = MENU_TAB_SYMBOLS;

    draw_main_menu_tabs(tab);
    if (tab == MENU_TAB_SETTINGS) {
        ui_draw_settings(cursor);
        return;
    }
    if (tab == MENU_TAB_BOOKMARKS) {
        ui_draw_bookmarks(cursor, scroll_offset);
        return;
    }
    if (tab == MENU_TAB_FUNCS) {
        ui_draw_funcmanager(cursor, scroll_offset);
        return;
    }

    if (tab == MENU_TAB_RECENTS) {
        symbols_page = 0;
    } else if (symbols_page == 0 || symbols_page >= MENU_PAGES) {
        symbols_page = 1;
    }

    count = ui_menu_get_count(symbols_page);
    items = ui_menu_get_page(symbols_page);

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    if (tab == MENU_TAB_SYMBOLS) {
        snprintf(buf, sizeof(buf), "%s  (%d/%d)", menu_titles[symbols_page],
                 (int)symbols_page, MENU_PAGES - 1);
    } else {
        snprintf(buf, sizeof(buf), "%s", menu_titles[0]);
    }
    gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    visible_rows = (INPUT_Y - y - (LINE_H_HEADER * 2)) / LINE_H_SMALL;
    if (visible_rows < 1) visible_rows = 1;
    if (count > 0 && scroll_offset >= count) scroll_offset = count - 1;
    start = scroll_offset;
    if (cursor < start) start = cursor;
    if (cursor >= start + visible_rows) start = cursor - visible_rows + 1;

    if (count == 0) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(tab == MENU_TAB_RECENTS ?
                          "No recent items yet." :
                          "No symbols available.", CONTENT_MARGIN_X, y);
    } else if (items) {
        for (i = start; i < count && i < start + visible_rows; i++) {
            gfx_SetTextFGColor(i == cursor ? COL_BLUE : COL_BLACK);
            snprintf(buf, sizeof(buf), "%s%u. %s",
                     i == cursor ? ">" : " ",
                     (unsigned)(i + 1),
                     items[i].label);
            gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
            y += LINE_H_SMALL;
        }
    }

    if (start > 0) {
        gfx_SetTextFGColor(COL_ORANGE);
        gfx_PrintStringXY("^", SCROLL_ARROW_X, WORK_Y + LINE_H_HEADER + GAP_MEDIUM + GAP_SMALL);
    }
    if (count > 0 && start + visible_rows < count) {
        gfx_SetTextFGColor(COL_ORANGE);
        gfx_PrintStringXY("v", SCROLL_ARROW_X, INPUT_Y - LINE_H_HEADER);
    }

    gfx_SetTextFGColor(COL_DKGRAY);
    if (tab == MENU_TAB_SYMBOLS) {
        gfx_PrintStringXY("2nd+L/R=tab  L/R=category",
                          CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER * 2);
        gfx_PrintStringXY("ENTER=insert  CLEAR=back", CONTENT_MARGIN_X,
                          INPUT_Y - LINE_H_HEADER);
    } else {
        gfx_PrintStringXY("2nd+L/R=tab  Up/Dn=scroll",
                          CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER * 2);
        gfx_PrintStringXY(tab == MENU_TAB_SETTINGS ?
                          "ENTER=toggle  CLEAR=back" :
                          (tab == MENU_TAB_BOOKMARKS || tab == MENU_TAB_FUNCS) ?
                          "ENTER=recall  DEL=delete  CLEAR=back" :
                          "ENTER=insert  CLEAR=back",
                          CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER);
    }
}

/* ── Window editor (viewport settings) ─────────────────────── */

void ui_draw_option_menu(const char *title, const char *subtitle,
                         const char *const *options, uint8_t count,
                         uint8_t cursor, uint8_t scroll_offset)
{
    int y = WORK_Y + GAP_MEDIUM;
    int start;
    int i;
    const int visible_rows = 4;
    char buf[42];

    gfx_SetTextBGColor(COL_WHITE);
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_PrintStringXY(title && title[0] ? title : "OPTIONS",
                      CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    if (subtitle && subtitle[0]) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(subtitle, CONTENT_MARGIN_X, y);
        y += LINE_H_HEADER;
    }

    if (count == 0 || !options) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("No options available.", CONTENT_MARGIN_X, y);
    } else {
        if (scroll_offset >= count) scroll_offset = count - 1;
        start = scroll_offset;
        if (cursor < start) start = cursor;
        if (cursor >= start + visible_rows) start = cursor - visible_rows + 1;

        for (i = start; i < count && i < start + visible_rows; i++) {
            gfx_SetTextFGColor(i == cursor ? COL_BLUE : COL_BLACK);
            snprintf(buf, sizeof(buf), "%s%u. %s",
                     i == cursor ? ">" : " ",
                     (unsigned)(i + 1),
                     options[i] ? options[i] : "");
            gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
            y += LINE_H_SMALL;
        }

        if (start > 0) {
            gfx_SetTextFGColor(COL_ORANGE);
            gfx_PrintStringXY("^", SCROLL_ARROW_X,
                              WORK_Y + GAP_MEDIUM + LINE_H_HEADER * 2);
        }
        if (start + visible_rows < count) {
            gfx_SetTextFGColor(COL_ORANGE);
            gfx_PrintStringXY("v", SCROLL_ARROW_X, INPUT_Y - LINE_H_HEADER * 2);
        }
    }

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("Up/Dn=scroll  ENTER=solve",
                      CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER * 2);
    gfx_PrintStringXY("CLEAR/QUIT=back", CONTENT_MARGIN_X,
                      INPUT_Y - LINE_H_HEADER);
}

void ui_draw_window_editor(ce_float_t xmin, ce_float_t xmax,
                           ce_float_t ymin, ce_float_t ymax,
                           uint8_t cursor)
{
    char buf[42];
    int y = WORK_Y + GAP_MEDIUM;

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_PrintStringXY("WINDOW  (Up/Down, L/R=adjust)", CONTENT_MARGIN_X, y);
    y += LINE_H_LARGE;

    gfx_SetTextFGColor(cursor == 0 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Xmin = %.4g", cursor == 0 ? ">" : " ", (double)xmin);
    gfx_PrintStringXY(buf, INDENT_X, y); y += LINE_H_HEADER;

    gfx_SetTextFGColor(cursor == 1 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Xmax = %.4g", cursor == 1 ? ">" : " ", (double)xmax);
    gfx_PrintStringXY(buf, INDENT_X, y); y += LINE_H_HEADER;

    gfx_SetTextFGColor(cursor == 2 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Ymin = %.4g", cursor == 2 ? ">" : " ", (double)ymin);
    gfx_PrintStringXY(buf, INDENT_X, y); y += LINE_H_HEADER;

    gfx_SetTextFGColor(cursor == 3 ? COL_BLUE : COL_BLACK);
    snprintf(buf, sizeof(buf), "%s Ymax = %.4g", cursor == 3 ? ">" : " ", (double)ymax);
    gfx_PrintStringXY(buf, INDENT_X, y); y += LINE_H_LARGE + GAP_MEDIUM;

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("ENTER=apply  CLEAR=cancel", INDENT_X, y);
}

/* ── Zoom presets ──────────────────────────────────────────── */

static const char * const zoom_labels[] = {
    "ZStandard  [-10,10]",
    "ZDecimal   [-4.7,4.7]",
    "ZTrig      [-2pi,2pi]",
    "ZSquare    equal scale",
    "ZFit       auto-range",
    "Zoom In    2x closer",
    "Zoom Out   2x wider"
};

void ui_draw_zoom_presets(uint8_t cursor)
{
    int y = WORK_Y + GAP_MEDIUM;
    uint8_t i;
    char buf[42];

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_PrintStringXY("ZOOM  (Up/Down, ENTER=select)", CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    for (i = 0; i < ZOOM_PRESETS && y + LINE_H_SMALL < INPUT_Y; i++) {
        gfx_SetTextFGColor(i == cursor ? COL_BLUE : COL_BLACK);
        snprintf(buf, sizeof(buf), "%s%s",
                 i == cursor ? ">" : " ", zoom_labels[i]);
        gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
        y += LINE_H_SMALL;
    }

    y += GAP_MEDIUM;
    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("CLEAR=cancel", CONTENT_MARGIN_X, y);
}

/* ── Input templates (pre-built problem templates) ─────────── */

/* Template categories: Calculus, Algebra, Stats, Finance, Quick */
static const char * const tmpl_titles[] = {
    "CALC TEMPLATES", "ALGEBRA TEMPLATES", "STAT TEMPLATES",
    "FINANCE TEMPLATES", "QUICK TEMPLATES"
};

typedef struct {
    const char *label;       /* display text */
    const char *insert_str;  /* string to insert into input */
    uint8_t     slot_offset; /* cursor offset from start of insert_str to first fill slot */
} tmpl_item_t;

/* slot_offset = cursor position of first user-fill slot within insert_str.
 * The insert_str is a complete, parseable expression used as the default.
 * The cursor is placed at slot_offset so the user can overtype the first arg. */
static const tmpl_item_t tmpl_calc[] = {
    { "Derivative        d/dx(x^2)",             "d/dx(x^2)",              5 },
    { "Integral          int(x^2,x)",            "int(x^2,x)",             4 },
    { "Def. integral     int(x^2,x,0,1)",        "int(x^2,x,0,1)",         4 },
    { "Limit             lim(sin(x)/x,x,0)",     "lim(sin(x)/x,x,0)",      4 },
    { "Sum               sum(1/n^2,n,1,100)",    "sum(1/n^2,n,1,100)",      4 },
    { "Taylor            taylor(sin(x),x,0,5)",  "taylor(sin(x),x,0,5)",    7 },
    { "Tangent           tangent(x^2,x,3)",      "tangent(x^2,x,3)",        8 },
    { "Convergence       nderiv(x^2,x,2)",        "nderiv(x^2,x,2)",          7 },
    { "Improper int      improper(1/x^2,x,1,inf)","improper(1/x^2,x,1,inf)",9 },
    { "Area between crv  areabetween(x^2,x^3,x,0,1)","areabetween(x^2,x^3,x,0,1)",12 },
    { "Int by parts      intparts(x*e^x,x)",     "intparts(x*e^x,x)",        9 },
    { "U-substitution    usub(f,x,u-expr)",        "usub(2x*sin(x^2),x,x^2)",  5 },
    { "Trig sub          trigsub(sqrt(1-x^2),x)","trigsub(sqrt(1-x^2),x)",   8 },
};
static const tmpl_item_t tmpl_alg[] = {
    { "Solve quadratic   x^2-5x+6=0",   "solve(x^2-5x+6=0,x)",  6 },
    { "Solve linear      2x+3=7",        "2x+3=7",                0 },
    { "Factor            x^2-1",         "factor(x^2-1)",         7 },
    { "Expand            (x+1)^3",       "expand((x+1)^3)",       7 },
    { "Compl. square     x^2+6x+2",      "csquare(x^2+6x+2,x)",  8 },
    { "Partial frac      1/(x^2-1)",     "pfrac(1/(x^2-1),x)",    6 },
    { "Systems 2x2       sys(2,1,5,...)", "sys(2,1,5,1,-1,1)",     4 },
    { "Triangle SSS      tri(3,4,5,...)", "tri(3,4,5,0,0,0)",      4 },
};
static const tmpl_item_t tmpl_stat[] = {
    { "Mean              mean(1,2,3,4)",  "mean(1,2,3,4)",         5 },
    { "Std dev           stdev(1,2,3,4)", "stdev(1,2,3,4)",        6 },
    { "Lin reg           linreg(1,2,...)", "linreg(1,2,3,4,5,6)",  7 },
    { "Normal CDF        normalcdf(...)", "normalcdf(-1,1,0,1)",   10 },
    { "Inv norm          invnorm(0.975)", "invnorm(0.975,0,1)",     8 },
    { "Binom PDF         binompdf(10,...)", "binompdf(10,0.5,5)",   9 },
    { "nCr               ncr(10,3)",      "ncr(10,3)",              4 },
    { "Z-test            ztest(52,...)",  "ztest(52,50,5,30)",      6 },
};
static const tmpl_item_t tmpl_fin[] = {
    { "Present val       pv(0.05,10,...)", "pv(0.05,10,100,0)",     3 },
    { "Future val        fv(0.05,10,...)", "fv(0.05,10,100,0)",     3 },
    { "Payment           pmt(0.05,...)",   "pmt(0.05,10,1000,0)",   4 },
    { "Num periods       nper(0.05,...)",  "nper(0.05,100,1000,0)", 5 },
    { "Rate              rate(10,...)",    "rate(10,-100,1000,0)",   5 },
    { "Nom rate          nom(0.05,12)",    "nom(0.05,12)",           4 },
    { "Eff rate          eff(0.05,12)",    "eff(0.05,12)",           4 },
};
static const tmpl_item_t tmpl_quick[] = {
    { "Matrix det        det(mat(1,2;3,4))", "det(mat(1,2;3,4))",   8 },
    { "Matrix inv        inv(mat(1,2;3,4))", "inv(mat(1,2;3,4))",   8 },
    { "Unit circle       unitcirc(pi/6)",    "unitcirc(pi/6)",       9 },
    { "To fraction       tofrac(0.333)",     "tofrac(0.333)",        7 },
    { "Complex           (3+2i)*(1-i)",      "(3+2i)*(1-i)",         1 },
    { "GCD               gcd(24,36)",        "gcd(24,36)",           4 },
};

static const tmpl_item_t * const tmpl_pages[] = {
    tmpl_calc, tmpl_alg, tmpl_stat, tmpl_fin, tmpl_quick
};
static const uint8_t tmpl_page_count[] = { 13, 8, 8, 7, 6 };

uint8_t ui_template_page_count(uint8_t page)
{
    if (page >= TEMPLATE_PAGES) return 0;
    return tmpl_page_count[page];
}

const char *ui_template_get_insert(uint8_t page, uint8_t idx)
{
    if (page >= TEMPLATE_PAGES || idx >= tmpl_page_count[page]) return NULL;
    return tmpl_pages[page][idx].insert_str;
}

uint8_t ui_template_get_slot(uint8_t page, uint8_t idx)
{
    if (page >= TEMPLATE_PAGES || idx >= tmpl_page_count[page]) return 0;
    return tmpl_pages[page][idx].slot_offset;
}

void ui_draw_templates(uint8_t cursor, uint8_t page)
{
    int y = WORK_Y + GAP_MEDIUM;
    uint8_t i, count;
    char buf[42];

    if (page >= TEMPLATE_PAGES) page = 0;
    count = tmpl_page_count[page];

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%s  (%d/%d)", tmpl_titles[page],
             (int)page + 1, TEMPLATE_PAGES);
    gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    for (i = 0; i < count && y + LINE_H_SMALL < INPUT_Y; i++) {
        gfx_SetTextFGColor(i == cursor ? COL_BLUE : COL_BLACK);
        snprintf(buf, sizeof(buf), "%s%s",
                 i == cursor ? ">" : " ", tmpl_pages[page][i].label);
        gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
        y += LINE_H_SMALL;
    }

    y += GAP_MEDIUM;
    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("L/R=page Up/Dn ENTER=fill", CONTENT_MARGIN_X, y);
}

/* ── Context-sensitive help ──────────────────────────────────── */

typedef struct {
    const char *prefix;   /* function prefix to match */
    const char *help;     /* help text (max ~35 chars per line) */
} ctx_help_entry_t;

static const ctx_help_entry_t ctx_help_table[] = {
    { "d/dx(",    "d/dx(expr)\n  Derivative of expr w.r.t. x" },
    { "int(",     "int(expr, var [,lo, hi])\n  Integral (indef. or def.)" },
    { "lim(",     "lim(expr, var, point [,dir])\n  dir: + or - for one-sided" },
    { "sum(",     "sum(expr, var, lo, hi)\n  Finite sum or convergence" },
    { "taylor(",  "taylor(expr, var, center, deg)\n  Taylor polynomial" },
    { "tangent(", "tangent(expr, var, point)\n  Tangent line at point" },
    { "nderiv(",  "nderiv(expr, var, n)\n  nth derivative" },
    { "solve(",   "solve(equation, var)\n  Solve for variable" },
    { "factor(",  "factor(expr)\n  Factor polynomial" },
    { "expand(",  "expand(expr)\n  Expand polynomial" },
    { "csquare(", "csquare(expr, var)\n  Complete the square" },
    { "pfrac(",   "pfrac(expr, var)\n  Partial fraction decomp." },
    { "nlsolve(", "nlsolve(eq1,eq2,v1,v2)\n  Nonlinear system" },
    { "converge(","converge(expr, var)\n  Convergence test" },
    { "fourier(", "fourier(expr, var [,n])\n  Fourier series" },
    { "mean(",    "mean(x1, x2, ...)\n  Arithmetic mean" },
    { "stdev(",   "stdev(x1, x2, ...)\n  Standard deviation" },
    { "linreg(",  "linreg(x1,y1, x2,y2, ...)\n  Linear regression" },
    { "normalcdf(","normalcdf(lo, hi, mu, sigma)\n  Normal CDF" },
    { "invnorm(", "invnorm(area, mu, sigma)\n  Inverse normal" },
    { "binompdf(","binompdf(n, p, k)\n  Binomial probability" },
    { "tcdf(",    "tcdf(lo, hi, df)\n  t-distribution CDF" },
    { "mat(",     "mat(a,b; c,d)\n  Matrix (APPS=; new row)" },
    { "det(",     "det(mat(...))\n  Determinant" },
    { "inv(",     "inv(mat(...))\n  Matrix inverse" },
    { "rref(",    "rref(mat(...))\n  Row echelon form" },
    { "pv(",      "pv(rate, nper, pmt, fv)\n  Present value" },
    { "fv(",      "fv(rate, nper, pmt, pv)\n  Future value" },
    { "pmt(",     "pmt(rate, nper, pv, fv)\n  Payment amount" },
    { "tofrac(",  "tofrac(decimal)\n  Convert to fraction" },
    { "unitcirc(","unitcirc(angle)\n  Unit circle lookup" },
    { "arclen(",  "arclen(expr, var, a, b)\n  Arc length" },
    { "polar(",   "polar(r, theta)\n  Polar to rectangular" },
    { "rect(",    "rect(x, y)\n  Rectangular to polar" },
    { "newton(",       "newton(expr, var, x0)\n  Newton's method" },
    { "bisect(",       "bisect(expr, var, a, b)\n  Bisection method" },
    { "improper(",     "improper(expr, var, lo, hi)\n  Explicit improper integral\n  lo/hi can be inf or -inf" },
    { "areabetween(",  "areabetween(f, g, var, a, b)\n  Area between two curves\n  from a to b" },
    { "intparts(",     "intparts(expr, var)\n  Force integration by parts" },
    { "usub(",         "usub(expr, var [,u])\n  Force u-substitution" },
    { "trigsub(",      "trigsub(expr, var)\n  Force trig substitution" },
    { NULL, NULL }
};

const char *ui_context_help(const char *input_buf)
{
    int i;
    if (!input_buf || input_buf[0] == '\0') return NULL;

    /* Find the last function-like prefix in the input */
    for (i = 0; ctx_help_table[i].prefix; i++) {
        /* Check if input starts with or contains this prefix */
        const char *found = strstr(input_buf, ctx_help_table[i].prefix);
        if (found) {
            /* Return help for the last (innermost) match */
            const char *best = NULL;
            const char *pfx = ctx_help_table[i].prefix;
            const char *scan = input_buf;
            while ((found = strstr(scan, pfx)) != NULL) {
                best = ctx_help_table[i].help;
                scan = found + 1;
            }
            if (best) return best;
        }
    }
    return NULL;
}

void ui_draw_context_help(const char *help_text)
{
    int y = WORK_Y + GAP_MEDIUM;
    int max_px = SCREEN_W - INDENT_X - CONTENT_MARGIN_X;
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_PrintStringXY("HINT:", CONTENT_MARGIN_X, y);
    y += LINE_H;

    gfx_SetTextFGColor(COL_DKGRAY);
    /* Pixel-width wrapped help text */
    while (*help_text && y + CURSOR_H < INPUT_Y) {
        char line[WRAP_CHUNK_MAX + 1];
        int n;
        if (*help_text == '\n') { help_text++; y += LINE_H; continue; }
        n = wrapped_chunk_len(help_text, max_px);
        if (n > WRAP_CHUNK_MAX) n = WRAP_CHUNK_MAX;
        if (n > 0) { memcpy(line, help_text, n); line[n] = '\0'; }
        else       { line[0] = '\0'; }
        while (n > 0 && line[n - 1] == ' ') line[--n] = '\0';
        gfx_PrintStringXY(line, INDENT_X, y);
        help_text += n;
        while (*help_text == ' ') help_text++;
        if (*help_text == '\n') help_text++;
        y += LINE_H;
    }
}

/* ── First-run tutorial ────────────────────────────────────── */

void ui_draw_tutorial(uint8_t page)
{
    int y = WORK_Y + GAP_MEDIUM;
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);

    if (page == 0) {
        gfx_PrintStringXY("WELCOME TO CALCENGINE  (1/3)", CONTENT_MARGIN_X, y); y += LINE_H_HEADER;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("A math suite for TI-84 CE", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("Type an expression, press ENTER", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("to evaluate it.", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_PrintStringXY("Try:  2+3*4", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("Try:  sin(pi/6)", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("Try:  sqrt(144)", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Quick keys:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" Graph: Y= WINDOW ZOOM TRACE", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY(" 2nd+CLEAR or 2nd+0 = catalog", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_BLUE);
        gfx_PrintStringXY("Right arrow = next page", CONTENT_MARGIN_X, y);
    } else if (page == 1) {
        gfx_PrintStringXY("CALCULUS & ALGEBRA  (2/3)", CONTENT_MARGIN_X, y); y += LINE_H_HEADER;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Derivatives:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" d/dx(x^3+2x)  =>  3*x^2+2", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Integrals:", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" int(x^2,x)    =>  x^3/3+C", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Equations (just type them!):", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY(" 2x+3=7        =>  x = 2", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY(" x^2-4=0       =>  x = 2, -2", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("MODE = settings  CLEAR = reset", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_BLUE);
        gfx_PrintStringXY("Right = next  Left = prev", CONTENT_MARGIN_X, y);
    } else {
        gfx_PrintStringXY("TIPS & TRICKS  (3/3)", CONTENT_MARGIN_X, y); y += LINE_H_HEADER;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("2nd+CLEAR or 2nd+0 = catalog", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("Y= WINDOW ZOOM TRACE GRAPH", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("2nd+. = pi   2nd+(-) = e", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("2nd+, = ans  2nd+ENTER = ->", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("MODE on result = frac/sci/dec", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("ALPHA = type variable letters", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("UP/DOWN = scroll steps/history", CONTENT_MARGIN_X, y); y += LINE_H + GAP_SMALL;
        gfx_SetTextFGColor(COL_BLACK);
        gfx_PrintStringXY("Steps are shown automatically", CONTENT_MARGIN_X, y); y += LINE_H;
        gfx_PrintStringXY("Press ENTER to reveal one-by-one", CONTENT_MARGIN_X, y); y += LINE_H_SECTION;
        gfx_SetTextFGColor(COL_GREEN);
        gfx_PrintStringXY("ENTER = start using CalcEngine!", CONTENT_MARGIN_X, y);
    }
}

/* ── Example problems ──────────────────────────────────────── */

typedef struct {
    const char *label;
    const char *insert_str;
} example_item_t;

static const example_item_t ex_calc[] = {
    { "Find d/dx of x^3-2x+1",     "d/dx(x^3-2x+1)" },
    { "Integrate sin(x)cos(x)",     "int(sin(x)*cos(x),x)" },
    { "Evaluate int(x^2,x,0,3)",    "int(x^2,x,0,3)" },
    { "Limit of sin(x)/x as x->0", "lim(sin(x)/x,x,0)" },
    { "Does 1/n^2 converge?",       "converge(1/n^2,n)" },
    { "Taylor of e^x, degree 6",    "taylor(e^x,x,0,6)" },
    { "Tangent to x^2 at x=1",     "tangent(x^2,x,1)" },
    { "Find IVP: f'=2x, f(0)=3",   "ivp(2x,x,0,3)" },
};
static const example_item_t ex_alg[] = {
    { "Solve 3x+7 = 22",            "3x+7=22" },
    { "Solve x^2-5x+6 = 0",        "solve(x^2-5x+6=0,x)" },
    { "Factor x^3-8",               "factor(x^3-8)" },
    { "Expand (2x-1)^4",            "expand((2x-1)^4)" },
    { "System: 2x+y=5, x-y=1",     "sys(2,1,5,1,-1,1)" },
    { "Triangle SSS: 3,4,5",        "tri(3,4,5,0,0,0)" },
    { "Complete square x^2+6x+2",   "csquare(x^2+6x+2,x)" },
    { "Unit circle at pi/4",        "unitcirc(pi/4)" },
};
static const example_item_t ex_other[] = {
    { "Normal CDF P(-1 < Z < 1)",   "normalcdf(-1,1,0,1)" },
    { "Matrix determinant",          "det(mat(2,3;1,4))" },
    { "Linear regression",           "linreg(1,2,2,4,3,5,4,8)" },
    { "Present value of annuity",    "pv(0.08,10,-500,0)" },
    { "10 choose 3",                 "ncr(10,3)" },
    { "Convert 0.375 to fraction",   "tofrac(0.375)" },
    { "Complex: (2+3i)^2",          "(2+3i)^2" },
    { "GCD and LCM",                 "gcd(48,36)" },
};

static const example_item_t * const ex_pages[] = { ex_calc, ex_alg, ex_other };
static const uint8_t ex_page_count[] = { 8, 8, 8 };
static const char * const ex_titles[] = {
    "CALCULUS EXAMPLES", "ALGEBRA EXAMPLES", "OTHER EXAMPLES"
};

uint8_t ui_example_page_count(uint8_t page)
{
    if (page >= EXAMPLE_PAGES) return 0;
    return ex_page_count[page];
}

const char *ui_example_get_insert(uint8_t page, uint8_t idx)
{
    if (page >= EXAMPLE_PAGES || idx >= ex_page_count[page]) return NULL;
    return ex_pages[page][idx].insert_str;
}

void ui_draw_examples(uint8_t cursor, uint8_t page)
{
    int y = WORK_Y + GAP_MEDIUM;
    uint8_t i, count;
    char buf[42];

    if (page >= EXAMPLE_PAGES) page = 0;
    count = ex_page_count[page];

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%s  (%d/%d)", ex_titles[page],
             (int)page + 1, EXAMPLE_PAGES);
    gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    for (i = 0; i < count && y + LINE_H_SMALL < INPUT_Y; i++) {
        gfx_SetTextFGColor(i == cursor ? COL_BLUE : COL_BLACK);
        snprintf(buf, sizeof(buf), "%s%s",
                 i == cursor ? ">" : " ", ex_pages[page][i].label);
        gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
        y += LINE_H_SMALL;
    }

    y += GAP_MEDIUM;
    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("L/R=page Up/Dn ENTER=try it", CONTENT_MARGIN_X, y);
}

/* ── Step-by-step reveal (one step per ENTER) ──────────────── */

void ui_draw_steps_reveal(int reveal_count)
{
    int total = steps_count();
    int i;
    int y = WORK_Y + RESULT_FALLBACK_Y;  /* below result line */
    int max_visible;
    int show = (reveal_count < total) ? reveal_count : total;

    if (total == 0) return;

    max_visible = (INPUT_Y - y) / LINE_H;
    if (max_visible < 2) return;

    /* Draw "Steps:" label with counter */
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    {
        char hdr[40];
        snprintf(hdr, sizeof(hdr), "Steps: (%d/%d)", (int)show, (int)total);
        gfx_PrintStringXY(hdr, CONTENT_MARGIN_X, y);
    }
    y += LINE_H_HEADER;
    if (max_visible > 1) max_visible--;

    /* Show last max_visible steps from 0..show-1 */
    gfx_SetTextFGColor(COL_DKGRAY);
    {
        int start = 0;
        if (show > max_visible) start = show - max_visible;
        for (i = start; i < show; i++) {
            const char *line = steps_get(i);
            char pretty_line[MAX_RESULT_LEN * 2];
            if (line) {
                ui_format_math_pretty(line, pretty_line, (int)sizeof(pretty_line));
                gfx_PrintStringXY(pretty_line, INDENT_X, y);
            }
            y += LINE_H;
        }
    }

    /* Prompt for next step */
    if (show < total) {
        gfx_SetTextFGColor(COL_BLUE);
        gfx_PrintStringXY("[ENTER] next step", CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER);
    } else {
        gfx_SetTextFGColor(COL_GREEN);
        gfx_PrintStringXY("All steps shown.", CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER);
    }
}

void ui_draw_step_prompt(const char *next_op_desc, int ckpt_idx, int ckpt_total)
{
    int y = INPUT_Y - LINE_H_HEADER * 2;
    char buf[80];

    if (next_op_desc && next_op_desc[0]) {
        gfx_SetTextFGColor(COL_BLUE);
        snprintf(buf, sizeof(buf), "Next: %s (%d/%d)",
                 next_op_desc, ckpt_idx + 1, ckpt_total);
        gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
        y += LINE_H;
    }

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("[ENTER] advance  [MODE] finish", CONTENT_MARGIN_X, y);
}

void ui_draw_steps_checkpoint(int ckpt_reveal)
{
    int total_ckpts = steps_checkpoint_count();
    int total_steps = steps_count();
    int show_up_to;
    int i;
    int y = WORK_Y + RESULT_FALLBACK_Y;
    int max_visible;

    if (total_steps == 0) return;

    /* Determine how many steps to show based on checkpoint reveal count */
    if (ckpt_reveal >= total_ckpts) {
        show_up_to = total_steps; /* show everything */
    } else {
        show_up_to = steps_get_checkpoint_step(ckpt_reveal);
    }

    max_visible = (INPUT_Y - y) / LINE_H;
    if (max_visible < 2) return;

    /* Draw "Steps:" label with checkpoint counter */
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    {
        char hdr[48];
        int show_ckpt = ckpt_reveal < total_ckpts ? ckpt_reveal : total_ckpts;
        snprintf(hdr, sizeof(hdr), "Steps: (op %d/%d)",
                 show_ckpt, total_ckpts);
        gfx_PrintStringXY(hdr, CONTENT_MARGIN_X, y);
    }
    y += LINE_H_HEADER;
    if (max_visible > 1) max_visible--;

    /* Show steps up to the current checkpoint boundary */
    {
        int start = 0;
        if (show_up_to > max_visible) start = show_up_to - max_visible;
        for (i = start; i < show_up_to; i++) {
            const char *line = steps_get(i);
            char pretty_line[MAX_RESULT_LEN * 2];
            gfx_SetTextFGColor((i % 2 == 0) ? COL_BLACK : COL_DKGRAY);
            if (line) {
                ui_format_math_pretty(line, pretty_line, (int)sizeof(pretty_line));
                gfx_PrintStringXY(pretty_line, INDENT_X, y);
            }
            y += LINE_H;
        }
    }

    /* Prompt or completion message */
    if (ckpt_reveal < total_ckpts) {
        const char *next_desc = steps_get_checkpoint(ckpt_reveal);
        ui_draw_step_prompt(next_desc, ckpt_reveal, total_ckpts);
    } else {
        gfx_SetTextFGColor(COL_GREEN);
        gfx_PrintStringXY("All operations complete.",
                           CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER);
    }
}

void ui_draw_checkpoint_results(int ckpt_reveal)
{
    int total_ckpts = steps_checkpoint_count();
    int completed = ckpt_reveal;
    int y = WORK_Y + RESULT_FALLBACK_Y;
    int max_visible;
    int start;
    int i;

    if (total_ckpts == 0) return;
    if (completed > total_ckpts) completed = total_ckpts;

    max_visible = (INPUT_Y - y) / LINE_H;
    if (max_visible < 3) return;

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    {
        char hdr[48];
        snprintf(hdr, sizeof(hdr), "Checkpoints: (%d/%d)",
                 completed, total_ckpts);
        gfx_PrintStringXY(hdr, CONTENT_MARGIN_X, y);
    }
    y += LINE_H_HEADER;
    max_visible--;

    start = 0;
    if (completed > max_visible) start = completed - max_visible;
    for (i = start; i < completed; i++) {
        const char *desc = steps_get_checkpoint(i);
        const char *result = steps_get_checkpoint_result(i);
        char pretty_line[MAX_RESULT_LEN * 2];
        char summary[MAX_STEP_LEN + MAX_CKPT_LEN + 12];

        if (result && result[0]) {
            snprintf(summary, sizeof(summary), "%d. %s -> %s",
                     i + 1, desc, result);
        } else {
            snprintf(summary, sizeof(summary), "%d. %s",
                     i + 1, desc);
        }

        gfx_SetTextFGColor((i % 2 == 0) ? COL_BLACK : COL_DKGRAY);
        ui_format_math_pretty(summary, pretty_line, (int)sizeof(pretty_line));
        gfx_PrintStringXY(pretty_line, CONTENT_MARGIN_X, y);
        y += LINE_H;
    }

    if (completed < total_ckpts) {
        ui_draw_step_prompt(steps_get_checkpoint(completed), completed, total_ckpts);
    } else {
        gfx_SetTextFGColor(COL_GREEN);
        gfx_PrintStringXY("All operations complete.",
                          CONTENT_MARGIN_X, INPUT_Y - LINE_H_HEADER);
    }
}

void ui_flip(void)
{
    gfx_SwapDraw();
}

ui_result_enter_action_t ui_result_enter_action(bool checkpoint_nav_active,
                                                bool step_nav_active,
                                                bool show_steps,
                                                bool steps_enabled)
{
    if (checkpoint_nav_active && show_steps) {
        return UI_RESULT_ENTER_ADVANCE_CHECKPOINT;
    }
    if (step_nav_active && show_steps && steps_enabled) {
        return UI_RESULT_ENTER_ADVANCE_STEP;
    }
    return UI_RESULT_ENTER_REPROCESS;
}

/* ── Keyboard polling ────────────────────────────────────── */

key_event_t ui_poll_key(void)
{
    key_event_t evt = { KEY_NONE, 0, NULL };
    uint16_t g1, g2, g3, g4, g5, g6, g7;

    kb_Scan();

    g1 = pressed(1); g2 = pressed(2); g3 = pressed(3);
    g4 = pressed(4); g5 = pressed(5); g6 = pressed(6);
    g7 = pressed(7);

    /* Track 2nd modifier (latching toggle) */
    if (g1 & kb_2nd) { second_held = !second_held; goto done; }

    /* Track ALPHA modifier (latching toggle) */
    if (g2 & kb_Alpha) { alpha_held = !alpha_held; goto done; }

    /* ── ALPHA mode: keys produce lowercase letters ───── */
    /* TI-84 Plus CE ALPHA layer as printed on the keycaps (same table as
     * src/core/launcher/main.c; see the source note there).  ALPHA+0 is a
     * space; θ " : ? have no Algebrix token and are consumed as no-ops. */
    if (alpha_held) {
        char ac = 0;
        if (g2 & kb_Math)    ac = 'a';
        if (g3 & kb_Apps)    ac = 'b';
        if (g4 & kb_Prgm)    ac = 'c';
        if (g2 & kb_Recip)   ac = 'd';
        if (g3 & kb_Sin)     ac = 'e';
        if (g4 & kb_Cos)     ac = 'f';
        if (g5 & kb_Tan)     ac = 'g';
        if (g6 & kb_Power)   ac = 'h';
        if (g2 & kb_Square)  ac = 'i';
        if (g3 & kb_Comma)   ac = 'j';
        if (g4 & kb_LParen)  ac = 'k';
        if (g5 & kb_RParen)  ac = 'l';
        if (g6 & kb_Div)     ac = 'm';
        if (g2 & kb_Log)     ac = 'n';
        if (g3 & kb_7)       ac = 'o';
        if (g4 & kb_8)       ac = 'p';
        if (g5 & kb_9)       ac = 'q';
        if (g6 & kb_Mul)     ac = 'r';
        if (g2 & kb_Ln)      ac = 's';
        if (g3 & kb_4)       ac = 't';
        if (g4 & kb_5)       ac = 'u';
        if (g5 & kb_6)       ac = 'v';
        if (g6 & kb_Sub)     ac = 'w';
        if (g2 & kb_Sto)     ac = 'x';
        if (g3 & kb_1)       ac = 'y';
        if (g4 & kb_2)       ac = 'z';
        if (g3 & kb_0)       ac = ' ';
        if (ac) {
            evt.action = KEY_CHAR; evt.ch = ac;
            alpha_held = false;
            goto done;
        }
        /* θ " : ? are printed on these keys but have no Algebrix token:
         * consume the ALPHA press as a no-op instead of falling through to
         * the digit/operator handlers with ALPHA still armed. */
        if ((g5 & kb_3) || (g6 & kb_Add) || (g4 & kb_DecPnt) || (g5 & kb_Chs)) {
            alpha_held = false;
            goto done;
        }
    }

    /* ── Navigation / control keys ────────────────────── */
    if (g6 & kb_Enter) {
        if (second_held) { evt.action = KEY_STR; evt.str = "->"; second_held = false; goto done; }
        evt.action = KEY_ENTER; goto done;
    }
    if (g6 & kb_Clear) {
        if (second_held) { evt.action = KEY_MENU; second_held = false; goto done; }
        evt.action = KEY_CLEAR; goto done;
    }
    if (g1 & kb_Del) {
        if (second_held) {
            evt.action = KEY_DEL_FORWARD;
            second_held = false;
            goto done;
        }
        evt.action = KEY_DEL;
        goto done;
    }
    if (g1 & kb_Mode) {
        if (second_held) { evt.action = KEY_QUIT; second_held = false; goto done; }
        evt.action = KEY_MODE; goto done;
    }
    if (g7 & kb_Left)  {
        if (second_held) { evt.action = KEY_TAB_LEFT; second_held = false; goto done; }
        evt.action = KEY_LEFT;  goto done;
    }
    if (g7 & kb_Right) {
        if (second_held) { evt.action = KEY_TAB_RIGHT; second_held = false; goto done; }
        evt.action = KEY_RIGHT; goto done;
    }
    if (g7 & kb_Up)    { evt.action = KEY_UP;    goto done; }
    if (g7 & kb_Down)  { evt.action = KEY_DOWN;  goto done; }

    /* ── Digit keys ───────────────────────────────────── */
    if (g3 & kb_0) {
        if (second_held) { evt.action = KEY_MENU; second_held = false; }
        else             { evt.action = KEY_CHAR; evt.ch = '0'; }
        goto done;
    }
    if (g3 & kb_1) { evt.action = KEY_CHAR; evt.ch = '1'; goto done; }
    if (g4 & kb_2) { evt.action = KEY_CHAR; evt.ch = '2'; goto done; }
    if (g5 & kb_3) { evt.action = KEY_CHAR; evt.ch = '3'; goto done; }
    if (g3 & kb_4) { evt.action = KEY_CHAR; evt.ch = '4'; goto done; }
    if (g4 & kb_5) { evt.action = KEY_CHAR; evt.ch = '5'; goto done; }
    if (g5 & kb_6) { evt.action = KEY_CHAR; evt.ch = '6'; goto done; }
    if (g3 & kb_7) { evt.action = KEY_CHAR; evt.ch = '7'; goto done; }
    if (g4 & kb_8) { evt.action = KEY_CHAR; evt.ch = '8'; goto done; }
    if (g5 & kb_9) { evt.action = KEY_CHAR; evt.ch = '9'; goto done; }

    /* ── Decimal point / sign (2nd → pi / e) ────────── */
    if (g4 & kb_DecPnt) {
        if (second_held) { evt.action = KEY_STR; evt.str = "pi"; second_held = false; }
        else             { evt.action = KEY_CHAR; evt.ch = '.'; }
        goto done;
    }
    if (g5 & kb_Chs) {
        if (second_held) { evt.action = KEY_CHAR; evt.ch = 'e'; second_held = false; }
        else             { evt.action = KEY_STR; evt.str = "(-"; }
        goto done;
    }

    /* ── Operators ────────────────────────────────────── */
    if (g6 & kb_Add)   { evt.action = KEY_CHAR; evt.ch = '+'; goto done; }
    if (g6 & kb_Sub)   { evt.action = KEY_CHAR; evt.ch = '-'; goto done; }
    if (g6 & kb_Mul)   { evt.action = KEY_CHAR; evt.ch = '*'; goto done; }
    if (g6 & kb_Div)   { evt.action = KEY_CHAR; evt.ch = '/'; goto done; }
    if (g6 & kb_Power) { evt.action = KEY_CHAR; evt.ch = '^'; goto done; }

    /* ── Parens / comma (2nd+comma → ans) ──────────── */
    if (g4 & kb_LParen) { evt.action = KEY_CHAR; evt.ch = '('; goto done; }
    if (g5 & kb_RParen) { evt.action = KEY_CHAR; evt.ch = ')'; goto done; }
    if (g3 & kb_Comma) {
        if (second_held) { evt.action = KEY_STR; evt.str = "ans"; second_held = false; }
        else             { evt.action = KEY_CHAR; evt.ch = ','; }
        goto done;
    }
    if (g3 & kb_Apps) {
        evt.action = KEY_CHAR;
        evt.ch = ';';
        goto done;
    }

    /* ── Trig functions (2nd → inverse) ───────────────── */
    if (g3 & kb_Sin) {
        evt.action = KEY_STR;
        evt.str = second_held ? "asin(" : "sin(";
        second_held = false; goto done;
    }
    if (g4 & kb_Cos) {
        evt.action = KEY_STR;
        evt.str = second_held ? "acos(" : "cos(";
        second_held = false; goto done;
    }
    if (g5 & kb_Tan) {
        evt.action = KEY_STR;
        evt.str = second_held ? "atan(" : "tan(";
        second_held = false; goto done;
    }

    /* ── ln / log  (2nd → e^( / 10^( ) ─────────────────── */
    if (g2 & kb_Ln) {
        evt.action = KEY_STR;
        evt.str = second_held ? "e^(" : "ln(";
        second_held = false; goto done;
    }
    if (g2 & kb_Log) {
        evt.action = KEY_STR;
        evt.str = second_held ? "10^(" : "log(";
        second_held = false; goto done;
    }

    /* ── x² → ^2,  2nd+x² → sqrt( ────────────────────── */
    if (g2 & kb_Square) {
        if (second_held) { evt.action = KEY_STR; evt.str = "sqrt("; second_held = false; }
        else             { evt.action = KEY_STR; evt.str = "^2";    }
        goto done;
    }

    /* ── x^(-1) key,  2nd+x^(-1) → abs( ────────────────── */
    if (g2 & kb_Recip) {
        evt.action = KEY_STR;
        evt.str = second_held ? "abs(" : "^(-1)";
        second_held = false; goto done;
    }

    /* ── MATH key → nderiv(,  2nd+MATH → solve( ─────── */
    if (g2 & kb_Math) {
        evt.action = KEY_STR;
        evt.str = second_held ? "solve(" : "nderiv(";
        second_held = false; goto done;
    }

    /* ── X,T,θ,n variable key → 'x' ──────────────────── */
    if (g3 & kb_GraphVar) { evt.action = KEY_CHAR; evt.ch = 'x'; goto done; }

    /* ── Graph keys → native graphing controls ────────── */
    if (g1 & kb_Yequ) {
        evt.action = KEY_GRAPH_YEQU;
        second_held = false; goto done;
    }
    if (g1 & kb_Window) {
        evt.action = KEY_GRAPH_WINDOW;
        second_held = false; goto done;
    }
    if (g1 & kb_Zoom) {
        evt.action = KEY_GRAPH_ZOOM;
        second_held = false; goto done;
    }
    if (g1 & kb_Trace) {
        evt.action = KEY_GRAPH_TRACE;
        second_held = false; goto done;
    }
    if (g1 & kb_Graph) {
        if (second_held) {
            evt.action = KEY_HELP;
            second_held = false;
        } else {
            evt.action = KEY_GRAPH_DRAW;
        }
        goto done;
    }

done:
    /* Clear 2nd state if event was produced but key didn't consume it */
    if (evt.action != KEY_NONE && second_held) {
        /* Keys that explicitly consume second_held already set it to false.
         * If we get here with second_held still true, the key didn't use it. */
        second_held = false;
    }
    /* Save state for next call */
    {
        int g;
        for (g = 1; g <= 7; g++) prev_kb[g] = kb_Data[g];
    }
    return evt;
}

/* ── Input buffer helpers ────────────────────────────────── */

void input_buf_init(input_buf_t *ib)
{
    memset(ib, 0, sizeof(*ib));
}

bool input_buf_insert(input_buf_t *ib, char c)
{
    if (ib->len >= MAX_INPUT_LEN - 1) return false;

    /* Auto-close: typing ')' when next char is already ')' skips over it */
    if (c == ')' && ib->cursor < ib->len && ib->buf[ib->cursor] == ')') {
        ib->cursor++;
        return true;
    }

    /* shift right from cursor */
    memmove(ib->buf + ib->cursor + 1, ib->buf + ib->cursor,
            ib->len - ib->cursor);
    ib->buf[ib->cursor] = c;
    ib->len++;
    ib->cursor++;
    ib->buf[ib->len] = '\0';

    /* Auto-close: insert matching ')' after '(' without advancing cursor */
    if (c == '(' && ib->len < MAX_INPUT_LEN - 1) {
        memmove(ib->buf + ib->cursor + 1, ib->buf + ib->cursor,
                ib->len - ib->cursor);
        ib->buf[ib->cursor] = ')';
        ib->len++;
        ib->buf[ib->len] = '\0';
    }

    return true;
}

bool input_buf_insert_str(input_buf_t *ib, const char *s)
{
    while (*s) {
        if (!input_buf_insert(ib, *s++)) return false;
    }
    return true;
}

bool input_buf_backspace(input_buf_t *ib)
{
    if (ib->cursor == 0) return false;
    memmove(ib->buf + ib->cursor - 1, ib->buf + ib->cursor,
            ib->len - ib->cursor);
    ib->cursor--;
    ib->len--;
    ib->buf[ib->len] = '\0';
    return true;
}

bool input_buf_delete(input_buf_t *ib)
{
    if (ib->cursor >= ib->len) return false;
    memmove(ib->buf + ib->cursor, ib->buf + ib->cursor + 1,
            ib->len - ib->cursor - 1);
    ib->len--;
    ib->buf[ib->len] = '\0';
    return true;
}

void input_buf_clear(input_buf_t *ib)
{
    ib->len    = 0;
    ib->cursor = 0;
    ib->buf[0] = '\0';
}

/* ── Bookmark tab drawing ─────────────────────────────────── */

uint8_t ui_bookmarks_count(void)
{
    return (uint8_t)bookmark_count();
}

void ui_draw_bookmarks(uint8_t cursor, uint8_t scroll)
{
    int y = WORK_Y + GAP_MEDIUM;
    int count = bookmark_count();
    int i;
    char buf[56];

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "BOOKMARKS  (%d saved)", count);
    gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    if (count == 0) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("No bookmarks saved.", CONTENT_MARGIN_X, y);
        y += LINE_H;
        gfx_PrintStringXY("Use bookmark(\"name\") to save.", CONTENT_MARGIN_X, y);
        return;
    }

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("ENTER=recall  DEL=delete", CONTENT_MARGIN_X, y);
    y += LINE_H_SMALL;

    for (i = (int)scroll; i < count && y < SCREEN_H - LINE_H; i++) {
        const bookmark_t *bm = bookmark_get(i);
        if (!bm) continue;
        bool selected = ((uint8_t)i == cursor);
        if (selected) {
            gfx_SetColor(COL_HIGHLIGHT);
            gfx_FillRectangle(CONTENT_MARGIN_X - 1, y - 1, SCREEN_W - CONTENT_MARGIN_X * 2, LINE_H + 1);
        }
        gfx_SetTextFGColor(selected ? COL_WHITE : COL_DKBLUE);
        gfx_SetTextBGColor(selected ? COL_HIGHLIGHT : COL_WHITE);
        snprintf(buf, sizeof(buf), "[%s]", bm->name);
        gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
        gfx_SetTextFGColor(selected ? COL_LTGRAY : COL_DKGRAY);
        /* Show truncated expression */
        {
            char expr[28];
            strncpy(expr, bm->expr, 27);
            expr[27] = '\0';
            if (strlen(bm->expr) > 27) { expr[24] = '.'; expr[25] = '.'; expr[26] = '.'; }
            gfx_PrintStringXY(expr, CONTENT_MARGIN_X + 60, y);
        }
        y += LINE_H_SMALL;
    }
    gfx_SetTextBGColor(COL_WHITE);
}

/* ── Function manager tab drawing ─────────────────────────── */

uint8_t ui_funcs_count(void)
{
    return (uint8_t)userfunc_count();
}

void ui_draw_funcmanager(uint8_t cursor, uint8_t scroll)
{
    int y = WORK_Y + GAP_MEDIUM;
    int count = userfunc_count();
    int i;
    char buf[56];
    const char *ws_name = userfunc_active_ws_name();

    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "FUNCTIONS  ws: %s  (%d)", ws_name ? ws_name : "default", count);
    gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
    y += LINE_H_HEADER;

    if (count == 0) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_PrintStringXY("No functions defined.", CONTENT_MARGIN_X, y);
        y += LINE_H;
        gfx_PrintStringXY("Use define(f,x,body) to add.", CONTENT_MARGIN_X, y);
        y += LINE_H;
        gfx_PrintStringXY("Or: f(x) = body", CONTENT_MARGIN_X, y);
        return;
    }

    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_PrintStringXY("ENTER=recall  DEL=delete  ws()=switch", CONTENT_MARGIN_X, y);
    y += LINE_H_SMALL;

    for (i = (int)scroll; i < count && y < SCREEN_H - LINE_H; i++) {
        const userfunc_def_t *fn = userfunc_get(i);
        if (!fn) continue;
        bool selected = ((uint8_t)i == cursor);
        if (selected) {
            gfx_SetColor(COL_HIGHLIGHT);
            gfx_FillRectangle(CONTENT_MARGIN_X - 1, y - 1, SCREEN_W - CONTENT_MARGIN_X * 2, LINE_H + 1);
        }
        gfx_SetTextFGColor(selected ? COL_WHITE : COL_DKBLUE);
        gfx_SetTextBGColor(selected ? COL_HIGHLIGHT : COL_WHITE);
        snprintf(buf, sizeof(buf), "%s(%s)", fn->name, fn->param);
        gfx_PrintStringXY(buf, CONTENT_MARGIN_X, y);
        gfx_SetTextFGColor(selected ? COL_LTGRAY : COL_DKGRAY);
        gfx_PrintStringXY("=", CONTENT_MARGIN_X + 60, y);
        /* Show truncated body */
        {
            char body[20];
            strncpy(body, fn->body, 19);
            body[19] = '\0';
            if (strlen(fn->body) > 19) { body[16]='.'; body[17]='.'; body[18]='.'; }
            gfx_PrintStringXY(body, CONTENT_MARGIN_X + 68, y);
        }
        y += LINE_H_SMALL;
    }
    gfx_SetTextBGColor(COL_WHITE);
}
