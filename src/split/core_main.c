#include <tice.h>
#include <string.h>
#include <stdio.h>

#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "ast.h"
#include "parser.h"
#include "steps.h"
#include "ui.h"
#include "mathprint.h"
#include "split/ipc.h"
#include "split/classify.h"
#include "split/astwire.h"
#include "split/module_runtime.h"

/* Forward declarations for re-entry callback chain */
int main(void);
static int core_reentry(void *data, int retval)
{
    (void)data;
    (void)retval;
    ce_module_runtime_mark_reentry();
    return main();
}

static input_buf_t g_input;
static app_state_t g_state;
static bool g_show_steps;
static int g_scroll_pos;
static bool g_ckpt_nav_active;   /* true while stepping through checkpoints */
static int  g_ckpt_reveal;       /* number of checkpoints revealed so far   */

/* Disambiguation chooser state */
#define DISAMBIG_MAX_OPTIONS 6
#define DISAMBIG_LABEL_LEN   32
#define DISAMBIG_EXPR_LEN    MAX_INPUT_LEN
static bool  g_disambig_active;
static uint8_t g_disambig_count;
static uint8_t g_disambig_cursor;
static char  g_disambig_labels[DISAMBIG_MAX_OPTIONS][DISAMBIG_LABEL_LEN];
static char  g_disambig_exprs[DISAMBIG_MAX_OPTIONS][DISAMBIG_EXPR_LEN];
static char g_result[MAX_RESULT_LEN];
static char g_error[128];
static ce_shared_state_t g_shared;
static tokenizer_t g_tokenizer;
static parser_t g_parser;
static ce_request_t g_request;
static ce_response_t g_response;

/* Parse disambiguation response: result = "LABEL1\nEXPR1\nLABEL2\nEXPR2\n..."
 * Returns number of options parsed (0 on failure). */
static uint8_t parse_disambig_options(const char *encoded)
{
    const char *p = encoded;
    uint8_t count = 0;
    while (*p && count < DISAMBIG_MAX_OPTIONS) {
        const char *nl1, *nl2;
        nl1 = p;
        while (*nl1 && *nl1 != '\n') nl1++;
        if (!*nl1) break;  /* no second line */
        nl2 = nl1 + 1;
        while (*nl2 && *nl2 != '\n') nl2++;
        /* copy label */
        {
            int llen = (int)(nl1 - p);
            if (llen >= DISAMBIG_LABEL_LEN) llen = DISAMBIG_LABEL_LEN - 1;
            memcpy(g_disambig_labels[count], p, (size_t)llen);
            g_disambig_labels[count][llen] = '\0';
        }
        /* copy expression */
        {
            int elen = (int)(nl2 - (nl1 + 1));
            if (elen >= DISAMBIG_EXPR_LEN) elen = DISAMBIG_EXPR_LEN - 1;
            memcpy(g_disambig_exprs[count], nl1 + 1, (size_t)elen);
            g_disambig_exprs[count][elen] = '\0';
        }
        count++;
        p = (*nl2) ? nl2 + 1 : nl2;
    }
    return count;
}

static bool parse_for_domain(const char *input,
                             ast_node_t **out_root,
                             bool *out_had_equals,
                             ce_domain_t *out_domain)
{
    ast_node_t *root = NULL;
    const char *expr = input;
    const char *p;

    if (!input || !input[0] || !out_root || !out_had_equals || !out_domain) return false;

    *out_root = NULL;
    *out_had_equals = false;
    *out_domain = CE_DOMAIN_UNKNOWN;

    for (p = expr; *p; p++) {
        if (*p == '=') {
            *out_had_equals = true;
            break;
        }
    }

    mem_free_all();
    tok_init(&g_tokenizer, expr);
    if (tok_tokenize(&g_tokenizer) != CE_OK) return false;
    parser_init(&g_parser, &g_tokenizer);
    root = parser_parse(&g_parser);
    if (!root) return false;

    *out_domain = ce_classify_expression(root, *out_had_equals);
    *out_root = root;
    return true;
}

