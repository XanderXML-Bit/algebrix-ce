/*
 * graph.c — Function graphing engine
 * Plots f(x) on the 320x240 TI-84 CE color display.
 *
 * Coordinate system:
 *   World coords (x_min..x_max, y_min..y_max) map to screen pixels
 *   in the graph region (GRAPH_X0..GRAPH_X0+GRAPH_W, GRAPH_Y0..GRAPH_Y0+GRAPH_AREA_H).
 *   Screen Y is inverted: top of screen = low pixel Y = high world Y.
 */
#include "graph.h"
#include "eval.h"
#include "memory.h"

#include <graphx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── Coordinate mapping ─────────────────────────────────── */

int graph_world_to_sx(const graph_viewport_t *vp, ce_float_t wx)
{
    ce_float_t range = vp->x_max - vp->x_min;
    if (fabsf(range) < 1e-20f) return GRAPH_X0;
    ce_float_t frac = (wx - vp->x_min) / range;
    int sx = GRAPH_X0 + (int)(frac * (GRAPH_W - 1) + 0.5f);
    if (sx < GRAPH_X0) sx = GRAPH_X0;
    if (sx >= GRAPH_X0 + GRAPH_W) sx = GRAPH_X0 + GRAPH_W - 1;
    return sx;
}

int graph_world_to_sy(const graph_viewport_t *vp, ce_float_t wy)
{
    ce_float_t range = vp->y_max - vp->y_min;
    if (fabsf(range) < 1e-20f) return GRAPH_Y0 + GRAPH_AREA_H / 2;
    /* Screen Y is inverted: higher world Y = lower screen Y */
    ce_float_t frac = (vp->y_max - wy) / range;
    int sy = GRAPH_Y0 + (int)(frac * (GRAPH_AREA_H - 1) + 0.5f);
    if (sy < GRAPH_Y0) sy = GRAPH_Y0;
    if (sy >= GRAPH_Y0 + GRAPH_AREA_H) sy = GRAPH_Y0 + GRAPH_AREA_H - 1;
    return sy;
}

ce_float_t graph_sx_to_world(const graph_viewport_t *vp, int sx)
{
    ce_float_t frac = (ce_float_t)(sx - GRAPH_X0) / (GRAPH_W - 1);
    return vp->x_min + frac * (vp->x_max - vp->x_min);
}

ce_float_t graph_sy_to_world(const graph_viewport_t *vp, int sy)
{
    ce_float_t frac = (ce_float_t)(sy - GRAPH_Y0) / (GRAPH_AREA_H - 1);
    return vp->y_max - frac * (vp->y_max - vp->y_min);
}

/* ── Viewport manipulation ──────────────────────────────── */

void graph_default_viewport(graph_viewport_t *vp)
{
    vp->x_min = -10.0f;
    vp->x_max =  10.0f;
    vp->y_min = -6.5f;
    vp->y_max =  6.5f;
}

void graph_auto_range(graph_viewport_t *vp,
                      const ast_node_t *expr, char var)
{
    /* Sample the function at many points across the x range */
    ce_float_t y_lo =  1e30f;
    ce_float_t y_hi = -1e30f;
    int valid = 0;
    int i;
    int samples = 160;  /* half the screen width — fast enough */

    for (i = 0; i <= samples; i++) {
        ce_float_t x = vp->x_min +
            (vp->x_max - vp->x_min) * i / samples;
        ce_error_t err = CE_OK;
        ce_float_t y = eval_at(expr, var, x, &err);

        if (err != CE_OK || !ce_isfinite(y)) continue;

        /* Clamp extreme values to keep graph reasonable */
        if (y > 1e6f) y = 1e6f;
        if (y < -1e6f) y = -1e6f;

        if (y < y_lo) y_lo = y;
        if (y > y_hi) y_hi = y;
        valid++;
    }

    if (valid < 2 || y_lo >= y_hi) {
        /* Fallback: symmetric default */
        vp->y_min = -10.0f;
        vp->y_max =  10.0f;
        return;
    }

    /* Add 10% padding */
    ce_float_t margin = (y_hi - y_lo) * 0.1f;
    if (margin < 0.5f) margin = 0.5f;
    vp->y_min = y_lo - margin;
    vp->y_max = y_hi + margin;
}

