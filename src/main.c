/*
 * main.c — CalcEngine entry point
 *
 * TI-84 Plus CE application.
 * Main loop: type expression → ENTER → tokenize → parse → evaluate → display.
 */
#include <tice.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "ast.h"
#include "parser.h"
#include "eval.h"
#include "deriv.h"
#include "integrate.h"
#include "limits.h"
#include "series.h"
#include "algebra.h"
#include "simplify.h"
#include "steps.h"
#include "stats.h"
#include "matrix.h"
#include "polar.h"
#include "ui.h"
#include "solver.h"
#include "history.h"
#include "macros.h"
#include "graph.h"
#include "mathprint.h"
#include "mathprint_template.h"
#include "split/disambig.h"
#include "dsolve.h"
#include "vector.h"
#include "laplace.h"
#include "units.h"
#include "userfunc.h"
#include "bookmark.h"
#include <fileioc.h>

/* ── Cancel check (hardware implementation) ────────────── */
#ifndef DESKTOP_BUILD
#include <keypadc.h>
bool check_cancel(void)
{
    /* Accept CLEAR (in-app cancel) or ON (hardware break) as a user-abort
     * signal.  TI-84 CE users expect ON to halt any long-running program;
     * without this, a non-convergent integral or a slow series expansion
     * can only be escaped by CLEAR or by waiting out the OS watchdog. */
    kb_Scan();
    if ((kb_Data[6] & kb_Clear) != 0) return true;
    if ((kb_Data[1] & kb_On)    != 0) return true;
    return false;
}
#endif

/* ── Progress indicator (hardware implementation) ────────── */
#ifndef DESKTOP_BUILD
#include <graphx.h>
#define CONTENT_MARGIN_X  6    /* must match ui.c layout constant */
static bool g_progress_shown = false;

void progress_start(void)
{
    g_progress_shown = false;
}

void progress_check(void)
{
    /* On hardware, show "Computing..." after first progress_check call.
     * Timer-based delay check is approximated by the caller only
     * calling this every 256+ iterations. */
    if (!g_progress_shown) {
        g_progress_shown = true;
        gfx_SetTextFGColor(COL_DKGRAY);
        gfx_SetTextBGColor(COL_WHITE);
        gfx_PrintStringXY("Computing...", CONTENT_MARGIN_X,
                           INPUT_Y - 14);
        gfx_SwapDraw();
    }
}
#else
/* Desktop stubs provided by tice_mock.c */
#endif

/* ── History persistence via AppVars ─────────────────────── */
#define HISTORY_APPVAR "CEHISTOR"

/* Forward declarations — defined below after other static state */
typedef struct {
    uint8_t count;
    history_entry_t entries[HISTORY_MAX_ENTRIES];
} history_store_t;

static void history_save(void)
{
    history_store_t store;
    int count;
    int i;
    ti_var_t slot = ti_Open(HISTORY_APPVAR, "w");
    if (!slot) return;

    memset(&store, 0, sizeof(store));
    count = history_count();
    if (count > HISTORY_MAX_ENTRIES) count = HISTORY_MAX_ENTRIES;
    store.count = (uint8_t)count;
    for (i = 0; i < count; i++) {
        const history_entry_t *entry = history_get(i);
        if (entry) store.entries[i] = *entry;
    }

    ti_Write(&store, sizeof(store), 1, slot);
    ti_Close(slot);
}

static void history_load(void)
{
    history_store_t store;
    int i;
    ti_var_t slot = ti_Open(HISTORY_APPVAR, "r");
    history_clear();
    if (!slot) return;
    if (ti_Read(&store, sizeof(store), 1, slot) != 1) goto fail;
    if (store.count > HISTORY_MAX_ENTRIES) goto fail;
    for (i = (int)store.count - 1; i >= 0; i--) {
        const history_entry_t *entry = &store.entries[i];
        if (!entry->input[0] && !entry->result[0]) continue;
        history_push(entry->input, entry->result,
                     entry->numeric_result, entry->has_numeric);
    }
    history_navigate_reset();
    ti_Close(slot);
    return;
fail:
    history_clear();
    ti_Close(slot);
}

/* ── Settings persistence via AppVars ─────────────────────── */
#define SETTINGS_APPVAR "CESETTNG"
#define SETTINGS_MAGIC  0xCE58  /* magic number for validation (bumped on struct change) */

static void settings_save(void)
{
    ti_var_t slot = ti_Open(SETTINGS_APPVAR, "w");
    uint16_t magic = SETTINGS_MAGIC;
    if (!slot) return;
    ti_Write(&magic, sizeof(magic), 1, slot);
    ti_Write(&g_settings, sizeof(g_settings), 1, slot);
    ti_Close(slot);
}

static void settings_load(void)
{
    ti_var_t slot = ti_Open(SETTINGS_APPVAR, "r");
    uint16_t magic = 0;
    settings_t tmp;
    if (!slot) return;
    if (ti_Read(&magic, sizeof(magic), 1, slot) != 1) goto fail;
    if (magic != SETTINGS_MAGIC) goto fail;
    if (ti_Read(&tmp, sizeof(tmp), 1, slot) != 1) goto fail;
    /* Validate loaded values */
    if (tmp.angle_mode > ANGLE_DEGREES) goto fail;
    if (tmp.precision < 1 || tmp.precision > 9) goto fail;
    if (tmp.complex_mode > COMPLEX_POLAR) goto fail;
    if (tmp.step_detail > STEP_COMPACT) goto fail;
    if (tmp.solve_mode > SOLVE_STEP_BY_STEP) goto fail;
    if (tmp.integ_method > METHOD_PARTIAL_FRAC) goto fail;
    /* show_method_choices is bool — any value is valid */
    g_settings = tmp;
    ti_Close(slot);
    return;
fail:
    ti_Close(slot);
}

/* ── Helper: find first variable in expression ─────────── */
static char find_var_in(const ast_node_t *e)
{
    char v;
    if (!e) return 0;
    switch (e->type) {
    case NODE_VARIABLE: return e->d.variable;
    case NODE_BINOP:
        v = find_var_in(e->d.binop.left);
        return v ? v : find_var_in(e->d.binop.right);
    case NODE_UNOP: return find_var_in(e->d.unop.operand);
    case NODE_FUNC: return find_var_in(e->d.func.arg);
    default: return 0;
    }
}

/* ── Global settings ─────────────────────────────────────── */
settings_t g_settings = { ANGLE_RADIANS, true, 6, COMPLEX_OFF, false, STEP_ALL,
                          SOLVE_FULL_AUTO, METHOD_AUTO, false, false };

/* ── Previous answer ─────────────────────────────────────── */
ce_float_t g_last_answer = 0;

/* ── Static state (avoid stack overflow on 4 KB stack) ──── */
static tokenizer_t tokenizer;
static parser_t    parser;
static eval_ctx_t  eval_ctx;
static input_buf_t input;
static app_state_t state;
static char        result_buf[MAX_RESULT_LEN];
static char        error_buf[128];
static int         scroll_pos;       /* step-list scroll offset */
static bool        show_steps;       /* true when deriv steps visible */
static bool        in_help;          /* help screen active */
static uint8_t     help_page;        /* help page (0-2)   */
static bool        in_menu;          /* main menu active */
static bool        in_option_menu;   /* method/decision prompt active */
static menu_tab_t  menu_tab;         /* active menu tab */
static uint8_t     menu_page;        /* symbol category page (1-7) */
static uint8_t     menu_cursor;      /* main menu cursor */
static uint8_t     menu_scroll;      /* scrolled list offset */
static uint8_t     option_cursor;    /* option menu cursor */
static uint8_t     option_scroll;    /* option menu scroll offset */
static uint8_t     option_count;     /* option menu item count */
static solve_method_t option_methods[MAX_METHOD_OPTIONS];
static const char *option_labels[MAX_METHOD_OPTIONS];
static solve_method_t option_recommended;
static int         step_reveal;      /* step navigation: steps revealed so far */
static bool        step_nav_active;  /* step-by-step navigation mode active */
static int         ckpt_reveal;      /* checkpoint navigation: checkpoints revealed */
static bool        ckpt_nav_active;  /* checkpoint-based navigation mode active */
static bool        in_templates;     /* template browser active */
static uint8_t     tmpl_cursor;      /* template cursor */
static uint8_t     tmpl_page;        /* template page */
static bool        in_tutorial;      /* first-run tutorial active */
static uint8_t     tutorial_page;    /* tutorial page (0-2)      */
static bool        in_examples;      /* example browser active   */
static uint8_t     ex_cursor;        /* example cursor */
static uint8_t     ex_page;          /* example page */
static bool        first_run;        /* true on first launch     */
static bool        in_graph;         /* graph mode active        */
static graph_viewport_t graph_vp;    /* current graph viewport   */
static ast_node_t *graph_expr;       /* expression being graphed */
static char        graph_var;        /* variable name            */
static int         trace_sx;         /* trace cursor screen x    */
static bool        trace_active;     /* trace mode enabled       */
static ast_node_t *graph_expr2;       /* second expression (grapheq) */
static bool        graph_eq_mode;    /* equation graph mode      */
static graph_intersection_t graph_isects[GRAPH_MAX_INTERSECTIONS];
static int         graph_isect_count; /* number of intersections  */
static bool        graph_shade_on;   /* shading enabled          */
static ce_float_t  graph_shade_a;    /* shade region left bound  */
static ce_float_t  graph_shade_b;    /* shade region right bound */
static bool        graph_sf_mode;    /* slope field mode active  */
static char        graph_sf_yvar;    /* slope field y variable   */
static bool        graph_eq_entry;   /* Y= equation entry mode   */
static bool        in_window_edit;   /* WINDOW viewport editor   */
static uint8_t     window_cursor;    /* viewport field cursor    */
static bool        in_zoom_menu;     /* ZOOM presets menu        */
static uint8_t     zoom_cursor;      /* zoom preset cursor       */
/* ── MathPrint visual input template ──────────────────────── */
static mp_template_state_t math_template; /* active template state    */
static bool        in_math_template; /* template input mode      */
/* ── Smart disambiguation dialog ──────────────────────────── */
static bool        in_disambig_menu; /* disambig dialog active   */
static ce_disambig_option_t disambig_opts[DISAMBIG_MAX_OPTS];
static uint8_t     disambig_count;   /* number of options        */
static uint8_t     disambig_cursor;  /* selected option          */
static char        disambig_var;     /* variable from root AST   */
/* ── Step-by-step nav: prevent immediate re-run after nav ends */
static bool        result_post_nav;  /* nav just ended, stay on result */
/* ── Segment pause mode ────────────────────────────────────── */
static bool        seg_pause_active; /* segment-pause nav active       */
static int         seg_pause_idx;    /* current checkpoint index (0-based) */
/* ── Undo stack (8 states, menu-accessible only) ─────────── */
#define UNDO_STACK_SIZE 8
static char        undo_stack[UNDO_STACK_SIZE][MAX_INPUT_LEN];
static uint8_t     undo_head  = 0;   /* index of next write slot       */
static uint8_t     undo_count = 0;   /* number of valid snapshots       */

static void undo_push(void)
{
    strncpy(undo_stack[undo_head], input.buf, MAX_INPUT_LEN - 1);
    undo_stack[undo_head][MAX_INPUT_LEN - 1] = '\0';
    undo_head = (undo_head + 1) % UNDO_STACK_SIZE;
    if (undo_count < UNDO_STACK_SIZE) undo_count++;
}

static bool undo_pop(void)
{
    if (undo_count == 0) return false;
    undo_head = (undo_head + UNDO_STACK_SIZE - 1) % UNDO_STACK_SIZE;
    undo_count--;
    strncpy(input.buf, undo_stack[undo_head], MAX_INPUT_LEN - 1);
    input.buf[MAX_INPUT_LEN - 1] = '\0';
    input.len    = (uint8_t)strlen(input.buf);
    input.cursor = input.len;
    return true;
}

#define SETTINGS_COUNT 10
#define HELP_PAGES     3
#define WINDOW_FIELDS  4
#define OPTION_VISIBLE_ROWS 4
#define WINDOW_STEP_X  1.0f
#define WINDOW_STEP_Y  0.5f
#define MAIN_LOOP_IDLE_MS 10U

/* ── Answer format cycling ──────────────────────────────── */
typedef enum {
    FMT_DECIMAL = 0,  /* default: %.6g          */
    FMT_FRACTION,     /* a/b via Stern-Brocot    */
    FMT_SCI,          /* scientific: 1.23e+04    */
    FMT_COUNT         /* sentinel (cycle modulo) */
} result_format_t;
static result_format_t fmt_mode;           /* current display format  */
static ce_float_t      last_numeric;       /* last numeric answer     */
static bool            has_numeric_result; /* true if last_numeric valid */

/* ── Expression/result history navigation ─────────────────── */
static char hist_saved[MAX_INPUT_LEN];

/* Graph/session reset helpers */
static void exit_graph_mode(void)
{
    in_graph = false;
    if (graph_expr) { ast_free(graph_expr); graph_expr = NULL; }
    if (graph_expr2) { ast_free(graph_expr2); graph_expr2 = NULL; }
    graph_eq_mode = false;
    graph_sf_mode = false;
    graph_isect_count = 0;
    trace_active = false;
    graph_shade_on = false;
    graph_eq_entry = false;
    in_window_edit = false;
    in_zoom_menu = false;
}

/* Reformat the stored numeric result according to fmt_mode */
static void reformat_result(void)
{
    if (!has_numeric_result) return;

    switch (fmt_mode) {
    case FMT_FRACTION: {
        char frac_buf[64];
        if (algebra_dec_to_frac(last_numeric, 10000, frac_buf, sizeof(frac_buf))) {
            snprintf(result_buf, sizeof(result_buf), "= %s", frac_buf);
        } else {
            /* Fraction not found — show decimal with note */
            snprintf(result_buf, sizeof(result_buf), "= %.6g  (no exact fraction)",
                     (double)last_numeric);
        }
        break;
    }
    case FMT_SCI:
        snprintf(result_buf, sizeof(result_buf), "= %e", (double)last_numeric);
        break;
    default: /* FMT_DECIMAL */
        snprintf(result_buf, sizeof(result_buf), "= %.6g", (double)last_numeric);
        break;
    }
}

static const char *current_result_display(void)
{
    if (state != STATE_RESULT || !show_steps ||
        g_settings.solve_mode != SOLVE_STEP_BY_STEP) {
        return result_buf;
    }

    if (ckpt_nav_active) {
        if (ckpt_reveal <= 0) return input.buf;
        {
            const char *checkpoint_result = steps_get_checkpoint_result(ckpt_reveal - 1);
            if (checkpoint_result && checkpoint_result[0]) return checkpoint_result;
        }
    }

    if (step_nav_active) {
        if (step_reveal <= 0) return input.buf;
        {
            const char *step_line = steps_get(step_reveal - 1);
            if (step_line && step_line[0]) return step_line;
        }
    }

    return result_buf;
}

static void history_reset_browse(void)
{
    history_navigate_reset();
    hist_saved[0] = '\0';
}

static bool history_restore_input_text(const char *text)
{
    if (!text) return false;
    input_buf_clear(&input);
    return input_buf_insert_str(&input, text);
}

static bool history_restore_reentry(int index)
{
    char recall[MAX_INPUT_LEN];

    if (index < 0) {
        return history_restore_input_text(hist_saved);
    }
    if (!history_get_reentry_text(index, recall, sizeof(recall))) return false;
    return history_restore_input_text(recall);
}

static const char *history_capture_text(void)
{
    const char *display = current_result_display();

    if ((!display || !display[0] || strcmp(display, input.buf) == 0) &&
        result_buf[0] != '\0') {
        display = result_buf;
    }
    return display;
}