static void apply_response(const ce_response_t *resp)
{
    int i;
    if (!resp) return;

    g_settings = resp->settings;
    g_last_answer = resp->last_answer;
    g_show_steps = resp->show_steps != 0;

    steps_clear();
    for (i = 0; i < resp->step_count && i < MAX_STEPS; i++) {
        steps_add(resp->steps[i]);
    }

    if (resp->ok == 2) {
        /* Disambiguation response: parse options from result field */
        uint8_t nopt = parse_disambig_options(resp->result);
        if (nopt >= 2) {
            g_disambig_active = true;
            g_disambig_count  = nopt;
            g_disambig_cursor = 0;
            g_state = STATE_DISAMBIG;
        } else {
            /* Fallback: treat as error */
            strncpy(g_error, "Ambiguous expression", sizeof(g_error) - 1);
            g_error[sizeof(g_error) - 1] = '\0';
            g_state = STATE_ERROR;
        }
        g_scroll_pos = 0;
        g_ckpt_nav_active = false;
        g_ckpt_reveal = 0;
        return;
    }

    if (resp->ok) {
        strncpy(g_result, resp->result, sizeof(g_result) - 1);
        g_result[sizeof(g_result) - 1] = '\0';
        g_state = STATE_RESULT;
    } else {
        strncpy(g_error, resp->error, sizeof(g_error) - 1);
        g_error[sizeof(g_error) - 1] = '\0';
        g_state = STATE_ERROR;
    }
    g_scroll_pos = 0;

    /* Enable interactive checkpoint stepping when steps are present and the
       user has show_steps enabled.  Start at reveal=0 (no steps shown yet). */
    if (g_show_steps && g_settings.show_steps && steps_checkpoint_count() > 0) {
        g_ckpt_nav_active = true;
        g_ckpt_reveal     = 0;
    } else {
        g_ckpt_nav_active = false;
        g_ckpt_reveal     = 0;
    }
}

static void history_push(ce_shared_state_t *state, const char *expr)
{
    uint8_t last;
    if (!state || !expr || !expr[0]) return;

    if (state->hist_count > 0) {
        last = (uint8_t)((state->hist_write + CE_HISTORY_SIZE - 1) % CE_HISTORY_SIZE);
        if (strcmp(state->history[last], expr) == 0) return;
    }

    strncpy(state->history[state->hist_write], expr, MAX_INPUT_LEN - 1);
    state->history[state->hist_write][MAX_INPUT_LEN - 1] = '\0';
    state->hist_write = (uint8_t)((state->hist_write + 1) % CE_HISTORY_SIZE);
    if (state->hist_count < CE_HISTORY_SIZE) state->hist_count++;
}

static void redraw(void)
{
    ui_draw_header();
    ui_clear_work_area();

    if (g_state == STATE_RESULT) {
        ui_draw_result_colored(g_result);
        if (g_show_steps && g_settings.show_steps) {
            if (g_ckpt_nav_active) {
                /* Interactive checkpoint stepping: show steps up to current reveal */
                ui_draw_steps_checkpoint(g_ckpt_reveal);
            } else {
                ui_draw_steps(g_scroll_pos);
            }
        }
    } else if (g_state == STATE_DISAMBIG) {
        /* Build option pointer array for ui_draw_option_menu */
        const char *opt_ptrs[DISAMBIG_MAX_OPTIONS];
        uint8_t i;
        for (i = 0; i < g_disambig_count && i < DISAMBIG_MAX_OPTIONS; i++)
            opt_ptrs[i] = g_disambig_labels[i];
        ui_draw_option_menu("How to handle this?",
                            "Choose an operation:",
                            opt_ptrs, g_disambig_count,
                            g_disambig_cursor, 0);
    } else if (g_state == STATE_ERROR) {
        ui_draw_error(g_error);
    }

    ui_draw_input(&g_input);
    ui_draw_status(mem_nodes_used(), mem_nodes_free());
    ui_flip();
}

