/*
 * mathprint.c — MathPrint 2D graphical renderer
 *
 * Renders AST expressions as typeset 2D math using the graphx library.
 * Two-pass approach: measure bounding boxes, then draw at pixel positions.
 * Static memory only — no malloc.
 */
#include "mathprint.h"
#include "matrix.h"
#include "ui.h"
#include "token.h"
#include "parser.h"
#include "macros.h"
#include <graphx.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Layout constants (matching ui.c) ────────────────────── */
#define MP_INPUT_STRIP_H  14   /* height of input area strip */

/* Keep input-render parser state off the tiny CE stack. */
static tokenizer_t s_input_tokenizer;
static parser_t s_input_parser;
static char s_input_expanded[MAX_INPUT_LEN];
static char s_input_macro_err[80];

/* ── Helpers ─────────────────────────────────────────────── */

static int mp_max(int a, int b) { return a > b ? a : b; }

/* Format a number for display */
static int mp_fmt_num(ce_float_t val, char *buf, int size)
{
    if (fabsf(val) < 1e-15f) return snprintf(buf, (size_t)size, "0");
    if (val == (int)val && fabsf(val) < 1e7f)
        return snprintf(buf, (size_t)size, "%d", (int)val);
    return snprintf(buf, (size_t)size, "%.*g",
                    (int)g_settings.precision, (double)val);
}

/* Get pixel width of a string (with fallback for mock) */
static int mp_text_w(const char *s)
{
    int w;
    if (!s || !*s) return 0;
    w = gfx_GetStringWidth(s);
    if (w > 0) return w;
    /* Fallback for desktop/mock builds: 8px per char */
    {
        int n = 0;
        while (s[n]) n++;
        return n * MP_CHAR_W;
    }
}

/* Draw text centered vertically on the center line */
static void mp_draw_text(const char *s, int x, int yc)
{
    if (s && *s)
        gfx_PrintStringXY(s, x, yc - MP_HALF_H);
}

/* Draw a single character */
static void mp_draw_char(char c, int x, int yc)
{
    char buf[2] = { c, '\0' };
    mp_draw_text(buf, x, yc);
}

static mp_bbox_t mp_text_bbox(const char *s)
{
    return (mp_bbox_t){ (int16_t)mp_text_w(s), MP_HALF_H, MP_HALF_H };
}

/* ── Function name lookup ────────────────────────────────── */

static const char *mp_func_name(func_id_t id)
{
    switch (id) {
        case FN_SIN:  return "sin";
        case FN_COS:  return "cos";
        case FN_TAN:  return "tan";
        case FN_SEC:  return "sec";
        case FN_CSC:  return "csc";
        case FN_COT:  return "cot";
        case FN_ASIN: return "asin";
        case FN_ACOS: return "acos";
        case FN_ATAN: return "atan";
        case FN_LN:   return "ln";
        case FN_LOG:  return "log";
        case FN_SQRT: return "sqrt";
        case FN_ABS:  return "abs";
        case FN_EXP:  return "exp";
        case FN_SINH: return "sinh";
        case FN_COSH: return "cosh";
        case FN_TANH: return "tanh";
        case FN_ASINH: return "asinh";
        case FN_ACOSH: return "acosh";
        case FN_ATANH: return "atanh";
        case FN_ASEC: return "asec";
        case FN_ACSC: return "acsc";
        case FN_ACOT: return "acot";
        case FN_SECH: return "sech";
        case FN_CSCH: return "csch";
        case FN_COTH: return "coth";
        case FN_FLOOR: return "floor";
        case FN_CEIL:  return "ceil";
        case FN_ROUND: return "round";
        case FN_IPART: return "iPart";
        case FN_FPART: return "fPart";
        case FN_DEC:  return "dec";
        case FN_HEX:  return "hex";
        case FN_BIN:  return "bin";
        case FN_OCT:  return "oct";
        case FN_REAL: return "real";
        case FN_IMAG: return "imag";
        case FN_CONJ: return "conj";
        case FN_ANGLE: return "angle";
        case FN_SIGN: return "sign";
        case FN_HEAVISIDE: return "H";
    }
    return "f";
}

static const char *mp_matrix_op_name(matrix_op_t op)
{
    switch (op) {
        case MAT_DET:       return "det";
        case MAT_INV:       return "inv";
        case MAT_RREF:      return "rref";
        case MAT_TRANSPOSE: return "transpose";
        default:            return "mat";
    }
}

static const char *mp_polar_name(polar_op_t op)
{
    switch (op) {
        case POLOP_TO_RECT:      return "polar";
        case POLOP_FROM_RECT:    return "rect";
        case POLOP_ARCLEN:       return "arclen";
        case POLOP_ARCLEN_PARAM: return "arclen";
        case POLOP_SURFREV:      return "surfarea";
        case POLOP_DISK:         return "disk";
        case POLOP_WASHER:       return "washer";
        case POLOP_SHELL:        return "shell";
        case POLOP_RELRATE:      return "relrate";
        case POLOP_CSQUARE:      return "csquare";
        case POLOP_PFRAC:        return "pfrac";
        case POLOP_DESCARTES:    return "descartes";
        case POLOP_LINE2PT:      return "line";
        case POLOP_LINEPT:       return "linept";
        case POLOP_PARABOLA:     return "parabola";
        case POLOP_CONTINUITY:   return "continuity";
        case POLOP_TOFRAC:       return "tofrac";
        case POLOP_RECSEQ:       return "recseq";
        case POLOP_UNITCIRC:     return "unitcirc";
        case POLOP_NLSOLVE:      return "nlsolve";
        case POLOP_ARITHSUM:     return "arithsum";
        case POLOP_GEOMSUM:      return "geomsum";
        case POLOP_MIDPOINT:     return "midpoint";
        case POLOP_DISTANCE:     return "distance";
        case POLOP_SLOPE:        return "slope";
        case POLOP_NEWTON:       return "newton";
        case POLOP_BISECT:       return "bisect";
        case POLOP_SECANT:       return "secant";
        case POLOP_PDIFF:        return "pdiff";
        case POLOP_GRADIENT:     return "gradient";
        case POLOP_DBLINT:       return "dblint";
        case POLOP_CRITICAL:     return "critical";
        case POLOP_POLYDIV:      return "polydiv";
        case POLOP_GRAPH:        return "graph";
        case POLOP_GRAPHEQ:      return "grapheq";
        case POLOP_ODE_SEP:      return "ode_sep";
        case POLOP_ODE_LINEAR:   return "ode_linear";
        case POLOP_DIFFEQ:       return "diffeq";
        case POLOP_EULER:        return "euler";
        case POLOP_SLOPEFIELD:   return "slopefield";
        case POLOP_INTPARTS:     return "intparts";
        case POLOP_TRIGSUB:      return "trigsub";
        case POLOP_USUB:         return "usub";
        case POLOP_INTSTRATEGY:  return "intstrategy";
        case POLOP_AREABETWEEN:  return "areabetween";
        case POLOP_IMPROPER:     return "improper";
        /* ODE solver */
        case POLOP_DSOLVE:       return "dsolve";
        /* Polar calculus */
        case POLOP_POLARAREA:    return "polararea";
        case POLOP_POLARARCLEN:  return "polararclen";
        /* Vector calculus */
        case POLOP_VEC_DOT:      return "dot";
        case POLOP_VEC_CROSS:    return "cross";
        case POLOP_VEC_GRAD:     return "grad";
        case POLOP_VEC_DIV:      return "div";
        case POLOP_VEC_CURL:     return "curl";
        /* Laplace transforms */
        case POLOP_LAPLACE:      return "laplace";
        case POLOP_ILAPLACE:     return "ilaplace";
        /* Unit conversion */
        case POLOP_CONVERT:      return "convert";
        /* Second-order ODE */
        case POLOP_DSOLVE2:      return "dsolve2";
        /* Eigenvalues / eigenvectors */
        case POLOP_EIGENVAL:     return "eigenval";
        case POLOP_EIGENVEC:     return "eigenvec";
        /* Implicit graphing */
        case POLOP_GRAPHIMPL:    return "graphimpl";
    }
    return "polar";
}

static const char *mp_list_op_name(uint8_t fn)
{
    static const char *const k_names[] = {
        "sum", "prod", "min", "max", "mean", "median", "stdev",
        "dim", "cumsum", "deltalist", "sorta", "sortd", "seq"
    };
    if (fn < (uint8_t)(sizeof(k_names) / sizeof(k_names[0]))) {
        return k_names[fn];
    }
    return "list";
}