static void history_capture_active_result(void)
{
    const char *display;
    const history_entry_t *last;
    bool store_numeric;

    if (state != STATE_RESULT || input.buf[0] == '\0') return;

    display = history_capture_text();
    if (!display || display[0] == '\0') return;

    last = history_get(0);
    if (last &&
        strcmp(last->input, input.buf) == 0 &&
        strcmp(last->result, display) == 0) {
        return;
    }

    store_numeric = has_numeric_result && result_buf[0] != '\0' &&
                    strcmp(display, result_buf) == 0;
    history_push(input.buf, display,
                 store_numeric ? last_numeric : 0.0f,
                 store_numeric);
    history_reset_browse();
}

/* ── process_input handler functions ─────────────────────── */

static bool is_method_menu_strategy(solve_method_t method)
{
    switch (method) {
        case SOLVE_METHOD_INTEGRATE_USUB:
        case SOLVE_METHOD_INTEGRATE_PARTS:
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC:
            return true;
        default:
            return false;
    }
}

static integ_method_t solve_method_to_setting(solve_method_t method)
{
    switch (method) {
        case SOLVE_METHOD_INTEGRATE_USUB:         return METHOD_USUB;
        case SOLVE_METHOD_INTEGRATE_PARTS:        return METHOD_PARTS;
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:      return METHOD_TRIGSUB;
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC: return METHOD_PARTIAL_FRAC;
        default:                                  return METHOD_AUTO;
    }
}

static bool build_method_option_menu(const ast_node_t *root)
{
    method_report_t report;
    int i;
    int applicable_count = 0;

    if (!root || root->type != NODE_INTEGRAL) return false;
    if (g_settings.solve_mode != SOLVE_STEP_BY_STEP) return false;
    if (g_settings.integ_method != METHOD_AUTO) return false;

    solver_get_methods(root, &report);
    option_count = 0;
    option_recommended = report.recommended;
    option_methods[option_count] = SOLVE_METHOD_AUTO;
    option_labels[option_count] = solver_method_name(SOLVE_METHOD_AUTO);
    option_count++;

    for (i = 0; i < report.count && option_count < MAX_METHOD_OPTIONS; i++) {
        solve_method_t method = report.options[i].method;
        if (!report.options[i].applicable) continue;
        if (!is_method_menu_strategy(method)) continue;
        option_methods[option_count] = method;
        option_labels[option_count] = report.options[i].name;
        option_count++;
        applicable_count++;
    }

    if (applicable_count <= 0) {
        option_count = 0;
        return false;
    }

    option_cursor = 0;
    option_scroll = 0;
    in_option_menu = true;
    return true;
}

static bool maybe_open_method_option_menu(void)
{
    ast_node_t *root;
    tokenizer_t local_tokenizer;
    parser_t local_parser;
    char expr_input[MAX_INPUT_LEN];
    ce_error_t err;
    char *eq;
    bool opened = false;

    if (input.len == 0) return false;
    if (g_settings.solve_mode != SOLVE_STEP_BY_STEP ||
        g_settings.integ_method != METHOD_AUTO) {
        return false;
    }

    strncpy(expr_input, input.buf, sizeof(expr_input) - 1);
    expr_input[sizeof(expr_input) - 1] = '\0';
    eq = strchr(expr_input, '=');
    if (eq) *eq = '-';

    mem_free_all();
    steps_clear();
    eval_init(&eval_ctx);

    tok_init(&local_tokenizer, expr_input);
    err = tok_tokenize(&local_tokenizer);
    if (err == CE_OK) {
        parser_init(&local_parser, &local_tokenizer);
        root = parser_parse(&local_parser);
        if (root) {
            opened = build_method_option_menu(root);
            ast_free(root);
        }
    }

    mem_free_all();
    steps_clear();
    eval_init(&eval_ctx);
    return opened;
}

static void handle_deriv(ast_node_t *root)
{
    ast_node_t *deriv_result;
    char var = root->d.deriv.var;
    const ast_node_t *inner = root->d.deriv.expr;
    char ast_str[128];

    steps_add_checkpoint("Differentiate expression");
    deriv_result = deriv_diff(inner, var);
    if (!deriv_result) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Cannot differentiate this expression");
        ast_free(root);
        return;
    }

    ast_to_str(deriv_result, ast_str, sizeof(ast_str));
    steps_set_checkpoint_resultf("d/d%c = %s", var, ast_str);

    steps_add_checkpoint("Simplify result");
    snprintf(result_buf, sizeof(result_buf), "d/d%c = %s", var, ast_str);
    steps_set_checkpoint_result(result_buf);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(deriv_result);
    ast_free(root);
}

static void handle_tangent(ast_node_t *root)
{
    char var = root->d.tangent.var;
    const ast_node_t *inner = root->d.tangent.expr;
    ast_node_t *deriv_result;
    ce_float_t a_val, fa, slope, intercept;
    char ast_str[128];

    steps_add_checkpoint("Evaluate function at point");
    eval_init(&eval_ctx);
    a_val = eval_expr(&eval_ctx, root->d.tangent.point);
    if (eval_error(&eval_ctx) != CE_OK) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Cannot evaluate the point value");
        ast_free(root);
        return;
    }

    eval_init(&eval_ctx);
    eval_set_var(&eval_ctx, var, a_val);
    fa = eval_expr(&eval_ctx, inner);
    if (eval_error(&eval_ctx) != CE_OK) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Cannot evaluate f(a)");
        ast_free(root);
        return;
    }
    steps_addf("f(%.4g) = %.6g", (double)a_val, (double)fa);
    steps_set_checkpoint_resultf("f(%.4g) = %.6g", (double)a_val, (double)fa);

    steps_add_checkpoint("Compute derivative");
    {
        int sc = steps_count();
        deriv_result = deriv_diff_quiet(inner, var);
        steps_restore(sc);
    }
    if (!deriv_result) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Cannot differentiate this function");
        ast_free(root);
        return;
    }

    ast_to_str(deriv_result, ast_str, sizeof(ast_str));
    steps_addf("f'(%c) = %s", var, ast_str);
    steps_set_checkpoint_resultf("f'(%c) = %s", var, ast_str);

    steps_add_checkpoint("Evaluate slope at point");
    eval_init(&eval_ctx);
    eval_set_var(&eval_ctx, var, a_val);
    slope = eval_expr(&eval_ctx, deriv_result);
    ast_free(deriv_result);
    if (eval_error(&eval_ctx) != CE_OK) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Cannot evaluate the derivative at point");
        ast_free(root);
        return;
    }
    steps_addf("f'(%.4g) = %.6g (slope)", (double)a_val, (double)slope);

    intercept = fa - slope * a_val;
    steps_addf("b = f(a) - m*a = %.6g", (double)intercept);

    if (fabsf(intercept) < 1e-6f)
        snprintf(result_buf, sizeof(result_buf),
                 "y = %.6g*%c", (double)slope, var);
    else if (intercept > 0)
        snprintf(result_buf, sizeof(result_buf),
                 "y = %.6g*%c + %.6g", (double)slope, var, (double)intercept);
    else
        snprintf(result_buf, sizeof(result_buf),
                 "y = %.6g*%c - %.6g", (double)slope, var, (double)(-intercept));
    steps_set_checkpoint_result(result_buf);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(root);
}

static void handle_integral(ast_node_t *root)
{
    char var = root->d.integral.var;
    const ast_node_t *inner = root->d.integral.expr;
    char ast_str[128];
    const char *antideriv_desc = "Find antiderivative";

    switch (g_settings.integ_method) {
    case METHOD_USUB:         antideriv_desc = "Apply u-substitution"; break;
    case METHOD_PARTS:        antideriv_desc = "Apply integration by parts"; break;
    case METHOD_TRIGSUB:      antideriv_desc = "Apply trig substitution"; break;
    case METHOD_PARTIAL_FRAC: antideriv_desc = "Rewrite as partial fractions"; break;
    default: break;
    }

    if (root->d.integral.lo && root->d.integral.hi) {
        steps_add_checkpoint("Evaluate bounds");
        ce_float_t lo_val, hi_val, result_val;
        ce_error_t ierr;
        char antideriv_str[128];

        eval_init(&eval_ctx);
        lo_val = eval_expr(&eval_ctx, root->d.integral.lo);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "Eval: %s (lower bound)",
                    eval_error_msg(&eval_ctx));
            ast_free(root);
            return;
        }
        hi_val = eval_expr(&eval_ctx, root->d.integral.hi);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "Eval: %s (upper bound)",
                    eval_error_msg(&eval_ctx));
            ast_free(root);
            return;
        }
        steps_set_checkpoint_resultf("%c in [%.6g, %.6g]",
                                     var, (double)lo_val, (double)hi_val);

        steps_add_checkpoint(antideriv_desc);
        result_val = integrate_definite(inner, var,
            lo_val, hi_val, antideriv_str,
            (int)sizeof(antideriv_str), &ierr);

        if (ierr != CE_OK) {
            ce_float_t num_val;
            eval_init(&eval_ctx);
            num_val = eval_expr(&eval_ctx, root);
            if (ierr == CE_ERR_OVERFLOW || ierr == CE_ERR_PRECISION ||
                eval_error(&eval_ctx) != CE_OK ||
                !ce_isfinite(num_val)) {
                /* An overflowing or singular F at a bound makes a Simpson
                 * value untrustworthy too (see split/processor.c). */
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf), "Integral: %s",
                        (ierr == CE_ERR_OVERFLOW || ierr == CE_ERR_PRECISION)
                                                ? "could not decide" :
                        eval_error(&eval_ctx) != CE_OK ? eval_error_msg(&eval_ctx)
                                                       : "result overflow");
                ast_free(root);
                return;
            }
            g_last_answer = num_val;
            last_numeric = num_val;
            has_numeric_result = true;
            snprintf(result_buf, sizeof(result_buf), "= %.6g  (numeric)",
                    (double)num_val);
            steps_set_checkpoint_result(result_buf);
            show_steps = false;
        } else {
            g_last_answer = result_val;
            last_numeric = result_val;
            has_numeric_result = true;
            steps_set_checkpoint_resultf("F(%c) = %s", var, antideriv_str);
            steps_add_checkpoint("Evaluate definite integral bounds");
            snprintf(result_buf, sizeof(result_buf), "F(%c) = %s\n= %.6g",
                    var, antideriv_str, (double)result_val);
            steps_set_checkpoint_resultf("= %.6g", (double)result_val);
            show_steps = true;
            scroll_pos = 0;
        }

        state = STATE_RESULT;
        ast_free(root);
        return;
    }

    /* Indefinite integral */
    {
        ast_node_t *integ_result;
        steps_add_checkpoint(antideriv_desc);
        /* Apply forced method if set in settings */
        if (g_settings.integ_method != METHOD_AUTO) {
            integ_strategy_t strat = INTEG_STRATEGY_AUTO;
            switch (g_settings.integ_method) {
            case METHOD_USUB:         strat = INTEG_STRATEGY_USUB; break;
            case METHOD_PARTS:        strat = INTEG_STRATEGY_PARTS; break;
            case METHOD_TRIGSUB:      strat = INTEG_STRATEGY_TRIGSUB; break;
            case METHOD_PARTIAL_FRAC: strat = INTEG_STRATEGY_PARTIAL_FRAC; break;
            default: break;
            }
            integ_result = integrate_with_strategy(inner, var, strat, NULL);
        } else {
            integ_result = integrate(inner, var);
        }
        if (!integ_result) {
            state = STATE_ERROR;
            if (g_settings.integ_method != METHOD_AUTO) {
                snprintf(error_buf, sizeof(error_buf),
                        "Method failed: %s\nTry Auto or another method",
                        integrate_last_failure_reason());
            } else {
                snprintf(error_buf, sizeof(error_buf),
                        "Cannot find symbolic antiderivative: %s",
                        integrate_last_failure_reason());
            }
            ast_free(root);
            return;
        }

        ast_to_str(integ_result, ast_str, sizeof(ast_str));
        snprintf(result_buf, sizeof(result_buf), "int = %s + C", ast_str);
        steps_set_checkpoint_result(result_buf);

        show_steps = true;
        scroll_pos = 0;
        state = STATE_RESULT;
        ast_free(integ_result);
        ast_free(root);
    }
}

static void handle_limit(ast_node_t *root)
{
    char var = root->d.limit.var;
    const ast_node_t *inner = root->d.limit.expr;
    const ast_node_t *point = root->d.limit.point;
    int8_t dir = root->d.limit.direction;
    ce_float_t lim_val;
    lim_status_t ls;

    if (!point) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Limit needs a point (e.g. lim(f(x),x,0))");
        ast_free(root);
        return;
    }

    steps_add_checkpoint("Evaluate limit");
    ls = compute_limit(inner, var, point, dir, &lim_val);

    switch (ls) {
    case LIM_OK:
        g_last_answer = lim_val;
        last_numeric = lim_val;
        has_numeric_result = true;
        snprintf(result_buf, sizeof(result_buf), "= %.6g", (double)lim_val);
        break;
    case LIM_POS_INF:
        snprintf(result_buf, sizeof(result_buf), "= +inf");
        break;
    case LIM_NEG_INF:
        snprintf(result_buf, sizeof(result_buf), "= -inf");
        break;
    case LIM_DNE:
        snprintf(result_buf, sizeof(result_buf), "= DNE (does not exist)");
        break;
    default:
        {
            ce_float_t num_val;
            eval_init(&eval_ctx);
            num_val = eval_expr(&eval_ctx, root);
            if (eval_error(&eval_ctx) != CE_OK) {
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf),
                         "Could not evaluate limit");
                ast_free(root);
                return;
            }
            g_last_answer = num_val;
            last_numeric = num_val;
            has_numeric_result = true;
            snprintf(result_buf, sizeof(result_buf), "= %.6g  (numeric)",
                    (double)num_val);
        }
        break;
    }

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    steps_set_checkpoint_result(result_buf);
    ast_free(root);
}

static void handle_taylor(ast_node_t *root)
{
    char var = root->d.taylor.var;
    ce_float_t center = 0;
    int deg = 5;
    ast_node_t *result_ast;
    char ast_str[128];

    if (root->d.taylor.center) {
        eval_init(&eval_ctx);
        center = eval_expr(&eval_ctx, root->d.taylor.center);
        if (eval_error(&eval_ctx) != CE_OK) center = 0;
    }
    if (root->d.taylor.degree) {
        eval_init(&eval_ctx);
        deg = (int)eval_expr(&eval_ctx, root->d.taylor.degree);
        if (eval_error(&eval_ctx) != CE_OK) deg = 5;
    }

    steps_add_checkpoint("Compute Taylor coefficients");
    result_ast = taylor_series(root->d.taylor.expr, var, center, deg);
    if (!result_ast) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Could not compute Taylor series");
        ast_free(root);
        return;
    }

    ast_to_str(result_ast, ast_str, sizeof(ast_str));
    snprintf(result_buf, sizeof(result_buf), "T%d = %s", deg, ast_str);
    steps_set_checkpoint_result(result_buf);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(result_ast);
    ast_free(root);
}

static void handle_fourier(ast_node_t *root)
{
    char var = root->d.fourier.var;
    int n_terms = 5;
    ast_node_t *result_ast;
    char ast_str[128];

    if (root->d.fourier.nterms) {
        eval_init(&eval_ctx);
        n_terms = (int)eval_expr(&eval_ctx, root->d.fourier.nterms);
        if (eval_error(&eval_ctx) != CE_OK) n_terms = 5;
    }

    result_ast = fourier_series(root->d.fourier.expr, var, n_terms);
    if (!result_ast) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Could not compute Fourier series");
        ast_free(root);
        return;
    }

    ast_to_str(result_ast, ast_str, sizeof(ast_str));
    snprintf(result_buf, sizeof(result_buf), "F%d = %s", n_terms, ast_str);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(result_ast);
    ast_free(root);
}

