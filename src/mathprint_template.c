/*
 * mathprint_template.c — Visual MathPrint input template system
 *
 * Renders fill-in-the-blank templates using graphx primitives.
 * The drawing mirrors the output renderer in mathprint.c but operates on
 * partially-typed slot buffers rather than a complete AST.
 */
#include "mathprint_template.h"
#include <graphx.h>
#include <string.h>
#include <stdio.h>

/* ── Layout constants ────────────────────────────────────────── */
#define TMPL_STRIP_Y      INPUT_Y          /* y of input strip start         */
#define TMPL_STRIP_H_STD  14               /* standard 1-line strip          */
#define TMPL_STRIP_H_MED  28               /* 2-line (fraction/deriv/limit)  */
#define TMPL_STRIP_H_TALL 42               /* 3-line (integral/sum with bnds)*/

#define TMPL_CHAR_W    8                   /* monospace glyph width          */
#define TMPL_CHAR_H    8                   /* glyph height                   */
#define TMPL_HALF_H    4                   /* half glyph height              */
#define TMPL_SYM_W     8                   /* integral/sigma symbol width    */
#define TMPL_SYM_H_STD 10                  /* symbol half-height for std     */
#define TMPL_SYM_H_TALL 18                 /* symbol half-height when tall   */
#define TMPL_SLOT_PAD   3                  /* px padding inside a slot box   */
#define TMPL_SLOT_MIN_W 16                 /* minimum slot box width         */
#define TMPL_OP_GAP    4                   /* gap between elements           */
#define TMPL_LABEL_X   (SCREEN_W - 70)    /* "TEMPLATE:" label x            */

/* ── Helper: gfx_GetStringWidth with fallback ─────────────────── */
static int tmpl_text_w(const char *s)
{
    int w;
    if (!s || !*s) return 0;
    w = gfx_GetStringWidth(s);
    if (w > 0) return w;
    {
        int n = 0;
        while (s[n]) n++;
        return n * TMPL_CHAR_W;
    }
}

/* ── Slot box geometry ───────────────────────────────────────── */
static int slot_box_w(const mp_slot_t *s)
{
    int tw = (s->len > 0)
             ? tmpl_text_w(s->buf)
             : tmpl_text_w(s->placeholder);
    int w = tw + 2 * TMPL_SLOT_PAD;
    if (w < TMPL_SLOT_MIN_W) w = TMPL_SLOT_MIN_W;
    return w;
}

/* Draw a single slot box at pixel (x, yc). */
static void draw_slot(const mp_slot_t *s, int x, int yc, bool active)
{
    int bw   = slot_box_w(s);
    int bh   = TMPL_CHAR_H + 2 * TMPL_SLOT_PAD - 2;
    int by   = yc - bh / 2;
    uint8_t border_col = active ? COL_BLUE : COL_LTGRAY;
    uint8_t text_col;

    /* Box outline */
    gfx_SetColor(border_col);
    gfx_Rectangle(x, by, bw, bh);
    /* White interior */
    gfx_SetColor(COL_WHITE);
    gfx_FillRectangle(x + 1, by + 1, bw - 2, bh - 2);

    if (s->len > 0) {
        text_col = COL_BLACK;
        gfx_SetTextFGColor(text_col);
        gfx_SetTextBGColor(COL_WHITE);
        gfx_PrintStringXY(s->buf, x + TMPL_SLOT_PAD, yc - TMPL_HALF_H);

        /* Cursor bar inside active slot */
        if (active) {
            char tmp[MAX_INPUT_LEN];
            int cw;
            if (s->cursor > 0 && s->cursor <= s->len) {
                int n = (int)s->cursor;
                if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
                memcpy(tmp, s->buf, (size_t)n);
                tmp[n] = '\0';
                cw = tmpl_text_w(tmp);
            } else {
                cw = 0;
            }
            gfx_SetColor(COL_BLUE);
            gfx_FillRectangle(x + TMPL_SLOT_PAD + cw, by + 1, 2, bh - 2);
        }
    } else {
        /* Empty: draw placeholder in gray (or blue for active) */
        text_col = active ? COL_BLUE : COL_DKGRAY;
        gfx_SetTextFGColor(text_col);
        gfx_SetTextBGColor(COL_WHITE);
        gfx_PrintStringXY(s->placeholder, x + TMPL_SLOT_PAD, yc - TMPL_HALF_H);

        /* Cursor at left edge of empty active slot */
        if (active) {
            gfx_SetColor(COL_BLUE);
            gfx_FillRectangle(x + TMPL_SLOT_PAD - 1, by + 1, 2, bh - 2);
        }
    }
}

/* ── Integral symbol ────────────────────────────────────────── */
static void draw_integral_sym(int x, int yc, int half_h)
{
    gfx_SetColor(COL_BLACK);
    gfx_Line(x + 5, yc - half_h,     x + 6, yc - half_h + 1);
    gfx_Line(x + 6, yc - half_h + 1, x + 5, yc - half_h + 2);
    gfx_VertLine(x + 4, yc - half_h + 2, 2 * half_h - 4);
    gfx_Line(x + 4, yc + half_h - 2, x + 3, yc + half_h - 1);
    gfx_Line(x + 3, yc + half_h - 1, x + 2, yc + half_h);
}

