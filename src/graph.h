/*
 * graph.h — Function graphing engine
 * Plots f(x) on 320x240 display with axes, zoom, pan, trace, and shading.
 */
#ifndef GRAPH_MODULE_H
#define GRAPH_MODULE_H

#include "calcengine.h"
#include "ast.h"

/* ── Graph viewport (screen region) ─────────────────────── */
#define GRAPH_X0   0
#define GRAPH_Y0   (HEADER_H + 2)          /* below header bar  */
#define GRAPH_W    SCREEN_W                 /* 320 pixels wide   */
#define GRAPH_AREA_H    (INPUT_Y - GRAPH_Y0 - 2) /* ~196 pixels tall  */

/* ── Viewport (world coordinates) ───────────────────────── */
typedef struct {
    ce_float_t x_min, x_max;
    ce_float_t y_min, y_max;
} graph_viewport_t;

/* ── Coordinate mapping ─────────────────────────────────── */

/* World → screen pixel (clamped to graph region) */
int graph_world_to_sx(const graph_viewport_t *vp, ce_float_t wx);
int graph_world_to_sy(const graph_viewport_t *vp, ce_float_t wy);

/* Screen pixel → world coordinate */
ce_float_t graph_sx_to_world(const graph_viewport_t *vp, int sx);
ce_float_t graph_sy_to_world(const graph_viewport_t *vp, int sy);

/* ── Viewport manipulation ──────────────────────────────── */

/* Set viewport to default [-10, 10] × auto-ranged y */
void graph_default_viewport(graph_viewport_t *vp);

/* Auto-range y by sampling the function across x range */
void graph_auto_range(graph_viewport_t *vp,
                      const ast_node_t *expr, char var);

/* Zoom: factor < 1 zooms in, factor > 1 zooms out (centered) */
void graph_zoom(graph_viewport_t *vp, ce_float_t factor);

/* Pan by world-coordinate deltas */
void graph_pan(graph_viewport_t *vp, ce_float_t dx, ce_float_t dy);

/* ── Drawing ────────────────────────────────────────────── */

/* Draw coordinate axes with tick marks and origin label */
void graph_draw_axes(const graph_viewport_t *vp);

/* Draw the function curve */
void graph_draw_function(const graph_viewport_t *vp,
                         const ast_node_t *expr, char var,
                         uint8_t color);

/* Shade area between curve and x-axis from x=a to x=b */
void graph_draw_shade(const graph_viewport_t *vp,
                      const ast_node_t *expr, char var,
                      ce_float_t a, ce_float_t b, uint8_t shade_color);

/* Draw slope field: short line segments at grid points showing dy/dx */
void graph_draw_slopefield(const graph_viewport_t *vp,
                            const ast_node_t *rhs, char xvar, char yvar,
                            int nx, int ny, uint8_t color);

/* Draw trace cursor crosshair at screen column sx */
void graph_draw_trace(const graph_viewport_t *vp,
                      const ast_node_t *expr, char var,
                      int sx, char *label_buf, int label_size);

/* ── Trace evaluation ───────────────────────────────────── */

/* Evaluate f(x) at the world x corresponding to screen column sx */
ce_float_t graph_trace_eval(const graph_viewport_t *vp,
                            const ast_node_t *expr, char var,
                            int sx, ce_error_t *err);

/* ── Equation graphing (two-function intersection) ─────── */

/* Max number of intersection points to find */
#define GRAPH_MAX_INTERSECTIONS 16

/* Intersection point */
typedef struct {
    ce_float_t x, y;
} graph_intersection_t;

/* Auto-range y by sampling two functions across x range */
void graph_auto_range_two(graph_viewport_t *vp,
                          const ast_node_t *expr1,
                          const ast_node_t *expr2, char var);

/* Find intersections of f(x) and g(x) within the viewport x range.
   Returns the number of intersections found (up to max_pts). */
int graph_find_intersections(const graph_viewport_t *vp,
                             const ast_node_t *expr1,
                             const ast_node_t *expr2, char var,
                             graph_intersection_t *pts, int max_pts);

/* Draw intersection markers (small circles with coordinate labels) */
void graph_draw_intersections(const graph_viewport_t *vp,
                              const graph_intersection_t *pts, int count);

/* Draw trace cursor for two functions at screen column sx */
void graph_draw_trace_two(const graph_viewport_t *vp,
                          const ast_node_t *expr1,
                          const ast_node_t *expr2, char var,
                          int sx, char *label_buf, int label_size);

/* ── Implicit curve graphing ────────────────────────────── */

/*
 * graph_draw_implicit — draw the implicit curve defined by F(x,y) = 0
 * using a marching-squares-style contour approach.
 *
 * eq_expr: AST of F(x,y) (caller arranges so that lhs-rhs=0)
 * xvar: character for the x variable (usually 'x')
 * yvar: character for the y variable (usually 'y')
 * color: pixel color index
 *
 * Algorithm:
 *   Sample F on a grid of (GRAPH_W × GRAPH_AREA_H/2) cells.
 *   For each adjacent column pair, find sign changes along each row
 *   and interpolate x crossing; connect column-to-column crossings
 *   with gfx_SetPixel calls.
 *   Then scan each row for sign changes along y and do the same.
 *   This yields a ≤2-pixel-wide curve without a full marching-squares
 *   table (saves RAM on the CE).
 */
void graph_draw_implicit(const graph_viewport_t *vp,
                         const ast_node_t *eq_expr,
                         char xvar, char yvar,
                         uint8_t color);

/*
 * graph_implicit_result — render implicit graph, write description to buf.
 * Returns true if any curve pixels were drawn.
 */
bool graph_implicit_result(const ast_node_t *eq_expr,
                            char xvar, char yvar,
                            ce_float_t xmin, ce_float_t xmax,
                            ce_float_t ymin, ce_float_t ymax,
                            char *buf, int buf_size);

#endif /* GRAPH_MODULE_H */