/* Returns true if handled (infinite-bound convergence wizard ran). */
static bool handle_sum(ast_node_t *root)
{
    ce_float_t val;
    conv_info_t ci;

    eval_init(&eval_ctx);
    val = eval_expr(&eval_ctx, root->d.sum.hi);
    if (eval_error(&eval_ctx) != CE_OK || !ce_isinf(val) || val <= 0)
        return false;   /* not infinite upper bound — fall through */

    ci = series_converge(root->d.sum.expr, root->d.sum.var);
    switch (ci.result) {
    case CONV_CONVERGES:
        if (ci.has_sum)
            snprintf(result_buf, sizeof(result_buf), "CONVERGES (%s)\nSum ~ %.6g",
                    ci.test_name, (double)ci.sum_estimate);
        else
            snprintf(result_buf, sizeof(result_buf), "CONVERGES (%s)",
                    ci.test_name);
        break;
    case CONV_DIVERGES:
        snprintf(result_buf, sizeof(result_buf), "DIVERGES (%s)", ci.test_name);
        break;
    default:
        if (ci.has_sum)
            snprintf(result_buf, sizeof(result_buf),
                    "INCONCLUSIVE\nPartial sum ~ %.6g",
                    (double)ci.sum_estimate);
        else
            snprintf(result_buf, sizeof(result_buf), "INCONCLUSIVE");
        break;
    }

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(root);
    return true;
}

static void handle_expand(ast_node_t *root)
{
    char var = root->d.expand.var;
    ast_node_t *result_ast;
    char ast_str[128];

    steps_add_checkpoint("Expand expression");
    result_ast = algebra_expand(root->d.expand.expr, var);
    if (!result_ast) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Could not expand expression");
        ast_free(root);
        return;
    }

    ast_to_str(result_ast, ast_str, sizeof(ast_str));
    snprintf(result_buf, sizeof(result_buf), "= %s", ast_str);
    steps_set_checkpoint_result(result_buf);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(result_ast);
    ast_free(root);
}

static void handle_factor(ast_node_t *root)
{
    char var = root->d.factor.var;
    ast_node_t *result_ast;
    char ast_str[128];

    steps_add_checkpoint("Factor expression");
    result_ast = algebra_factor(root->d.factor.expr, var);
    if (!result_ast) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                 "Could not factor expression");
        ast_free(root);
        return;
    }

    ast_to_str(result_ast, ast_str, sizeof(ast_str));
    snprintf(result_buf, sizeof(result_buf), "= %s", ast_str);
    steps_set_checkpoint_result(result_buf);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(result_ast);
    ast_free(root);
}

static void handle_solve(ast_node_t *root)
{
    char var = root->d.solve.var;

    steps_add_checkpoint("Solve for variable");
    algebra_solve(root->d.solve.expr, var,
                  result_buf, MAX_RESULT_LEN);
    steps_set_checkpoint_result(result_buf);

    show_steps = true;
    scroll_pos = 0;
    state = STATE_RESULT;
    ast_free(root);
}

static void handle_stat(ast_node_t *root)
{
    ce_float_t val;

    /* linreg gets special formatting */
    if (root->d.stat.fn == STAT_LINREG) {
        static ce_float_t sbuf[MAX_STAT_DATA];
        int count = 0;
        const ast_node_t *pair = root->d.stat.args;
        ce_float_t m, b, r;
        ce_error_t serr;

        eval_init(&eval_ctx);
        while (pair && pair->type == NODE_PAIR && count < MAX_STAT_DATA) {
            sbuf[count] = eval_expr(&eval_ctx, pair->d.pair.head);
            if (eval_error(&eval_ctx) != CE_OK) {
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf), "Eval: %s",
                        eval_error_msg(&eval_ctx));
                ast_free(root);
                return;
            }
            count++;
            pair = pair->d.pair.tail;
        }
        serr = stat_linreg(sbuf, count, &m, &b, &r);
        if (serr != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                    "linreg: need >= 2 (x,y) pairs");
            ast_free(root);
            return;
        }
        snprintf(result_buf, sizeof(result_buf),
                "y = %.4gx + %.4g\nr = %.4g  r^2 = %.4g",
                (double)m, (double)b, (double)r, (double)(r * r));
        show_steps = false;
        state = STATE_RESULT;
        ast_free(root);
        return;
    }

    /* Single-value stat functions */
    eval_init(&eval_ctx);
    val = eval_expr(&eval_ctx, root);
    if (eval_error(&eval_ctx) != CE_OK) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf), "Stat: %s",
                eval_error_msg(&eval_ctx));
        ast_free(root);
        return;
    }
    g_last_answer = val;
    last_numeric = val;
    has_numeric_result = true;
    snprintf(result_buf, sizeof(result_buf), "%s = %.6g",
            stat_fn_name(root->d.stat.fn), (double)val);
    show_steps = false;
    state = STATE_RESULT;
    ast_free(root);
}