/* ── Sigma symbol Σ ──────────────────────────────────────────── */
static void draw_sigma_sym(int x, int yc, int half_h)
{
    gfx_SetColor(COL_BLACK);
    gfx_HorizLine(x, yc - half_h, TMPL_SYM_W);
    gfx_Line(x, yc - half_h, x + TMPL_SYM_W / 2, yc);
    gfx_Line(x + TMPL_SYM_W / 2, yc, x, yc + half_h);
    gfx_HorizLine(x, yc + half_h, TMPL_SYM_W);
}

/* ── Pi symbol Π ─────────────────────────────────────────────── */
static void draw_pi_sym(int x, int yc, int half_h)
{
    gfx_SetColor(COL_BLACK);
    gfx_HorizLine(x, yc - half_h, TMPL_SYM_W);
    gfx_VertLine(x + 2, yc - half_h, 2 * half_h);
    gfx_VertLine(x + TMPL_SYM_W - 3, yc - half_h, 2 * half_h);
}

/* ── Strip background helper ─────────────────────────────────── */
static void draw_strip_bg(int y_top, int total_h)
{
    gfx_SetColor(COL_WHITE);
    gfx_FillRectangle(0, y_top, SCREEN_W, total_h);
    gfx_SetColor(COL_BLUE);
    gfx_HorizLine(0, y_top, SCREEN_W);
}

/* ── Inline text helper ──────────────────────────────────────── */
static void draw_text(const char *s, int x, int yc, uint8_t col)
{
    gfx_SetTextFGColor(col);
    gfx_SetTextBGColor(COL_WHITE);
    if (s && *s)
        gfx_PrintStringXY(s, x, yc - TMPL_HALF_H);
}

/* ── per-type draw ───────────────────────────────────────────── */

static void draw_integral_indef(const mp_template_state_t *tmpl)
{
    /* ∫ [f] d[x]                                     */
    /* slots: 0=f, 1=x                                */
    int ys     = TMPL_STRIP_Y;
    int total  = TMPL_STRIP_H_STD;
    int yc     = ys + total / 2;
    int x      = 4;

    draw_strip_bg(ys, total);

    draw_integral_sym(x, yc, TMPL_SYM_H_STD / 2);
    x += TMPL_SYM_W + TMPL_OP_GAP;

    draw_slot(&tmpl->slots[0], x, yc, tmpl->active_slot == 0);
    x += slot_box_w(&tmpl->slots[0]) + TMPL_OP_GAP;

    draw_text("d", x, yc, COL_BLACK);
    x += TMPL_CHAR_W;

    draw_slot(&tmpl->slots[1], x, yc, tmpl->active_slot == 1);
}

static void draw_integral_def(const mp_template_state_t *tmpl)
{
    /* tall: bounds above/below ∫                     */
    /* slots: 0=f, 1=x(var), 2=a(lo), 3=b(hi)       */
    int y_top  = TMPL_STRIP_Y - (TMPL_STRIP_H_TALL - TMPL_STRIP_H_STD);
    int total  = TMPL_STRIP_H_TALL;
    int yc     = TMPL_STRIP_Y + TMPL_STRIP_H_STD / 2;   /* main expression line */
    int x      = 4;
    int sym_hh = TMPL_SYM_H_TALL / 2;

    draw_strip_bg(y_top, total);

    /* Upper bound slot above the ∫ top */
    {
        int yc_hi = yc - sym_hh - 4 - slot_box_w(&tmpl->slots[3]) / 2;
        /* keep it in the strip */
        if (yc_hi < y_top + 2) yc_hi = y_top + 2 + TMPL_CHAR_H / 2;
        draw_slot(&tmpl->slots[3], x, yc_hi, tmpl->active_slot == 3);
    }
    /* ∫ symbol */
    draw_integral_sym(x, yc, sym_hh);
    /* Lower bound slot below ∫ bottom */
    {
        int yc_lo = yc + sym_hh + 4;
        if (yc_lo + TMPL_CHAR_H / 2 > y_top + total - 2)
            yc_lo = y_top + total - 2 - TMPL_CHAR_H / 2;
        draw_slot(&tmpl->slots[2], x, yc_lo, tmpl->active_slot == 2);
    }

    x += TMPL_SYM_W + TMPL_OP_GAP;

    draw_slot(&tmpl->slots[0], x, yc, tmpl->active_slot == 0);
    x += slot_box_w(&tmpl->slots[0]) + TMPL_OP_GAP;

    draw_text("d", x, yc, COL_BLACK);
    x += TMPL_CHAR_W;

    draw_slot(&tmpl->slots[1], x, yc, tmpl->active_slot == 1);
}

