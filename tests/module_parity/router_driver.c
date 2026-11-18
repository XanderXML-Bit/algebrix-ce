/*
 * router_driver.c — run one CE router module's main() on the desktop and
 * print the program it forwards to.
 *
 * Router modules (CEALG, CECINT, CECLIM, CECPOL, CECIAD) link no processor.
 * They read the request AST and forward it to a worker with
 * ce_module_runtime_forward().  tests/cemu/test_op_dispatch_parity.py
 * compiles the router's own main.c with its PROGRAM_CFLAGS plus
 * -Dmain=ce_router_main, together with this driver.  The driver replaces
 * src/split/module_runtime.c and src/split/module_forward.c: it hands the
 * router the AST and records the first program the router forwards to.
 *
 * For each argv expression the driver parses it on the "launcher" side,
 * serializes it with astwire, deserializes it on the "router" side, and runs
 * the router.  It prints one line per expression: the forwarded program, or
 * "-" when the router answered without forwarding, or "parse error".
 */
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tice.h>

#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "parser.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/module_runtime.h"

int ce_router_main(void);

static ast_node_t *g_root;
static char g_forwarded[16];

/* ipc.c references os_RunPrgm for module hand-off; the stubs never launch. */
int os_RunPrgm(const char *prgm, void *data, size_t size,
               os_runprgm_callback_t callback)
{
    (void)prgm; (void)data; (void)size; (void)callback;
    return -1;
}

void ce_module_runtime_mark_reentry(void) {}
bool ce_module_runtime_was_reentered(void) { return false; }
void ce_module_runtime_reset_reentry(void) {}

bool ce_module_runtime_load(ce_request_t *req, ce_response_t *resp,
                            ce_shared_state_t *state, ast_node_t **root_out,
                            bool *had_equals_out)
{
    memset(req, 0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    memset(state, 0, sizeof(*state));
    *root_out = g_root;
    *had_equals_out = false;
    return g_root != NULL;
}

bool ce_module_runtime_resume_forward(const char *self_program,
                                      ce_request_t *req,
                                      ce_shared_state_t *state)
{
    (void)self_program; (void)req; (void)state;
    return false;
}

bool ce_module_runtime_resume_peek(const char *self_program,
                                   ce_request_t *req,
                                   ce_shared_state_t *state,
                                   ce_response_t *resp,
                                   char *child_out,
                                   size_t child_out_size)
{
    (void)self_program; (void)req; (void)state; (void)resp;
    if (child_out && child_out_size) child_out[0] = '\0';
    return false;
}

void ce_module_runtime_reclaim_forward(const char *self_program,
                                       ce_shared_state_t *state)
{
    (void)self_program; (void)state;
}

bool ce_module_runtime_recover_child_failure(const char *self_program,
                                             const char *pending_target,
                                             ce_request_t *req,
                                             ce_shared_state_t *state,
                                             char *err_buf,
                                             size_t err_buf_size)
{
    (void)self_program; (void)pending_target; (void)req; (void)state;
    if (err_buf && err_buf_size) err_buf[0] = '\0';
    return false;
}

void ce_module_runtime_finish(const ce_request_t *req,
                              const ce_response_t *resp,
                              ce_shared_state_t *state)
{
    (void)req; (void)resp; (void)state;
}

/* Record only the first hop.  Returning false makes the router report a
 * launch failure and finish, so it never tries a second program. */
bool ce_module_runtime_forward(const char *caller,
                               const char *program,
                               ce_request_t *req,
                               ce_shared_state_t *state,
                               ce_response_t *resp,
                               char *err_buf,
                               size_t err_buf_size)
{
    (void)caller; (void)req; (void)state;
    if (!g_forwarded[0] && program) {
        strncpy(g_forwarded, program, sizeof(g_forwarded) - 1);
        g_forwarded[sizeof(g_forwarded) - 1] = '\0';
    }
    if (resp) memset(resp, 0, sizeof(*resp));
    if (err_buf && err_buf_size) {
        snprintf(err_buf, err_buf_size, "router_driver: forward stopped");
    }
    return false;
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        static uint8_t wire[CE_AST_WIRE_MAX];
        uint16_t wire_size = 0;
        ast_node_t *parsed;
        static tokenizer_t tok;
        static parser_t parser;

        mem_free_all();
        g_forwarded[0] = '\0';

        tok_init(&tok, argv[i]);
        if (tok_tokenize(&tok) != CE_OK) {
            printf("parse error\n");
            continue;
        }
        parser_init(&parser, &tok);
        parsed = parser_parse(&parser);
        if (!parsed ||
            !ce_ast_serialize(parsed, wire, (uint16_t)sizeof(wire),
                              &wire_size)) {
            printf("parse error\n");
            continue;
        }
        ast_free(parsed);
        mem_free_all();

        g_root = ce_ast_deserialize(wire, wire_size);
        if (!g_root) {
            printf("parse error\n");
            continue;
        }
        ce_router_main();
        printf("%s\n", g_forwarded[0] ? g_forwarded : "-");
    }
    return 0;
}