static void handle_polar(ast_node_t *root)
{
    polar_op_t pop = root->d.polar.op;

    /* ── Polar to rectangular ──────────────────── */
    if (pop == POLOP_TO_RECT) {
        ce_float_t r_val, theta_val, xr, yr;
        eval_init(&eval_ctx);
        r_val = eval_expr(&eval_ctx, root->d.polar.expr1);
        theta_val = eval_expr(&eval_ctx, root->d.polar.expr2);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "Eval error");
            ast_free(root); return;
        }
        polar_to_rect(r_val, theta_val, &xr, &yr);
        steps_addf("r = %.6g, theta = %.6g", (double)r_val, (double)theta_val);
        steps_addf("x = r*cos(theta) = %.6g", (double)xr);
        steps_addf("y = r*sin(theta) = %.6g", (double)yr);
        snprintf(result_buf, sizeof(result_buf),
                 "x = %.6g\ny = %.6g", (double)xr, (double)yr);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Rectangular to polar ──────────────────── */
    if (pop == POLOP_FROM_RECT) {
        ce_float_t x_val, y_val, rp, tp;
        eval_init(&eval_ctx);
        x_val = eval_expr(&eval_ctx, root->d.polar.expr1);
        y_val = eval_expr(&eval_ctx, root->d.polar.expr2);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "Eval error");
            ast_free(root); return;
        }
        rect_to_polar(x_val, y_val, &rp, &tp);
        steps_addf("x = %.6g, y = %.6g", (double)x_val, (double)y_val);
        steps_addf("r = sqrt(x^2+y^2) = %.6g", (double)rp);
        steps_addf("theta = atan2(y,x) = %.6g", (double)tp);
        snprintf(result_buf, sizeof(result_buf),
                 "r = %.6g\ntheta = %.6g", (double)rp, (double)tp);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Related rates (chain rule w.r.t. time) ────── */
    if (pop == POLOP_RELRATE) {
        const ast_node_t *expr = root->d.polar.expr1;
        const char *vars = "xyzrswuvab";
        char found[10];
        int nfound = 0, i;
        int rlen;

        for (i = 0; vars[i] && nfound < 10; i++) {
            if (ast_contains_var(expr, vars[i]))
                found[nfound++] = vars[i];
        }

        if (nfound == 0) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "No variables found");
            ast_free(root); return;
        }

        steps_add("Related rates: d/dt of expression");
        steps_add("Chain rule: sum of df/dv * dv/dt");

        snprintf(result_buf, sizeof(result_buf), "d/dt = ");
        rlen = (int)strlen(result_buf);

        for (i = 0; i < nfound; i++) {
            ast_node_t *partial;
            char partial_str[64];
            int sc = steps_count();

            partial = deriv_diff_quiet(expr, found[i]);
            steps_restore(sc);

            if (partial) {
                ast_to_str(partial, partial_str, sizeof(partial_str));
                if (i > 0 && rlen + 3 < MAX_RESULT_LEN) {
                    rlen += snprintf(result_buf + rlen,
                            sizeof(result_buf) - rlen, " + ");
                }
                rlen += snprintf(result_buf + rlen,
                        sizeof(result_buf) - rlen,
                        "(%s)*d%c/dt", partial_str, found[i]);
                steps_addf("df/d%c = %s", found[i], partial_str);
                ast_free(partial);
            }
        }

        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Complete the square ─────────────────────────── */
    if (pop == POLOP_CSQUARE) {
        char var = root->d.polar.var;
        int ok = algebra_complete_square(root->d.polar.expr1, var,
                                          result_buf, sizeof(result_buf));
        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "csquare: not a quadratic");
            ast_free(root); return;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Partial fractions ──────────────────────────── */
    if (pop == POLOP_PFRAC) {
        char var = root->d.polar.var;
        ast_node_t *pf = algebra_partial_fractions(root->d.polar.expr1, var,
                                                    result_buf, sizeof(result_buf));
        if (!pf) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "pfrac: decomposition failed");
            ast_free(root); return;
        }
        ast_free(pf);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Descartes' Rule of Signs ──────────────────── */
    if (pop == POLOP_DESCARTES) {
        char var = root->d.polar.var;
        int ok = algebra_descartes(root->d.polar.expr1, var,
                                    result_buf, sizeof(result_buf));
        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "descartes: not a polynomial");
            ast_free(root); return;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Line from two points ──────────────────────── */
    if (pop == POLOP_LINE2PT) {
        ce_float_t x1v, y1v, x2v, y2v;
        eval_init(&eval_ctx);
        x1v = eval_expr(&eval_ctx, root->d.polar.expr1);
        y1v = eval_expr(&eval_ctx, root->d.polar.expr2);
        x2v = eval_expr(&eval_ctx, root->d.polar.lo);
        y2v = eval_expr(&eval_ctx, root->d.polar.hi);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "line: eval error");
            ast_free(root); return;
        }
        if (!algebra_line_two_points(x1v, y1v, x2v, y2v,
                                      result_buf, sizeof(result_buf))) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "line: duplicate points");
            ast_free(root); return;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Line from slope + point ───────────────────── */
    if (pop == POLOP_LINEPT) {
        ce_float_t mv, x1v, y1v;
        eval_init(&eval_ctx);
        mv  = eval_expr(&eval_ctx, root->d.polar.expr1);
        x1v = eval_expr(&eval_ctx, root->d.polar.expr2);
        y1v = eval_expr(&eval_ctx, root->d.polar.lo);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "linept: eval error");
            ast_free(root); return;
        }
        algebra_line_slope_point(mv, x1v, y1v,
                                  result_buf, sizeof(result_buf));
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Parabola from vertex + point ──────────────── */
    if (pop == POLOP_PARABOLA) {
        ce_float_t vx, vy, px, py;
        eval_init(&eval_ctx);
        vx = eval_expr(&eval_ctx, root->d.polar.expr1);
        vy = eval_expr(&eval_ctx, root->d.polar.expr2);
        px = eval_expr(&eval_ctx, root->d.polar.lo);
        py = eval_expr(&eval_ctx, root->d.polar.hi);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "parabola: eval error");
            ast_free(root); return;
        }
        if (!algebra_parabola_vertex(vx, vy, px, py,
                                      result_buf, sizeof(result_buf))) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "parabola: vertex = point x");
            ast_free(root); return;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Continuity analysis ───────────────────────── */
    if (pop == POLOP_CONTINUITY) {
        char var = root->d.polar.var;
        continuity_t ct;
        if (!root->d.polar.expr2) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "continuity: need point");
            ast_free(root); return;
        }
        ct = limits_continuity(root->d.polar.expr1, var, root->d.polar.expr2);
        switch (ct) {
        case CONT_CONTINUOUS: snprintf(result_buf, sizeof(result_buf), "Continuous"); break;
        case CONT_REMOVABLE:  snprintf(result_buf, sizeof(result_buf), "Removable discontinuity"); break;
        case CONT_JUMP:       snprintf(result_buf, sizeof(result_buf), "Jump discontinuity"); break;
        case CONT_INFINITE:   snprintf(result_buf, sizeof(result_buf), "Infinite discontinuity"); break;
        case CONT_ESSENTIAL:  snprintf(result_buf, sizeof(result_buf), "Essential discontinuity"); break;
        default:              snprintf(result_buf, sizeof(result_buf), "Continuity: could not determine"); break;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Decimal to fraction ───────────────────────── */
    if (pop == POLOP_TOFRAC) {
        eval_init(&eval_ctx);
        ce_float_t val = eval_expr(&eval_ctx, root->d.polar.expr1);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "tofrac: eval error");
            ast_free(root); return;
        }
        if (!algebra_dec_to_frac(val, 10000, result_buf, sizeof(result_buf))) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "tofrac: no simple fraction");
            ast_free(root); return;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Recursive sequence ─────────────────────────── */
    if (pop == POLOP_RECSEQ) {
        ce_float_t init_val, terms[201];
        int k_val, n_terms, i;
        eval_init(&eval_ctx);
        init_val = eval_expr(&eval_ctx, root->d.polar.lo);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "recseq: bad init value");
            ast_free(root); return;
        }
        eval_init(&eval_ctx);
        k_val = (int)eval_expr(&eval_ctx, root->d.polar.hi);
        if (eval_error(&eval_ctx) != CE_OK || k_val < 0) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "recseq: bad k value");
            ast_free(root); return;
        }
        if (k_val > 200) k_val = 200;
        n_terms = series_recseq(root->d.polar.expr1, root->d.polar.var,
                                 init_val, k_val, terms, k_val + 1);
        if (n_terms <= 0) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "recseq: computation failed");
            ast_free(root); return;
        }
        {
            int pos = 0;
            pos += snprintf(result_buf + pos, sizeof(result_buf) - pos,
                            "s(%d) = %.6g", n_terms - 1, (double)terms[n_terms - 1]);
            if (n_terms >= 2 && pos < (int)sizeof(result_buf) - 20) {
                pos += snprintf(result_buf + pos, sizeof(result_buf) - pos,
                                "\nTerms: ");
                for (i = 0; i < n_terms && i < 8 && pos < (int)sizeof(result_buf) - 12; i++) {
                    if (i > 0) pos += snprintf(result_buf + pos, sizeof(result_buf) - pos, ", ");
                    pos += snprintf(result_buf + pos, sizeof(result_buf) - pos, "%.4g", (double)terms[i]);
                }
                if (n_terms > 8)
                    snprintf(result_buf + pos, sizeof(result_buf) - pos, ", ...");
            }
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Unit circle lookup ────────────────────────── */
    if (pop == POLOP_UNITCIRC) {
        eval_init(&eval_ctx);
        ce_float_t angle = eval_expr(&eval_ctx, root->d.polar.expr1);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "unitcirc: eval error");
            ast_free(root); return;
        }
        if (!algebra_unit_circle(angle, result_buf, sizeof(result_buf))) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s", result_buf);
            ast_free(root); return;
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Nonlinear system solving ─────────────────────── */
    if (pop == POLOP_NLSOLVE) {
        const ast_node_t *eqs[2];
        char vars[3];
        eqs[0] = root->d.polar.expr1;
        eqs[1] = root->d.polar.expr2;
        vars[0] = root->d.polar.var;
        vars[1] = root->d.polar.lo ?
                   (char)(int)root->d.polar.lo->d.number : 'y';
        vars[2] = '\0';
        algebra_solve_nonlinear(eqs, vars,
                                 result_buf, sizeof(result_buf));
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Arithmetic/geometric series sum ────────────── */
    if (pop == POLOP_ARITHSUM || pop == POLOP_GEOMSUM) {
        eval_init(&eval_ctx);
        ce_float_t val = eval_expr(&eval_ctx, root);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s",
                     eval_error_msg(&eval_ctx));
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf),
                 "%s = %.6g",
                 pop == POLOP_ARITHSUM ? "Arithmetic sum" : "Geometric sum",
                 (double)val);
        g_last_answer = val; has_numeric_result = true;
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Coordinate geometry: midpoint, distance, slope ── */
    if (pop == POLOP_MIDPOINT) {
        ce_float_t x1v, y1v, x2v, y2v, mx, my;
        eval_init(&eval_ctx);
        x1v = eval_expr(&eval_ctx, root->d.polar.expr1);
        y1v = eval_expr(&eval_ctx, root->d.polar.expr2);
        x2v = eval_expr(&eval_ctx, root->d.polar.lo);
        y2v = eval_expr(&eval_ctx, root->d.polar.hi);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "midpoint: eval error");
            ast_free(root); return;
        }
        mx = (x1v + x2v) / 2.0f;
        my = (y1v + y2v) / 2.0f;
        steps_addf("Midpoint: ((%.6g+%.6g)/2, (%.6g+%.6g)/2)",
                   (double)x1v, (double)x2v, (double)y1v, (double)y2v);
        snprintf(result_buf, sizeof(result_buf),
                 "Midpoint = (%.6g, %.6g)", (double)mx, (double)my);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }
    if (pop == POLOP_DISTANCE || pop == POLOP_SLOPE) {
        eval_init(&eval_ctx);
        ce_float_t val = eval_expr(&eval_ctx, root);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s",
                     eval_error_msg(&eval_ctx));
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf),
                 "%s = %.6g",
                 pop == POLOP_DISTANCE ? "Distance" : "Slope",
                 (double)val);
        g_last_answer = val; has_numeric_result = true;
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Function graphing ──────────────────────────── */
    if (pop == POLOP_GRAPH) {
        /* Set up graph viewport */
        graph_default_viewport(&graph_vp);
        /* Use custom x range if provided */
        if (root->d.polar.lo && root->d.polar.hi) {
            eval_init(&eval_ctx);
            graph_vp.x_min = eval_expr(&eval_ctx, root->d.polar.lo);
            eval_init(&eval_ctx);
            graph_vp.x_max = eval_expr(&eval_ctx, root->d.polar.hi);
            if (graph_vp.x_min >= graph_vp.x_max) {
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf),
                         "graph: xmin must be less than xmax");
                ast_free(root); return;
            }
        }
        /* Clone expression and enter graph mode */
        graph_expr = ast_clone(root->d.polar.expr1);
        if (!graph_expr) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "graph: not enough memory for expression");
            ast_free(root); return;
        }
        graph_var = root->d.polar.var ? root->d.polar.var : 'x';
        graph_auto_range(&graph_vp, graph_expr, graph_var);
        in_graph = true;
        trace_active = false;
        trace_sx = GRAPH_X0 + GRAPH_W / 2;
        graph_shade_on = false;
        graph_eq_mode = false;
        graph_expr2 = NULL;
        graph_isect_count = 0;
        ast_free(root);
        return;
    }

    /* ── Equation graphing ─────────────────────────────── */
    if (pop == POLOP_GRAPHEQ) {
        graph_default_viewport(&graph_vp);
        if (root->d.polar.lo && root->d.polar.hi) {
            eval_init(&eval_ctx);
            graph_vp.x_min = eval_expr(&eval_ctx, root->d.polar.lo);
            eval_init(&eval_ctx);
            graph_vp.x_max = eval_expr(&eval_ctx, root->d.polar.hi);
            if (graph_vp.x_min >= graph_vp.x_max) {
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf),
                         "grapheq: xmin must be less than xmax");
                ast_free(root); return;
            }
        }
        graph_expr = ast_clone(root->d.polar.expr1);
        if (!graph_expr) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "grapheq: not enough memory");
            ast_free(root); return;
        }
        graph_expr2 = ast_clone(root->d.polar.expr2);
        if (!graph_expr2) {
            ast_free(graph_expr); graph_expr = NULL;
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "grapheq: not enough memory");
            ast_free(root); return;
        }
        graph_var = root->d.polar.var ? root->d.polar.var : 'x';
        graph_auto_range_two(&graph_vp, graph_expr, graph_expr2,
                             graph_var);
        graph_isect_count = graph_find_intersections(
            &graph_vp, graph_expr, graph_expr2, graph_var,
            graph_isects, GRAPH_MAX_INTERSECTIONS);
        in_graph = true;
        graph_eq_mode = true;
        trace_active = false;
        trace_sx = GRAPH_X0 + GRAPH_W / 2;
        graph_shade_on = false;
        ast_free(root);
        return;
    }

    /* ── ODE solvers ───────────────────────────────────────── */
    if (pop == POLOP_ODE_SEP || pop == POLOP_ODE_LINEAR || pop == POLOP_DIFFEQ) {
        char xvar = root->d.polar.var ? root->d.polar.var : 'x';
        char yvar = (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                  ? root->d.polar.expr2->d.variable : 'y';
        bool ok = false;
        char sol[256];
        sol[0] = '\0';

        if (xvar == yvar) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "ODE: independent and dependent variables must differ");
            ast_free(root);
            return;
        }

        if (pop == POLOP_ODE_SEP) {
            ok = ode_solve_separable(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
        } else if (pop == POLOP_ODE_LINEAR) {
            ok = ode_solve_linear(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
        } else {
            ok = ode_solve_separable(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
            if (!ok)
                ok = ode_solve_linear(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));
            if (!ok && !sol[0]) {
                strncpy(sol, "Unsolvable by current ODE methods (separable/linear)",
                        sizeof(sol) - 1);
                sol[sizeof(sol) - 1] = '\0';
            }
        }

        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s",
                     sol[0] ? sol : "Unsolvable ODE by current methods");
            ast_free(root); return;
        }

        snprintf(result_buf, sizeof(result_buf), "%s", sol);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Forced integration strategy helpers ──────────────── */
    if (pop == POLOP_INTPARTS || pop == POLOP_TRIGSUB || pop == POLOP_USUB) {
        integ_strategy_t strategy = INTEG_STRATEGY_AUTO;
        const ast_node_t *u_hint = NULL;
        ast_node_t *anti;
        char abuf[160];
        char var = root->d.polar.var ? root->d.polar.var : 'x';

        if (pop == POLOP_INTPARTS) strategy = INTEG_STRATEGY_PARTS;
        else if (pop == POLOP_TRIGSUB) strategy = INTEG_STRATEGY_TRIGSUB;
        else { strategy = INTEG_STRATEGY_USUB; u_hint = root->d.polar.lo; }

        anti = integrate_with_strategy(root->d.polar.expr1, var, strategy, u_hint);
        if (!anti) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s",
                     integrate_last_failure_reason());
            ast_free(root); return;
        }

        ast_to_str(anti, abuf, sizeof(abuf));
        snprintf(result_buf, sizeof(result_buf), "int = %s + C", abuf);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT;
        ast_free(anti);
        ast_free(root);
        return;
    }

    if (pop == POLOP_INTSTRATEGY) {
        char var = root->d.polar.var ? root->d.polar.var : 'x';
        integrate_strategy_report(root->d.polar.expr1, var,
                                  result_buf, (int)sizeof(result_buf));
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Numerical ODE helpers ─────────────────────────────── */
    if (pop == POLOP_EULER) {
        ce_float_t val;
        eval_init(&eval_ctx);
        val = eval_expr(&eval_ctx, root);
        if (eval_error(&eval_ctx) != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s",
                     eval_error_msg(&eval_ctx));
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf), "Euler y(xf) ~= %.6g", (double)val);
        g_last_answer = val;
        last_numeric = val;
        has_numeric_result = true;
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    if (pop == POLOP_SLOPEFIELD) {
        char xv = root->d.polar.var ? root->d.polar.var : 'x';
        char yv = (root->d.polar.expr2 &&
                   root->d.polar.expr2->type == NODE_VARIABLE)
                  ? root->d.polar.expr2->d.variable : 'y';

        /* Set up graph viewport from bounds or defaults */
        graph_vp.x_min = -5.0f; graph_vp.x_max = 5.0f;
        graph_vp.y_min = -5.0f; graph_vp.y_max = 5.0f;
        if (root->d.polar.lo && root->d.polar.hi) {
            if (root->d.polar.lo->type == NODE_PAIR &&
                root->d.polar.hi->type == NODE_PAIR) {
                eval_init(&eval_ctx);
                graph_vp.x_min = eval_expr(&eval_ctx, root->d.polar.lo->d.pair.head);
                graph_vp.x_max = eval_expr(&eval_ctx, root->d.polar.lo->d.pair.tail);
                graph_vp.y_min = eval_expr(&eval_ctx, root->d.polar.hi->d.pair.head);
                graph_vp.y_max = eval_expr(&eval_ctx, root->d.polar.hi->d.pair.tail);
            }
        }
        if (graph_vp.x_min >= graph_vp.x_max ||
            graph_vp.y_min >= graph_vp.y_max) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "slopefield: invalid window bounds");
            ast_free(root); return;
        }

        /* Clone the RHS expression for graph mode rendering */
        graph_expr = ast_clone(root->d.polar.expr1);
        if (!graph_expr) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "slopefield: not enough memory");
            ast_free(root); return;
        }
        graph_var = xv;
        graph_sf_yvar = yv;
        graph_sf_mode = true;
        graph_eq_mode = false;
        graph_expr2 = NULL;
        graph_isect_count = 0;
        in_graph = true;
        trace_active = false;
        trace_sx = GRAPH_X0 + GRAPH_W / 2;
        graph_shade_on = false;

        /* Also run the numerical evaluation for steps */
        eval_init(&eval_ctx);
        eval_expr(&eval_ctx, root);

        ast_free(root);
        return;
    }

    /* ── dsolve(eq, y, x) — auto ODE solver (sep → linear → exact) ── */
    if (pop == POLOP_DSOLVE) {
        char xvar = root->d.polar.var ? root->d.polar.var : 'x';
        char yvar = (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                  ? root->d.polar.expr2->d.variable : 'y';
        char sol[256];
        bool ok = false;
        sol[0] = '\0';

        if (xvar == yvar) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                     "dsolve: independent and dependent variables must differ");
            ast_free(root); return;
        }

        ok = dsolve_auto(root->d.polar.expr1, xvar, yvar, sol, sizeof(sol));

        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s",
                     sol[0] ? sol : "dsolve: no closed-form solution found");
            ast_free(root); return;
        }

        snprintf(result_buf, sizeof(result_buf), "%s", sol);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── grad(f, x, y) — gradient vector (symbolic) ── */
    if (pop == POLOP_VEC_GRAD) {
        char v1 = root->d.polar.var ? root->d.polar.var : 'x';
        char v2 = (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                  ? root->d.polar.expr2->d.variable : 'y';
        char ebuf1[64], ebuf2[64];
        bool ok = vector_gradient(root->d.polar.expr1, v1, v2,
                                  ebuf1, sizeof(ebuf1), ebuf2, sizeof(ebuf2));
        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "grad: differentiation failed");
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf), "<%s, %s>", ebuf1, ebuf2);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── cross(a1,a2,a3, b1,b2,b3) — 3D cross product (symbolic) ── */
    if (pop == POLOP_VEC_CROSS) {
        char rbuf[128];
        bool ok = vector_cross(root->d.polar.expr1, rbuf, sizeof(rbuf));
        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "cross: need exactly 6 args (u1,u2,u3,v1,v2,v3)");
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf), "%s", rbuf);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── curl(Fx, Fy, Fz, x, y, z) — 3D curl (symbolic) ── */
    if (pop == POLOP_VEC_CURL) {
        char v1 = root->d.polar.var ? root->d.polar.var : 'x';
        /* Unpack: expr2=PAIR(Fy,Fz), lo=PAIR(vy_node,vz_node) */
        if (!root->d.polar.expr2 || root->d.polar.expr2->type != NODE_PAIR ||
            !root->d.polar.lo    || root->d.polar.lo->type != NODE_PAIR) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "curl: invalid argument encoding");
            ast_free(root); return;
        }
        {
            ast_node_t *Fx  = root->d.polar.expr1;
            ast_node_t *Fy  = root->d.polar.expr2->d.pair.head;
            ast_node_t *Fz  = root->d.polar.expr2->d.pair.tail;
            char v2 = (root->d.polar.lo->d.pair.head->type == NODE_VARIABLE)
                      ? root->d.polar.lo->d.pair.head->d.variable : 'y';
            char v3 = (root->d.polar.lo->d.pair.tail->type == NODE_VARIABLE)
                      ? root->d.polar.lo->d.pair.tail->d.variable : 'z';
            char rbuf[256];
            bool ok = vector_curl(Fx, Fy, Fz, v1, v2, v3, rbuf, sizeof(rbuf));
            if (!ok) {
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf), "curl: computation failed");
                ast_free(root); return;
            }
            snprintf(result_buf, sizeof(result_buf), "%s", rbuf);
        }
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── laplace(f, t, s) — forward Laplace transform ── */
    if (pop == POLOP_LAPLACE) {
        char t_var = root->d.polar.var ? root->d.polar.var : 't';
        char s_var = (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                   ? root->d.polar.expr2->d.variable : 's';
        char rbuf[256];
        bool ok = laplace_forward(root->d.polar.expr1, t_var, s_var, rbuf, sizeof(rbuf));
        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "laplace: no table match found");
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf), "L{f} = %s", rbuf);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── ilaplace(F, s, t) — inverse Laplace transform ── */
    if (pop == POLOP_ILAPLACE) {
        char s_var = root->d.polar.var ? root->d.polar.var : 's';
        char t_var = (root->d.polar.expr2 && root->d.polar.expr2->type == NODE_VARIABLE)
                   ? root->d.polar.expr2->d.variable : 't';
        char rbuf[256];
        bool ok = laplace_inverse(root->d.polar.expr1, s_var, t_var, rbuf, sizeof(rbuf));
        if (!ok) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "ilaplace: no table match found");
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf), "L^-1{F} = %s", rbuf);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }

    /* ── Arc length, surface area, disk, washer, shell ─ */
    {
        ce_float_t a_val = 0, b_val = 1, result_val;
        ce_error_t perr = CE_OK;
        const char *label = "";

        if (root->d.polar.lo) {
            eval_init(&eval_ctx);
            a_val = eval_expr(&eval_ctx, root->d.polar.lo);
        }
        if (root->d.polar.hi) {
            eval_init(&eval_ctx);
            b_val = eval_expr(&eval_ctx, root->d.polar.hi);
        }

        switch (pop) {
        case POLOP_ARCLEN:
            label = "Arc length";
            result_val = polar_arclen(root->d.polar.expr1,
                root->d.polar.var, a_val, b_val, &perr);
            break;
        case POLOP_ARCLEN_PARAM:
            label = "Parametric arc length";
            result_val = polar_arclen_param(root->d.polar.expr1,
                root->d.polar.expr2, root->d.polar.var,
                a_val, b_val, &perr);
            break;
        case POLOP_SURFREV:
            label = "Surface area";
            result_val = polar_surfrev(root->d.polar.expr1,
                root->d.polar.var, a_val, b_val, &perr);
            break;
        case POLOP_DISK:
            label = "Volume (disk)";
            result_val = polar_disk(root->d.polar.expr1,
                root->d.polar.var, a_val, b_val, &perr);
            break;
        case POLOP_WASHER:
            label = "Volume (washer)";
            result_val = polar_washer(root->d.polar.expr1,
                root->d.polar.expr2, root->d.polar.var,
                a_val, b_val, &perr);
            break;
        case POLOP_SHELL:
            label = "Volume (shell)";
            result_val = polar_shell(root->d.polar.expr1,
                root->d.polar.var, a_val, b_val, &perr);
            break;
        default:
            perr = CE_ERR_INTERNAL;
            result_val = 0;
            label = "?";
            break;
        }

        if (perr != CE_OK) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf), "%s: computation failed", label);
            ast_free(root); return;
        }
        snprintf(result_buf, sizeof(result_buf),
                 "%s = %.6g", label, (double)result_val);
        show_steps = true; scroll_pos = 0;
        state = STATE_RESULT; ast_free(root); return;
    }
}

