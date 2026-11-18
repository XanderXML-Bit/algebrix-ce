/*
 * module_driver.c — run one CE module's processor on the desktop.
 *
 * tests/cemu/test_module_flag_parity.py compiles this file together with a
 * split module's PROGRAM_SOURCES and PROGRAM_CFLAGS (read from
 * split/Makefile.<module>), so the processor is built with exactly the
 * feature flags the .8xp is built with.  The desktop test runner builds every
 * feature in (CE_PROC_CALC=1, simplify linked), which is how an on-target gap
 * such as "derivatives are not simplified in CECALC" went unseen.
 *
 * For each argv expression the driver mirrors the launcher -> worker path:
 * parse on the "launcher" side, serialize the AST with astwire, deserialize
 * it on the "module" side and call ce_processor_run_preparsed().  It prints
 * one line per expression: "<ok>\t<result>" (newlines in the result are
 * shown as "\n").
 *
 * An argument of the form "--free=N" (N >= 0) applies to the following
 * expressions: before the processor runs, nodes are taken from the pool
 * until only N are free, so pool exhaustion part-way through a computation
 * can be tested.  "--free=-1" turns that off again. "--steps=1" appends
 * each CE_RESP step with a |STEP| separator; "--steps=0" suppresses only
 * the driver dump. Show work is a launcher setting, not a processor argument.
 * "--alloc-stats=1" appends |ALLOC|N, the failed allocations during the run.
 *
 * Each output line ends with two more tab-separated counters: nodes still
 * in use after the run (minus the nodes taken by --free), i.e. leaked, and
 * the number of double frees the pool ignored during the run ("-" when the
 * expression never reached the module, e.g. a parse error).  The parity
 * test builds with -DMEM_POISON_ON_FREE, so a read through a freed node
 * crashes under ASan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tice.h>

#include "calcengine.h"
#include "memory.h"
#include "steps.h"
#include "token.h"
#include "parser.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/processor.h"

static ce_response_t g_resp;

/* ipc.c references os_RunPrgm for module hand-off.  The processor path never
 * launches another program, so the driver only needs the symbol.  It is
 * defined here instead of linking tests/mock/tice_mock.c, which would clash
 * with the module's own src/split/runtime_globals.c. */
int os_RunPrgm(const char *prgm, void *data, size_t size,
               os_runprgm_callback_t callback)
{
    (void)prgm; (void)data; (void)size; (void)callback;
    return -1;
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
    bool dump_steps = false;
    bool dump_allocs = false;

    for (i = 1; i < argc; i++) {
        static uint8_t wire[CE_AST_WIRE_MAX];
        uint16_t wire_size = 0;
        ast_node_t *parsed;
        ast_node_t *root;
        uint16_t before, squeezed, dfree, allocs;
        static tokenizer_t tok;
        static parser_t parser;

        if (strncmp(argv[i], "--free=", 7) == 0) {
            free_target = strtol(argv[i] + 7, NULL, 10);
            continue;
        }

        if (strncmp(argv[i], "--steps=", 8) == 0) {
            dump_steps = atoi(argv[i] + 8) != 0;
            continue;
        }
        if (strncmp(argv[i], "--alloc-stats=", 14) == 0) {
            dump_allocs = atoi(argv[i] + 14) != 0;
            continue;
        }
        mem_free_all();
        steps_clear(); /* Real workers start each request with a fresh recorder. */
        memset(&g_resp, 0, sizeof(g_resp));

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

        root = ce_ast_deserialize(wire, wire_size);
        if (!root) {
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
        allocs = mem_alloc_failures();
        ce_processor_run_preparsed(argv[i], root, false, &g_resp);
        printf("%d\t", g_resp.ok ? 1 : 0);
        print_escaped(g_resp.ok ? g_resp.result : g_resp.error);
        if (dump_steps) {
            int k;
            for (k = 0; k < g_resp.step_count; k++) {
                fputs("|STEP|", stdout);
                print_escaped(g_resp.steps[k]);
            }
        }
        if (dump_allocs)
            printf("|ALLOC|%u", (unsigned)(uint16_t)(mem_alloc_failures() - allocs));
        printf("\t%u\t%u\n", (unsigned)(mem_nodes_used() - squeezed),
               (unsigned)(uint16_t)(mem_double_frees() - dfree));
    }
    return 0;
}