/* ── Parenthesization logic ──────────────────────────────── */

/* Check if a child node needs parentheses in the given context */
static bool mp_needs_parens(binop_t parent_op, const ast_node_t *child,
                            bool is_right)
{
    if (!child || child->type != NODE_BINOP) {
        /* Unary negation in base of power needs parens: (-x)^2 */
        if (parent_op == OP_POW && !is_right &&
            child && child->type == NODE_UNOP &&
            child->d.unop.op == UOP_NEG)
            return true;
        return false;
    }
    {
        binop_t cop = child->d.binop.op;

        /* Division renders as 2D fraction — no parens needed for children */
        if (parent_op == OP_DIV) return false;

        /* Power: base needs parens if it's any binop (except inner power) */
        if (parent_op == OP_POW && !is_right) {
            return (cop == OP_ADD || cop == OP_SUB ||
                    cop == OP_MUL || cop == OP_DIV);
        }
        /* Power: exponent rendered as superscript, no parens needed */
        if (parent_op == OP_POW && is_right) return false;

        /* Multiply: children that are add/sub need parens */
        if (parent_op == OP_MUL)
            return (cop == OP_ADD || cop == OP_SUB);

        /* Subtract: right child add/sub needs parens */
        if (parent_op == OP_SUB && is_right)
            return (cop == OP_ADD || cop == OP_SUB);
    }
    return false;
}

/* ── Auto-parenthesized child bbox ───────────────────────── */

static mp_bbox_t mp_paren_bbox(mp_bbox_t inner)
{
    mp_bbox_t b;
    b.w = inner.w + 2 * MP_PAREN_W;
    b.ascent = inner.ascent + MP_PAREN_OVERSHOOT;
    b.descent = inner.descent + MP_PAREN_OVERSHOOT;
    return b;
}

/* ── Symbol drawing ──────────────────────────────────────── */

/* Draw tall left parenthesis */
static void mp_draw_lparen(int x, int yc, int ascent, int descent)
{
    int top = yc - ascent;
    int bot = yc + descent;
    int h = bot - top;

    if (h <= MP_CHAR_H + 2) {
        /* Small: just draw "(" character */
        mp_draw_char('(', x, yc);
    } else {
        /* Tall: draw with lines */
        gfx_Line(x + 3, top, x + 1, top + 3);
        gfx_VertLine(x + 1, top + 3, h - 6);
        gfx_Line(x + 1, bot - 3, x + 3, bot);
    }
}

/* Draw tall right parenthesis */
static void mp_draw_rparen(int x, int yc, int ascent, int descent)
{
    int top = yc - ascent;
    int bot = yc + descent;
    int h = bot - top;

    if (h <= MP_CHAR_H + 2) {
        mp_draw_char(')', x, yc);
    } else {
        gfx_Line(x, top, x + 2, top + 3);
        gfx_VertLine(x + 2, top + 3, h - 6);
        gfx_Line(x + 2, bot - 3, x, bot);
    }
}

/* Draw integral symbol ∫ */
static void mp_draw_integral_sym(int x, int yc, int half_h)
{
    /* Top serif curve */
    gfx_Line(x + 5, yc - half_h, x + 6, yc - half_h + 1);
    gfx_Line(x + 6, yc - half_h + 1, x + 5, yc - half_h + 2);
    /* Main vertical stem */
    gfx_VertLine(x + 4, yc - half_h + 2, 2 * half_h - 4);
    /* Bottom serif curve */
    gfx_Line(x + 4, yc + half_h - 2, x + 3, yc + half_h - 1);
    gfx_Line(x + 3, yc + half_h - 1, x + 2, yc + half_h);
}

/* Draw sigma symbol Σ */
static void mp_draw_sigma_sym(int x, int yc, int half_h)
{
    /* Top horizontal bar */
    gfx_HorizLine(x, yc - half_h, MP_SYM_W);
    /* Diagonal from top-right to center */
    gfx_Line(x, yc - half_h, x + MP_SYM_W / 2, yc);
    /* Diagonal from center to bottom-right */
    gfx_Line(x + MP_SYM_W / 2, yc, x, yc + half_h);
    /* Bottom horizontal bar */
    gfx_HorizLine(x, yc + half_h, MP_SYM_W);
}

/* Draw product symbol Π */
static void mp_draw_product_sym(int x, int yc, int half_h)
{
    /* Top horizontal bar */
    gfx_HorizLine(x, yc - half_h, MP_SYM_W);
    /* Left vertical leg */
    gfx_VertLine(x + 2, yc - half_h, 2 * half_h);
    /* Right vertical leg */
    gfx_VertLine(x + MP_SYM_W - 3, yc - half_h, 2 * half_h);
}

/* Draw radical symbol √ with vinculum over content */
static void mp_draw_radical(int x, int yc, int inner_ascent, int inner_descent,
                            int inner_w)
{
    int vinc_y = yc - inner_ascent - MP_RADICAL_PAD;
    int bot_y  = yc + inner_descent;
    int hook_x = x + MP_RADICAL_HOOK_W;

    /* Small tick at left */
    gfx_Line(x, yc, x + 2, bot_y);
    /* Main diagonal up to vinculum start */
    gfx_Line(x + 2, bot_y, hook_x, vinc_y);
    /* Vinculum (horizontal bar over radicand) */
    gfx_HorizLine(hook_x, vinc_y, inner_w + 1);
}

static void mp_draw_lbracket(int x, int yc, int ascent, int descent)
{
    int top = yc - ascent;
    int bot = yc + descent;
    int h = bot - top;

    if (h <= MP_CHAR_H + 2) {
        mp_draw_char('[', x, yc);
    } else {
        gfx_HorizLine(x + 1, top, MP_MATRIX_BRACKET_W - 1);
        gfx_VertLine(x + 1, top, h);
        gfx_HorizLine(x + 1, bot, MP_MATRIX_BRACKET_W - 1);
    }
}

static void mp_draw_rbracket(int x, int yc, int ascent, int descent)
{
    int top = yc - ascent;
    int bot = yc + descent;
    int h = bot - top;

    if (h <= MP_CHAR_H + 2) {
        mp_draw_char(']', x, yc);
    } else {
        gfx_HorizLine(x, top, MP_MATRIX_BRACKET_W - 1);
        gfx_VertLine(x + MP_MATRIX_BRACKET_W - 2, top, h);
        gfx_HorizLine(x, bot, MP_MATRIX_BRACKET_W - 1);
    }
}

typedef struct {
    const ast_node_t *node;
    const char *text;
} mp_arg_t;

static mp_bbox_t mp_measure_arg_item(const mp_arg_t *arg)
{
    if (!arg) return (mp_bbox_t){0, 0, 0};
    if (arg->text) return mp_text_bbox(arg->text);
    return mp_measure(arg->node);
}

static void mp_draw_arg_item(const mp_arg_t *arg, int x, int yc)
{
    if (!arg) return;
    if (arg->text) {
        mp_draw_text(arg->text, x, yc);
    } else {
        mp_draw(arg->node, x, yc);
    }
}

static mp_bbox_t mp_measure_call_items(const char *name,
                                       const mp_arg_t *args,
                                       size_t count)
{
    mp_bbox_t inner = { 0, 0, 0 };
    mp_bbox_t bbox;
    size_t i;

    for (i = 0; i < count; i++) {
        mp_bbox_t item = mp_measure_arg_item(&args[i]);
        if (i > 0) inner.w += mp_text_w(", ") + 2 * MP_OP_PAD;
        inner.w += item.w;
        inner.ascent = (int16_t)mp_max(inner.ascent, item.ascent);
        inner.descent = (int16_t)mp_max(inner.descent, item.descent);
    }

    if (count == 0) {
        inner = mp_text_bbox("");
    }

    bbox.w = (int16_t)(mp_text_w(name) + 2 * MP_PAREN_W + inner.w);
    bbox.ascent = (int16_t)mp_max(MP_HALF_H, inner.ascent + MP_PAREN_OVERSHOOT);
    bbox.descent = (int16_t)mp_max(MP_HALF_H, inner.descent + MP_PAREN_OVERSHOOT);
    return bbox;
}