static void draw_deriv(const mp_template_state_t *tmpl)
{
    /* d/d[x](f)  —  rendered as stacked d / d[x] fraction */
    /* slots: 0=f, 1=x                                     */
    int y_top  = TMPL_STRIP_Y - (TMPL_STRIP_H_MED - TMPL_STRIP_H_STD);
    int total  = TMPL_STRIP_H_MED;
    int yc     = TMPL_STRIP_Y + TMPL_STRIP_H_STD / 2;
    int x      = 4;
    int frac_half = TMPL_CHAR_H + 2;   /* half-height of the d/dx fraction */
    int num_yc = yc - frac_half;
    int den_yc = yc + frac_half;
    int frac_w;
    char den_buf[4] = { 'd', 0, 0, 0 };

    draw_strip_bg(y_top, total);

    /* Build "d[x]" string */
    if (tmpl->slots[1].len > 0 && tmpl->slots[1].buf[0] != '\0')
        den_buf[1] = tmpl->slots[1].buf[0];
    else
        den_buf[1] = 'x';   /* default */
    den_buf[2] = '\0';

    {
        int dw  = tmpl_text_w("d");
        int dvw = tmpl_text_w(den_buf);
        frac_w  = (dw > dvw ? dw : dvw) + 4;
    }

    /* Numerator "d" */
    draw_text("d", x + (frac_w - TMPL_CHAR_W) / 2, num_yc, COL_BLACK);
    /* Fraction bar */
    gfx_SetColor(COL_BLACK);
    gfx_HorizLine(x, yc, frac_w);
    /* Denominator "d[x]" (or slot if active) */
    if (tmpl->active_slot == 1) {
        /* Draw variable slot inline below fraction bar */
        char d_str[2] = { 'd', '\0' };
        draw_text(d_str, x, den_yc, COL_BLACK);
        draw_slot(&tmpl->slots[1], x + TMPL_CHAR_W, den_yc, true);
    } else {
        draw_text(den_buf, x + (frac_w - tmpl_text_w(den_buf)) / 2,
                  den_yc, COL_BLACK);
    }
    x += frac_w + TMPL_OP_GAP;

    /* Parenthesised expression slot */
    draw_text("(", x, yc, COL_BLACK);
    x += TMPL_CHAR_W;
    draw_slot(&tmpl->slots[0], x, yc, tmpl->active_slot == 0);
    x += slot_box_w(&tmpl->slots[0]);
    draw_text(")", x, yc, COL_BLACK);
}

static void draw_limit(const mp_template_state_t *tmpl)
{
    /* lim_{[x]->[a]} [f]                              */
    /* slots: 0=f, 1=x, 2=a                           */
    int y_top  = TMPL_STRIP_Y - (TMPL_STRIP_H_MED - TMPL_STRIP_H_STD);
    int total  = TMPL_STRIP_H_MED;
    int yc     = TMPL_STRIP_Y + TMPL_STRIP_H_STD / 2;
    int sub_yc = yc + TMPL_CHAR_H + 2;
    int x      = 4;

    draw_strip_bg(y_top, total);

    /* "lim" label */
    draw_text("lim", x, yc, COL_DKBLUE);
    x += tmpl_text_w("lim");

    /* Subscript: [x]→[a] */
    if (tmpl->active_slot == 1) {
        draw_slot(&tmpl->slots[1], x, sub_yc, true);
    } else {
        if (tmpl->slots[1].len > 0)
            draw_text(tmpl->slots[1].buf, x, sub_yc, COL_BLACK);
        else
            draw_text(tmpl->slots[1].placeholder, x, sub_yc, COL_DKGRAY);
    }
    x += slot_box_w(&tmpl->slots[1]);
    draw_text("->", x, sub_yc, COL_BLACK);
    x += tmpl_text_w("->");
    if (tmpl->active_slot == 2) {
        draw_slot(&tmpl->slots[2], x, sub_yc, true);
    } else {
        if (tmpl->slots[2].len > 0)
            draw_text(tmpl->slots[2].buf, x, sub_yc, COL_BLACK);
        else
            draw_text(tmpl->slots[2].placeholder, x, sub_yc, COL_DKGRAY);
    }

    /* Reset x to right of "lim" for the main expression */
    x = 4 + tmpl_text_w("lim") + TMPL_OP_GAP;
    draw_slot(&tmpl->slots[0], x, yc, tmpl->active_slot == 0);
}

static void draw_sum_product(const mp_template_state_t *tmpl, bool is_sum)
{
    /* Σ or Π with bounds and body                     */
    /* slots: 0=f, 1=n(var), 2=lo, 3=hi               */
    int y_top  = TMPL_STRIP_Y - (TMPL_STRIP_H_TALL - TMPL_STRIP_H_STD);
    int total  = TMPL_STRIP_H_TALL;
    int yc     = TMPL_STRIP_Y + TMPL_STRIP_H_STD / 2;
    int sym_hh = TMPL_SYM_H_TALL / 2;
    int x      = 4;

    draw_strip_bg(y_top, total);

    /* Upper bound */
    {
        int yc_hi = yc - sym_hh - 4;
        if (yc_hi < y_top + 2 + TMPL_CHAR_H / 2) yc_hi = y_top + 2 + TMPL_CHAR_H / 2;
        draw_slot(&tmpl->slots[3], x, yc_hi, tmpl->active_slot == 3);
    }
    /* Symbol */
    if (is_sum) draw_sigma_sym(x, yc, sym_hh);
    else        draw_pi_sym(x, yc, sym_hh);
    /* Lower bound: var=lo */
    {
        int yc_lo = yc + sym_hh + 4;
        if (yc_lo + TMPL_CHAR_H / 2 > y_top + total - 2)
            yc_lo = y_top + total - 2 - TMPL_CHAR_H / 2;
        /* var name then "=" then lo slot */
        int lx = x;
        if (tmpl->active_slot == 1) {
            draw_slot(&tmpl->slots[1], lx, yc_lo, true);
        } else {
            if (tmpl->slots[1].len > 0)
                draw_text(tmpl->slots[1].buf, lx, yc_lo, COL_BLACK);
            else
                draw_text(tmpl->slots[1].placeholder, lx, yc_lo, COL_DKGRAY);
        }
        lx += slot_box_w(&tmpl->slots[1]);
        draw_text("=", lx, yc_lo, COL_BLACK);
        lx += TMPL_CHAR_W;
        draw_slot(&tmpl->slots[2], lx, yc_lo, tmpl->active_slot == 2);
    }

    x += TMPL_SYM_W + TMPL_OP_GAP;
    draw_slot(&tmpl->slots[0], x, yc, tmpl->active_slot == 0);
}