static void dispatch_to_module(ce_domain_t domain,
                               const char *expression,
                               const ast_node_t *root,
                               bool had_equals)
{
    ce_request_t *req = &g_request;
    ce_response_t *resp = &g_response;
    const char *program = ce_domain_program(domain);
    int run_ret;
    bool got_resp = false;

    memset(req, 0, sizeof(*req));
    req->magic = CE_IPC_MAGIC;
    req->version = CE_IPC_VERSION;
    req->domain = (uint8_t)domain;
    req->flags = had_equals ? CE_REQ_FLAG_HAD_EQUALS : 0;
    req->ast_size = 0;
    req->settings = g_settings;
    req->last_answer = g_last_answer;
    strncpy(req->caller, CE_PRGM_CORE, sizeof(req->caller) - 1);
    strncpy(req->return_to, CE_PRGM_CORE, sizeof(req->return_to) - 1);
    strncpy(req->expression, expression ? expression : g_input.buf, sizeof(req->expression) - 1);
    req->expression[sizeof(req->expression) - 1] = '\0';
    if (root && ce_ast_serialize(root, req->ast_payload, sizeof(req->ast_payload), &req->ast_size)) {
        req->flags |= CE_REQ_FLAG_HAS_AST;
    }

    g_shared.settings = g_settings;
    g_shared.last_answer = g_last_answer;
    g_shared.has_pending_response = 0;
    strncpy(g_shared.last_expression, expression ? expression : g_input.buf,
            sizeof(g_shared.last_expression) - 1);
    strncpy(g_shared.pending_module, program, sizeof(g_shared.pending_module) - 1);
    g_shared.pending_module[sizeof(g_shared.pending_module) - 1] = '\0';
    history_push(&g_shared, expression ? expression : g_input.buf);

    ce_response_delete();
    ce_request_write(req);
    ce_state_save(&g_shared);

    if (!ce_program_exists(program)) {
        g_state = STATE_ERROR;
        snprintf(g_error, sizeof(g_error), "Module %s missing", program);
        return;
    }

    /* On real hardware, os_RunPrgm destroys this program. The callback
       re-enters main() when the child exits, and the pending-response
       check at startup displays the result. On the desktop mock,
       os_RunPrgm returns immediately and we fall through below. */
    run_ret = ce_launch_program_reentry(program, core_reentry);

    /* --- Desktop/mock synchronous fallback (dead code on real CE) --- */
    if (ce_response_read(resp)) {
        apply_response(resp);
        got_resp = true;
        ce_response_delete();
    }

    ce_state_load(&g_shared);
    g_settings = g_shared.settings;
    g_last_answer = g_shared.last_answer;
    g_shared.has_pending_response = 0;
    if (g_shared.pending_module[0]) {
        ce_program_set_archived(g_shared.pending_module, true);
        g_shared.pending_module[0] = '\0';
    }
    ce_state_save(&g_shared);

    if (run_ret != 0) {
        g_state = STATE_ERROR;
        snprintf(g_error, sizeof(g_error), "Run %s failed (%d)", program, run_ret);
        return;
    }

    if (!got_resp) {
        g_state = STATE_ERROR;
        snprintf(g_error, sizeof(g_error), "No response from %s", program);
    }
}

static void process_enter(void)
{
    ce_domain_t domain;
    ast_node_t *root = NULL;
    bool had_equals = false;

    if (g_input.len == 0) return;

    if (!parse_for_domain(g_input.buf, &root, &had_equals, &domain)) {
        g_state = STATE_ERROR;
        snprintf(g_error, sizeof(g_error), "Could not parse input");
        return;
    }

    dispatch_to_module(domain, g_input.buf, root, had_equals);
    ast_free(root);
}