static void mp_draw_call_items(const char *name,
                               const mp_arg_t *args,
                               size_t count,
                               int x,
                               int yc)
{
    mp_bbox_t inner = { 0, 0, 0 };
    int name_w = mp_text_w(name);
    int sep_w = mp_text_w(", ");
    int cx;
    size_t i;

    for (i = 0; i < count; i++) {
        mp_bbox_t item = mp_measure_arg_item(&args[i]);
        inner.w = (int16_t)(inner.w + item.w + (i > 0 ? sep_w + 2 * MP_OP_PAD : 0));
        inner.ascent = (int16_t)mp_max(inner.ascent, item.ascent);
        inner.descent = (int16_t)mp_max(inner.descent, item.descent);
    }
    if (count == 0) {
        inner = mp_text_bbox("");
    }

    mp_draw_text(name, x, yc);
    cx = x + name_w;
    mp_draw_lparen(cx, yc,
                   mp_max(MP_HALF_H, inner.ascent + MP_PAREN_OVERSHOOT),
                   mp_max(MP_HALF_H, inner.descent + MP_PAREN_OVERSHOOT));
    cx += MP_PAREN_W;
    for (i = 0; i < count; i++) {
        if (i > 0) {
            mp_draw_text(", ", cx + MP_OP_PAD, yc);
            cx += sep_w + 2 * MP_OP_PAD;
        }
        mp_draw_arg_item(&args[i], cx, yc);
        cx += mp_measure_arg_item(&args[i]).w;
    }
    mp_draw_rparen(cx, yc,
                   mp_max(MP_HALF_H, inner.ascent + MP_PAREN_OVERSHOOT),
                   mp_max(MP_HALF_H, inner.descent + MP_PAREN_OVERSHOOT));
}

static mp_bbox_t mp_measure_list_delimited(const ast_node_t *items,
                                           const char *open_s,
                                           const char *close_s)
{
    mp_bbox_t inner = mp_measure(items);
    mp_bbox_t bbox;
    int side_w = mp_text_w(open_s) + mp_text_w(close_s);

    bbox.w = (int16_t)(side_w + inner.w);
    bbox.ascent = (int16_t)mp_max(MP_HALF_H, inner.ascent);
    bbox.descent = (int16_t)mp_max(MP_HALF_H, inner.descent);
    return bbox;
}

static void mp_draw_list_delimited(const ast_node_t *items,
                                   const char *open_s,
                                   const char *close_s,
                                   int x,
                                   int yc)
{
    int open_w = mp_text_w(open_s);
    mp_draw_text(open_s, x, yc);
    mp_draw(items, x + open_w, yc);
    mp_draw_text(close_s, x + open_w + mp_measure(items).w, yc);
}

static mp_bbox_t mp_measure_matrix_literal(const ast_node_t *node)
{
    int col_w[MAX_MATRIX_DIM] = {0};
    int row_a[MAX_MATRIX_DIM] = {0};
    int row_d[MAX_MATRIX_DIM] = {0};
    const ast_node_t *pair;
    int rows, cols, r, c;
    int w = 0;
    int h = 0;
    int asc;

    if (!node || node->type != NODE_MATRIX_OP || node->d.matrix_op.op != MAT_LITERAL) {
        return mp_text_bbox("mat()");
    }

    rows = node->d.matrix_op.rows;
    cols = node->d.matrix_op.cols;
    if (rows <= 0 || cols <= 0 || rows > MAX_MATRIX_DIM || cols > MAX_MATRIX_DIM) {
        return mp_text_bbox("mat()");
    }

    pair = node->d.matrix_op.arg;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            mp_bbox_t cell = { MP_CHAR_W, MP_HALF_H, MP_HALF_H };
            if (pair && pair->type == NODE_PAIR) {
                cell = mp_measure(pair->d.pair.head);
                pair = pair->d.pair.tail;
            }
            col_w[c] = mp_max(col_w[c], cell.w);
            row_a[r] = mp_max(row_a[r], cell.ascent);
            row_d[r] = mp_max(row_d[r], cell.descent);
        }
    }

    w = 2 * (MP_MATRIX_BRACKET_W + MP_MATRIX_SIDE_PAD);
    for (c = 0; c < cols; c++) {
        w += col_w[c];
        if (c + 1 < cols) w += MP_MATRIX_COL_GAP;
    }
    for (r = 0; r < rows; r++) {
        h += row_a[r] + row_d[r];
        if (r + 1 < rows) h += MP_MATRIX_ROW_GAP;
    }
    if (h <= 0) h = MP_CHAR_H;
    asc = h / 2;
    return (mp_bbox_t){ (int16_t)w, (int16_t)asc, (int16_t)(h - asc) };
}

static void mp_draw_matrix_literal(const ast_node_t *node, int x, int yc)
{
    int col_w[MAX_MATRIX_DIM] = {0};
    int row_a[MAX_MATRIX_DIM] = {0};
    int row_d[MAX_MATRIX_DIM] = {0};
    const ast_node_t *pair;
    int rows, cols, r, c;
    int top;
    int row_top;
    int content_x;
    mp_bbox_t matrix_box = mp_measure_matrix_literal(node);

    if (!node || node->type != NODE_MATRIX_OP || node->d.matrix_op.op != MAT_LITERAL) {
        mp_draw_text("mat()", x, yc);
        return;
    }

    rows = node->d.matrix_op.rows;
    cols = node->d.matrix_op.cols;
    if (rows <= 0 || cols <= 0 || rows > MAX_MATRIX_DIM || cols > MAX_MATRIX_DIM) {
        mp_draw_text("mat()", x, yc);
        return;
    }

    pair = node->d.matrix_op.arg;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            mp_bbox_t cell = { MP_CHAR_W, MP_HALF_H, MP_HALF_H };
            if (pair && pair->type == NODE_PAIR) {
                cell = mp_measure(pair->d.pair.head);
                pair = pair->d.pair.tail;
            }
            col_w[c] = mp_max(col_w[c], cell.w);
            row_a[r] = mp_max(row_a[r], cell.ascent);
            row_d[r] = mp_max(row_d[r], cell.descent);
        }
    }

    mp_draw_lbracket(x, yc, matrix_box.ascent, matrix_box.descent);
    mp_draw_rbracket(x + matrix_box.w - MP_MATRIX_BRACKET_W, yc,
                     matrix_box.ascent, matrix_box.descent);

    top = yc - matrix_box.ascent;
    row_top = top;
    content_x = x + MP_MATRIX_BRACKET_W + MP_MATRIX_SIDE_PAD;
    pair = node->d.matrix_op.arg;
    for (r = 0; r < rows; r++) {
        int row_yc = row_top + row_a[r];
        int cx = content_x;
        for (c = 0; c < cols; c++) {
            const ast_node_t *cell_node = NULL;
            mp_bbox_t cell = { MP_CHAR_W, MP_HALF_H, MP_HALF_H };
            if (pair && pair->type == NODE_PAIR) {
                cell_node = pair->d.pair.head;
                cell = mp_measure(cell_node);
                pair = pair->d.pair.tail;
            }
            if (cell_node) {
                mp_draw(cell_node, cx + (col_w[c] - cell.w) / 2, row_yc);
            }
            cx += col_w[c];
            if (c + 1 < cols) cx += MP_MATRIX_COL_GAP;
        }
        row_top += row_a[r] + row_d[r];
        if (r + 1 < rows) row_top += MP_MATRIX_ROW_GAP;
    }
}

/* ── Child helper: measure with optional parens ──────────── */

static mp_bbox_t mp_measure_child(const ast_node_t *child,
                                  binop_t parent_op, bool is_right)
{
    mp_bbox_t cb = mp_measure(child);
    if (mp_needs_parens(parent_op, child, is_right))
        cb = mp_paren_bbox(cb);
    return cb;
}

/* Draw a child node with optional auto-parentheses */
static void mp_draw_child(const ast_node_t *child, int x, int yc,
                           binop_t parent_op, bool is_right)
{
    if (mp_needs_parens(parent_op, child, is_right)) {
        mp_bbox_t cb = mp_measure(child);
        int a = cb.ascent + MP_PAREN_OVERSHOOT;
        int d = cb.descent + MP_PAREN_OVERSHOOT;
        mp_draw_lparen(x, yc, a, d);
        mp_draw(child, x + MP_PAREN_W, yc);
        mp_draw_rparen(x + MP_PAREN_W + cb.w, yc, a, d);
    } else {
        mp_draw(child, x, yc);
    }
}

/* ── Measure (recursive) ─────────────────────────────────── */

