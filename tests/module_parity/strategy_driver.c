/*
 * strategy_driver.c — run one CE integration strategy worker's main() on the
 * desktop.
 *
 * The strategy workers (CEIPRT, CEITRG, CEIUSB, CEIPFR) have no processor;
 * src/modules/calc_int/strategy_main.c reads the request AST, calls
 * integrate_with_strategy() and formats "int = <result> + C".
 * tests/cemu/test_module_flag_parity.py compiles that file with the worker's
 * own PROGRAM_CFLAGS plus -Dmain=ce_worker_main, together with this driver,
 * which replaces src/split/module_runtime.c: ce_module_runtime_load() hands
 * the worker the AST, ce_module_runtime_finish() captures its response.
 *
 * For each argv expression the driver parses it on the "launcher" side,
 * serializes it with astwire, deserializes it on the "worker" side and runs
 * the worker.  Output is one line per expression: "<ok>\t<result or error>".
 *
 * An argument of the form "--free=N" (N >= 0) applies to the following
 * expressions: before the worker runs, nodes are taken from the pool until
 * only N are free, so pool exhaustion part-way through integration can be
 * tested.  "--free=-1" turns that off again.
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

int ce_worker_main(void);

static ast_node_t *g_root;
static ce_response_t g_out;
static bool g_finished;

/* ipc.c references os_RunPrgm for module hand-off; workers never launch. */
int os_RunPrgm(const char *prgm, void *data, size_t size,
               os_runprgm_callback_t callback)
{
    (void)prgm; (void)data; (void)size; (void)callback;
    return -1;
}

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

void ce_module_runtime_finish(const ce_request_t *req,
                              const ce_response_t *resp,
                              ce_shared_state_t *state)
{
    (void)req; (void)state;
    g_out = *resp;
    g_finished = true;
}

static void print_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') {
            fputs("\\n", stdout);
        } else {
            putchar(*s);
        }
    }
}

int main(int argc, char **argv)
{
    int i;
    long free_target = -1;

    for (i = 1; i < argc; i++) {
        static uint8_t wire[CE_AST_WIRE_MAX];
        uint16_t wire_size = 0;
        ast_node_t *parsed;
        uint16_t before, squeezed, dfree;
        static tokenizer_t tok;
        static parser_t parser;

        if (strncmp(argv[i], "--free=", 7) == 0) {
            free_target = strtol(argv[i] + 7, NULL, 10);
            continue;
        }

        mem_free_all();
        memset(&g_out, 0, sizeof(g_out));
        g_finished = false;

        tok_init(&tok, argv[i]);
        if (tok_tokenize(&tok) != CE_OK) {
            printf("0\ttokenize error\t-\t-\n");
            continue;
        }
        parser_init(&parser, &tok);
        parsed = parser_parse(&parser);
        if (!parsed) {
            printf("0\tparse error\t-\t-\n");
            continue;
        }
        if (!ce_ast_serialize(parsed, wire, (uint16_t)sizeof(wire),
                              &wire_size)) {
            printf("0\tserialize error\t-\t-\n");
            continue;
        }
        ast_free(parsed);
        mem_free_all();

        g_root = ce_ast_deserialize(wire, wire_size);
        if (!g_root) {
            printf("0\tdeserialize error\t-\t-\n");
            continue;
        }
        before = mem_nodes_used();
        if (free_target >= 0) {
            while ((long)mem_nodes_free() > free_target) {
                if (!mem_alloc_node()) break;
            }
        }
        squeezed = mem_nodes_used() - before;
        dfree = mem_double_frees();
        ce_worker_main();
        if (!g_finished) {
            printf("0\tworker did not finish\t-\t-\n");
            continue;
        }
        printf("%d\t", g_out.ok ? 1 : 0);
        print_escaped(g_out.ok ? g_out.result : g_out.error);
        printf("\t%u\t%u\n", (unsigned)(mem_nodes_used() - squeezed),
               (unsigned)(uint16_t)(mem_double_frees() - dfree));
    }
    return 0;
}
