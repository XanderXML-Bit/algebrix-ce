/*
 * graphx_mock.c — Stateful graphx mock for desktop tests
 */
#include "graphx.h"

#include <string.h>

uint8_t gfx_mock_current_color = gfx_black;
uint8_t gfx_mock_text_fg_color = gfx_black;
uint8_t gfx_mock_text_bg_color = gfx_white;
uint8_t gfx_mock_text_transparent_color = gfx_white;
int gfx_mock_begin_count = 0;
int gfx_mock_end_count = 0;
int gfx_mock_swap_count = 0;
int gfx_mock_fill_screen_count = 0;
int gfx_mock_fill_rect_count = 0;
int gfx_mock_horiz_line_count = 0;
int gfx_mock_pixel_count = 0;
int gfx_mock_line_count = 0;
int gfx_mock_vert_line_count = 0;
int gfx_mock_rect_count = 0;

static gfx_mock_text_call_t gfx_mock_text_calls[GFX_MOCK_MAX_TEXT_CALLS];
static int gfx_mock_text_calls_used = 0;
static uint8_t gfx_mock_framebuffer[GFX_MOCK_SCREEN_H][GFX_MOCK_SCREEN_W];
static uint8_t gfx_mock_draw_mask[GFX_MOCK_SCREEN_H][GFX_MOCK_SCREEN_W];

static bool gfx_mock_in_bounds(int x, int y)
{
    return x >= 0 && x < GFX_MOCK_SCREEN_W &&
           y >= 0 && y < GFX_MOCK_SCREEN_H;
}

static void gfx_mock_plot(int x, int y, uint8_t color)
{
    if (!gfx_mock_in_bounds(x, y)) return;
    gfx_mock_framebuffer[y][x] = color;
    gfx_mock_draw_mask[y][x] = 1;
}

static void gfx_mock_draw_horiz_span(int x, int y, int w, uint8_t color)
{
    int start;
    int end;
    int px;

    if (y < 0 || y >= GFX_MOCK_SCREEN_H || w <= 0) return;
    start = x;
    end = x + w - 1;
    if (end < 0 || start >= GFX_MOCK_SCREEN_W) return;
    if (start < 0) start = 0;
    if (end >= GFX_MOCK_SCREEN_W) end = GFX_MOCK_SCREEN_W - 1;
    for (px = start; px <= end; px++) {
        gfx_mock_plot(px, y, color);
    }
}

static void gfx_mock_draw_vert_span(int x, int y, int h, uint8_t color)
{
    int start;
    int end;
    int py;

    if (x < 0 || x >= GFX_MOCK_SCREEN_W || h <= 0) return;
    start = y;
    end = y + h - 1;
    if (end < 0 || start >= GFX_MOCK_SCREEN_H) return;
    if (start < 0) start = 0;
    if (end >= GFX_MOCK_SCREEN_H) end = GFX_MOCK_SCREEN_H - 1;
    for (py = start; py <= end; py++) {
        gfx_mock_plot(x, py, color);
    }
}

void gfx_mock_reset(void)
{
    gfx_mock_current_color = gfx_black;
    gfx_mock_text_fg_color = gfx_black;
    gfx_mock_text_bg_color = gfx_white;
    gfx_mock_text_transparent_color = gfx_white;
    gfx_mock_begin_count = 0;
    gfx_mock_end_count = 0;
    gfx_mock_swap_count = 0;
    gfx_mock_fill_screen_count = 0;
    gfx_mock_fill_rect_count = 0;
    gfx_mock_horiz_line_count = 0;
    gfx_mock_pixel_count = 0;
    gfx_mock_line_count = 0;
    gfx_mock_vert_line_count = 0;
    gfx_mock_rect_count = 0;
    gfx_mock_text_calls_used = 0;
    memset(gfx_mock_text_calls, 0, sizeof(gfx_mock_text_calls));
    memset(gfx_mock_framebuffer, gfx_white, sizeof(gfx_mock_framebuffer));
    memset(gfx_mock_draw_mask, 0, sizeof(gfx_mock_draw_mask));
}

int gfx_mock_text_call_count(void)
{
    return gfx_mock_text_calls_used;
}

const gfx_mock_text_call_t *gfx_mock_text_call_at(int idx)
{
    if (idx < 0 || idx >= gfx_mock_text_calls_used) return NULL;
    return &gfx_mock_text_calls[idx];
}

bool gfx_mock_text_contains(const char *needle)
{
    int i;
    if (!needle || !*needle) return false;
    for (i = 0; i < gfx_mock_text_calls_used; i++) {
        if (strstr(gfx_mock_text_calls[i].text, needle)) return true;
    }
    return false;
}

uint8_t gfx_mock_color_at(int x, int y)
{
    if (!gfx_mock_in_bounds(x, y)) return 0;
    return gfx_mock_framebuffer[y][x];
}

bool gfx_mock_was_drawn(int x, int y)
{
    if (!gfx_mock_in_bounds(x, y)) return false;
    return gfx_mock_draw_mask[y][x] != 0;
}

