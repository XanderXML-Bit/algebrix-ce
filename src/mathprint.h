/*
 * mathprint.h — MathPrint 2D graphical renderer
 *
 * Renders AST expressions as 2D typeset math:
 *   fractions (stacked), exponents (superscript), radicals (vinculum),
 *   integrals/sums/products (with bounds), auto-sized parentheses.
 *
 * Walks the AST in two logical passes:
 *   1. Measure — compute bounding boxes (width, ascent, descent)
 *   2. Draw   — render at pixel positions using graphx primitives
 */
#ifndef MATHPRINT_H
#define MATHPRINT_H

#include "calcengine.h"
#include "ast.h"
#include "ui.h"

/* ── Font / layout metrics (pixels) ──────────────────────── */
#define MP_CHAR_W           8   /* monospace character width          */
#define MP_CHAR_H           8   /* character height                   */
#define MP_HALF_H           4   /* MP_CHAR_H / 2                     */
#define MP_FRAC_PAD         2   /* gap between fraction bar & content */
#define MP_FRAC_SIDE_PAD    2   /* extra width padding on fractions   */
#define MP_SUP_OVERLAP      2   /* exponent overlap with base top     */
#define MP_RADICAL_HOOK_W   6   /* width of radical (√) hook          */
#define MP_RADICAL_PAD      2   /* padding under vinculum             */
#define MP_PAREN_W          4   /* width of each drawn parenthesis    */
#define MP_PAREN_OVERSHOOT  1   /* extra paren height beyond content  */
#define MP_OP_PAD           2   /* horizontal padding around +−· ops  */
#define MP_MUL_GAP          2   /* implicit multiplication gap        */
#define MP_SYM_W            8   /* integral/sum/product symbol width  */
#define MP_BOUND_GAP        1   /* gap between symbol and bounds      */
#define MP_MATRIX_BRACKET_W 4   /* matrix bracket width               */
#define MP_MATRIX_SIDE_PAD  3   /* matrix inner left/right padding    */
#define MP_MATRIX_COL_GAP   4   /* gap between matrix columns         */
#define MP_MATRIX_ROW_GAP   3   /* gap between matrix rows            */

/* ── Bounding box ────────────────────────────────────────── */
typedef struct {
    int16_t w;          /* total width in pixels                   */
    int16_t ascent;     /* pixels above center line (positive up)  */
    int16_t descent;    /* pixels below center line (positive down)*/
} mp_bbox_t;

/* Total height = ascent + descent */
#define MP_BBOX_H(b)  ((b).ascent + (b).descent)

/* ── Measure ─────────────────────────────────────────────── */
/*
 * Compute the bounding box of an AST subtree rendered in MathPrint.
 * Does NOT draw anything. Pure function of the AST structure.
 */
mp_bbox_t mp_measure(const ast_node_t *node);

/* ── Draw ────────────────────────────────────────────────── */
/*
 * Render an AST subtree at position (x, yc) where yc is the
 * y-coordinate of the center line (fraction bars align here,
 * text is vertically centered on this line).
 *
 * Caller must set gfx text FG color and fill color before calling.
 */
void mp_draw(const ast_node_t *node, int x, int yc);

/* ── High-level rendering ────────────────────────────────── */
/*
 * Draw an AST result in the work area using MathPrint layout.
 * Replaces ui_draw_result() when an AST is available.
 */
void mp_draw_result(const ast_node_t *node);

/* ── 2D cursor navigation for input ──────────────────────── */
/*
 * Navigate the text cursor up/down within 2D structures
 * (fractions, exponents). Modifies ib->cursor position.
 * Returns true if the cursor moved.
 */
bool mp_cursor_up(input_buf_t *ib);
bool mp_cursor_down(input_buf_t *ib);

/* ── Input rendering with MathPrint ──────────────────────── */
/*
 * Try to render the input buffer using MathPrint.
 * Parses the input to AST, renders in 2D, draws cursor.
 * Returns true if MathPrint rendering succeeded.
 * Returns false if parsing failed (caller should fall back to text).
 */
bool mp_try_draw_input(const input_buf_t *ib);

#endif /* MATHPRINT_H */