void graph_zoom(graph_viewport_t *vp, ce_float_t factor)
{
    if (!ce_isfinite(factor) || factor <= 0.0f) return;
    ce_float_t cx = (vp->x_min + vp->x_max) * 0.5f;
    ce_float_t cy = (vp->y_min + vp->y_max) * 0.5f;
    ce_float_t hw = (vp->x_max - vp->x_min) * 0.5f * factor;
    ce_float_t hh = (vp->y_max - vp->y_min) * 0.5f * factor;
    vp->x_min = cx - hw;
    vp->x_max = cx + hw;
    vp->y_min = cy - hh;
    vp->y_max = cy + hh;
}

void graph_pan(graph_viewport_t *vp, ce_float_t dx, ce_float_t dy)
{
    vp->x_min += dx;
    vp->x_max += dx;
    vp->y_min += dy;
    vp->y_max += dy;
}

/* ── Drawing helpers ────────────────────────────────────── */

/* Draw a single pixel (bounds-checked to graph region) */
static void put_pixel(int x, int y)
{
    if (x >= GRAPH_X0 && x < GRAPH_X0 + GRAPH_W &&
        y >= GRAPH_Y0 && y < GRAPH_Y0 + GRAPH_AREA_H) {
        gfx_SetPixel(x, y);
    }
}

/* Draw vertical line segment within graph region */
static void vert_seg(int x, int y1, int y2)
{
    int lo, hi, y;
    if (y1 > y2) { lo = y2; hi = y1; }
    else          { lo = y1; hi = y2; }
    if (lo < GRAPH_Y0) lo = GRAPH_Y0;
    if (hi >= GRAPH_Y0 + GRAPH_AREA_H) hi = GRAPH_Y0 + GRAPH_AREA_H - 1;
    if (x < GRAPH_X0 || x >= GRAPH_X0 + GRAPH_W) return;
    for (y = lo; y <= hi; y++)
        gfx_SetPixel(x, y);
}

/* ── Drawing ────────────────────────────────────────────── */