mp_bbox_t mp_measure(const ast_node_t *node)
{
    mp_bbox_t z = { 0, 0, 0 };
    if (!node) return z;

    switch (node->type) {

    case NODE_NUMBER: {
        char buf[24];
        mp_fmt_num(node->d.number, buf, sizeof(buf));
        return (mp_bbox_t){ (int16_t)mp_text_w(buf), MP_HALF_H, MP_HALF_H };
    }

    case NODE_VARIABLE:
        return (mp_bbox_t){ MP_CHAR_W, MP_HALF_H, MP_HALF_H };

    case NODE_CONSTANT: {
        const char *s;
        switch (node->d.constant) {
            case CONST_PI:  s = "pi"; break;
            case CONST_E:   s = "e";  break;
            case CONST_INF: s = "inf"; break;
            case CONST_I:   s = "i";  break;
            default:        s = "?";  break;
        }
        return (mp_bbox_t){ (int16_t)mp_text_w(s), MP_HALF_H, MP_HALF_H };
    }

    case NODE_BINOP: {
        switch (node->d.binop.op) {

        case OP_DIV: { /* 2D fraction */
            mp_bbox_t nb = mp_measure(node->d.binop.left);
            mp_bbox_t db = mp_measure(node->d.binop.right);
            int w = mp_max(nb.w, db.w) + 2 * MP_FRAC_SIDE_PAD;
            return (mp_bbox_t){ (int16_t)w,
                                (int16_t)(MP_BBOX_H(nb) + MP_FRAC_PAD),
                                (int16_t)(MP_BBOX_H(db) + MP_FRAC_PAD) };
        }

        case OP_POW: { /* superscript */
            mp_bbox_t bb = mp_measure_child(node->d.binop.left, OP_POW, false);
            mp_bbox_t eb = mp_measure(node->d.binop.right);
            int exp_h = MP_BBOX_H(eb);
            int asc = bb.ascent + mp_max(0, exp_h - MP_SUP_OVERLAP);
            return (mp_bbox_t){ (int16_t)(bb.w + eb.w),
                                (int16_t)asc,
                                bb.descent };
        }

        case OP_MUL: { /* multiplication: implicit gap or · */
            mp_bbox_t lb = mp_measure_child(node->d.binop.left, OP_MUL, false);
            mp_bbox_t rb = mp_measure_child(node->d.binop.right, OP_MUL, true);
            int gap = MP_MUL_GAP;
            /* Use dot for non-obvious multiplication */
            if (node->d.binop.left->type != NODE_NUMBER ||
                (node->d.binop.right->type != NODE_VARIABLE &&
                 node->d.binop.right->type != NODE_FUNC &&
                 node->d.binop.right->type != NODE_CONSTANT))
                gap = mp_text_w("*") + 2 * MP_OP_PAD;
            return (mp_bbox_t){ (int16_t)(lb.w + gap + rb.w),
                                (int16_t)mp_max(lb.ascent, rb.ascent),
                                (int16_t)mp_max(lb.descent, rb.descent) };
        }

        default: { /* OP_ADD, OP_SUB */
            mp_bbox_t lb = mp_measure_child(node->d.binop.left,
                                            node->d.binop.op, false);
            mp_bbox_t rb = mp_measure_child(node->d.binop.right,
                                            node->d.binop.op, true);
            const char *op_s = (node->d.binop.op == OP_ADD) ? "+" : "-";
            int op_w = mp_text_w(op_s) + 2 * MP_OP_PAD;
            return (mp_bbox_t){ (int16_t)(lb.w + op_w + rb.w),
                                (int16_t)mp_max(lb.ascent, rb.ascent),
                                (int16_t)mp_max(lb.descent, rb.descent) };
        }

        } /* end switch binop.op */
    }

    case NODE_UNOP: {
        mp_bbox_t ob = mp_measure(node->d.unop.operand);
        if (node->d.unop.op == UOP_NEG) {
            int neg_w = mp_text_w("-");
            return (mp_bbox_t){ (int16_t)(neg_w + ob.w), ob.ascent, ob.descent };
        } else { /* UOP_FACT */
            int fact_w = mp_text_w("!");
            return (mp_bbox_t){ (int16_t)(ob.w + fact_w), ob.ascent, ob.descent };
        }
    }

    case NODE_PAIR: {
        const ast_node_t *cur = node;
        mp_bbox_t bbox = { 0, 0, 0 };
        int sep_w = mp_text_w(", ");

        while (cur && cur->type == NODE_PAIR) {
            mp_bbox_t item = mp_measure(cur->d.pair.head);
            if (bbox.w > 0) bbox.w = (int16_t)(bbox.w + sep_w + 2 * MP_OP_PAD);
            bbox.w = (int16_t)(bbox.w + item.w);
            bbox.ascent = (int16_t)mp_max(bbox.ascent, item.ascent);
            bbox.descent = (int16_t)mp_max(bbox.descent, item.descent);
            cur = cur->d.pair.tail;
        }
        return bbox;
    }

    case NODE_FUNC: {
        if (node->d.func.id == FN_SQRT) {
            /* Radical with vinculum */
            mp_bbox_t ib = mp_measure(node->d.func.arg);
            int w = MP_RADICAL_HOOK_W + ib.w + 1;
            int asc = ib.ascent + MP_RADICAL_PAD + 1;
            return (mp_bbox_t){ (int16_t)w, (int16_t)asc, ib.descent };
        }
        if (node->d.func.id == FN_ABS) {
            /* |expr| with vertical bars */
            mp_bbox_t ib = mp_measure(node->d.func.arg);
            return (mp_bbox_t){ (int16_t)(ib.w + 2 * MP_PAREN_W),
                                (int16_t)(ib.ascent + MP_PAREN_OVERSHOOT),
                                (int16_t)(ib.descent + MP_PAREN_OVERSHOOT) };
        }
        { /* Normal function: name(arg) */
            const char *name = mp_func_name(node->d.func.id);
            mp_bbox_t ab = mp_measure(node->d.func.arg);
            int name_w = mp_text_w(name);
            int paren_w = 2 * MP_PAREN_W;
            return (mp_bbox_t){ (int16_t)(name_w + paren_w + ab.w),
                                (int16_t)mp_max(MP_HALF_H, ab.ascent + MP_PAREN_OVERSHOOT),
                                (int16_t)mp_max(MP_HALF_H, ab.descent + MP_PAREN_OVERSHOOT) };
        }
    }

    case NODE_DERIV: { /* d/dx rendered as fraction + expr */
        /* Numerator: "d", Denominator: "dx" */
        int d_w = mp_text_w("d");
        char dv[4] = { 'd', node->d.deriv.var, '\0', '\0' };
        int dv_w = mp_text_w(dv);
        int frac_w = mp_max(d_w, dv_w) + 2 * MP_FRAC_SIDE_PAD;
        int frac_asc = MP_CHAR_H + MP_FRAC_PAD;
        int frac_des = MP_CHAR_H + MP_FRAC_PAD;
        mp_bbox_t eb = mp_measure(node->d.deriv.expr);
        int w = frac_w + MP_OP_PAD + MP_PAREN_W + eb.w + MP_PAREN_W;
        return (mp_bbox_t){ (int16_t)w,
                            (int16_t)mp_max(frac_asc, eb.ascent + MP_PAREN_OVERSHOOT),
                            (int16_t)mp_max(frac_des, eb.descent + MP_PAREN_OVERSHOOT) };
    }

    case NODE_INTEGRAL: { /* ∫ symbol + bounds + body + dvar */
        mp_bbox_t body = mp_measure(node->d.integral.expr);
        int sym_w = MP_SYM_W;
        int body_w = body.w;
        char dv[3] = { 'd', node->d.integral.var, '\0' };
        int dv_w = mp_text_w(dv);
        int sym_h = mp_max(MP_CHAR_H, MP_BBOX_H(body));
        int asc = sym_h / 2 + MP_HALF_H;
        int des = sym_h / 2 + MP_HALF_H;

        if (node->d.integral.lo && node->d.integral.hi) {
            mp_bbox_t lo = mp_measure(node->d.integral.lo);
            mp_bbox_t hi = mp_measure(node->d.integral.hi);
            sym_w = mp_max(sym_w, mp_max(lo.w, hi.w));
            asc = sym_h / 2 + MP_HALF_H + MP_BBOX_H(hi) + MP_BOUND_GAP;
            des = sym_h / 2 + MP_HALF_H + MP_BBOX_H(lo) + MP_BOUND_GAP;
        }

        return (mp_bbox_t){ (int16_t)(sym_w + MP_OP_PAD + body_w + MP_OP_PAD + dv_w),
                            (int16_t)mp_max(asc, body.ascent),
                            (int16_t)mp_max(des, body.descent) };
    }

    case NODE_SUM:
    case NODE_PRODUCT: { /* Σ / Π symbol + bounds + body */
        ast_node_t *expr_node, *lo_node, *hi_node;
        if (node->type == NODE_SUM) {
            expr_node = node->d.sum.expr;
            lo_node   = node->d.sum.lo;
            hi_node   = node->d.sum.hi;
        } else {
            expr_node = node->d.product.expr;
            lo_node   = node->d.product.lo;
            hi_node   = node->d.product.hi;
        }
        {
            mp_bbox_t body = mp_measure(expr_node);
            int sym_w = MP_SYM_W;
            int sym_h = mp_max(MP_CHAR_H, MP_BBOX_H(body));
            int asc = sym_h / 2 + MP_HALF_H;
            int des = sym_h / 2 + MP_HALF_H;

            if (lo_node && hi_node) {
                mp_bbox_t lo = mp_measure(lo_node);
                mp_bbox_t hi = mp_measure(hi_node);
                /* Lower bound includes "var=" prefix */
                int lo_total_w = MP_CHAR_W + mp_text_w("=") + lo.w;
                sym_w = mp_max(sym_w, mp_max(lo_total_w, hi.w));
                asc = sym_h / 2 + MP_HALF_H + MP_BBOX_H(hi) + MP_BOUND_GAP;
                des = sym_h / 2 + MP_HALF_H + MP_BBOX_H(lo) + MP_BOUND_GAP;
            }

            return (mp_bbox_t){ (int16_t)(sym_w + MP_OP_PAD + body.w),
                                (int16_t)mp_max(asc, body.ascent),
                                (int16_t)mp_max(des, body.descent) };
        }
    }

    case NODE_LIMIT: { /* lim with subscript x→a, then expr */
        mp_bbox_t eb = mp_measure(node->d.limit.expr);
        mp_bbox_t pt = mp_measure(node->d.limit.point);
        int lim_w = mp_text_w("lim");
        /* subscript: "x->a" */
        int sub_w = MP_CHAR_W + mp_text_w("->") + pt.w;
        int head_w = mp_max(lim_w, sub_w);
        int asc = mp_max(MP_HALF_H, eb.ascent);
        int des = mp_max(MP_HALF_H + MP_CHAR_H + MP_BOUND_GAP, eb.descent);
        return (mp_bbox_t){ (int16_t)(head_w + MP_OP_PAD + eb.w),
                            (int16_t)asc, (int16_t)des };
    }

    case NODE_TAYLOR: {
        mp_arg_t args[4] = {
            { node->d.taylor.expr, NULL },
            { NULL, NULL },
            { node->d.taylor.center, NULL },
            { node->d.taylor.degree, NULL }
        };
        char var_buf[2] = { node->d.taylor.var ? node->d.taylor.var : 'x', '\0' };
        args[1].text = var_buf;
        return mp_measure_call_items("taylor", args, 4);
    }

    case NODE_FOURIER: {
        mp_arg_t args[3] = {
            { node->d.fourier.expr, NULL },
            { NULL, NULL },
            { node->d.fourier.nterms, NULL }
        };
        char var_buf[2] = { node->d.fourier.var ? node->d.fourier.var : 'x', '\0' };
        args[1].text = var_buf;
        return mp_measure_call_items("fourier", args, 3);
    }

    case NODE_EXPAND: {
        mp_arg_t args[1] = { { node->d.expand.expr, NULL } };
        return mp_measure_call_items("expand", args, 1);
    }

    case NODE_FACTOR: {
        mp_arg_t args[1] = { { node->d.factor.expr, NULL } };
        return mp_measure_call_items("factor", args, 1);
    }

    case NODE_SOLVE: {
        mp_arg_t args[2] = {
            { node->d.solve.expr, NULL },
            { NULL, NULL }
        };
        char var_buf[2] = { node->d.solve.var ? node->d.solve.var : 'x', '\0' };
        args[1].text = var_buf;
        return mp_measure_call_items("solve", args, 2);
    }

    case NODE_STAT: {
        mp_arg_t args[1] = { { node->d.stat.args, NULL } };
        return mp_measure_call_items(stat_fn_name(node->d.stat.fn), args, 1);
    }

    case NODE_TANGENT: {
        mp_arg_t args[3] = {
            { node->d.tangent.expr, NULL },
            { NULL, NULL },
            { node->d.tangent.point, NULL }
        };
        char var_buf[2] = { node->d.tangent.var ? node->d.tangent.var : 'x', '\0' };
        args[1].text = var_buf;
        return mp_measure_call_items("tangent", args, 3);
    }

    case NODE_MATRIX:
        return mp_text_bbox("mat");

    case NODE_MATRIX_OP:
        if (node->d.matrix_op.op == MAT_LITERAL) {
            return mp_measure_matrix_literal(node);
        } else {
            mp_arg_t args[1] = { { node->d.matrix_op.arg, NULL } };
            return mp_measure_call_items(mp_matrix_op_name(node->d.matrix_op.op), args, 1);
        }

    case NODE_POLAR: {
        mp_arg_t args[5];
        size_t count = 0;
        char var_buf[2] = { node->d.polar.var ? node->d.polar.var : 'x', '\0' };
        if (node->d.polar.expr1) args[count++] = (mp_arg_t){ node->d.polar.expr1, NULL };
        if (node->d.polar.expr2) args[count++] = (mp_arg_t){ node->d.polar.expr2, NULL };
        if (node->d.polar.var) {
            args[count++] = (mp_arg_t){ NULL, var_buf };
        }
        if (node->d.polar.lo) args[count++] = (mp_arg_t){ node->d.polar.lo, NULL };
        if (node->d.polar.hi) args[count++] = (mp_arg_t){ node->d.polar.hi, NULL };
        return mp_measure_call_items(mp_polar_name(node->d.polar.op), args, count);
    }

    case NODE_LIST_LITERAL:
        return mp_measure_list_delimited(node->d.list_literal.elems, "{", "}");

    case NODE_LIST_REF: {
        char lname[3] = { 'L', (char)('1' + node->d.list_ref.idx), '\0' };
        return mp_text_bbox(lname);
    }

    case NODE_LIST_OP: {
        mp_arg_t args[1] = { { node->d.list_op.arg, NULL } };
        return mp_measure_call_items(mp_list_op_name(node->d.list_op.fn), args, 1);
    }

    case NODE_LIST_ELEM: {
        mp_arg_t args[1] = { { node->d.list_elem.index, NULL } };
        char lname[3] = { 'L', (char)('1' + node->d.list_elem.list_idx), '\0' };
        return mp_measure_call_items(lname, args, 1);
    }

    case NODE_STORE: {
        char lname[3] = { 'L', (char)('1' + node->d.store.list_idx), '\0' };
        mp_bbox_t expr = mp_measure(node->d.store.expr);
        mp_bbox_t list = mp_text_bbox(lname);
        int arrow_w = mp_text_w("->") + 2 * MP_OP_PAD;
        return (mp_bbox_t){
            (int16_t)(expr.w + arrow_w + list.w),
            (int16_t)mp_max(expr.ascent, list.ascent),
            (int16_t)mp_max(expr.descent, list.descent)
        };
    }

    case NODE_PIECEWISE: {
        mp_arg_t args[1] = { { node->d.piecewise.pieces, NULL } };
        return mp_measure_call_items("piecewise", args, 1);
    }

    default: {
        /* Fallback: render as text using ast_to_str */
        char buf[128];
        ast_to_str(node, buf, (int)sizeof(buf));
        return (mp_bbox_t){ (int16_t)mp_text_w(buf), MP_HALF_H, MP_HALF_H };
    }

    } /* end switch */
}