int gfx_mock_drawn_pixel_count_in_rect(int x, int y, int w, int h)
{
    int start_x;
    int end_x;
    int start_y;
    int end_y;
    int px;
    int py;
    int count = 0;

    if (w <= 0 || h <= 0) return 0;

    start_x = x < 0 ? 0 : x;
    start_y = y < 0 ? 0 : y;
    end_x = x + w - 1;
    end_y = y + h - 1;

    if (start_x >= GFX_MOCK_SCREEN_W || start_y >= GFX_MOCK_SCREEN_H) return 0;
    if (end_x < 0 || end_y < 0) return 0;
    if (end_x >= GFX_MOCK_SCREEN_W) end_x = GFX_MOCK_SCREEN_W - 1;
    if (end_y >= GFX_MOCK_SCREEN_H) end_y = GFX_MOCK_SCREEN_H - 1;

    for (py = start_y; py <= end_y; py++) {
        for (px = start_x; px <= end_x; px++) {
            if (gfx_mock_draw_mask[py][px]) count++;
        }
    }

    return count;
}

bool gfx_mock_any_drawn_pixel_outside_rect(int x, int y, int w, int h)
{
    int px;
    int py;

    for (py = 0; py < GFX_MOCK_SCREEN_H; py++) {
        for (px = 0; px < GFX_MOCK_SCREEN_W; px++) {
            if (!gfx_mock_draw_mask[py][px]) continue;
            if (px < x || px >= x + w || py < y || py >= y + h) {
                return true;
            }
        }
    }

    return false;
}

void gfx_Begin(void)
{
    gfx_mock_begin_count++;
}

void gfx_End(void)
{
    gfx_mock_end_count++;
}

void gfx_SetDrawBuffer(void) {}

void gfx_SwapDraw(void)
{
    gfx_mock_swap_count++;
}

void gfx_FillScreen(uint8_t c)
{
    int y;
    gfx_mock_current_color = c;
    gfx_mock_fill_screen_count++;
    for (y = 0; y < GFX_MOCK_SCREEN_H; y++) {
        memset(gfx_mock_framebuffer[y], c, sizeof(gfx_mock_framebuffer[y]));
        memset(gfx_mock_draw_mask[y], 1, sizeof(gfx_mock_draw_mask[y]));
    }
}

void gfx_SetColor(uint8_t c)
{
    gfx_mock_current_color = c;
}

void gfx_SetTextFGColor(uint8_t c)
{
    gfx_mock_text_fg_color = c;
}

void gfx_SetTextBGColor(uint8_t c)
{
    gfx_mock_text_bg_color = c;
}

void gfx_SetTextTransparentColor(uint8_t c)
{
    gfx_mock_text_transparent_color = c;
}

void gfx_FillRectangle(int x, int y, int w, int h)
{
    int py;
    gfx_mock_fill_rect_count++;
    for (py = y; py < y + h; py++) {
        gfx_mock_draw_horiz_span(x, py, w, gfx_mock_current_color);
    }
}

void gfx_HorizLine(int x, int y, int w)
{
    gfx_mock_horiz_line_count++;
    gfx_mock_draw_horiz_span(x, y, w, gfx_mock_current_color);
}

void gfx_PrintStringXY(const char *s, int x, int y)
{
    gfx_mock_text_call_t *call;
    if (!s) return;
    if (gfx_mock_text_calls_used >= GFX_MOCK_MAX_TEXT_CALLS) return;

    call = &gfx_mock_text_calls[gfx_mock_text_calls_used++];
    strncpy(call->text, s, GFX_MOCK_MAX_TEXT_LEN);
    call->text[GFX_MOCK_MAX_TEXT_LEN] = '\0';
    call->x = x;
    call->y = y;
    call->fg = gfx_mock_text_fg_color;
    call->bg = gfx_mock_text_bg_color;
}

int gfx_GetStringWidth(const char *s)
{
    /* Return 8 pixels per character for consistent layout in tests */
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n * 8;
}

void gfx_SetPixel(int x, int y)
{
    gfx_mock_pixel_count++;
    gfx_mock_plot(x, y, gfx_mock_current_color);
}

void gfx_Line(int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? -(y1 - y0) : -(y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    gfx_mock_line_count++;
    while (1) {
        gfx_mock_plot(x0, y0, gfx_mock_current_color);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = err * 2;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

void gfx_VertLine(int x, int y, int h)
{
    gfx_mock_vert_line_count++;
    gfx_mock_draw_vert_span(x, y, h, gfx_mock_current_color);
}

void gfx_Rectangle(int x, int y, int w, int h)
{
    gfx_mock_rect_count++;
    if (w <= 0 || h <= 0) return;
    gfx_mock_draw_horiz_span(x,         y,         w, gfx_mock_current_color);
    gfx_mock_draw_horiz_span(x,         y + h - 1, w, gfx_mock_current_color);
    gfx_mock_draw_vert_span (x,         y,         h, gfx_mock_current_color);
    gfx_mock_draw_vert_span (x + w - 1, y,         h, gfx_mock_current_color);
}