void graph_draw_axes(const graph_viewport_t *vp)
{
    int axis_x, axis_y, i;
    ce_float_t tick_step, t;

    /* Clear graph area */
    gfx_SetColor(COL_WHITE);
    gfx_FillRectangle(GRAPH_X0, GRAPH_Y0, GRAPH_W, GRAPH_AREA_H);

    /* Draw x-axis (horizontal line at y=0) if visible */
    axis_y = graph_world_to_sy(vp, 0.0f);
    if (axis_y >= GRAPH_Y0 && axis_y < GRAPH_Y0 + GRAPH_AREA_H) {
        gfx_SetColor(COL_DKGRAY);
        gfx_HorizLine(GRAPH_X0, axis_y, GRAPH_W);
    }

    /* Draw y-axis (vertical line at x=0) if visible */
    axis_x = graph_world_to_sx(vp, 0.0f);
    if (axis_x >= GRAPH_X0 && axis_x < GRAPH_X0 + GRAPH_W) {
        gfx_SetColor(COL_DKGRAY);
        for (i = GRAPH_Y0; i < GRAPH_Y0 + GRAPH_AREA_H; i++)
            gfx_SetPixel(axis_x, i);
    }

    /* Tick marks on axes — choose a nice step */
    {
        ce_float_t x_range = vp->x_max - vp->x_min;
        ce_float_t raw = x_range / 10.0f;
        ce_float_t mag = powf(10.0f, floorf(log10f(fabsf(raw) + 1e-30f)));
        ce_float_t norm = raw / mag;
        if (norm < 1.5f) tick_step = 1.0f * mag;
        else if (norm < 3.5f) tick_step = 2.0f * mag;
        else if (norm < 7.5f) tick_step = 5.0f * mag;
        else tick_step = 10.0f * mag;
    }

    /* X-axis tick marks */
    if (axis_y >= GRAPH_Y0 && axis_y < GRAPH_Y0 + GRAPH_AREA_H) {
        ce_float_t start = ceilf(vp->x_min / tick_step) * tick_step;
        gfx_SetColor(COL_DKGRAY);
        for (t = start; t <= vp->x_max; t += tick_step) {
            if (fabsf(t) < tick_step * 0.01f) continue; /* skip origin */
            int tx = graph_world_to_sx(vp, t);
            for (i = -2; i <= 2; i++)
                put_pixel(tx, axis_y + i);
        }
    }

    /* Y-axis tick marks */
    if (axis_x >= GRAPH_X0 && axis_x < GRAPH_X0 + GRAPH_W) {
        ce_float_t y_range = vp->y_max - vp->y_min;
        ce_float_t raw_y = y_range / 8.0f;
        ce_float_t mag_y = powf(10.0f, floorf(log10f(fabsf(raw_y) + 1e-30f)));
        ce_float_t norm_y = raw_y / mag_y;
        ce_float_t ystep;
        ce_float_t ystart;
        if (norm_y < 1.5f) ystep = 1.0f * mag_y;
        else if (norm_y < 3.5f) ystep = 2.0f * mag_y;
        else if (norm_y < 7.5f) ystep = 5.0f * mag_y;
        else ystep = 10.0f * mag_y;

        ystart = ceilf(vp->y_min / ystep) * ystep;
        gfx_SetColor(COL_DKGRAY);
        for (t = ystart; t <= vp->y_max; t += ystep) {
            if (fabsf(t) < ystep * 0.01f) continue;
            int ty = graph_world_to_sy(vp, t);
            for (i = -2; i <= 2; i++)
                put_pixel(axis_x + i, ty);
        }
    }

    /* Origin label "0" */
    if (axis_x >= GRAPH_X0 && axis_x < GRAPH_X0 + GRAPH_W &&
        axis_y >= GRAPH_Y0 && axis_y < GRAPH_Y0 + GRAPH_AREA_H) {
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_SetTextBGColor(COL_WHITE);
        if (axis_x + 4 < GRAPH_X0 + GRAPH_W &&
            axis_y + 2 < GRAPH_Y0 + GRAPH_AREA_H)
            gfx_PrintStringXY("0", axis_x + 4, axis_y + 2);
    }
}

void graph_draw_function(const graph_viewport_t *vp,
                         const ast_node_t *expr, char var,
                         uint8_t color)
{
    int sx;
    int prev_sy = -1;
    bool prev_valid = false;

    gfx_SetColor(color);

    for (sx = GRAPH_X0; sx < GRAPH_X0 + GRAPH_W; sx++) {
        ce_float_t wx = graph_sx_to_world(vp, sx);
        ce_error_t err = CE_OK;
        ce_float_t wy = eval_at(expr, var, wx, &err);

        if (err != CE_OK || !ce_isfinite(wy)) {
            prev_valid = false;
            continue;
        }

        int sy = graph_world_to_sy(vp, wy);

        /* Draw pixel at this point */
        put_pixel(sx, sy);

        /* Connect to previous point if gap is small (no asymptote) */
        if (prev_valid && sx > GRAPH_X0) {
            int dy = sy - prev_sy;
            if (dy < 0) dy = -dy;
            /* Only connect if vertical gap < half the graph height
               (avoids connecting across asymptotes) */
            if (dy < GRAPH_AREA_H / 2) {
                vert_seg(sx, prev_sy, sy);
            }
        }

        prev_sy = sy;
        prev_valid = true;
    }
}

