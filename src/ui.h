/*
 * ui.h — Display rendering & keypad input
 * Uses CE graphx + keypadc libraries.
 */
#ifndef UI_H
#define UI_H

#include "calcengine.h"

/* ── Input buffer ────────────────────────────────────────── */
typedef struct {
    char    buf[MAX_INPUT_LEN];
    uint8_t len;
    uint8_t cursor;
} input_buf_t;

/* ── Application state ───────────────────────────────────── */
typedef enum {
    STATE_INPUT,      /* typing an expression  */
    STATE_RESULT,     /* showing result/steps  */
    STATE_ERROR,      /* showing error         */
    STATE_DISAMBIG    /* method-choice dialog  */
} app_state_t;

/* Maximum disambiguation options */
#define DISAMBIG_MAX_OPTIONS 6
#define DISAMBIG_LABEL_LEN   32
#define DISAMBIG_EXPR_LEN    MAX_INPUT_LEN

/* ── Menu item definition ───────────────────────────────── */
typedef struct {
    const char *label;       /* display text                               */
    const char *insert_str;  /* string to insert into input                */
    uint8_t     tmpl_type;   /* mp_tmpl_type_t; 0 = insert text directly   */
} menu_item_t;

typedef enum {
    MENU_TAB_SYMBOLS = 0,
    MENU_TAB_RECENTS,
    MENU_TAB_SETTINGS,
    MENU_TAB_BOOKMARKS,  /* saved expression bookmarks */
    MENU_TAB_FUNCS,      /* user-defined function manager */
    MENU_TAB_COUNT
} menu_tab_t;

/* ── Key event ───────────────────────────────────────────── */
typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,         /* printable char in evt.ch              */
    KEY_STR,          /* string to insert (e.g. "sin(")        */
    KEY_ENTER,
    KEY_CLEAR,
    KEY_DEL,
    KEY_DEL_FORWARD,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_TAB_LEFT,
    KEY_TAB_RIGHT,
    KEY_MODE,
    KEY_HELP,
    KEY_MENU,
    KEY_QUIT,
    KEY_GRAPH_YEQU,   /* Y= key → graph equation entry          */
    KEY_GRAPH_WINDOW, /* WINDOW key → viewport settings          */
    KEY_GRAPH_ZOOM,   /* ZOOM key → zoom presets                 */
    KEY_GRAPH_TRACE,  /* TRACE key → trace mode                  */
    KEY_GRAPH_DRAW    /* GRAPH key → draw/refresh graph          */
} key_action_t;

typedef struct {
    key_action_t action;
    char         ch;
    const char  *str;    /* valid when action == KEY_STR */
} key_event_t;

typedef enum {
    UI_RESULT_ENTER_REPROCESS = 0,
    UI_RESULT_ENTER_ADVANCE_CHECKPOINT,
    UI_RESULT_ENTER_ADVANCE_STEP
} ui_result_enter_action_t;

/* ── UI lifecycle ────────────────────────────────────────── */
void ui_init(void);
void ui_shutdown(void);

/* ── Drawing ─────────────────────────────────────────────── */
void ui_draw_header(void);
void ui_draw_status(uint16_t nodes_used, uint16_t nodes_free);
void ui_draw_input(const input_buf_t *ib);
void ui_draw_result(const char *text);
void ui_draw_result_colored(const char *text);
void ui_draw_error(const char *msg);
void ui_draw_steps(int scroll_offset);
void ui_draw_settings(uint8_t cursor);
void ui_draw_help(uint8_t page);
void ui_draw_menu(uint8_t cursor, uint8_t page);
void ui_draw_main_menu(menu_tab_t tab, uint8_t symbols_page,
                       uint8_t cursor, uint8_t scroll_offset);
void ui_draw_option_menu(const char *title, const char *subtitle,
                         const char *const *options, uint8_t count,
                         uint8_t cursor, uint8_t scroll_offset);
void ui_draw_window_editor(ce_float_t xmin, ce_float_t xmax,
                           ce_float_t ymin, ce_float_t ymax,
                           uint8_t cursor);
void ui_draw_zoom_presets(uint8_t cursor);

/* ── Menu accessor functions (page 0 = recents, 1-11 = categories) ── */
#define MENU_PAGES 12  /* 1 recents + 11 categories */
#define ZOOM_PRESETS 7
const menu_item_t *ui_menu_get_page(uint8_t page);
uint8_t ui_menu_get_count(uint8_t page);
const char *ui_menu_tab_title(menu_tab_t tab);
/* Copies the item struct but borrows its label/insert_str pointers: both
 * strings must have static storage duration (menu page literals). */
void ui_menu_add_recent(const menu_item_t *item);
uint8_t ui_menu_recents_count(void);
void ui_draw_steps_reveal(int reveal_count);
void ui_draw_step_prompt(const char *next_op_desc, int ckpt_idx, int ckpt_total);
void ui_draw_steps_checkpoint(int ckpt_reveal);
void ui_draw_checkpoint_results(int ckpt_reveal);
void ui_draw_templates(uint8_t cursor, uint8_t page);
void ui_clear_work_area(void);
void ui_flip(void);

/* ── Templates ──────────────────────────────────────────── */
#define TEMPLATE_PAGES 5
uint8_t ui_template_page_count(uint8_t page);
const char *ui_template_get_insert(uint8_t page, uint8_t idx);
/* Returns cursor slot offset (position of first fill slot) for a template */
uint8_t ui_template_get_slot(uint8_t page, uint8_t idx);

/* ── Context-sensitive help ─────────────────────────────── */
const char *ui_context_help(const char *input_buf);
void ui_draw_context_help(const char *help_text);

/* ── First-run tutorial ─────────────────────────────────── */
void ui_draw_tutorial(uint8_t page);
#define TUTORIAL_PAGES 3

/* ── Example problems ───────────────────────────────────── */
#define EXAMPLE_PAGES 3
uint8_t ui_example_page_count(uint8_t page);
const char *ui_example_get_insert(uint8_t page, uint8_t idx);
void ui_draw_examples(uint8_t cursor, uint8_t page);
void ui_format_math_pretty(const char *input, char *out, int out_size);
ui_result_enter_action_t ui_result_enter_action(bool checkpoint_nav_active,
                                                bool step_nav_active,
                                                bool show_steps,
                                                bool steps_enabled);

/* ── Bookmark tab ────────────────────────────────────────── */
void ui_draw_bookmarks(uint8_t cursor, uint8_t scroll);
uint8_t ui_bookmarks_count(void);

/* ── Function manager tab ───────────────────────────────── */
void ui_draw_funcmanager(uint8_t cursor, uint8_t scroll);
uint8_t ui_funcs_count(void);

/* ── MathPrint 2D rendering ─────────────────────────────── */
/* Draw result using 2D MathPrint layout from an AST node.   */
void ui_draw_result_ast(const ast_node_t *node);

/* ── Input ───────────────────────────────────────────────── */
key_event_t ui_poll_key(void);

/* ── Input-buffer helpers ────────────────────────────────── */
void input_buf_init(input_buf_t *ib);
bool input_buf_insert(input_buf_t *ib, char c);
bool input_buf_insert_str(input_buf_t *ib, const char *s);
bool input_buf_backspace(input_buf_t *ib);
bool input_buf_delete(input_buf_t *ib);
void input_buf_clear(input_buf_t *ib);

#endif /* UI_H */