/* ── Draw (recursive) ────────────────────────────────────── */

void mp_draw(const ast_node_t *node, int x, int yc)
{
    if (!node) return;

    switch (node->type) {

    case NODE_NUMBER: {
        char buf[24];
        mp_fmt_num(node->d.number, buf, sizeof(buf));
        mp_draw_text(buf, x, yc);
        break;
    }

    case NODE_VARIABLE:
        mp_draw_char(node->d.variable, x, yc);
        break;

    case NODE_CONSTANT: {
        const char *s;
        switch (node->d.constant) {
            case CONST_PI:  s = "pi"; break;
            case CONST_E:   s = "e";  break;
            case CONST_INF: s = "inf"; break;
            case CONST_I:   s = "i";  break;
            default:        s = "?";  break;
        }
        mp_draw_text(s, x, yc);
        break;
    }

    case NODE_BINOP: {
        switch (node->d.binop.op) {

        case OP_DIV: { /* 2D fraction */
            mp_bbox_t nb = mp_measure(node->d.binop.left);
            mp_bbox_t db = mp_measure(node->d.binop.right);
            mp_bbox_t fb = mp_measure(node);
            int num_x = x + (fb.w - nb.w) / 2;
            int den_x = x + (fb.w - db.w) / 2;
            int num_yc = yc - MP_FRAC_PAD - nb.descent;
            int den_yc = yc + MP_FRAC_PAD + db.ascent;

            mp_draw(node->d.binop.left, num_x, num_yc);
            gfx_HorizLine(x, yc, fb.w);
            mp_draw(node->d.binop.right, den_x, den_yc);
            break;
        }

        case OP_POW: { /* superscript */
            mp_bbox_t bb = mp_measure_child(node->d.binop.left, OP_POW, false);
            mp_bbox_t eb = mp_measure(node->d.binop.right);

            /* Draw base (possibly with parens) */
            mp_draw_child(node->d.binop.left, x, yc, OP_POW, false);

            /* Draw exponent raised above base */
            {
                int exp_yc = yc - bb.ascent + MP_SUP_OVERLAP - eb.descent;
                mp_draw(node->d.binop.right, x + bb.w, exp_yc);
            }
            break;
        }

        case OP_MUL: { /* multiplication */
            mp_bbox_t lb = mp_measure_child(node->d.binop.left, OP_MUL, false);
            int gap;
            int cx = x;

            /* Decide gap style */
            if (node->d.binop.left->type == NODE_NUMBER &&
                (node->d.binop.right->type == NODE_VARIABLE ||
                 node->d.binop.right->type == NODE_FUNC ||
                 node->d.binop.right->type == NODE_CONSTANT))
                gap = MP_MUL_GAP;
            else
                gap = mp_text_w("*") + 2 * MP_OP_PAD;

            mp_draw_child(node->d.binop.left, cx, yc, OP_MUL, false);
            cx += lb.w;

            if (gap > MP_MUL_GAP) {
                /* Draw centered dot */
                mp_draw_text("*", cx + MP_OP_PAD, yc);
            }
            cx += gap;

            mp_draw_child(node->d.binop.right, cx, yc, OP_MUL, true);
            break;
        }

        default: { /* OP_ADD, OP_SUB */
            mp_bbox_t lb = mp_measure_child(node->d.binop.left,
                                            node->d.binop.op, false);
            const char *op_s = (node->d.binop.op == OP_ADD) ? "+" : "-";
            int op_w = mp_text_w(op_s);
            int cx = x;

            mp_draw_child(node->d.binop.left, cx, yc,
                          node->d.binop.op, false);
            cx += lb.w + MP_OP_PAD;

            mp_draw_text(op_s, cx, yc);
            cx += op_w + MP_OP_PAD;

            mp_draw_child(node->d.binop.right, cx, yc,
                          node->d.binop.op, true);
            break;
        }

        } /* end switch binop.op */
        break;
    }

    case NODE_UNOP: {
        if (node->d.unop.op == UOP_NEG) {
            int neg_w = mp_text_w("-");
            mp_draw_text("-", x, yc);
            mp_draw(node->d.unop.operand, x + neg_w, yc);
        } else { /* UOP_FACT */
            mp_bbox_t ob = mp_measure(node->d.unop.operand);
            mp_draw(node->d.unop.operand, x, yc);
            mp_draw_text("!", x + ob.w, yc);
        }
        break;
    }

    case NODE_PAIR: {
        const ast_node_t *cur = node;
        int cx = x;
        int sep_w = mp_text_w(", ");

        while (cur && cur->type == NODE_PAIR) {
            if (cx > x) {
                mp_draw_text(", ", cx + MP_OP_PAD, yc);
                cx += sep_w + 2 * MP_OP_PAD;
            }
            mp_draw(cur->d.pair.head, cx, yc);
            cx += mp_measure(cur->d.pair.head).w;
            cur = cur->d.pair.tail;
        }
        break;
    }

    case NODE_FUNC: {
        if (node->d.func.id == FN_SQRT) {
            /* Radical with vinculum */
            mp_bbox_t ib = mp_measure(node->d.func.arg);
            mp_draw_radical(x, yc, ib.ascent, ib.descent, ib.w);
            mp_draw(node->d.func.arg, x + MP_RADICAL_HOOK_W, yc);
            break;
        }
        if (node->d.func.id == FN_ABS) {
            /* |expr| */
            mp_bbox_t ib = mp_measure(node->d.func.arg);
            int a = ib.ascent + MP_PAREN_OVERSHOOT;
            int d = ib.descent + MP_PAREN_OVERSHOOT;
            /* Left bar */
            gfx_VertLine(x + 1, yc - a, a + d);
            mp_draw(node->d.func.arg, x + MP_PAREN_W, yc);
            /* Right bar */
            gfx_VertLine(x + MP_PAREN_W + ib.w + 1, yc - a, a + d);
            break;
        }
        { /* Normal function: name(arg) */
            const char *name = mp_func_name(node->d.func.id);
            mp_bbox_t ab = mp_measure(node->d.func.arg);
            int name_w = mp_text_w(name);
            int a = mp_max(MP_HALF_H, ab.ascent + MP_PAREN_OVERSHOOT);
            int d = mp_max(MP_HALF_H, ab.descent + MP_PAREN_OVERSHOOT);
            int cx = x;

            mp_draw_text(name, cx, yc);
            cx += name_w;
            mp_draw_lparen(cx, yc, a, d);
            cx += MP_PAREN_W;
            mp_draw(node->d.func.arg, cx, yc);
            cx += ab.w;
            mp_draw_rparen(cx, yc, a, d);
        }
        break;
    }

    case NODE_DERIV: { /* d/dx as fraction, then (expr) */
        int d_w = mp_text_w("d");
        char dv[4] = { 'd', node->d.deriv.var, '\0', '\0' };
        int dv_w = mp_text_w(dv);
        int frac_w = mp_max(d_w, dv_w) + 2 * MP_FRAC_SIDE_PAD;
        int num_yc = yc - MP_FRAC_PAD - MP_HALF_H;
        int den_yc = yc + MP_FRAC_PAD + MP_HALF_H;
        mp_bbox_t eb = mp_measure(node->d.deriv.expr);
        int a = mp_max(MP_HALF_H, eb.ascent + MP_PAREN_OVERSHOOT);
        int d = mp_max(MP_HALF_H, eb.descent + MP_PAREN_OVERSHOOT);
        int cx = x;

        /* Draw fraction d/dvar */
        mp_draw_text("d", cx + (frac_w - d_w) / 2, num_yc);
        gfx_HorizLine(cx, yc, frac_w);
        mp_draw_text(dv, cx + (frac_w - dv_w) / 2, den_yc);
        cx += frac_w + MP_OP_PAD;

        /* Draw (expr) */
        mp_draw_lparen(cx, yc, a, d);
        cx += MP_PAREN_W;
        mp_draw(node->d.deriv.expr, cx, yc);
        cx += eb.w;
        mp_draw_rparen(cx, yc, a, d);
        break;
    }

    case NODE_INTEGRAL: { /* ∫ with bounds, body, dvar */
        mp_bbox_t body = mp_measure(node->d.integral.expr);
        int sym_w = MP_SYM_W;
        int sym_h = mp_max(MP_CHAR_H, MP_BBOX_H(body));
        char dv[3] = { 'd', node->d.integral.var, '\0' };
        int cx = x;

        if (node->d.integral.lo && node->d.integral.hi) {
            mp_bbox_t lo = mp_measure(node->d.integral.lo);
            mp_bbox_t hi = mp_measure(node->d.integral.hi);
            sym_w = mp_max(sym_w, mp_max(lo.w, hi.w));

            /* Draw ∫ symbol */
            mp_draw_integral_sym(cx, yc, sym_h / 2 + MP_HALF_H);

            /* Upper bound (above symbol) */
            {
                int hi_yc = yc - sym_h / 2 - MP_HALF_H - MP_BOUND_GAP - hi.descent;
                mp_draw(node->d.integral.hi, cx, hi_yc);
            }
            /* Lower bound (below symbol) */
            {
                int lo_yc = yc + sym_h / 2 + MP_HALF_H + MP_BOUND_GAP + lo.ascent;
                mp_draw(node->d.integral.lo, cx, lo_yc);
            }
        } else {
            mp_draw_integral_sym(cx, yc, sym_h / 2);
        }
        cx += sym_w + MP_OP_PAD;

        /* Body */
        mp_draw(node->d.integral.expr, cx, yc);
        cx += body.w + MP_OP_PAD;

        /* dvar */
        mp_draw_text(dv, cx, yc);
        break;
    }

    case NODE_SUM:
    case NODE_PRODUCT: { /* Σ / Π with bounds, body */
        ast_node_t *expr_node, *lo_node, *hi_node;
        char var;
        if (node->type == NODE_SUM) {
            expr_node = node->d.sum.expr;
            lo_node   = node->d.sum.lo;
            hi_node   = node->d.sum.hi;
            var       = node->d.sum.var;
        } else {
            expr_node = node->d.product.expr;
            lo_node   = node->d.product.lo;
            hi_node   = node->d.product.hi;
            var       = node->d.product.var;
        }
        {
            mp_bbox_t body = mp_measure(expr_node);
            int sym_w = MP_SYM_W;
            int sym_h = mp_max(MP_CHAR_H, MP_BBOX_H(body));
            int cx = x;

            if (lo_node && hi_node) {
                mp_bbox_t lo = mp_measure(lo_node);
                mp_bbox_t hi = mp_measure(hi_node);
                int lo_total_w = MP_CHAR_W + mp_text_w("=") + lo.w;
                sym_w = mp_max(sym_w, mp_max(lo_total_w, hi.w));

                /* Draw symbol */
                if (node->type == NODE_SUM)
                    mp_draw_sigma_sym(cx, yc, sym_h / 2 + MP_HALF_H);
                else
                    mp_draw_product_sym(cx, yc, sym_h / 2 + MP_HALF_H);

                /* Upper bound */
                {
                    int hi_yc = yc - sym_h / 2 - MP_HALF_H - MP_BOUND_GAP - hi.descent;
                    mp_draw(hi_node, cx, hi_yc);
                }
                /* Lower bound: var=lo */
                {
                    int lo_yc = yc + sym_h / 2 + MP_HALF_H + MP_BOUND_GAP + lo.ascent;
                    char eq[2] = { '=', '\0' };
                    mp_draw_char(var, cx, lo_yc);
                    mp_draw_text(eq, cx + MP_CHAR_W, lo_yc);
                    mp_draw(lo_node, cx + MP_CHAR_W + mp_text_w("="), lo_yc);
                }
            } else {
                if (node->type == NODE_SUM)
                    mp_draw_sigma_sym(cx, yc, sym_h / 2);
                else
                    mp_draw_product_sym(cx, yc, sym_h / 2);
            }
            cx += sym_w + MP_OP_PAD;

            /* Body */
            mp_draw(expr_node, cx, yc);
        }
        break;
    }

    case NODE_LIMIT: { /* lim with subscript x→a, then expr */
        mp_bbox_t pt = mp_measure(node->d.limit.point);
        int lim_w = mp_text_w("lim");
        int sub_w = MP_CHAR_W + mp_text_w("->") + pt.w;
        int head_w = mp_max(lim_w, sub_w);
        int sub_yc;
        int cx = x;

        /* "lim" text at baseline */
        mp_draw_text("lim", cx + (head_w - lim_w) / 2, yc);

        /* Subscript "x→a" below */
        sub_yc = yc + MP_HALF_H + MP_BOUND_GAP + MP_HALF_H;
        mp_draw_char(node->d.limit.var, cx, sub_yc);
        {
            const char *arrow;
            if (node->d.limit.direction < 0) arrow = "->-";
            else if (node->d.limit.direction > 0) arrow = "->+";
            else arrow = "->";
            mp_draw_text(arrow, cx + MP_CHAR_W, sub_yc);
            mp_draw(node->d.limit.point,
                    cx + MP_CHAR_W + mp_text_w(arrow), sub_yc);
        }
        cx += head_w + MP_OP_PAD;

        /* Expression */
        mp_draw(node->d.limit.expr, cx, yc);
        break;
    }

    case NODE_TAYLOR: {
        mp_arg_t args[4] = {
            { node->d.taylor.expr, NULL },
            { NULL, NULL },
            { node->d.taylor.center, NULL },
            { node->d.taylor.degree, NULL }
        };
        char var_buf[2] = { node->d.taylor.var ? node->d.taylor.var : 'x', '\0' };
        args[1].text = var_buf;
        mp_draw_call_items("taylor", args, 4, x, yc);
        break;
    }

    case NODE_FOURIER: {
        mp_arg_t args[3] = {
            { node->d.fourier.expr, NULL },
            { NULL, NULL },
            { node->d.fourier.nterms, NULL }
        };
        char var_buf[2] = { node->d.fourier.var ? node->d.fourier.var : 'x', '\0' };
        args[1].text = var_buf;
        mp_draw_call_items("fourier", args, 3, x, yc);
        break;
    }

    case NODE_EXPAND: {
        mp_arg_t args[1] = { { node->d.expand.expr, NULL } };
        mp_draw_call_items("expand", args, 1, x, yc);
        break;
    }

    case NODE_FACTOR: {
        mp_arg_t args[1] = { { node->d.factor.expr, NULL } };
        mp_draw_call_items("factor", args, 1, x, yc);
        break;
    }

    case NODE_SOLVE: {
        mp_arg_t args[2] = {
            { node->d.solve.expr, NULL },
            { NULL, NULL }
        };
        char var_buf[2] = { node->d.solve.var ? node->d.solve.var : 'x', '\0' };
        args[1].text = var_buf;
        mp_draw_call_items("solve", args, 2, x, yc);
        break;
    }

    case NODE_STAT: {
        mp_arg_t args[1] = { { node->d.stat.args, NULL } };
        mp_draw_call_items(stat_fn_name(node->d.stat.fn), args, 1, x, yc);
        break;
    }

    case NODE_TANGENT: {
        mp_arg_t args[3] = {
            { node->d.tangent.expr, NULL },
            { NULL, NULL },
            { node->d.tangent.point, NULL }
        };
        char var_buf[2] = { node->d.tangent.var ? node->d.tangent.var : 'x', '\0' };
        args[1].text = var_buf;
        mp_draw_call_items("tangent", args, 3, x, yc);
        break;
    }

    case NODE_MATRIX:
        mp_draw_text("mat", x, yc);
        break;

    case NODE_MATRIX_OP:
        if (node->d.matrix_op.op == MAT_LITERAL) {
            mp_draw_matrix_literal(node, x, yc);
        } else {
            mp_arg_t args[1] = { { node->d.matrix_op.arg, NULL } };
            mp_draw_call_items(mp_matrix_op_name(node->d.matrix_op.op), args, 1, x, yc);
        }
        break;

    case NODE_POLAR: {
        mp_arg_t args[5];
        size_t count = 0;
        char var_buf[2] = { node->d.polar.var ? node->d.polar.var : 'x', '\0' };

        if (node->d.polar.expr1) args[count++] = (mp_arg_t){ node->d.polar.expr1, NULL };
        if (node->d.polar.expr2) args[count++] = (mp_arg_t){ node->d.polar.expr2, NULL };
        if (node->d.polar.var) args[count++] = (mp_arg_t){ NULL, var_buf };
        if (node->d.polar.lo) args[count++] = (mp_arg_t){ node->d.polar.lo, NULL };
        if (node->d.polar.hi) args[count++] = (mp_arg_t){ node->d.polar.hi, NULL };
        mp_draw_call_items(mp_polar_name(node->d.polar.op), args, count, x, yc);
        break;
    }

    case NODE_LIST_LITERAL:
        mp_draw_list_delimited(node->d.list_literal.elems, "{", "}", x, yc);
        break;

    case NODE_LIST_REF: {
        char lname[3] = { 'L', (char)('1' + node->d.list_ref.idx), '\0' };
        mp_draw_text(lname, x, yc);
        break;
    }

    case NODE_LIST_OP: {
        mp_arg_t args[1] = { { node->d.list_op.arg, NULL } };
        mp_draw_call_items(mp_list_op_name(node->d.list_op.fn), args, 1, x, yc);
        break;
    }

    case NODE_LIST_ELEM: {
        mp_arg_t args[1] = { { node->d.list_elem.index, NULL } };
        char lname[3] = { 'L', (char)('1' + node->d.list_elem.list_idx), '\0' };
        mp_draw_call_items(lname, args, 1, x, yc);
        break;
    }

    case NODE_STORE: {
        char lname[3] = { 'L', (char)('1' + node->d.store.list_idx), '\0' };
        int expr_w = mp_measure(node->d.store.expr).w;
        int arrow_w = mp_text_w("->");
        mp_draw(node->d.store.expr, x, yc);
        mp_draw_text("->", x + expr_w + MP_OP_PAD, yc);
        mp_draw_text(lname, x + expr_w + arrow_w + 2 * MP_OP_PAD, yc);
        break;
    }

    case NODE_PIECEWISE: {
        mp_arg_t args[1] = { { node->d.piecewise.pieces, NULL } };
        mp_draw_call_items("piecewise", args, 1, x, yc);
        break;
    }

    default: {
        /* Fallback: render as plain text */
        char buf[128];
        ast_to_str(node, buf, (int)sizeof(buf));
        mp_draw_text(buf, x, yc);
        break;
    }

    } /* end switch */
}