void graph_draw_shade(const graph_viewport_t *vp,
                      const ast_node_t *expr, char var,
                      ce_float_t a, ce_float_t b, uint8_t shade_color)
{
    int sx_a = graph_world_to_sx(vp, a);
    int sx_b = graph_world_to_sx(vp, b);
    int sx;
    int y_axis = graph_world_to_sy(vp, 0.0f);

    if (sx_a > sx_b) {
        int tmp = sx_a;
        sx_a = sx_b;
        sx_b = tmp;
    }

    gfx_SetColor(shade_color);

    for (sx = sx_a; sx <= sx_b && sx < GRAPH_X0 + GRAPH_W; sx++) {
        if (sx < GRAPH_X0) continue;
        ce_float_t wx = graph_sx_to_world(vp, sx);
        ce_error_t err = CE_OK;
        ce_float_t wy = eval_at(expr, var, wx, &err);

        if (err != CE_OK || !ce_isfinite(wy)) continue;

        int sy = graph_world_to_sy(vp, wy);
        /* Shade from x-axis to curve */
        vert_seg(sx, y_axis, sy);
    }
}

void graph_draw_slopefield(const graph_viewport_t *vp,
                            const ast_node_t *rhs, char xvar, char yvar,
                            int nx, int ny, uint8_t color)
{
    int ix, iy;
    eval_ctx_t sf_ctx;
    ce_float_t x_range = vp->x_max - vp->x_min;
    ce_float_t y_range = vp->y_max - vp->y_min;
    /* Segment half-length in pixels — fit between grid points */
    int seg_half_x;
    int seg_half_y;
    int seg_half;

    if (nx < 2 || ny < 2) return;
    seg_half_x = (GRAPH_W / nx) / 3;
    seg_half_y = (GRAPH_AREA_H / ny) / 3;
    seg_half = (seg_half_x < seg_half_y) ? seg_half_x : seg_half_y;
    if (seg_half < 2) seg_half = 2;

    gfx_SetColor(color);

    for (ix = 0; ix < nx; ix++) {
        ce_float_t wx = vp->x_min + x_range * ix / (nx - 1);
        for (iy = 0; iy < ny; iy++) {
            ce_float_t wy = vp->y_min + y_range * iy / (ny - 1);
            ce_float_t slope;
            ce_error_t err = CE_OK;
            int cx, cy;   /* center pixel */
            int x0, y0, x1, y1;  /* segment endpoints */
            ce_float_t dx, dy, mag;

            eval_init(&sf_ctx);
            eval_set_var(&sf_ctx, xvar, wx);
            eval_set_var(&sf_ctx, yvar, wy);
            slope = eval_expr(&sf_ctx, rhs);
            err = eval_error(&sf_ctx);

            if (err != CE_OK || !ce_isfinite(slope)) continue;

            cx = graph_world_to_sx(vp, wx);
            cy = graph_world_to_sy(vp, wy);

            /* Compute unit direction vector (1, slope) then scale to seg_half */
            mag = sqrtf(1.0f + slope * slope);
            dx = (ce_float_t)seg_half / mag;
            dy = (ce_float_t)seg_half * slope / mag;

            x0 = cx - (int)(dx + 0.5f);
            x1 = cx + (int)(dx + 0.5f);
            /* Screen Y is inverted: positive slope goes up = lower screen Y */
            y0 = cy + (int)(dy + 0.5f);
            y1 = cy - (int)(dy + 0.5f);

            /* Draw segment using Bresenham-style line */
            {
                int adx = x1 - x0;
                int ady = y1 - y0;
                int sx_step = (adx >= 0) ? 1 : -1;
                int sy_step = (ady >= 0) ? 1 : -1;
                int abs_dx = adx * sx_step;
                int abs_dy = ady * sy_step;
                int px = x0, py = y0;

                if (abs_dx >= abs_dy) {
                    int err_b = abs_dx / 2;
                    while (px != x1) {
                        put_pixel(px, py);
                        err_b -= abs_dy;
                        if (err_b < 0) { py += sy_step; err_b += abs_dx; }
                        px += sx_step;
                    }
                    put_pixel(x1, y1);
                } else {
                    int err_b = abs_dy / 2;
                    while (py != y1) {
                        put_pixel(px, py);
                        err_b -= abs_dx;
                        if (err_b < 0) { px += sx_step; err_b += abs_dy; }
                        py += sy_step;
                    }
                    put_pixel(x1, y1);
                }
            }
        }
    }
}