static void draw_fraction(const mp_template_state_t *tmpl)
{
    /* [num] / [den]  —  2D fraction                  */
    /* slots: 0=num, 1=den                            */
    int y_top  = TMPL_STRIP_Y - (TMPL_STRIP_H_MED - TMPL_STRIP_H_STD);
    int total  = TMPL_STRIP_H_MED;
    int yc     = TMPL_STRIP_Y + TMPL_STRIP_H_STD / 2;
    int num_yc = yc - TMPL_CHAR_H - 2;
    int den_yc = yc + TMPL_CHAR_H + 2;
    int w0     = slot_box_w(&tmpl->slots[0]);
    int w1     = slot_box_w(&tmpl->slots[1]);
    int frac_w = (w0 > w1 ? w0 : w1) + 4;
    int x      = 4;

    draw_strip_bg(y_top, total);

    draw_slot(&tmpl->slots[0], x + (frac_w - w0) / 2, num_yc,
              tmpl->active_slot == 0);
    gfx_SetColor(COL_BLACK);
    gfx_HorizLine(x, yc, frac_w);
    draw_slot(&tmpl->slots[1], x + (frac_w - w1) / 2, den_yc,
              tmpl->active_slot == 1);
}

static void draw_function_call(const mp_template_state_t *tmpl,
                               const char *name)
{
    /* name([s0], [s1], ...)  — single line function call */
    int ys    = TMPL_STRIP_Y;
    int total = TMPL_STRIP_H_STD;
    int yc    = ys + total / 2;
    int x     = 4;
    uint8_t i;

    draw_strip_bg(ys, total);

    draw_text(name, x, yc, COL_DKBLUE);
    x += tmpl_text_w(name);
    draw_text("(", x, yc, COL_BLACK);
    x += TMPL_CHAR_W;

    for (i = 0; i < tmpl->num_slots; i++) {
        if (i > 0) {
            draw_text(",", x, yc, COL_BLACK);
            x += TMPL_CHAR_W + 2;
        }
        draw_slot(&tmpl->slots[i], x, yc, tmpl->active_slot == (int8_t)i);
        x += slot_box_w(&tmpl->slots[i]);
    }

    draw_text(")", x, yc, COL_BLACK);
}

/* ── Tab label on the right side ────────────────────────────── */
static void draw_tab_label(int y_top, int total_h,
                           int active_slot, int num_slots)
{
    char buf[20];
    int y = y_top + total_h / 2 - TMPL_CHAR_H / 2;

    snprintf(buf, sizeof(buf), "[%d/%d]", (int)(active_slot + 1), (int)num_slots);
    gfx_SetTextFGColor(COL_DKGRAY);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_PrintStringXY(buf, TMPL_LABEL_X, y);

    gfx_SetTextFGColor(COL_LTGRAY);
    gfx_PrintStringXY("TAB", TMPL_LABEL_X, y + TMPL_CHAR_H + 1);
}