/* ── High-level result rendering ─────────────────────────── */

void mp_draw_result(const ast_node_t *node)
{
    mp_bbox_t bbox;
    int yc;

    if (!node) return;

    bbox = mp_measure(node);
    /* Center vertically in work area */
    yc = WORK_Y + 4 + bbox.ascent;
    if (yc + bbox.descent > INPUT_Y - 4)
        yc = INPUT_Y - 4 - bbox.descent;

    gfx_SetTextFGColor(COL_BLACK);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_SetColor(COL_BLACK);
    mp_draw(node, 6, yc);
}

/* ── 2D cursor navigation ────────────────────────────────── */

/*
 * Find the nearest '/' at the "top level" relative to the cursor.
 * Returns the index of '/', or -1 if not found.
 * depth tracks parenthesis nesting.
 */
static int mp_find_nearby_slash(const char *buf, int len, int cursor,
                                 int direction)
{
    int i, depth;

    if (direction <= 0) {
        /* Search backward for '/' */
        depth = 0;
        for (i = cursor - 1; i >= 0; i--) {
            if (buf[i] == ')') depth++;
            else if (buf[i] == '(') {
                if (depth > 0) depth--;
                else break; /* hit unmatched open paren */
            }
            else if (buf[i] == '/' && depth == 0) return i;
        }
    }
    if (direction >= 0) {
        /* Search forward for '/' */
        depth = 0;
        for (i = cursor; i < len; i++) {
            if (buf[i] == '(') depth++;
            else if (buf[i] == ')') {
                if (depth > 0) depth--;
                else break;
            }
            else if (buf[i] == '/' && depth == 0) return i;
        }
    }
    return -1;
}