static void handle_matrix(ast_node_t *root)
{
    static ce_matrix_t mat_a, mat_result;  /* static: save 4KB stack */
    ast_node_t *inner = root;
    matrix_op_t mop;

    /* Find the operation and inner matrix literal */
    if (root->d.matrix_op.op != MAT_LITERAL) {
        mop = root->d.matrix_op.op;
        inner = root->d.matrix_op.arg;
    } else {
        mop = MAT_LITERAL;
    }

    /* Extract matrix data from inner PAIR chain */
    if (!inner || inner->type != NODE_MATRIX_OP ||
        inner->d.matrix_op.op != MAT_LITERAL) {
        state = STATE_ERROR;
        snprintf(error_buf, sizeof(error_buf),
                "Expected mat(...) argument");
        ast_free(root);
        return;
    }
    {
        const ast_node_t *pair = inner->d.matrix_op.arg;
        int idx = 0;
        matrix_init(&mat_a, inner->d.matrix_op.rows,
                            inner->d.matrix_op.cols);
        eval_init(&eval_ctx);
        while (pair && pair->type == NODE_PAIR &&
               idx < mat_a.rows * mat_a.cols) {
            mat_a.data[idx] = eval_expr(&eval_ctx, pair->d.pair.head);
            if (eval_error(&eval_ctx) != CE_OK) {
                state = STATE_ERROR;
                snprintf(error_buf, sizeof(error_buf), "Eval: %s",
                        eval_error_msg(&eval_ctx));
                ast_free(root);
                return;
            }
            idx++;
            pair = pair->d.pair.tail;
        }
    }

    if (mop == MAT_LITERAL) {
        matrix_to_str(&mat_a, result_buf, sizeof(result_buf));
    } else if (mop == MAT_DET) {
        ce_float_t d = matrix_det(&mat_a);
        g_last_answer = d;
        last_numeric = d;
        has_numeric_result = true;
        snprintf(result_buf, sizeof(result_buf), "det = %.6g", (double)d);
    } else if (mop == MAT_TRANSPOSE) {
        matrix_transpose(&mat_a, &mat_result);
        matrix_to_str(&mat_result, result_buf, sizeof(result_buf));
    } else if (mop == MAT_INV) {
        if (!matrix_inverse(&mat_a, &mat_result)) {
            state = STATE_ERROR;
            snprintf(error_buf, sizeof(error_buf),
                 "Matrix is singular (no inverse)");
            ast_free(root);
            return;
        }
        matrix_to_str(&mat_result, result_buf, sizeof(result_buf));
    } else if (mop == MAT_RREF) {
        mat_result = mat_a;  /* copy */
        matrix_rref(&mat_result);
        matrix_to_str(&mat_result, result_buf, sizeof(result_buf));
    }

    show_steps = false;
    state = STATE_RESULT;
    ast_free(root);
}

/* ── Process the current input: tokenize → parse → eval ─── */
static void process_input(void)
{
    ast_node_t *root;
    ce_error_t err;
    ce_float_t val;
    char       ast_str[128];
    char       expanded_input[MAX_INPUT_LEN];
    const char *expr_input;
    bool       input_had_equals = false;
    macro_cmd_status_t mcmd;

    if (input.len == 0) return;

    /* Reset answer format state */
    fmt_mode = FMT_DECIMAL;
    has_numeric_result = false;

    /* Macro management commands: macro(...), unmacro(...), macros() */
    mcmd = macros_try_command(input.buf,
                              result_buf, sizeof(result_buf),
                              error_buf, sizeof(error_buf));
    if (mcmd == MACRO_CMD_OK) {
        show_steps = false;
        state = STATE_RESULT;
        return;
    }
    if (mcmd == MACRO_CMD_ERROR) {
        state = STATE_ERROR;
        return;
    }

    /* User-defined function commands: define, undef, funcs, clearfuncs, ws */
    {
        userfunc_cmd_t ucmd = userfunc_try_command(input.buf,
                                                   result_buf, sizeof(result_buf),
                                                   error_buf, sizeof(error_buf));
        if (ucmd == UFUNC_CMD_OK) {
            show_steps = false;
            state = STATE_RESULT;
            return;
        }
        if (ucmd == UFUNC_CMD_ERROR) {
            state = STATE_ERROR;
            return;
        }
    }

    /* Bookmark commands: bookmark("name"), delbm("name"), clearbm() */
    {
        bookmark_cmd_t bcmd = bookmark_try_command(
            input.buf,
            input.buf,                /* current expression to bookmark */
            has_numeric_result ? result_buf : NULL,
            last_numeric, has_numeric_result,
            result_buf, sizeof(result_buf),
            error_buf, sizeof(error_buf));
        if (bcmd == BM_CMD_OK) {
            show_steps = false;
            state = STATE_RESULT;
            return;
        }
        if (bcmd == BM_CMD_ERROR) {
            state = STATE_ERROR;
            return;
        }
    }

    /* Unit conversion command: convert(value, from, to) */
    {
        const char *p = input.buf;
        while (*p == ' ') p++;
        if (strncasecmp(p, "convert", 7) == 0 && p[7] == '(') {
            /* Parse: convert(value_expr, from_str, to_str) */
            const char *q = p + 8;
            char from_str[24], to_str[24];
            char val_expr[64];
            int n = 0;
            /* Collect value expression (up to first comma) */
            while (*q && *q != ',' && n < (int)sizeof(val_expr) - 1)
                val_expr[n++] = *q++;
            val_expr[n] = '\0';
            if (*q == ',') q++;
            /* Skip whitespace and optional quote */
            while (*q == ' ') q++;
            if (*q == '"' || *q == '\'') q++;
            n = 0;
            while (*q && *q != '"' && *q != '\'' && *q != ',' && *q != ')' &&
                   n < (int)sizeof(from_str) - 1)
                from_str[n++] = *q++;
            from_str[n] = '\0';
            if (*q == '"' || *q == '\'') q++;
            if (*q == ',') q++;
            while (*q == ' ') q++;
            if (*q == '"' || *q == '\'') q++;
            n = 0;
            while (*q && *q != '"' && *q != '\'' && *q != ')' &&
                   n < (int)sizeof(to_str) - 1)
                to_str[n++] = *q++;
            to_str[n] = '\0';

            if (from_str[0] && to_str[0]) {
                /* Evaluate the value expression numerically */
                ce_float_t val_num = 0;
                bool val_ok = false;
                /* Simple numeric parse (covers most cases) */
                {
                    char *end;
                    val_num = (ce_float_t)strtod(val_expr, &end);
                    if (*end == '\0' || *end == ' ') val_ok = true;
                }
                if (!val_ok) {
                    /* Try full eval pipeline */
                    tokenizer_t tmp_tok;
                    parser_t    tmp_par;
                    ast_node_t *tmp_root;
                    mem_free_all();
                    tok_init(&tmp_tok, val_expr);
                    if (tok_tokenize(&tmp_tok) == CE_OK) {
                        parser_init(&tmp_par, &tmp_tok);
                        tmp_root = parser_parse(&tmp_par);
                        if (tmp_root) {
                            eval_ctx_t tmp_ctx;
                            eval_init(&tmp_ctx);
                            val_num = eval_expr(&tmp_ctx, tmp_root);
                            if (eval_error(&tmp_ctx) == CE_OK) val_ok = true;
                            ast_free(tmp_root);
                        }
                    }
                }
                if (val_ok) {
                    ce_float_t res;
                    char rbuf[128];
                    if (units_convert(val_num, from_str, to_str, &res, rbuf, sizeof(rbuf))) {
                        snprintf(result_buf, sizeof(result_buf), "%s", rbuf);
                        g_last_answer = res;
                        last_numeric = res;
                        has_numeric_result = true;
                        show_steps = false;
                        state = STATE_RESULT;
                        return;
                    } else {
                        snprintf(error_buf, sizeof(error_buf),
                                 "convert: unknown unit '%s' or '%s'", from_str, to_str);
                        state = STATE_ERROR;
                        return;
                    }
                }
                snprintf(error_buf, sizeof(error_buf),
                         "convert: could not evaluate '%s'", val_expr);
                state = STATE_ERROR;
                return;
            }
        }
    }

    /* Expand macros before tokenize/parse */
    if (!macros_expand(input.buf, expanded_input, sizeof(expanded_input),
                       error_buf, sizeof(error_buf))) {
        state = STATE_ERROR;
        return;
    }
    expr_input = expanded_input;

    /* Expand user-defined functions (up to 4 passes) */
    {
        char uf_expanded[MAX_INPUT_LEN];
        if (userfunc_expand(expr_input, uf_expanded, sizeof(uf_expanded),
                            error_buf, sizeof(error_buf))) {
            /* userfunc_expand writes into uf_expanded — copy back to expanded_input */
            strncpy(expanded_input, uf_expanded, sizeof(expanded_input) - 1);
            expanded_input[sizeof(expanded_input) - 1] = '\0';
            expr_input = expanded_input;
        }
        /* If expansion fails (error_buf set), try to continue with original — non-fatal */
        error_buf[0] = '\0';
    }

    /* Check if expanded input contains '=' (equation detection for auto-solve) */
    {
        const char *p = expr_input;
        bool in_func = false;
        /* Skip '=' inside store arrows (->), inequality tokens, etc.
         * A bare '=' with expressions on both sides means equation. */
        while (*p) {
            if (*p == '(') in_func = true;
            if (*p == ')') in_func = false;
            if (*p == '=' && !in_func) { input_had_equals = true; break; }
            p++;
        }
    }

    /* Reset pool and steps for new expression */
    mem_free_all();
    steps_clear();
    eval_init(&eval_ctx);

    /* ── Tokenize ─────────────────────────────────────── */
    tok_init(&tokenizer, expr_input);
    err = tok_tokenize(&tokenizer);
    if (err != CE_OK) {
        state = STATE_ERROR;
        if (err == CE_ERR_OVERFLOW)
            snprintf(error_buf, sizeof(error_buf),
                     "Expression too long (max %d tokens)", MAX_TOKENS);
        else
            snprintf(error_buf, sizeof(error_buf),
                     "Check your input for typos or\nmissing operators");
        return;
    }

    /* ── Parse ────────────────────────────────────────── */
    parser_init(&parser, &tokenizer);
    root = parser_parse(&parser);
    if (!root) {
        state = STATE_ERROR;
        const char *pmsg = parser_error_msg(&parser);
        if (pmsg && pmsg[0])
            snprintf(error_buf, sizeof(error_buf), "%s", pmsg);
        else
            snprintf(error_buf, sizeof(error_buf),
                     "Could not understand expression");
        return;
    }

    /* ── Dispatch to handler based on node type ────── */
    if (root->type == NODE_DERIV) { handle_deriv(root); return; }
    if (root->type == NODE_TANGENT) { handle_tangent(root); return; }
    if (root->type == NODE_INTEGRAL) { handle_integral(root); return; }
    if (root->type == NODE_LIMIT) { handle_limit(root); return; }
    if (root->type == NODE_TAYLOR) { handle_taylor(root); return; }
    if (root->type == NODE_FOURIER) { handle_fourier(root); return; }
    if (root->type == NODE_SUM && root->d.sum.hi) {
        if (handle_sum(root)) return;
    }
    if (root->type == NODE_EXPAND) { handle_expand(root); return; }
    if (root->type == NODE_FACTOR) { handle_factor(root); return; }
    if (root->type == NODE_SOLVE) { handle_solve(root); return; }
    if (root->type == NODE_STAT) { handle_stat(root); return; }
    if (root->type == NODE_POLAR) { handle_polar(root); return; }
    if (root->type == NODE_MATRIX_OP) { handle_matrix(root); return; }

    /* ── "Just works" auto-solve: equation with = sign ── */
    if (input_had_equals && root->type != NODE_SOLVE) {
        char var = find_var_in(root);
        if (var) {
            /* Input had '=' and expression contains a variable:
             * treat as an equation to solve (expr = 0 after parser's
             * conversion of 'a = b' to 'a - b'). */
            algebra_solve(root, var, result_buf, MAX_RESULT_LEN);
            show_steps = true;
            scroll_pos = 0;
            state = STATE_RESULT;
            ast_free(root);
            return;
        }
    }

    /* ── Smart disambiguation ─────────────────────────── */
    {
        ce_disambig_option_t dopts[DISAMBIG_MAX_OPTS];
        int dcount = ce_disambig_detect(root, dopts, DISAMBIG_MAX_OPTS,
                                         g_settings.show_method_choices);
        if (dcount > 0) {
            /* Save options for the disambig dialog in the main loop */
            int di;
            disambig_count = (uint8_t)(dcount < DISAMBIG_MAX_OPTS ? dcount : DISAMBIG_MAX_OPTS);
            for (di = 0; di < disambig_count; di++)
                disambig_opts[di] = dopts[di];
            disambig_cursor = 0;
            /* Find the first variable for build_expr */
            {
                const char *vord = "xyztnkuvabcmswpqrfghjl";
                int vi;
                disambig_var = 0;
                for (vi = 0; vord[vi] && !disambig_var; vi++) {
                    if (ast_contains_var(root, vord[vi]))
                        disambig_var = vord[vi];
                }
            }
            /* Free root and open dialog (process_input returns, main loop
             * will show the disambiguation menu on next redraw)          */
            ast_free(root);
            in_disambig_menu = true;
            state = STATE_INPUT;  /* stay in input state until user picks */
            return;
        }
    }

    /* ── Polynomial division detection ───────────────── */
    if (root->type == NODE_BINOP && root->d.binop.op == OP_DIV) {
        char var = find_var_in(root);
        if (var) {
            char rem_buf[64];
            ast_node_t *quot = algebra_poly_div(
                root->d.binop.left, root->d.binop.right,
                var, rem_buf, sizeof(rem_buf));
            if (quot) {
                ast_to_str(quot, ast_str, sizeof(ast_str));
                if (strcmp(rem_buf, "0") == 0)
                    snprintf(result_buf, sizeof(result_buf), "= %s", ast_str);
                else
                    snprintf(result_buf, sizeof(result_buf), "Q = %s\nR = %s",
                            ast_str, rem_buf);
                show_steps = true;
                scroll_pos = 0;
                state = STATE_RESULT;
                ast_free(quot);
                ast_free(root);
                return;
            }
        }
    }

    /* ── Show parsed form ─────────────────────────────── */
    ast_to_str(root, ast_str, sizeof(ast_str));

    /* ── Evaluate ─────────────────────────────────────── */
    show_steps = false;
    val = eval_expr(&eval_ctx, root);
    if (eval_error(&eval_ctx) != CE_OK) {
        state = STATE_ERROR;
        const char *emsg = eval_error_msg(&eval_ctx);
        /* Translate common error codes to plain English */
        if (strstr(emsg, "Cancelled") || strstr(emsg, "cancel")) {
            /* User cancelled via CLEAR — return to input with expression intact */
            state = STATE_INPUT;
            ast_free(root);
            return;
        }
        if (strstr(emsg, "DIV_ZERO") || strstr(emsg, "div"))
            snprintf(error_buf, sizeof(error_buf), "Cannot divide by zero");
        else if (strstr(emsg, "DOMAIN") || strstr(emsg, "domain"))
            snprintf(error_buf, sizeof(error_buf),
                     "Value outside function domain\n(e.g. sqrt of negative)");
        else if (strstr(emsg, "OVERFLOW"))
            snprintf(error_buf, sizeof(error_buf), "Result too large");
        else
            snprintf(error_buf, sizeof(error_buf), "%s", emsg);
        ast_free(root);
        return;
    }

    /* ── Store previous answer ────────────────────────── */
    g_last_answer = val;
    last_numeric = val;
    has_numeric_result = true;

    /* ── Format result ────────────────────────────────── */
    snprintf(result_buf, sizeof(result_buf), "= %.6g", (double)val);

    /* If the AST string differs from input, show parsed form too */
    {
        int rlen = (int)strlen(result_buf);
        if (rlen + 20 < MAX_RESULT_LEN) {
            snprintf(result_buf + rlen, sizeof(result_buf) - rlen, "\nParsed: %s", ast_str);
        }
    }

    state = STATE_RESULT;
    ast_free(root);
}

