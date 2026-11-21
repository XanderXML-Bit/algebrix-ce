/*
 * launcher_route_harness.c — print which program the real CALCENG launcher
 * (src/core/launcher/main.c) dispatches each input to.
 *
 * The launcher picks the worker with its own route_program() and
 * route_polar_program(); src/split/classify.c only supplies the fallback
 * domain.  The two disagreed for eigenval()/eigenvec(): classify.c said
 * CE_DOMAIN_MATRIX, but route_polar_program() sent them to CEBASE, whose
 * evaluator answers "Polar/advanced op (use at top level)".
 *
 * This file includes main.c so the static routing functions can be called
 * directly.  For each argv input it prints one line:
 *     "<program>\t<classify domain program>"
 * or "parse error".  Built and run by tests/cemu/test_module_flag_parity.py
 * (test_launcher_routes_eigen_to_cemat) with
 * split/Makefile.launcher's PROGRAM_CFLAGS.
 */
#define main launcher_main
#include "core/launcher/main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

/* TI-OS calls made only by launcher_main(), which never runs here. */
static void not_used(const char *name)
{
    fprintf(stderr, "launcher_route_harness: %s called\n", name);
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
        if (!parse_for_domain(argv[i], &root, &had_equals, &domain)) {
            printf("parse error\n");
            continue;
        }
        printf("%s\t%s\n", route_program(root, had_equals, domain),
               ce_domain_program(domain));
    }
    return 0;
}