/* Find the nearest '^' at top level relative to cursor */
static int mp_find_nearby_caret(const char *buf, int len, int cursor,
                                 int direction)
{
    int i, depth;

    if (direction <= 0) {
        depth = 0;
        for (i = cursor - 1; i >= 0; i--) {
            if (buf[i] == ')') depth++;
            else if (buf[i] == '(') {
                if (depth > 0) depth--;
                else break;
            }
            else if (buf[i] == '^' && depth == 0) return i;
        }
    }
    if (direction >= 0) {
        depth = 0;
        for (i = cursor; i < len; i++) {
            if (buf[i] == '(') depth++;
            else if (buf[i] == ')') {
                if (depth > 0) depth--;
                else break;
            }
            else if (buf[i] == '^' && depth == 0) return i;
        }
    }
    return -1;
}

bool mp_cursor_up(input_buf_t *ib)
{
    int slash, caret;

    if (!ib || ib->len == 0) return false;

    /* If cursor is in denominator of a fraction, move to numerator */
    slash = mp_find_nearby_slash(ib->buf, ib->len, ib->cursor, -1);
    if (slash >= 0 && ib->cursor > slash) {
        /* Move cursor to just before the slash (end of numerator) */
        ib->cursor = (uint8_t)slash;
        return true;
    }

    /* If cursor is at base of an exponent, move into exponent */
    caret = mp_find_nearby_caret(ib->buf, ib->len, ib->cursor, 1);
    if (caret >= 0) {
        /* Move cursor after the caret (into exponent) */
        ib->cursor = (uint8_t)(caret + 1);
        if (ib->cursor > ib->len) ib->cursor = ib->len;
        return true;
    }

    return false;
}