void graph_draw_trace(const graph_viewport_t *vp,
                      const ast_node_t *expr, char var,
                      int sx, char *label_buf, int label_size)
{
    ce_error_t err = CE_OK;
    ce_float_t wx = graph_sx_to_world(vp, sx);
    ce_float_t wy = eval_at(expr, var, wx, &err);

    if (err != CE_OK || !ce_isfinite(wy)) {
        snprintf(label_buf, label_size, "x=%.4g  y=undef",
                 (double)wx);
        return;
    }

    int sy = graph_world_to_sy(vp, wy);

    /* Vertical crosshair line */
    gfx_SetColor(COL_LTGRAY);
    {
        int y;
        for (y = GRAPH_Y0; y < GRAPH_Y0 + GRAPH_AREA_H; y += 2)
            gfx_SetPixel(sx, y);
    }

    /* Horizontal crosshair line */
    {
        int x;
        for (x = GRAPH_X0; x < GRAPH_X0 + GRAPH_W; x += 2)
            gfx_SetPixel(x, sy);
    }

    /* Cursor dot */
    gfx_SetColor(COL_RED);
    put_pixel(sx, sy);
    put_pixel(sx - 1, sy);
    put_pixel(sx + 1, sy);
    put_pixel(sx, sy - 1);
    put_pixel(sx, sy + 1);

    /* Label */
    snprintf(label_buf, label_size, "x=%.4g  y=%.4g",
             (double)wx, (double)wy);
}

/* ── Trace evaluation ───────────────────────────────────── */

ce_float_t graph_trace_eval(const graph_viewport_t *vp,
                            const ast_node_t *expr, char var,
                            int sx, ce_error_t *err)
{
    ce_float_t wx = graph_sx_to_world(vp, sx);
    return eval_at(expr, var, wx, err);
}

/* ── Equation graphing helpers ─────────────────────────── */

void graph_auto_range_two(graph_viewport_t *vp,
                          const ast_node_t *expr1,
                          const ast_node_t *expr2, char var)
{
    ce_float_t y_lo =  1e30f;
    ce_float_t y_hi = -1e30f;
    int valid = 0;
    int i;
    int samples = 160;

    for (i = 0; i <= samples; i++) {
        ce_float_t x = vp->x_min +
            (vp->x_max - vp->x_min) * i / samples;
        ce_error_t e1 = CE_OK, e2 = CE_OK;
        ce_float_t y1 = eval_at(expr1, var, x, &e1);
        ce_float_t y2 = eval_at(expr2, var, x, &e2);

        if (e1 == CE_OK && ce_isfinite(y1)) {
            if (y1 > 1e6f) y1 = 1e6f;
            if (y1 < -1e6f) y1 = -1e6f;
            if (y1 < y_lo) y_lo = y1;
            if (y1 > y_hi) y_hi = y1;
            valid++;
        }
        if (e2 == CE_OK && ce_isfinite(y2)) {
            if (y2 > 1e6f) y2 = 1e6f;
            if (y2 < -1e6f) y2 = -1e6f;
            if (y2 < y_lo) y_lo = y2;
            if (y2 > y_hi) y_hi = y2;
            valid++;
        }
    }

    if (valid < 2 || y_lo >= y_hi) {
        vp->y_min = -10.0f;
        vp->y_max =  10.0f;
        return;
    }

    ce_float_t margin = (y_hi - y_lo) * 0.1f;
    if (margin < 0.5f) margin = 0.5f;
    vp->y_min = y_lo - margin;
    vp->y_max = y_hi + margin;
}