int main(void)
{
    bool running = true;

    mem_init();
    ui_init();
    input_buf_init(&g_input);
    steps_clear();
    g_state = STATE_INPUT;
    g_show_steps = false;
    g_scroll_pos = 0;
    g_ckpt_nav_active = false;
    g_ckpt_reveal = 0;
    g_disambig_active = false;
    g_disambig_count = 0;
    g_disambig_cursor = 0;
    g_result[0] = '\0';
    g_error[0] = '\0';

    ce_state_load(&g_shared);
    g_settings = g_shared.settings;
    g_last_answer = g_shared.last_answer;

    if (g_shared.last_expression[0]) {
        input_buf_insert_str(&g_input, g_shared.last_expression);
    }

    {
        if (g_shared.has_pending_response && ce_response_read(&g_response)) {
            apply_response(&g_response);
            g_shared.settings = g_settings;
            g_shared.last_answer = g_last_answer;
            g_shared.has_pending_response = 0;
            if (g_shared.pending_module[0]) {
                ce_program_set_archived(g_shared.pending_module, true);
                g_shared.pending_module[0] = '\0';
            }
            ce_state_save(&g_shared);
            ce_response_delete();
        }
    }

    redraw();
    while (running) {
        key_event_t evt = ui_poll_key();
        switch (evt.action) {
        case KEY_QUIT:
            running = false;
            break;
        case KEY_ENTER:
            if (g_state == STATE_DISAMBIG) {
                /* User selected an option — dispatch with the chosen expression */
                if (g_disambig_cursor < g_disambig_count) {
                    const char *chosen = g_disambig_exprs[g_disambig_cursor];
                    ce_domain_t domain;
                    ast_node_t *root = NULL;
                    bool had_equals = false;
                    g_disambig_active = false;
                    g_state = STATE_INPUT;
                    if (parse_for_domain(chosen, &root, &had_equals, &domain)) {
                        dispatch_to_module(domain, chosen, root, had_equals);
                        ast_free(root);
                    } else {
                        g_state = STATE_ERROR;
                        snprintf(g_error, sizeof(g_error), "Could not parse selected operation");
                    }
                }
            } else if (g_state == STATE_RESULT && g_ckpt_nav_active) {
                /* Advance to next checkpoint */
                int total_ckpts = steps_checkpoint_count();
                if (g_ckpt_reveal < total_ckpts) {
                    g_ckpt_reveal++;
                } else {
                    /* All checkpoints revealed — ENTER now exits step mode */
                    g_ckpt_nav_active = false;
                }
            } else {
                process_enter();
            }
            break;
        case KEY_MODE:
            if (g_state == STATE_DISAMBIG) {
                /* MODE cancels disambiguation */
                g_disambig_active = false;
                g_state = STATE_INPUT;
            } else if (g_state == STATE_RESULT && g_ckpt_nav_active) {
                /* MODE finishes checkpoint stepping and shows all steps at once */
                g_ckpt_nav_active = false;
                g_ckpt_reveal = steps_checkpoint_count();
            }
            break;
        case KEY_CLEAR:
            if (g_state == STATE_DISAMBIG) {
                g_disambig_active = false;
                g_state = STATE_INPUT;
            } else if (g_state == STATE_RESULT || g_state == STATE_ERROR) {
                g_state = STATE_INPUT;
                g_show_steps = false;
                g_ckpt_nav_active = false;
                g_ckpt_reveal = 0;
            } else if (g_input.len > 0) {
                input_buf_clear(&g_input);
            }
            break;
        case KEY_DEL:
            input_buf_backspace(&g_input);
            g_state = STATE_INPUT;
            break;
        case KEY_DEL_FORWARD:
            input_buf_delete(&g_input);
            g_state = STATE_INPUT;
            break;
        case KEY_CHAR:
            input_buf_insert(&g_input, evt.ch);
            g_state = STATE_INPUT;
            break;
        case KEY_STR:
            input_buf_insert_str(&g_input, evt.str);
            g_state = STATE_INPUT;
            break;
        case KEY_UP:
            if (g_state == STATE_DISAMBIG) {
                if (g_disambig_cursor > 0) g_disambig_cursor--;
            } else if (g_state == STATE_RESULT && g_show_steps && g_scroll_pos > 0) {
                g_scroll_pos--;
            } else if (g_state == STATE_INPUT) {
                mp_cursor_up(&g_input);
            }
            break;
        case KEY_DOWN:
            if (g_state == STATE_DISAMBIG) {
                if (g_disambig_cursor + 1 < g_disambig_count) g_disambig_cursor++;
            } else if (g_state == STATE_RESULT && g_show_steps) {
                g_scroll_pos++;
            } else if (g_state == STATE_INPUT) {
                mp_cursor_down(&g_input);
            }
            break;
        default:
            break;
        }
        redraw();
        if (evt.action == KEY_NONE) delay(10);
    }

    ui_shutdown();
    return 0;
}