/* ── Dirty-region tracking for partial redraws ───────────── */
typedef enum {
    DIRTY_NONE   = 0,
    DIRTY_INPUT  = (1 << 0),   /* input line changed      */
    DIRTY_WORK   = (1 << 1),   /* work area changed       */
    DIRTY_STATUS = (1 << 2),   /* status bar changed      */
    DIRTY_ALL    = 0xFF        /* full redraw needed       */
} dirty_t;
static dirty_t dirty = DIRTY_ALL;

/* ── Redraw the full screen ──────────────────────────────── */
static void redraw(void)
{
    /* Header only redrawn on full redraw (it never changes) */
    if (dirty & DIRTY_ALL)
        ui_draw_header();

    /* Work area: only clear+redraw if work content changed */
    if (dirty & (DIRTY_WORK | DIRTY_ALL))
        ui_clear_work_area();

    if (in_graph) {
        graph_draw_axes(&graph_vp);
        if (graph_sf_mode) {
            graph_draw_slopefield(&graph_vp, graph_expr,
                                   graph_var, graph_sf_yvar,
                                   21, 21, COL_BLUE);
        } else {
            graph_draw_function(&graph_vp, graph_expr, graph_var, COL_BLUE);
        }
        if (graph_eq_mode && graph_expr2) {
            graph_draw_function(&graph_vp, graph_expr2, graph_var,
                                COL_RED);
            if (graph_isect_count > 0)
                graph_draw_intersections(&graph_vp, graph_isects,
                                         graph_isect_count);
        }
        if (graph_shade_on)
            graph_draw_shade(&graph_vp, graph_expr, graph_var,
                             graph_shade_a, graph_shade_b, COL_LTGRAY);
        if (trace_active) {
            char trace_label[48];
            if (graph_eq_mode && graph_expr2) {
                graph_draw_trace_two(&graph_vp, graph_expr, graph_expr2,
                                     graph_var, trace_sx,
                                     trace_label, sizeof(trace_label));
            } else {
                graph_draw_trace(&graph_vp, graph_expr, graph_var,
                                 trace_sx, trace_label,
                                 sizeof(trace_label));
            }
            gfx_SetTextFGColor(COL_BLACK);
            gfx_SetTextBGColor(COL_WHITE);
            gfx_PrintStringXY(trace_label, 4,
                              GRAPH_Y0 + GRAPH_AREA_H - 10);
        }
        /* Viewport info at top */
        {
            char info[48];
            if (graph_eq_mode)
                snprintf(info, sizeof(info),
                         "[%.3g,%.3g] %d isect",
                         (double)graph_vp.x_min, (double)graph_vp.x_max,
                         graph_isect_count);
            else
                snprintf(info, sizeof(info),
                         "[%.3g,%.3g]x[%.3g,%.3g]",
                         (double)graph_vp.x_min, (double)graph_vp.x_max,
                         (double)graph_vp.y_min, (double)graph_vp.y_max);
            gfx_SetTextFGColor(COL_DKGRAY);
            gfx_PrintStringXY(info, 4, GRAPH_Y0 + 2);
        }
    } else if (in_window_edit) {
        ui_draw_window_editor(graph_vp.x_min, graph_vp.x_max,
                              graph_vp.y_min, graph_vp.y_max,
                              window_cursor);
    } else if (in_zoom_menu) {
        ui_draw_zoom_presets(zoom_cursor);
    } else if (in_tutorial) {
        ui_draw_tutorial(tutorial_page);
    } else if (in_help) {
        ui_draw_help(help_page);
    } else if (in_examples) {
        ui_draw_examples(ex_cursor, ex_page);
    } else if (in_templates) {
        ui_draw_templates(tmpl_cursor, tmpl_page);
    } else if (in_option_menu) {
        char subtitle[48];
        snprintf(subtitle, sizeof(subtitle), "Recommended: %s",
                 solver_method_name(option_recommended));
        ui_draw_option_menu("METHOD OPTIONS", subtitle,
                            option_labels, option_count,
                            option_cursor, option_scroll);
    } else if (in_disambig_menu) {
        /* Show disambiguation choices */
        const char *dlabels[DISAMBIG_MAX_OPTS];
        int di;
        for (di = 0; di < disambig_count; di++)
            dlabels[di] = disambig_opts[di].label;
        ui_draw_option_menu("WHAT WOULD YOU LIKE TO DO?",
                            input.buf,
                            dlabels, disambig_count,
                            disambig_cursor, 0);
    } else if (in_menu) {
        ui_draw_main_menu(menu_tab, menu_page, menu_cursor, menu_scroll);
    } else {
        switch (state) {
        case STATE_RESULT:
            /* Segment pause: show ONLY the current stage result, one at a time */
            if (seg_pause_active) {
                const char *seg_result = steps_get_checkpoint_result(seg_pause_idx);
                int total_ckpts = steps_checkpoint_count();
                char seg_header[64];
                snprintf(seg_header, sizeof(seg_header),
                         "Stage %d / %d", seg_pause_idx + 1, total_ckpts);
                ui_draw_result_colored(seg_result && seg_result[0] ? seg_result : result_buf);
                ui_draw_step_prompt("ENTER=next stage  CLEAR=all",
                                    seg_pause_idx, total_ckpts);
            } else {
                /* Render result based on show_steps + solve_mode combination:
                 * - Full Auto + Show Work ON:  result + all steps (scrollable)
                 * - Step-by-Step + Show Work ON: result + checkpoint-gated steps
                 * - Full Auto + Show Work OFF: result only
                 * - Step-by-Step + Show Work OFF: result + checkpoint results only */
                ui_draw_result_colored(current_result_display());
                if (show_steps && g_settings.show_steps) {
                    if (ckpt_nav_active)
                        ui_draw_steps_checkpoint(ckpt_reveal);
                    else if (step_nav_active)
                        ui_draw_steps_reveal(step_reveal);
                    else
                        ui_draw_steps(scroll_pos);
                } else if (show_steps && !g_settings.show_steps &&
                           ckpt_nav_active) {
                    ui_draw_checkpoint_results(ckpt_reveal);
                }
            }
            break;
        case STATE_ERROR:
            ui_draw_error(error_buf);
            break;
        default:
            /* Context-sensitive help: show hint while typing */
            if (state == STATE_INPUT && input.len > 0) {
                const char *ctx = ui_context_help(input.buf);
                if (ctx) ui_draw_context_help(ctx);
            }
            break;
        }
    }

    /* Input: template renders over input strip when active */
    if (in_math_template) {
        mp_tmpl_draw(&math_template);
    } else {
        ui_draw_input(&input);
    }

    /* Status bar: always redraw (battery can change) */
    ui_draw_status(mem_nodes_used(), mem_nodes_free());

    ui_flip();
    dirty = DIRTY_NONE;
}