int graph_find_intersections(const graph_viewport_t *vp,
                             const ast_node_t *expr1,
                             const ast_node_t *expr2, char var,
                             graph_intersection_t *pts, int max_pts)
{
    int count = 0;
    int samples = GRAPH_W * 2;
    ce_float_t prev_diff = 0;
    bool prev_valid = false;
    ce_float_t prev_x = 0;
    int i;

    for (i = 0; i <= samples && count < max_pts; i++) {
        ce_float_t x = vp->x_min +
            (vp->x_max - vp->x_min) * i / samples;
        ce_error_t e1 = CE_OK, e2 = CE_OK;
        ce_float_t y1 = eval_at(expr1, var, x, &e1);
        ce_float_t y2 = eval_at(expr2, var, x, &e2);

        if (e1 != CE_OK || e2 != CE_OK ||
            !ce_isfinite(y1) || !ce_isfinite(y2)) {
            prev_valid = false;
            continue;
        }

        ce_float_t diff = y1 - y2;

        if (fabsf(diff) < 1e-6f) {
            pts[count].x = x;
            pts[count].y = y1;
            count++;
            prev_valid = false;
            continue;
        }

        if (prev_valid && ((prev_diff > 0 && diff < 0) ||
                           (prev_diff < 0 && diff > 0))) {
            ce_float_t lo = prev_x, hi = x;
            int iter;
            for (iter = 0; iter < 30; iter++) {
                ce_float_t mid = (lo + hi) * 0.5f;
                ce_error_t me1 = CE_OK, me2 = CE_OK;
                ce_float_t m1 = eval_at(expr1, var, mid, &me1);
                ce_float_t m2 = eval_at(expr2, var, mid, &me2);
                if (me1 != CE_OK || me2 != CE_OK ||
                    !ce_isfinite(m1) || !ce_isfinite(m2)) break;
                ce_float_t md = m1 - m2;
                if (fabsf(md) < 1e-7f) { lo = hi = mid; break; }
                if ((prev_diff > 0 && md > 0) ||
                    (prev_diff < 0 && md < 0))
                    lo = mid;
                else
                    hi = mid;
            }
            ce_float_t xr = (lo + hi) * 0.5f;
            ce_error_t re = CE_OK;
            ce_float_t yr = eval_at(expr1, var, xr, &re);
            if (re == CE_OK && ce_isfinite(yr)) {
                pts[count].x = xr;
                pts[count].y = yr;
                count++;
            }
        }

        prev_diff = diff;
        prev_x = x;
        prev_valid = true;
    }

    return count;
}

void graph_draw_intersections(const graph_viewport_t *vp,
                              const graph_intersection_t *pts, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        int sx = graph_world_to_sx(vp, pts[i].x);
        int sy = graph_world_to_sy(vp, pts[i].y);

        gfx_SetColor(COL_GREEN);
        put_pixel(sx, sy);
        put_pixel(sx - 1, sy); put_pixel(sx + 1, sy);
        put_pixel(sx, sy - 1); put_pixel(sx, sy + 1);
        put_pixel(sx - 2, sy); put_pixel(sx + 2, sy);
        put_pixel(sx, sy - 2); put_pixel(sx, sy + 2);
        put_pixel(sx - 1, sy - 1); put_pixel(sx + 1, sy - 1);
        put_pixel(sx - 1, sy + 1); put_pixel(sx + 1, sy + 1);

        {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "(%.3g,%.3g)",
                     (double)pts[i].x, (double)pts[i].y);
            int lx = sx - 20;
            int ly = sy + 4;
            if (lx < GRAPH_X0) lx = GRAPH_X0;
            if (lx + 60 >= GRAPH_X0 + GRAPH_W)
                lx = GRAPH_X0 + GRAPH_W - 61;
            if (ly + 8 >= GRAPH_Y0 + GRAPH_AREA_H) ly = sy - 12;
            gfx_SetTextFGColor(COL_GREEN);
            gfx_SetTextBGColor(COL_WHITE);
            gfx_PrintStringXY(lbl, lx, ly);
        }
    }
}

