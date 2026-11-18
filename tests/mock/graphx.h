/*
 * graphx.h — Mock CE graphics header for desktop compilation
 * Records draw/text calls so UI rendering can be validated in tests.
 */
#ifndef GRAPHX_H
#define GRAPHX_H

#include <stdbool.h>
#include <stdint.h>

/* Color constants */
#define gfx_black   0x00
#define gfx_white   0xFF
#define gfx_red     0xE0
#define gfx_green   0x03
#define gfx_blue    0x10
#define gfx_yellow  0xE7
#define gfx_orange  0xE3

#define GFX_MOCK_MAX_TEXT_CALLS 1024
#define GFX_MOCK_MAX_TEXT_LEN   127
#define GFX_MOCK_SCREEN_W       320
#define GFX_MOCK_SCREEN_H       240

typedef struct {
    char text[GFX_MOCK_MAX_TEXT_LEN + 1];
    int x;
    int y;
    uint8_t fg;
    uint8_t bg;
} gfx_mock_text_call_t;

extern uint8_t gfx_mock_current_color;
extern uint8_t gfx_mock_text_fg_color;
extern uint8_t gfx_mock_text_bg_color;
extern uint8_t gfx_mock_text_transparent_color;
extern int gfx_mock_begin_count;
extern int gfx_mock_end_count;
extern int gfx_mock_swap_count;
extern int gfx_mock_fill_screen_count;
extern int gfx_mock_fill_rect_count;
extern int gfx_mock_horiz_line_count;
extern int gfx_mock_pixel_count;
extern int gfx_mock_line_count;
extern int gfx_mock_vert_line_count;
extern int gfx_mock_rect_count;

void gfx_mock_reset(void);
int gfx_mock_text_call_count(void);
const gfx_mock_text_call_t *gfx_mock_text_call_at(int idx);
bool gfx_mock_text_contains(const char *needle);
uint8_t gfx_mock_color_at(int x, int y);
bool gfx_mock_was_drawn(int x, int y);
int gfx_mock_drawn_pixel_count_in_rect(int x, int y, int w, int h);
bool gfx_mock_any_drawn_pixel_outside_rect(int x, int y, int w, int h);

void gfx_Begin(void);
void gfx_End(void);
void gfx_SetDrawBuffer(void);
void gfx_SwapDraw(void);
void gfx_FillScreen(uint8_t c);
void gfx_SetColor(uint8_t c);
void gfx_SetTextFGColor(uint8_t c);
void gfx_SetTextBGColor(uint8_t c);
void gfx_SetTextTransparentColor(uint8_t c);
void gfx_FillRectangle(int x, int y, int w, int h);
void gfx_HorizLine(int x, int y, int w);
void gfx_PrintStringXY(const char *s, int x, int y);
int gfx_GetStringWidth(const char *s);
void gfx_SetPixel(int x, int y);
void gfx_Line(int x0, int y0, int x1, int y1);
void gfx_VertLine(int x, int y, int h);
void gfx_Rectangle(int x, int y, int w, int h);

#endif /* GRAPHX_H */