/* ── Public: mp_tmpl_draw ───────────────────────────────────── */
void mp_tmpl_draw(const mp_template_state_t *tmpl)
{
    if (!tmpl || tmpl->type == MP_TMPL_NONE) return;

    gfx_SetTextBGColor(COL_WHITE);

    switch (tmpl->type) {
    case MP_TMPL_INTEGRAL_INDEF:
        draw_integral_indef(tmpl);
        draw_tab_label(TMPL_STRIP_Y, TMPL_STRIP_H_STD,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    case MP_TMPL_INTEGRAL_DEF: {
        int yt = TMPL_STRIP_Y - (TMPL_STRIP_H_TALL - TMPL_STRIP_H_STD);
        draw_integral_def(tmpl);
        draw_tab_label(yt, TMPL_STRIP_H_TALL,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    }
    case MP_TMPL_DERIV: {
        int yt = TMPL_STRIP_Y - (TMPL_STRIP_H_MED - TMPL_STRIP_H_STD);
        draw_deriv(tmpl);
        draw_tab_label(yt, TMPL_STRIP_H_MED,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    }
    case MP_TMPL_LIMIT: {
        int yt = TMPL_STRIP_Y - (TMPL_STRIP_H_MED - TMPL_STRIP_H_STD);
        draw_limit(tmpl);
        draw_tab_label(yt, TMPL_STRIP_H_MED,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    }
    case MP_TMPL_SUM: {
        int yt = TMPL_STRIP_Y - (TMPL_STRIP_H_TALL - TMPL_STRIP_H_STD);
        draw_sum_product(tmpl, true);
        draw_tab_label(yt, TMPL_STRIP_H_TALL,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    }
    case MP_TMPL_PRODUCT: {
        int yt = TMPL_STRIP_Y - (TMPL_STRIP_H_TALL - TMPL_STRIP_H_STD);
        draw_sum_product(tmpl, false);
        draw_tab_label(yt, TMPL_STRIP_H_TALL,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    }
    case MP_TMPL_FRACTION: {
        int yt = TMPL_STRIP_Y - (TMPL_STRIP_H_MED - TMPL_STRIP_H_STD);
        draw_fraction(tmpl);
        draw_tab_label(yt, TMPL_STRIP_H_MED,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    }
    case MP_TMPL_TAYLOR:
        draw_function_call(tmpl, "taylor");
        draw_tab_label(TMPL_STRIP_Y, TMPL_STRIP_H_STD,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    case MP_TMPL_TANGENT:
        draw_function_call(tmpl, "tangent");
        draw_tab_label(TMPL_STRIP_Y, TMPL_STRIP_H_STD,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    case MP_TMPL_NDERIV:
        draw_function_call(tmpl, "nderiv");
        draw_tab_label(TMPL_STRIP_Y, TMPL_STRIP_H_STD,
                       tmpl->active_slot, tmpl->num_slots);
        break;
    default:
        break;
    }
}

/* ── Slot definitions per template type ─────────────────────── */

static void init_slots_integral_indef(mp_template_state_t *t)
{
    t->num_slots = 2;
    t->active_slot = 0;
    /* slot 0: integrand f */
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    /* slot 1: variable x */
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
}

static void init_slots_integral_def(mp_template_state_t *t)
{
    t->num_slots = 4;
    t->active_slot = 0;
    /* slot 0: integrand */
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    /* slot 1: variable */
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
    /* slot 2: lower bound */
    t->slots[2].len = 0; t->slots[2].cursor = 0;
    t->slots[2].placeholder = "a"; t->slots[2].mandatory = true;
    /* slot 3: upper bound */
    t->slots[3].len = 0; t->slots[3].cursor = 0;
    t->slots[3].placeholder = "b"; t->slots[3].mandatory = true;
}

static void init_slots_deriv(mp_template_state_t *t)
{
    t->num_slots = 2;
    t->active_slot = 0;
    /* slot 0: expression */
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    /* slot 1: variable (default x) */
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
}

static void init_slots_limit(mp_template_state_t *t)
{
    t->num_slots = 3;
    t->active_slot = 0;
    /* slot 0: expression */
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    /* slot 1: variable */
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
    /* slot 2: point */
    t->slots[2].len = 0; t->slots[2].cursor = 0;
    t->slots[2].placeholder = "a"; t->slots[2].mandatory = true;
}

static void init_slots_sum_product(mp_template_state_t *t)
{
    t->num_slots = 4;
    t->active_slot = 0;
    /* slot 0: expression */
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    /* slot 1: index variable */
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'n'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "n"; t->slots[1].mandatory = true;
    /* slot 2: lower bound */
    t->slots[2].len = 1; t->slots[2].cursor = 1;
    t->slots[2].buf[0] = '1'; t->slots[2].buf[1] = '\0';
    t->slots[2].placeholder = "lo"; t->slots[2].mandatory = true;
    /* slot 3: upper bound */
    t->slots[3].len = 0; t->slots[3].cursor = 0;
    t->slots[3].placeholder = "hi"; t->slots[3].mandatory = true;
}

static void init_slots_fraction(mp_template_state_t *t)
{
    t->num_slots = 2;
    t->active_slot = 0;
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "num"; t->slots[0].mandatory = true;
    t->slots[1].len = 0; t->slots[1].cursor = 0;
    t->slots[1].placeholder = "den"; t->slots[1].mandatory = true;
}

static void init_slots_taylor(mp_template_state_t *t)
{
    t->num_slots = 4;
    t->active_slot = 0;
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
    t->slots[2].len = 1; t->slots[2].cursor = 1;
    t->slots[2].buf[0] = '0'; t->slots[2].buf[1] = '\0';
    t->slots[2].placeholder = "ctr"; t->slots[2].mandatory = true;
    t->slots[3].len = 1; t->slots[3].cursor = 1;
    t->slots[3].buf[0] = '5'; t->slots[3].buf[1] = '\0';
    t->slots[3].placeholder = "deg"; t->slots[3].mandatory = true;
}

static void init_slots_tangent(mp_template_state_t *t)
{
    t->num_slots = 3;
    t->active_slot = 0;
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
    t->slots[2].len = 0; t->slots[2].cursor = 0;
    t->slots[2].placeholder = "a"; t->slots[2].mandatory = true;
}

static void init_slots_nderiv(mp_template_state_t *t)
{
    t->num_slots = 3;
    t->active_slot = 0;
    t->slots[0].len = 0; t->slots[0].cursor = 0;
    t->slots[0].placeholder = "f"; t->slots[0].mandatory = true;
    t->slots[1].len = 1; t->slots[1].cursor = 1;
    t->slots[1].buf[0] = 'x'; t->slots[1].buf[1] = '\0';
    t->slots[1].placeholder = "x"; t->slots[1].mandatory = true;
    t->slots[2].len = 1; t->slots[2].cursor = 1;
    t->slots[2].buf[0] = '2'; t->slots[2].buf[1] = '\0';
    t->slots[2].placeholder = "n"; t->slots[2].mandatory = true;
}

/* ── mp_tmpl_init ────────────────────────────────────────────── */
void mp_tmpl_init(mp_template_state_t *tmpl, mp_tmpl_type_t type)
{
    int i;
    if (!tmpl) return;
    memset(tmpl, 0, sizeof(*tmpl));
    tmpl->type = type;
    tmpl->active_slot = 0;

    /* Clear all slots */
    for (i = 0; i < MP_TMPL_MAX_SLOTS; i++) {
        tmpl->slots[i].len = 0;
        tmpl->slots[i].cursor = 0;
        tmpl->slots[i].placeholder = "";
        tmpl->slots[i].mandatory = false;
    }

    switch (type) {
    case MP_TMPL_INTEGRAL_INDEF: init_slots_integral_indef(tmpl); break;
    case MP_TMPL_INTEGRAL_DEF:   init_slots_integral_def(tmpl);   break;
    case MP_TMPL_DERIV:          init_slots_deriv(tmpl);          break;
    case MP_TMPL_LIMIT:          init_slots_limit(tmpl);          break;
    case MP_TMPL_SUM:
    case MP_TMPL_PRODUCT:        init_slots_sum_product(tmpl);    break;
    case MP_TMPL_FRACTION:       init_slots_fraction(tmpl);       break;
    case MP_TMPL_TAYLOR:         init_slots_taylor(tmpl);         break;
    case MP_TMPL_TANGENT:        init_slots_tangent(tmpl);        break;
    case MP_TMPL_NDERIV:         init_slots_nderiv(tmpl);         break;
    default: tmpl->type = MP_TMPL_NONE; break;
    }
}

void mp_tmpl_cancel(mp_template_state_t *tmpl)
{
    if (tmpl) memset(tmpl, 0, sizeof(*tmpl));
}

bool mp_tmpl_active(const mp_template_state_t *tmpl)
{
    return tmpl && tmpl->type != MP_TMPL_NONE;
}

/* ── Slot text editing helpers ───────────────────────────────── */

static bool slot_insert_char(mp_slot_t *s, char c)
{
    if (s->len >= MAX_INPUT_LEN - 1) return false;
    /* Shift right from cursor */
    if (s->cursor < s->len) {
        memmove(&s->buf[s->cursor + 1], &s->buf[s->cursor],
                s->len - s->cursor);
    }
    s->buf[s->cursor] = c;
    s->cursor++;
    s->len++;
    s->buf[s->len] = '\0';
    return true;
}

static bool slot_insert_str(mp_slot_t *s, const char *str)
{
    while (*str) {
        if (!slot_insert_char(s, *str)) return false;
        str++;
    }
    return true;
}

static bool slot_backspace(mp_slot_t *s)
{
    if (s->cursor == 0) return false;
    memmove(&s->buf[s->cursor - 1], &s->buf[s->cursor],
            s->len - s->cursor);
    s->cursor--;
    s->len--;
    s->buf[s->len] = '\0';
    return true;
}

static bool slot_delete_forward(mp_slot_t *s)
{
    if (s->cursor >= s->len) return false;
    memmove(&s->buf[s->cursor], &s->buf[s->cursor + 1],
            s->len - s->cursor - 1);
    s->len--;
    s->buf[s->len] = '\0';
    return true;
}

/* ── UP/DOWN slot mapping per template type ──────────────────── */
/* Returns next slot index when navigating vertically, or -1 */
static int slot_nav_vertical(mp_tmpl_type_t type, int cur_slot, bool up)
{
    /* Integral definite: visually lo(2) <-> f(0) <-> hi(3) */
    if (type == MP_TMPL_INTEGRAL_DEF) {
        if (up)  return (cur_slot == 2) ? 0 : (cur_slot == 0) ? 3 : -1;
        else     return (cur_slot == 3) ? 0 : (cur_slot == 0) ? 2 : -1;
    }
    /* Sum/Product: visually lo(2) <-> f(0) <-> hi(3) */
    if (type == MP_TMPL_SUM || type == MP_TMPL_PRODUCT) {
        if (up)  return (cur_slot == 2) ? 0 : (cur_slot == 0) ? 3 : -1;
        else     return (cur_slot == 3) ? 0 : (cur_slot == 0) ? 2 : -1;
    }
    /* Fraction: up->numerator(0), down->denominator(1) */
    if (type == MP_TMPL_FRACTION) {
        return up ? 0 : 1;
    }
    /* Deriv / Limit: up->expression(0), down->variable(1) */
    if (type == MP_TMPL_DERIV || type == MP_TMPL_LIMIT) {
        return up ? 0 : 1;
    }
    return -1;
}

/* ── mp_tmpl_key ─────────────────────────────────────────────── */
bool mp_tmpl_key(mp_template_state_t *tmpl,
                 const key_event_t   *evt,
                 bool                *done,
                 bool                *cancelled)
{
    mp_slot_t *s;

    if (!tmpl || tmpl->type == MP_TMPL_NONE) return false;
    if (!evt || evt->action == KEY_NONE) return false;
    if (done)      *done      = false;
    if (cancelled) *cancelled = false;

    s = &tmpl->slots[tmpl->active_slot];

    switch (evt->action) {

    case KEY_CLEAR:
    case KEY_QUIT:
        if (cancelled) *cancelled = true;
        return true;

    case KEY_ENTER:
        if (mp_tmpl_ready(tmpl)) {
            if (done) *done = true;
        }
        return true;

    case KEY_CHAR:
        slot_insert_char(s, evt->ch);
        return true;

    case KEY_STR:
        if (evt->str) slot_insert_str(s, evt->str);
        return true;

    case KEY_DEL:
        slot_backspace(s);
        return true;

    case KEY_DEL_FORWARD:
        slot_delete_forward(s);
        return true;

    case KEY_LEFT:
        if (s->cursor > 0) {
            s->cursor--;
        } else {
            /* At start of slot — go to previous slot if exists */
            if (tmpl->active_slot > 0) {
                tmpl->active_slot--;
                /* Position cursor at end of previous slot */
                tmpl->slots[tmpl->active_slot].cursor =
                    tmpl->slots[tmpl->active_slot].len;
            }
        }
        return true;

    case KEY_RIGHT:
        if (s->cursor < s->len) {
            s->cursor++;
        } else {
            /* At end of slot — go to next slot if exists */
            if (tmpl->active_slot + 1 < (int8_t)tmpl->num_slots) {
                tmpl->active_slot++;
                tmpl->slots[tmpl->active_slot].cursor = 0;
            }
        }
        return true;

    case KEY_TAB_RIGHT:
        if (tmpl->active_slot + 1 < (int8_t)tmpl->num_slots) {
            tmpl->active_slot++;
            tmpl->slots[tmpl->active_slot].cursor =
                tmpl->slots[tmpl->active_slot].len;
        } else {
            /* Wrap around */
            tmpl->active_slot = 0;
            tmpl->slots[0].cursor = tmpl->slots[0].len;
        }
        return true;

    case KEY_TAB_LEFT:
        if (tmpl->active_slot > 0) {
            tmpl->active_slot--;
            tmpl->slots[tmpl->active_slot].cursor =
                tmpl->slots[tmpl->active_slot].len;
        } else {
            /* Wrap to last */
            tmpl->active_slot = (int8_t)(tmpl->num_slots - 1);
            tmpl->slots[tmpl->active_slot].cursor =
                tmpl->slots[tmpl->active_slot].len;
        }
        return true;

    case KEY_UP:
    case KEY_DOWN: {
        int nxt = slot_nav_vertical(tmpl->type, tmpl->active_slot,
                                    evt->action == KEY_UP);
        if (nxt >= 0 && nxt < (int)tmpl->num_slots) {
            tmpl->active_slot = (int8_t)nxt;
            tmpl->slots[tmpl->active_slot].cursor =
                tmpl->slots[tmpl->active_slot].len;
        }
        return true;
    }

    default:
        return false;
    }
}

/* ── mp_tmpl_ready ───────────────────────────────────────────── */
bool mp_tmpl_ready(const mp_template_state_t *tmpl)
{
    uint8_t i;
    if (!tmpl || tmpl->type == MP_TMPL_NONE) return false;
    for (i = 0; i < tmpl->num_slots; i++) {
        if (tmpl->slots[i].mandatory && tmpl->slots[i].len == 0) return false;
    }
    return true;
}

/* ── mp_tmpl_assemble ────────────────────────────────────────── */
bool mp_tmpl_assemble(const mp_template_state_t *tmpl,
                      char *out_buf, int out_size)
{
    char tmp[MAX_INPUT_LEN * 6];
    int pos = 0;

    if (!tmpl || !out_buf || out_size <= 0) return false;
    if (tmpl->type == MP_TMPL_NONE) return false;

#define APPEND(s) do { \
    const char *_s = (s); \
    while (*_s && pos < (int)sizeof(tmp) - 1) tmp[pos++] = *_s++; \
} while (0)
#define APPEND_SLOT(i) do { \
    if (tmpl->slots[(i)].len > 0) APPEND(tmpl->slots[(i)].buf); \
    else APPEND(tmpl->slots[(i)].placeholder); \
} while (0)

    /* Prefix (expression before the template trigger) */
    if (tmpl->prefix_len > 0) {
        memcpy(tmp, tmpl->prefix, tmpl->prefix_len);
        pos = tmpl->prefix_len;
    }

    switch (tmpl->type) {
    case MP_TMPL_INTEGRAL_INDEF:
        /* int(f,x) */
        APPEND("int(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(")");
        break;

    case MP_TMPL_INTEGRAL_DEF:
        /* int(f,x,a,b) */
        APPEND("int(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(",");
        APPEND_SLOT(3);
        APPEND(")");
        break;

    case MP_TMPL_DERIV: {
        /* d/d[x](f) */
        char var = (tmpl->slots[1].len > 0) ? tmpl->slots[1].buf[0] : 'x';
        char dvar[6] = { 'd', '/', 'd', var, '(', '\0' };
        APPEND(dvar);
        APPEND_SLOT(0);
        APPEND(")");
        break;
    }

    case MP_TMPL_LIMIT:
        /* lim(f,x,a) */
        APPEND("lim(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(")");
        break;

    case MP_TMPL_SUM:
        /* sum(f,n,lo,hi) */
        APPEND("sum(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(",");
        APPEND_SLOT(3);
        APPEND(")");
        break;

    case MP_TMPL_PRODUCT:
        /* product(f,n,lo,hi) */
        APPEND("product(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(",");
        APPEND_SLOT(3);
        APPEND(")");
        break;

    case MP_TMPL_FRACTION:
        /* (num)/(den) */
        APPEND("(");
        APPEND_SLOT(0);
        APPEND(")/(");
        APPEND_SLOT(1);
        APPEND(")");
        break;

    case MP_TMPL_TAYLOR:
        /* taylor(f,x,ctr,deg) */
        APPEND("taylor(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(",");
        APPEND_SLOT(3);
        APPEND(")");
        break;

    case MP_TMPL_TANGENT:
        /* tangent(f,x,a) */
        APPEND("tangent(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(")");
        break;

    case MP_TMPL_NDERIV:
        /* nderiv(f,x,n) */
        APPEND("nderiv(");
        APPEND_SLOT(0);
        APPEND(",");
        APPEND_SLOT(1);
        APPEND(",");
        APPEND_SLOT(2);
        APPEND(")");
        break;

    default:
        return false;
    }

#undef APPEND
#undef APPEND_SLOT

    tmp[pos] = '\0';
    if (pos >= out_size) return false;
    memcpy(out_buf, tmp, (size_t)pos + 1);
    return true;
}

/* ── mp_tmpl_detect_trigger ──────────────────────────────────── */

typedef struct {
    const char    *trigger;   /* string that ends a function open, e.g. "int(" */
    mp_tmpl_type_t type;
} trigger_entry_t;

static const trigger_entry_t k_triggers[] = {
    /* Definite integral: "int(" with preceding context triggers indef first,
     * user upgrades by filling lo/hi slots via TAB */
    { "int(",      MP_TMPL_INTEGRAL_INDEF },
    { "d/dx(",     MP_TMPL_DERIV },
    { "d/dy(",     MP_TMPL_DERIV },
    { "d/dz(",     MP_TMPL_DERIV },
    { "d/dt(",     MP_TMPL_DERIV },
    { "d/dn(",     MP_TMPL_DERIV },
    { "lim(",      MP_TMPL_LIMIT },
    { "sum(",      MP_TMPL_SUM },
    { "product(",  MP_TMPL_PRODUCT },
    { "taylor(",   MP_TMPL_TAYLOR },
    { "tangent(",  MP_TMPL_TANGENT },
    { "nderiv(",   MP_TMPL_NDERIV },
    { NULL, MP_TMPL_NONE }
};

bool mp_tmpl_detect_trigger(const char     *buf,
                             uint8_t         len,
                             mp_tmpl_type_t *type_out,
                             uint8_t        *prefix_end_out)
{
    int i;
    if (!buf || len == 0) return false;

    for (i = 0; k_triggers[i].trigger; i++) {
        const char *trig = k_triggers[i].trigger;
        int tlen = 0;
        while (trig[tlen]) tlen++;

        if (len < (uint8_t)tlen) continue;

        /* Check if buf ends with trig */
        if (memcmp(buf + len - tlen, trig, (size_t)tlen) == 0) {
            /* Make sure the trigger is at a token boundary (or start) */
            int prefix_end = (int)len - tlen;
            /* If prefix ends with a letter/digit, this might be a longer
             * function name — skip (e.g., "nint(" should not trigger "int(") */
            if (prefix_end > 0) {
                char c = buf[prefix_end - 1];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_') {
                    continue;
                }
            }

            if (type_out)       *type_out       = k_triggers[i].type;
            if (prefix_end_out) *prefix_end_out = (uint8_t)prefix_end;
            return true;
        }
    }

    /* Special case: fraction template triggered by "/" between expressions.
     * Pattern: non-empty text, then "/", where the text parses as a valid
     * expression.  Only trigger when "/" is the very last char. */
    if (len >= 2 && buf[len - 1] == '/') {
        /* Make sure this is not inside parens (depth check) */
        int depth = 0;
        int j;
        for (j = 0; j < (int)len - 1; j++) {
            if (buf[j] == '(') depth++;
            else if (buf[j] == ')') depth--;
        }
        if (depth == 0) {
            if (type_out)       *type_out       = MP_TMPL_FRACTION;
            /* The numerator is everything before the "/" */
            if (prefix_end_out) *prefix_end_out = (uint8_t)(len - 1);
            return true;
        }
    }

    return false;
}