/* ── Main ────────────────────────────────────────────────── */
int main(void)
{
    bool running = true;

    /* Initialise subsystems */
    mem_init();
    macros_init();
    userfunc_init();
    bookmark_init();
    ui_init();
    history_clear();
    hist_saved[0] = '\0';
    history_load();
    settings_load();
    userfunc_load();
    bookmark_load_file();
    input_buf_init(&input);
    eval_init(&eval_ctx);
    state = STATE_INPUT;
    scroll_pos      = 0;
    show_steps      = false;
    in_help         = false;
    help_page       = 0;
    in_menu         = false;
    in_option_menu  = false;
    menu_tab        = MENU_TAB_SYMBOLS;
    menu_page       = 1;
    menu_cursor     = 0;
    menu_scroll     = 0;
    option_cursor   = 0;
    option_scroll   = 0;
    option_count    = 0;
    option_recommended = SOLVE_METHOD_AUTO;
    step_reveal     = 0;
    step_nav_active = false;
    ckpt_reveal     = 0;
    ckpt_nav_active = false;
    in_templates    = false;
    tmpl_cursor     = 0;
    tmpl_page       = 0;
    in_tutorial     = false;
    tutorial_page   = 0;
    in_examples     = false;
    ex_cursor       = 0;
    ex_page         = 0;
    first_run       = true;     /* show tutorial on first launch */
    fmt_mode        = FMT_DECIMAL;
    has_numeric_result = false;
    last_numeric    = 0;
    in_graph        = false;
    graph_expr      = NULL;
    graph_expr2     = NULL;
    graph_eq_mode   = false;
    graph_sf_mode   = false;
    graph_isect_count = 0;
    trace_active    = false;
    graph_shade_on  = false;
    graph_eq_entry  = false;
    in_window_edit  = false;
    window_cursor   = 0;
    in_zoom_menu    = false;
    zoom_cursor     = 0;
    in_math_template = false;
    memset(&math_template, 0, sizeof(math_template));
    in_disambig_menu = false;
    disambig_count  = 0;
    disambig_cursor = 0;
    disambig_var    = 0;
    result_post_nav = false;
    seg_pause_active = false;
    seg_pause_idx   = 0;

    /* Show tutorial on first run */
    if (first_run) {
        in_tutorial = true;
        tutorial_page = 0;
    }

    /* Initial draw */
    redraw();

    /* ── Main loop ────────────────────────────────────── */
    while (running) {
        key_event_t evt = ui_poll_key();

        /* Default: full redraw. Typing keys override below. */
        dirty = DIRTY_ALL;

        /* ── Graph mode key handling ─────────────────── */
        if (in_graph && evt.action != KEY_NONE) {
            ce_float_t pan_x = (graph_vp.x_max - graph_vp.x_min) * 0.15f;
            ce_float_t pan_y = (graph_vp.y_max - graph_vp.y_min) * 0.15f;

            switch (evt.action) {
            case KEY_CLEAR:
                /* Exit graph mode */
                exit_graph_mode();
                state = STATE_INPUT;
                break;
            case KEY_LEFT:
                if (trace_active) {
                    if (trace_sx > GRAPH_X0) trace_sx -= 2;
                } else {
                    graph_pan(&graph_vp, -pan_x, 0);
                }
                break;
            case KEY_RIGHT:
                if (trace_active) {
                    if (trace_sx < GRAPH_X0 + GRAPH_W - 1) trace_sx += 2;
                } else {
                    graph_pan(&graph_vp, pan_x, 0);
                }
                break;
            case KEY_UP:
                if (!trace_active) graph_pan(&graph_vp, 0, pan_y);
                break;
            case KEY_DOWN:
                if (!trace_active) graph_pan(&graph_vp, 0, -pan_y);
                break;
            case KEY_CHAR:
                if (evt.ch == '+') graph_zoom(&graph_vp, 0.7f);      /* zoom in  */
                else if (evt.ch == '-') graph_zoom(&graph_vp, 1.4f);  /* zoom out */
                break;
            case KEY_ENTER:
                /* Toggle trace mode */
                trace_active = !trace_active;
                if (trace_active) trace_sx = GRAPH_X0 + GRAPH_W / 2;
                break;
            case KEY_MODE:
                /* Reset to default viewport and auto-range */
                graph_default_viewport(&graph_vp);
                if (graph_eq_mode && graph_expr2) {
                    graph_auto_range_two(&graph_vp, graph_expr,
                                         graph_expr2, graph_var);
                    graph_isect_count = graph_find_intersections(
                        &graph_vp, graph_expr, graph_expr2, graph_var,
                        graph_isects, GRAPH_MAX_INTERSECTIONS);
                } else {
                    graph_auto_range(&graph_vp, graph_expr, graph_var);
                }
                break;
            case KEY_GRAPH_YEQU:
                /* Exit graph mode to edit equation */
                exit_graph_mode();
                state = STATE_INPUT;
                break;
            case KEY_GRAPH_WINDOW:
                /* Open viewport editor while in graph mode */
                in_window_edit = true;
                window_cursor = 0;
                break;
            case KEY_GRAPH_ZOOM:
                /* Open zoom presets while in graph mode */
                in_zoom_menu = true;
                zoom_cursor = 0;
                break;
            case KEY_GRAPH_TRACE:
                /* Toggle trace mode */
                trace_active = !trace_active;
                if (trace_active) trace_sx = GRAPH_X0 + GRAPH_W / 2;
                break;
            case KEY_GRAPH_DRAW:
                /* Refresh the graph (just redraw) */
                break;
            default:
                break;
            }
            redraw();
            continue;
        }

        if (in_option_menu && evt.action != KEY_NONE) {
            switch (evt.action) {
            case KEY_ENTER: {
                integ_method_t saved_method = g_settings.integ_method;
                g_settings.integ_method = solve_method_to_setting(
                    option_methods[option_cursor]);
                in_option_menu = false;
                progress_start();
                process_input();
                g_settings.integ_method = saved_method;
                ckpt_nav_active = false;
                step_nav_active = false;
                if (state == STATE_RESULT && show_steps && steps_count() > 0 &&
                    g_settings.solve_mode == SOLVE_STEP_BY_STEP) {
                    if (steps_checkpoint_count() > 0) {
                        ckpt_nav_active = true;
                        ckpt_reveal = 0;
                    } else if (g_settings.show_steps) {
                        step_nav_active = true;
                        step_reveal = 0;
                    }
                }
                break;
            }
            case KEY_UP:
                if (option_cursor > 0) option_cursor--;
                if (option_cursor < option_scroll) option_scroll = option_cursor;
                break;
            case KEY_DOWN:
                if (option_cursor + 1 < option_count) option_cursor++;
                if (option_cursor >= option_scroll + OPTION_VISIBLE_ROWS) {
                    option_scroll = option_cursor - OPTION_VISIBLE_ROWS + 1;
                }
                break;
            case KEY_CLEAR:
            case KEY_QUIT:
            case KEY_MODE:
                in_option_menu = false;
                break;
            default:
                break;
            }
            redraw();
            continue;
        }

        /* ── MathPrint visual input template ─────────────────────────
         * When a template is active, route editing keys into it.
         * ENTER assembles the expression; CLEAR/QUIT cancels.         */
        if (in_math_template && evt.action != KEY_NONE) {
            bool tmpl_done = false;
            bool tmpl_cancelled = false;
            bool consumed = mp_tmpl_key(&math_template, &evt,
                                        &tmpl_done, &tmpl_cancelled);
            if (tmpl_cancelled) {
                mp_tmpl_cancel(&math_template);
                in_math_template = false;
            } else if (tmpl_done) {
                char assembled[MAX_INPUT_LEN];
                if (mp_tmpl_assemble(&math_template, assembled, sizeof(assembled))) {
                    input_buf_clear(&input);
                    input_buf_insert_str(&input, assembled);
                }
                mp_tmpl_cancel(&math_template);
                in_math_template = false;
            }
            if (consumed || tmpl_done || tmpl_cancelled) {
                redraw();
                continue;
            }
        }

        /* ── Disambiguation dialog ────────────────────────────────────
         * When multiple operations are possible, show a numbered list. */
        if (in_disambig_menu && evt.action != KEY_NONE) {
            switch (evt.action) {
            case KEY_UP:
                if (disambig_cursor > 0) disambig_cursor--;
                break;
            case KEY_DOWN:
                if (disambig_cursor + 1 < disambig_count) disambig_cursor++;
                break;
            case KEY_ENTER: {
                char disambig_expr[MAX_INPUT_LEN];
                if (ce_disambig_build_expr(input.buf, disambig_var,
                                           disambig_opts[disambig_cursor].action,
                                           disambig_expr, sizeof(disambig_expr))) {
                    input_buf_clear(&input);
                    input_buf_insert_str(&input, disambig_expr);
                }
                in_disambig_menu = false;
                progress_start();
                process_input();
                ckpt_nav_active = false;
                step_nav_active = false;
                result_post_nav = false;
                if (state == STATE_RESULT && show_steps && steps_count() > 0 &&
                    g_settings.solve_mode == SOLVE_STEP_BY_STEP) {
                    if (steps_checkpoint_count() > 0) {
                        ckpt_nav_active = true;
                        ckpt_reveal = 0;
                    } else if (g_settings.show_steps) {
                        step_nav_active = true;
                        step_reveal = 0;
                    }
                }
                break;
            }
            case KEY_CLEAR:
            case KEY_QUIT:
            case KEY_MODE:
                in_disambig_menu = false;
                break;
            default:
                break;
            }
            redraw();
            continue;
        }

        switch (evt.action) {

        case KEY_QUIT:
            if (in_window_edit) {
                in_window_edit = false;
                redraw();
                break;
            }
            if (in_zoom_menu) {
                in_zoom_menu = false;
                redraw();
                break;
            }
            if (in_tutorial) {
                in_tutorial = false;
                first_run = false;
                redraw();
                break;
            }
            if (in_examples) {
                in_examples = false;
                redraw();
                break;
            }
            if (in_help) {
                in_help = false;
                redraw();
                break;
            }
            if (in_templates) {
                in_templates = false;
                redraw();
                break;
            }
            if (in_menu) {
                in_menu = false;
                redraw();
                break;
            }
            if (in_graph) {
                exit_graph_mode();
                state = STATE_INPUT;
                redraw();
                break;
            }
            if (state == STATE_RESULT) {
                history_capture_active_result();
                ckpt_nav_active = false;
                step_nav_active = false;
                scroll_pos = 0;
                state = STATE_INPUT;
                redraw();
                break;
            }
            if (state == STATE_ERROR) {
                state = STATE_INPUT;
                redraw();
                break;
            }
            running = false;
            break;

        case KEY_ENTER: {
            ui_result_enter_action_t result_enter_action;
            if (in_tutorial) {
                if (tutorial_page + 1 < TUTORIAL_PAGES) tutorial_page++;
                else { in_tutorial = false; first_run = false; }
                redraw(); break;
            }
            if (in_examples) {
                history_capture_active_result();
                history_reset_browse();
                const char *ins = ui_example_get_insert(ex_page, ex_cursor);
                if (ins) {
                    input_buf_clear(&input);
                    input_buf_insert_str(&input, ins);
                }
                in_examples = false;
                state = STATE_INPUT;
                redraw(); break;
            }
            if (in_help) { in_help = false; redraw(); break; }
            if (in_templates) {
                /* Insert selected template */
                history_capture_active_result();
                history_reset_browse();
                {
                    const char *ins = ui_template_get_insert(tmpl_page, tmpl_cursor);
                    if (ins) {
                        uint8_t slot = ui_template_get_slot(tmpl_page, tmpl_cursor);
                        input_buf_clear(&input);
                        input_buf_insert_str(&input, ins);
                        /* Position cursor at first fill slot (slot_offset from start) */
                        if (slot < input.len) {
                            input.cursor = slot;
                        }
                    }
                }
                in_templates = false;
                state = STATE_INPUT;
                redraw();
                break;
            }
            if (in_window_edit) {
                /* ENTER in window editor = apply and enter graph mode */
                in_window_edit = false;
                if (in_graph) {
                    /* Already in graph mode, just refresh with new viewport */
                } else if (graph_expr) {
                    /* Have an expression, enter graph mode */
                    in_graph = true;
                }
                redraw(); break;
            }
            if (in_zoom_menu) {
                /* Apply selected zoom preset */
                switch (zoom_cursor) {
                case 0: /* ZStandard */
                    graph_vp.x_min = -10.0f; graph_vp.x_max = 10.0f;
                    graph_vp.y_min = -6.5f;  graph_vp.y_max = 6.5f;
                    break;
                case 1: /* ZDecimal */
                    graph_vp.x_min = -4.7f; graph_vp.x_max = 4.7f;
                    graph_vp.y_min = -3.1f; graph_vp.y_max = 3.1f;
                    break;
                case 2: /* ZTrig */
                    graph_vp.x_min = -6.2832f; graph_vp.x_max = 6.2832f;
                    graph_vp.y_min = -4.0f;    graph_vp.y_max = 4.0f;
                    break;
                case 3: /* ZSquare */ {
                    ce_float_t cx = (graph_vp.x_min + graph_vp.x_max) * 0.5f;
                    ce_float_t cy = (graph_vp.y_min + graph_vp.y_max) * 0.5f;
                    ce_float_t hw = (graph_vp.x_max - graph_vp.x_min) * 0.5f;
                    graph_vp.x_min = cx - hw; graph_vp.x_max = cx + hw;
                    graph_vp.y_min = cy - hw * 0.65f; graph_vp.y_max = cy + hw * 0.65f;
                    break;
                }
                case 4: /* ZFit */
                    if (graph_expr) {
                        if (graph_eq_mode && graph_expr2)
                            graph_auto_range_two(&graph_vp, graph_expr,
                                                 graph_expr2, graph_var);
                        else
                            graph_auto_range(&graph_vp, graph_expr, graph_var);
                    }
                    break;
                case 5: /* Zoom In */
                    graph_zoom(&graph_vp, 0.5f);
                    break;
                case 6: /* Zoom Out */
                    graph_zoom(&graph_vp, 2.0f);
                    break;
                }
                in_zoom_menu = false;
                if (in_graph) { /* refresh */ }
                redraw(); break;
            }
            if (in_menu) {
                if (menu_tab == MENU_TAB_SETTINGS) {
                    switch (menu_cursor) {
                    case 0: g_settings.show_steps = !g_settings.show_steps; break;
                    case 1: g_settings.solve_mode = (g_settings.solve_mode == SOLVE_FULL_AUTO)
                                ? SOLVE_STEP_BY_STEP : SOLVE_FULL_AUTO; break;
                    case 2: g_settings.integ_method = (integ_method_t)
                                ((g_settings.integ_method + 1) % 5); break;
                    case 3: g_settings.angle_mode = (g_settings.angle_mode == ANGLE_RADIANS)
                                ? ANGLE_DEGREES : ANGLE_RADIANS; break;
                    case 4: g_settings.precision = (g_settings.precision % 9) + 1; break;
                    case 5: g_settings.complex_mode = (g_settings.complex_mode + 1) % 3; break;
                    case 6: g_settings.pmt_at_begin = !g_settings.pmt_at_begin; break;
                    case 7: g_settings.step_detail = (g_settings.step_detail == STEP_ALL)
                                ? STEP_COMPACT : STEP_ALL; break;
                    case 8: g_settings.show_method_choices = !g_settings.show_method_choices; break;
                    case 9: g_settings.segment_pause = !g_settings.segment_pause; break;
                    default: break;
                    }
                    settings_save();
                    redraw();
                    break;
                } else if (menu_tab == MENU_TAB_BOOKMARKS) {
                    /* Recall bookmark expression into input */
                    int bm_count = bookmark_count();
                    if (menu_cursor < (uint8_t)bm_count) {
                        const bookmark_t *bm = bookmark_get(menu_cursor);
                        if (bm && bm->expr[0]) {
                            history_capture_active_result();
                            history_reset_browse();
                            input_buf_clear(&input);
                            input_buf_insert_str(&input, bm->expr);
                        }
                    }
                    in_menu = false;
                    state = STATE_INPUT;
                    redraw();
                    break;
                } else if (menu_tab == MENU_TAB_FUNCS) {
                    /* Recall user function call into input */
                    int fn_count = userfunc_count();
                    if (menu_cursor < (uint8_t)fn_count) {
                        const userfunc_def_t *fn = userfunc_get(menu_cursor);
                        if (fn) {
                            char fn_call[32];
                            snprintf(fn_call, sizeof(fn_call), "%s(", fn->name);
                            history_capture_active_result();
                            history_reset_browse();
                            input_buf_insert_str(&input, fn_call);
                        }
                    }
                    in_menu = false;
                    state = STATE_INPUT;
                    redraw();
                    break;
                } else {
                    uint8_t page = (menu_tab == MENU_TAB_RECENTS) ? 0 : menu_page;
                    const menu_item_t *page_items = ui_menu_get_page(page);
                    uint8_t page_count = ui_menu_get_count(page);
                    if (page_items && menu_cursor < page_count) {
                        const menu_item_t *item = &page_items[menu_cursor];
                        const char *ins = item->insert_str;
                        /* Special action: UNDO sentinel */
                        if (ins[0] == '\x01' && strncmp(ins + 1, "UNDO", 4) == 0) {
                            if (undo_pop()) {
                                state = STATE_INPUT;
                            }
                        } else {
                            history_capture_active_result();
                            history_reset_browse();
                            ui_menu_add_recent(item);
                            /* If item has a template type, activate visual template */
                            if (item->tmpl_type != 0) {
                                /* Save current input as template prefix */
                                mp_tmpl_cancel(&math_template);
                                mp_tmpl_init(&math_template, (mp_tmpl_type_t)item->tmpl_type);
                                /* Copy current input buf to prefix */
                                if (input.len > 0) {
                                    memcpy(math_template.prefix, input.buf, input.len);
                                    math_template.prefix[input.len] = '\0';
                                    math_template.prefix_len = input.len;
                                }
                                in_math_template = true;
                            } else {
                                input_buf_insert_str(&input, ins);
                            }
                        }
                    }
                }
                in_menu = false;
                state = STATE_INPUT;
                redraw();
                break;
            }
            /* Segment pause: ENTER advances to next computation stage */
            if (seg_pause_active) {
                seg_pause_idx++;
                if (seg_pause_idx >= steps_checkpoint_count()) {
                    seg_pause_active = false;
                    result_post_nav = true;  /* prevent immediate re-run */
                }
                redraw();
                break;
            }
            result_enter_action = ui_result_enter_action(ckpt_nav_active,
                                                         step_nav_active,
                                                         show_steps,
                                                         g_settings.show_steps);
            /* Result-screen ENTER never exits; it reveals more work or reruns input. */
            if (result_enter_action == UI_RESULT_ENTER_ADVANCE_CHECKPOINT) {
                if (ckpt_reveal < steps_checkpoint_count()) {
                    ckpt_reveal++;
                    redraw();
                    break;
                }
                /* All checkpoints shown — exit checkpoint nav, stay on result */
                ckpt_nav_active = false;
                result_post_nav = true;
                redraw();
                break;
            }
            if (result_enter_action == UI_RESULT_ENTER_ADVANCE_STEP) {
                if (step_reveal < steps_count()) {
                    step_reveal++;
                    redraw();
                    break;
                }
                /* All steps shown — exit step nav, stay on result */
                step_nav_active = false;
                result_post_nav = true;
                redraw();
                break;
            }
            /* post-nav guard: first ENTER after nav ends stays on result */
            if (result_post_nav) {
                result_post_nav = false;
                redraw();
                break;
            }
            history_capture_active_result();
            history_reset_browse();
            if (maybe_open_method_option_menu()) {
                redraw();
                break;
            }
            undo_push();   /* snapshot input before evaluation */
            progress_start();
            process_input();
            /* Activate navigation based on solve mode + show_steps setting */
            ckpt_nav_active = false;
            step_nav_active = false;
            seg_pause_active = false;
            result_post_nav = false;
            if (state == STATE_RESULT && show_steps && steps_count() > 0) {
                if (g_settings.segment_pause && steps_checkpoint_count() > 0) {
                    /* Segment pause: show one stage at a time */
                    seg_pause_active = true;
                    seg_pause_idx = 0;
                } else if (g_settings.solve_mode == SOLVE_STEP_BY_STEP) {
                    if (steps_checkpoint_count() > 0) {
                        ckpt_nav_active = true;
                        ckpt_reveal = 0;
                    } else if (g_settings.show_steps) {
                        step_nav_active = true;
                        step_reveal = 0;
                    }
                }
            }
            redraw();
            break;
        }

        case KEY_CLEAR:
            /* ── CLEAR clears content first, exits second ──
             * This matches real TI-84 behavior: CLEAR removes
             * whatever is in the current context before exiting. */

            /* Menus/modes with no text input: CLEAR exits immediately */
            if (in_window_edit) { in_window_edit = false; redraw(); break; }
            if (in_zoom_menu) { in_zoom_menu = false; redraw(); break; }
            if (in_tutorial) { in_tutorial = false; first_run = false; redraw(); break; }
            if (in_help) { in_help = false; redraw(); break; }
            if (in_templates) { in_templates = false; redraw(); break; }
            if (in_menu) { in_menu = false; redraw(); break; }
            if (in_examples) { in_examples = false; redraw(); break; }

            /* Graph equation entry: clear input first, exit second */
            if (graph_eq_entry && input.len > 0) {
                input_buf_clear(&input);
                mem_free_all();
                redraw();
                break;
            }
            if (graph_eq_entry) {
                graph_eq_entry = false;
                redraw();
                break;
            }

            /* Graph mode: CLEAR exits */
            if (in_graph) {
                exit_graph_mode();
                state = STATE_INPUT; redraw(); break;
            }

            if (seg_pause_active) {
                /* CLEAR exits segment pause, show all stages at once */
                seg_pause_active = false;
                seg_pause_idx = steps_checkpoint_count() - 1;
                result_post_nav = false;
                redraw();
                break;
            }
            if (ckpt_nav_active) {
                /* CLEAR exits checkpoint navigation, show all steps */
                ckpt_nav_active = false;
                result_post_nav = false;
                redraw();
                break;
            }
            if (step_nav_active) {
                /* CLEAR exits step navigation, show all steps */
                step_nav_active = false;
                result_post_nav = false;
                redraw();
                break;
            }
            result_post_nav = false;

            if (state == STATE_RESULT || state == STATE_ERROR) {
                if (state == STATE_RESULT) {
                    history_capture_active_result();
                }
                history_reset_browse();
                scroll_pos = 0;
                /* First CLEAR: go back to input mode (keep input text) */
                state = STATE_INPUT;
            } else if (input.len > 0) {
                /* Second CLEAR: wipe input buffer */
                input_buf_clear(&input);
                history_reset_browse();
                mem_free_all();
            }
            /* Third CLEAR on empty input: already clear, no-op */
            redraw();
            break;

        case KEY_DEL:
            /* DEL in bookmark tab = delete selected bookmark */
            if (in_menu && menu_tab == MENU_TAB_BOOKMARKS) {
                const bookmark_t *bm = bookmark_get(menu_cursor);
                if (bm) {
                    bookmark_delete(bm->name);
                    bookmark_save_file();
                    if (menu_cursor >= ui_bookmarks_count() && menu_cursor > 0)
                        menu_cursor--;
                }
                redraw(); break;
            }
            /* DEL in funcs tab = delete selected function */
            if (in_menu && menu_tab == MENU_TAB_FUNCS) {
                const userfunc_def_t *fn = userfunc_get(menu_cursor);
                if (fn) {
                    userfunc_delete(fn->name);
                    userfunc_save();
                    if (menu_cursor >= ui_funcs_count() && menu_cursor > 0)
                        menu_cursor--;
                }
                redraw(); break;
            }
            history_capture_active_result();
            history_reset_browse();
            input_buf_backspace(&input);
            if (state != STATE_INPUT) { state = STATE_INPUT; dirty = DIRTY_ALL; }
            else dirty = DIRTY_INPUT | DIRTY_WORK;
            redraw();
            break;

        case KEY_DEL_FORWARD:
            history_capture_active_result();
            history_reset_browse();
            input_buf_delete(&input);
            if (state != STATE_INPUT) { state = STATE_INPUT; dirty = DIRTY_ALL; }
            else dirty = DIRTY_INPUT | DIRTY_WORK;
            redraw();
            break;

        case KEY_LEFT:
            if (in_window_edit) {
                /* Decrease the selected viewport value */
                switch (window_cursor) {
                case 0: graph_vp.x_min -= WINDOW_STEP_X; break;
                case 1: graph_vp.x_max -= WINDOW_STEP_X; break;
                case 2: graph_vp.y_min -= WINDOW_STEP_Y; break;
                case 3: graph_vp.y_max -= WINDOW_STEP_Y; break;
                }
                redraw(); break;
            }
            if (in_zoom_menu) { redraw(); break; } /* no left/right in zoom */
            if (in_tutorial) {
                if (tutorial_page > 0) tutorial_page--;
                redraw(); break;
            }
            if (in_examples) {
                if (ex_page > 0) { ex_page--; ex_cursor = 0; }
                redraw(); break;
            }
            if (in_help) {
                if (help_page > 0) help_page--;
                redraw(); break;
            }
            if (in_templates) {
                if (tmpl_page > 0) { tmpl_page--; tmpl_cursor = 0; }
                redraw(); break;
            }
            if (in_menu) {
                if (menu_tab == MENU_TAB_SYMBOLS && menu_page > 1) {
                    menu_page--;
                    menu_cursor = 0;
                    menu_scroll = 0;
                }
                redraw(); break;
            }
            if (input.cursor > 0) input.cursor--;
            redraw();
            break;

        case KEY_RIGHT:
            if (in_window_edit) {
                /* Increase the selected viewport value */
                switch (window_cursor) {
                case 0: graph_vp.x_min += WINDOW_STEP_X; break;
                case 1: graph_vp.x_max += WINDOW_STEP_X; break;
                case 2: graph_vp.y_min += WINDOW_STEP_Y; break;
                case 3: graph_vp.y_max += WINDOW_STEP_Y; break;
                }
                redraw(); break;
            }
            if (in_zoom_menu) { redraw(); break; }
            if (in_tutorial) {
                if (tutorial_page + 1 < TUTORIAL_PAGES) tutorial_page++;
                else { in_tutorial = false; first_run = false; }
                redraw(); break;
            }
            if (in_examples) {
                if (ex_page + 1 < EXAMPLE_PAGES) { ex_page++; ex_cursor = 0; }
                redraw(); break;
            }
            if (in_help) {
                if (help_page + 1 < HELP_PAGES) help_page++;
                redraw(); break;
            }
            if (in_templates) {
                if (tmpl_page + 1 < TEMPLATE_PAGES) { tmpl_page++; tmpl_cursor = 0; }
                redraw(); break;
            }
            if (in_menu) {
                if (menu_tab == MENU_TAB_SYMBOLS && menu_page + 1 < MENU_PAGES) {
                    menu_page++;
                    menu_cursor = 0;
                    menu_scroll = 0;
                }
                redraw(); break;
            }
            if (input.cursor < input.len) input.cursor++;
            redraw();
            break;

        case KEY_CHAR:
            history_capture_active_result();
            history_reset_browse();
            input_buf_insert(&input, evt.ch);
            if (state != STATE_INPUT) { state = STATE_INPUT; dirty = DIRTY_ALL; }
            else dirty = DIRTY_INPUT | DIRTY_WORK; /* context help may update */
            /* Live trigger detection for MathPrint templates */
            if (!in_math_template) {
                mp_tmpl_type_t trig_type;
                uint8_t prefix_end;
                if (mp_tmpl_detect_trigger(input.buf, input.len,
                                           &trig_type, &prefix_end)) {
                    /* Copy prefix (text before trigger) */
                    mp_tmpl_init(&math_template, trig_type);
                    if (prefix_end > 0) {
                        memcpy(math_template.prefix, input.buf, prefix_end);
                        math_template.prefix[prefix_end] = '\0';
                        math_template.prefix_len = prefix_end;
                    }
                    /* Any text after the trigger goes into slot 0 */
                    {
                        uint8_t after_len = input.len - prefix_end;
                        /* find trigger length (total - prefix) */
                        uint8_t after_start = input.len;
                        /* The trigger consumed everything after prefix_end,
                         * so nothing to pre-fill unless parser left residual.
                         * Just clear the input — template takes over.        */
                        (void)after_len; (void)after_start;
                    }
                    input_buf_clear(&input);
                    in_math_template = true;
                    dirty = DIRTY_ALL;
                }
            }
            redraw();
            break;

        case KEY_STR:
            history_capture_active_result();
            history_reset_browse();
            if (evt.str) input_buf_insert_str(&input, evt.str);
            if (state != STATE_INPUT) { state = STATE_INPUT; dirty = DIRTY_ALL; }
            else dirty = DIRTY_INPUT | DIRTY_WORK;
            /* Live trigger detection for MathPrint templates */
            if (!in_math_template) {
                mp_tmpl_type_t trig_type;
                uint8_t prefix_end;
                if (mp_tmpl_detect_trigger(input.buf, input.len,
                                           &trig_type, &prefix_end)) {
                    mp_tmpl_init(&math_template, trig_type);
                    if (prefix_end > 0) {
                        memcpy(math_template.prefix, input.buf, prefix_end);
                        math_template.prefix[prefix_end] = '\0';
                        math_template.prefix_len = prefix_end;
                    }
                    input_buf_clear(&input);
                    in_math_template = true;
                    dirty = DIRTY_ALL;
                }
            }
            redraw();
            break;

        case KEY_MODE:
            if (ckpt_nav_active) {
                ckpt_reveal = steps_checkpoint_count();
                ckpt_nav_active = false;
            } else if (step_nav_active) {
                step_reveal = steps_count();
                step_nav_active = false;
            } else if (in_menu && menu_tab == MENU_TAB_SETTINGS) {
                in_menu = false;
            } else if (in_help) {
                /* Close help */
                in_help = false;
            } else if (in_templates) {
                /* Close templates */
                in_templates = false;
            } else if (state == STATE_RESULT && has_numeric_result) {
                /* Cycle answer format: decimal → fraction → sci → decimal */
                fmt_mode = (result_format_t)((fmt_mode + 1) % FMT_COUNT);
                reformat_result();
            } else {
                in_menu = true;
                menu_tab = MENU_TAB_SETTINGS;
                menu_cursor = 0;
                menu_scroll = 0;
                if (menu_page == 0) menu_page = 1;
                in_help = false;
                in_templates = false;
                in_examples = false;
            }
            redraw();
            break;

        case KEY_UP:
            if (in_window_edit) {
                if (window_cursor > 0) window_cursor--;
                redraw(); break;
            }
            if (in_zoom_menu) {
                if (zoom_cursor > 0) zoom_cursor--;
                redraw(); break;
            }
            if (in_tutorial) { redraw(); break; }
            if (in_examples) {
                if (ex_cursor > 0) ex_cursor--;
                redraw(); break;
            }
            if (in_templates) {
                if (tmpl_cursor > 0) tmpl_cursor--;
                redraw(); break;
            }
            if (in_menu) {
                if (menu_cursor > 0) menu_cursor--;
                redraw(); break;
            }
            if (state == STATE_RESULT && show_steps && scroll_pos > 0) {
                scroll_pos--;
                redraw();
            } else if (state == STATE_INPUT) {
                if (mp_cursor_up(&input)) {
                    redraw();
                    break;
                }
                int pos;
                if (history_navigate_position() < 0) {
                    strncpy(hist_saved, input.buf, sizeof(hist_saved) - 1);
                    hist_saved[sizeof(hist_saved) - 1] = '\0';
                }
                pos = history_navigate_back();
                if (pos >= 0 && history_restore_reentry(pos)) {
                    redraw();
                }
            }
            break;

        case KEY_DOWN:
            if (in_window_edit) {
                if (window_cursor + 1 < WINDOW_FIELDS) window_cursor++;
                redraw(); break;
            }
            if (in_zoom_menu) {
                if (zoom_cursor + 1 < ZOOM_PRESETS) zoom_cursor++;
                redraw(); break;
            }
            if (in_tutorial) { redraw(); break; }
            if (in_examples) {
                if (ex_cursor + 1 < ui_example_page_count(ex_page)) ex_cursor++;
                redraw(); break;
            }
            if (in_templates) {
                if (tmpl_cursor + 1 < ui_template_page_count(tmpl_page))
                    tmpl_cursor++;
                redraw(); break;
            }
            if (in_menu) {
                uint8_t count;
                if (menu_tab == MENU_TAB_SETTINGS)      count = SETTINGS_COUNT;
                else if (menu_tab == MENU_TAB_BOOKMARKS) count = ui_bookmarks_count();
                else if (menu_tab == MENU_TAB_FUNCS)     count = ui_funcs_count();
                else count = ui_menu_get_count(menu_tab == MENU_TAB_RECENTS ? 0 : menu_page);
                if (menu_cursor + 1 < count)
                    menu_cursor++;
                redraw(); break;
            }
            if (state == STATE_RESULT && show_steps && scroll_pos + 1 < steps_count()) {
                scroll_pos++;
                redraw();
            } else if (state == STATE_INPUT) {
                if (mp_cursor_down(&input)) {
                    redraw();
                    break;
                }
                int pos = history_navigate_forward();
                if (pos >= 0) {
                    if (history_restore_reentry(pos)) {
                        redraw();
                    }
                } else if (history_restore_reentry(-1)) {
                    redraw();
                }
            }
            break;

        case KEY_HELP:
            if (in_window_edit) { in_window_edit = false; }
            if (in_zoom_menu) { in_zoom_menu = false; }
            if (in_tutorial) { in_tutorial = false; first_run = false; redraw(); break; }
            if (in_examples) { in_examples = false; redraw(); break; }
            in_help = !in_help;
            if (in_help) { help_page = 0; in_menu = false; in_examples = false; }
            redraw();
            break;

        case KEY_MENU:
            if (in_window_edit) { in_window_edit = false; redraw(); break; }
            if (in_zoom_menu) { in_zoom_menu = false; redraw(); break; }
            if (in_tutorial) { in_tutorial = false; first_run = false; redraw(); break; }
            if (in_examples) { in_examples = false; redraw(); break; }
            if (in_templates) {
                /* 2nd+CLEAR while in templates → open examples */
                in_templates = false;
                in_examples = true;
                ex_cursor = 0;
                ex_page = 0;
                redraw();
                break;
            }
            if (in_menu) {
                /* 2nd+CLEAR while in menu → open templates */
                in_menu = false;
                in_templates = true;
                tmpl_cursor = 0;
                tmpl_page = 0;
                redraw();
                break;
            }
            in_menu = !in_menu;
            if (in_menu) {
                menu_tab = (ui_menu_recents_count() > 0) ? MENU_TAB_RECENTS : MENU_TAB_SYMBOLS;
                menu_page = 1;
                menu_cursor = 0;
                menu_scroll = 0;
                in_help = false;
                in_templates = false; in_examples = false;
            }
            redraw();
            break;

        case KEY_TAB_LEFT:
            if (in_menu) {
                if (menu_tab == 0) menu_tab = MENU_TAB_COUNT - 1;
                else menu_tab = (menu_tab_t)(menu_tab - 1);
                menu_cursor = 0;
                menu_scroll = 0;
                redraw();
            }
            break;

        case KEY_TAB_RIGHT:
            if (in_menu) {
                menu_tab = (menu_tab_t)((menu_tab + 1) % MENU_TAB_COUNT);
                menu_cursor = 0;
                menu_scroll = 0;
                redraw();
            }
            break;

        /* ── Graph key handlers ──────────────────────────── */
        case KEY_GRAPH_YEQU:
            /* Y= → Graph equation entry / exit graph to edit equation */
            if (in_window_edit) { in_window_edit = false; }
            if (in_zoom_menu) { in_zoom_menu = false; }
            if (in_graph) {
                /* Exit graph mode so user can edit the equation */
                exit_graph_mode();
                state = STATE_INPUT;
            } else {
                /* Prepare for equation entry: clear input for fresh equation */
                history_capture_active_result();
                history_reset_browse();
                graph_eq_entry = true;
                in_help = false;
                in_menu = false; in_templates = false; in_examples = false;
                if (state != STATE_INPUT) {
                    state = STATE_INPUT;
                }
            }
            redraw();
            break;

        case KEY_GRAPH_WINDOW:
            /* WINDOW → viewport settings editor */
            if (in_zoom_menu) { in_zoom_menu = false; }
            if (!in_window_edit) {
                /* Initialize viewport to defaults if no graph yet */
                if (!in_graph && !graph_expr) {
                    graph_default_viewport(&graph_vp);
                }
                in_window_edit = true;
                window_cursor = 0;
                in_help = false;
                in_menu = false; in_templates = false; in_examples = false;
            } else {
                in_window_edit = false;
            }
            redraw();
            break;

        case KEY_GRAPH_ZOOM:
            /* ZOOM → zoom presets menu */
            if (in_window_edit) { in_window_edit = false; }
            if (!in_zoom_menu) {
                in_zoom_menu = true;
                zoom_cursor = 0;
                in_help = false;
                in_menu = false; in_templates = false; in_examples = false;
            } else {
                in_zoom_menu = false;
            }
            redraw();
            break;

        case KEY_GRAPH_TRACE:
            /* TRACE → toggle trace mode (graph mode only) */
            if (in_graph) {
                trace_active = !trace_active;
                if (trace_active) trace_sx = GRAPH_X0 + GRAPH_W / 2;
                redraw();
            }
            break;

        case KEY_GRAPH_DRAW: {
            /* GRAPH → draw/refresh the graph */
            if (in_window_edit) { in_window_edit = false; }
            if (in_zoom_menu) { in_zoom_menu = false; }
            if (in_graph) {
                /* Already in graph mode — just refresh */
                redraw();
                break;
            }
            /* Try to graph the current input expression */
            if (input.len > 0) {
                /* Build graph(input,x) command and process it */
                char graph_cmd[MAX_INPUT_LEN + 16];
                char saved_input[MAX_INPUT_LEN];
                history_capture_active_result();
                history_reset_browse();
                memcpy(saved_input, input.buf, input.len + 1);
                snprintf(graph_cmd, sizeof(graph_cmd), "graph(%s,x)",
                         saved_input);
                input_buf_clear(&input);
                input_buf_insert_str(&input, graph_cmd);
                progress_start();
                process_input();
                /* Restore original input text */
                input_buf_clear(&input);
                input_buf_insert_str(&input, saved_input);
                if (in_graph) {
                    graph_eq_entry = false;
                }
            }
            redraw();
            break;
        }

        case KEY_NONE:
        default:
            /* Idle path: use TI-OS delay to avoid a busy-spin main loop. */
            delay(MAIN_LOOP_IDLE_MS);
            break;
        }
    }

    /* Cleanup */
    history_save();
    settings_save();
    ui_shutdown();
    return 0;
}