void graph_draw_trace_two(const graph_viewport_t *vp,
                          const ast_node_t *expr1,
                          const ast_node_t *expr2, char var,
                          int sx, char *label_buf, int label_size)
{
    ce_error_t err1 = CE_OK, err2 = CE_OK;
    ce_float_t wx = graph_sx_to_world(vp, sx);
    ce_float_t wy1 = eval_at(expr1, var, wx, &err1);
    ce_float_t wy2 = eval_at(expr2, var, wx, &err2);

    gfx_SetColor(COL_LTGRAY);
    {
        int y;
        for (y = GRAPH_Y0; y < GRAPH_Y0 + GRAPH_AREA_H; y += 2)
            gfx_SetPixel(sx, y);
    }

    if (err1 == CE_OK && ce_isfinite(wy1)) {
        int sy1 = graph_world_to_sy(vp, wy1);
        gfx_SetColor(COL_BLUE);
        put_pixel(sx, sy1);
        put_pixel(sx - 1, sy1); put_pixel(sx + 1, sy1);
        put_pixel(sx, sy1 - 1); put_pixel(sx, sy1 + 1);
    }
    if (err2 == CE_OK && ce_isfinite(wy2)) {
        int sy2 = graph_world_to_sy(vp, wy2);
        gfx_SetColor(COL_RED);
        put_pixel(sx, sy2);
        put_pixel(sx - 1, sy2); put_pixel(sx + 1, sy2);
        put_pixel(sx, sy2 - 1); put_pixel(sx, sy2 + 1);
    }

    if (err1 == CE_OK && ce_isfinite(wy1) &&
        err2 == CE_OK && ce_isfinite(wy2)) {
        snprintf(label_buf, label_size, "x=%.3g f=%.3g g=%.3g",
                 (double)wx, (double)wy1, (double)wy2);
    } else if (err1 == CE_OK && ce_isfinite(wy1)) {
        snprintf(label_buf, label_size, "x=%.3g f=%.3g g=undef",
                 (double)wx, (double)wy1);
    } else if (err2 == CE_OK && ce_isfinite(wy2)) {
        snprintf(label_buf, label_size, "x=%.3g f=undef g=%.3g",
                 (double)wx, (double)wy2);
    } else {
        snprintf(label_buf, label_size, "x=%.3g f=undef g=undef",
                 (double)wx);
    }
}

/* ── Implicit curve graphing ─────────────────────────────── */

/*
 * Evaluate F(x,y) for implicit graphing.
 * Returns NAN on error.
 */
static ce_float_t eval_implicit(const ast_node_t *eq,
                                 char xv, ce_float_t xval,
                                 char yv, ce_float_t yval)
{
    eval_ctx_t ec;
    ce_float_t r;
    eval_init(&ec);
    if (!eval_set_var(&ec, xv, xval)) return NAN;
    if (!eval_set_var(&ec, yv, yval)) return NAN;
    r = eval_expr(&ec, eq);
    if (ec.error != CE_OK || !ce_isfinite(r)) return NAN;
    return r;
}

/*
 * graph_draw_implicit — draw the zero-contour of F(x,y) = 0.
 *
 * Strategy: scan each screen column sx from 0..GRAPH_W-1.
 * For each column, compute the world x.  Sample F(x, y_k) at each
 * screen row y_k.  When the sign changes between row k and k+1,
 * interpolate to find the exact screen row and light a pixel there.
 * Also scan rows (horizontal sign changes) and light pixels for those.
 * The two passes together give good coverage at low RAM cost.
 */
