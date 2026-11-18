/*
 * op_dispatch_harness.c — print how the CALCENG launcher classifies and
 * routes each input.
 *
 * tests/cemu/test_op_dispatch_parity.py builds this file with the
 * PROGRAM_CFLAGS of split/Makefile.launcher.  It includes
 * src/core/launcher/main.c, so the static routing functions
 * (parse_for_domain(), route_program()) are the ones CALCENG.8xp runs.
 *
 * For each argv input it prints one tab-separated line:
 *     <node type> <polar op or -1> <classify domain> <ce_domain_program()>
 *     <launcher route_program()> <wire node type> <wire polar op or -1>
 * or "parse error".  The wire columns come from the tree a worker receives:
 * the AST after ce_ast_serialize() and ce_ast_deserialize() (src/split/
 * astwire.c), or -2 -2 when the round trip fails.  The node type, polar op,
 * and domain are the enum values from ast.h and split/ipc.h.  The test maps
 * them to names.
 */
#define main launcher_main
#include "core/launcher/main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

/* TI-OS calls made only by launcher_main(), which never runs here. */
static void not_used(const char *name)
{
    fprintf(stderr, "op_dispatch_harness: %s called\n", name);
    exit(3);
}
void delay(unsigned int ms) { (void)ms; not_used("delay"); }
void os_ClrHome(void) { not_used("os_ClrHome"); }
uint8_t os_GetCSC(void) { not_used("os_GetCSC"); return 0; }
uint24_t os_PutStrFull(const char *s) { (void)s; not_used("os_PutStrFull"); return 0; }
uint24_t os_PutStrLine(const char *s) { (void)s; not_used("os_PutStrLine"); return 0; }
void os_SetCursorPos(uint8_t row, uint8_t col)
{
    (void)row; (void)col;
    not_used("os_SetCursorPos");
}
int os_RunPrgm(const char *prgm, void *data, size_t size,
               os_runprgm_callback_t callback)
{
    (void)prgm; (void)data; (void)size; (void)callback;
    not_used("os_RunPrgm");
    return -1;
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        ast_node_t *root = NULL;
        bool had_equals = false;
        ce_domain_t domain = CE_DOMAIN_UNKNOWN;
        if (!parse_for_domain(argv[i], &root, &had_equals, &domain) || !root) {
            printf("parse error\n");
            continue;
        }
        static uint8_t wire[CE_AST_WIRE_MAX];
        uint16_t wire_size = 0;
        ast_node_t *decoded = NULL;
        int wire_type = -2, wire_op = -2;

        printf("%d\t%d\t%d\t%s\t%s", (int)root->type,
               root->type == NODE_POLAR ? (int)root->d.polar.op : -1,
               (int)domain, ce_domain_program(domain),
               route_program(root, had_equals, domain));
        if (ce_ast_serialize(root, wire, (uint16_t)sizeof(wire), &wire_size)) {
            mem_free_all();
            decoded = ce_ast_deserialize(wire, wire_size);
        }
        if (decoded) {
            wire_type = (int)decoded->type;
            wire_op = decoded->type == NODE_POLAR ? (int)decoded->d.polar.op : -1;
        }
        printf("\t%d\t%d\n", wire_type, wire_op);
    }
    return 0;
}