bool mp_cursor_down(input_buf_t *ib)
{
    int slash, caret;

    if (!ib || ib->len == 0) return false;

    /* If cursor is in numerator of a fraction, move to denominator */
    slash = mp_find_nearby_slash(ib->buf, ib->len, ib->cursor, 1);
    if (slash >= 0 && ib->cursor <= slash) {
        /* Move cursor to just after the slash (start of denominator) */
        ib->cursor = (uint8_t)(slash + 1);
        if (ib->cursor > ib->len) ib->cursor = ib->len;
        return true;
    }

    /* If cursor is in exponent, move back to base */
    caret = mp_find_nearby_caret(ib->buf, ib->len, ib->cursor, -1);
    if (caret >= 0 && ib->cursor > caret) {
        /* Move cursor to just before the caret (end of base) */
        ib->cursor = (uint8_t)caret;
        return true;
    }

    return false;
}

/* ── Input rendering with MathPrint ──────────────────────── */

bool mp_try_draw_input(const input_buf_t *ib)
{
    ast_node_t *ast;
    mp_bbox_t   bbox;
    int y_strip, yc;

    if (!ib || ib->len == 0) return false;

    /* Try to expand macros and parse */
    if (!macros_expand(ib->buf,
                       s_input_expanded,
                       sizeof(s_input_expanded),
                       s_input_macro_err,
                       sizeof(s_input_macro_err)))
        return false;

    tok_init(&s_input_tokenizer, s_input_expanded);
    if (tok_tokenize(&s_input_tokenizer) != CE_OK) return false;
    parser_init(&s_input_parser, &s_input_tokenizer);
    ast = parser_parse(&s_input_parser);
    if (!ast) return false;

    /* Measure */
    bbox = mp_measure(ast);

    /* Position in input strip */
    y_strip = INPUT_Y;
    yc = y_strip + MP_INPUT_STRIP_H / 2 + 1;

    /* If too tall for the strip, expand upward (but cap) */
    if (MP_BBOX_H(bbox) > MP_INPUT_STRIP_H - 2) {
        int needed = MP_BBOX_H(bbox) + 4;
        y_strip = INPUT_Y + MP_INPUT_STRIP_H - needed;
        if (y_strip < INPUT_Y - 40) y_strip = INPUT_Y - 40;
        yc = y_strip + bbox.ascent + 2;
    }

    /* Draw background */
    gfx_SetColor(COL_WHITE);
    gfx_FillRectangle(0, y_strip, SCREEN_W, INPUT_Y + MP_INPUT_STRIP_H - y_strip);
    gfx_SetColor(COL_BLUE);
    gfx_HorizLine(0, y_strip, SCREEN_W);

    /* Draw the expression */
    gfx_SetTextFGColor(COL_DKBLUE);
    gfx_SetTextBGColor(COL_WHITE);
    gfx_SetColor(COL_DKBLUE);
    mp_draw(ast, 4, yc);

    /* Draw cursor: compute approximate x position */
    {
        /* Map cursor position to pixel x using proportional estimate */
        int total_w = bbox.w > 0 ? bbox.w : 1;
        int cursor_x;
        if (ib->len == 0) {
            cursor_x = 4;
        } else {
            /* Approximate: cursor_pos / len * total_width */
            cursor_x = 4 + (int)((long)ib->cursor * total_w / ib->len);
        }
        if (cursor_x < 4) cursor_x = 4;
        if (cursor_x > SCREEN_W - 4) cursor_x = SCREEN_W - 4;

        gfx_SetColor(COL_BLUE);
        gfx_FillRectangle(cursor_x, yc - MP_HALF_H, 2, MP_CHAR_H);
    }

    ast_free(ast);
    return true;
}