void graph_draw_implicit(const graph_viewport_t *vp,
                         const ast_node_t *eq_expr,
                         char xvar, char yvar,
                         uint8_t color)
{
    int sx, sy;
    int y0 = GRAPH_Y0;
    int yH = GRAPH_AREA_H;
    int xW = GRAPH_W;

    gfx_SetColor(color);

    /* --- Pass 1: scan columns, detect row sign changes --- */
    for (sx = 0; sx < xW; sx++) {
        ce_float_t wx = graph_sx_to_world(vp, sx);
        ce_float_t prev_f = NAN;
        int prev_row = -1;

        for (sy = 0; sy < yH; sy++) {
            ce_float_t wy = graph_sy_to_world(vp, y0 + sy);
            ce_float_t f  = eval_implicit(eq_expr, xvar, wx, yvar, wy);

            if (!ce_isnan(f) && !ce_isnan(prev_f) && prev_row >= 0) {
                if ((prev_f >= 0.0f) != (f >= 0.0f)) {
                    /* Sign change between row (y0+prev_row) and (y0+sy) */
                    /* Interpolate: t = prev_f / (prev_f - f) */
                    ce_float_t t = prev_f / (prev_f - f);
                    int mid_sy = y0 + prev_row + (int)(t + 0.5f);
                    if (mid_sy < y0) mid_sy = y0;
                    if (mid_sy >= y0 + yH) mid_sy = y0 + yH - 1;
                    gfx_SetPixel((unsigned int)sx, (unsigned int)mid_sy);
                }
            }
            prev_f   = f;
            prev_row = sy;
        }
    }

    /* --- Pass 2: scan rows, detect column sign changes --- */
    for (sy = 0; sy < yH; sy++) {
        ce_float_t wy = graph_sy_to_world(vp, y0 + sy);
        ce_float_t prev_f = NAN;
        int prev_col = -1;

        for (sx = 0; sx < xW; sx++) {
            ce_float_t wx = graph_sx_to_world(vp, sx);
            ce_float_t f  = eval_implicit(eq_expr, xvar, wx, yvar, wy);

            if (!ce_isnan(f) && !ce_isnan(prev_f) && prev_col >= 0) {
                if ((prev_f >= 0.0f) != (f >= 0.0f)) {
                    ce_float_t t = prev_f / (prev_f - f);
                    int mid_sx = prev_col + (int)(t + 0.5f);
                    if (mid_sx < 0) mid_sx = 0;
                    if (mid_sx >= xW) mid_sx = xW - 1;
                    gfx_SetPixel((unsigned int)mid_sx, (unsigned int)(y0 + sy));
                }
            }
            prev_f   = f;
            prev_col = sx;
        }
    }
}

bool graph_implicit_result(const ast_node_t *eq_expr,
                            char xvar, char yvar,
                            ce_float_t xmin, ce_float_t xmax,
                            ce_float_t ymin, ce_float_t ymax,
                            char *buf, int buf_size)
{
    graph_viewport_t vp;
    char eq_buf[64];

    if (!eq_expr || !buf) return false;

    /* Use provided viewport or defaults */
    if (xmin >= xmax) { xmin = -10.0f; xmax = 10.0f; }
    if (ymin >= ymax) { ymin = -10.0f; ymax = 10.0f; }
    vp.x_min = xmin; vp.x_max = xmax;
    vp.y_min = ymin; vp.y_max = ymax;

    ast_to_display_str(eq_expr, eq_buf, sizeof(eq_buf));
    snprintf(buf, (size_t)buf_size,
             "[Graph] %s=0 (%c:[%.2g,%.2g] %c:[%.2g,%.2g])",
             eq_buf, xvar, (double)xmin, (double)xmax,
             yvar, (double)ymin, (double)ymax);

    graph_draw_axes(&vp);
    graph_draw_implicit(&vp, eq_expr, xvar, yvar, COL_BLUE);
    return true;
}
