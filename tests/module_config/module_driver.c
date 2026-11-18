/*
 * module_driver.c - desktop driver for one split CE worker's build config.
 *
 * tests/cemu/test_module_config_desktop.py compiles this file together with
 * the PROGRAM_SOURCES of a split/Makefile.<module> (minus the CE entry point)
 * and that makefile's exact PROGRAM_CFLAGS, plus -DDESKTOP_BUILD. The full
 * desktop runner (tests/test_main.c) builds every processor feature at once,
 * so it cannot see a worker whose own feature flags compile its job out
 * (an internal review: CECIDF answered "Feature is routed to another module").
 *
 * "--deg" and "--rad" arguments switch the angle mode for the expressions
 * after them. After "--ans", each line also ends "|ANS=<g>": the Ans the
 * response carries, with Ans set to -12345 before each expression so an
 * unchanged Ans is visible. For each argv expression the driver parses it, sends the AST through the
 * same wire format the launcher uses, runs ce_processor_run_preparsed() and
 * prints one line:  OK|<result>   or   ERR|<error>
 * Newlines in the result are printed as the two characters "\n".
 */
#include <stdio.h>
#include <string.h>

#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "parser.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/processor.h"

extern ce_float_t g_last_answer;
static int s_print_ans;

static void print_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", stdout);
        else putchar(*s);
    }
}

static int run_one(const char *input)
{
    static tokenizer_t tok;
    static parser_t par;
    static uint8_t wire[CE_AST_WIRE_MAX];
    static ce_response_t resp;
    ast_node_t *root;
    ast_node_t *decoded;
    uint16_t wire_size = 0;
    bool had_equals = strchr(input, '=') != NULL;

    mem_free_all();
    tok_init(&tok, input);
    if (tok_tokenize(&tok) != CE_OK) {
        printf("ERR|tokenize failed\n");
        return 1;
    }
    parser_init(&par, &tok);
    root = parser_parse(&par);
    if (!root) {
        printf("ERR|parse failed\n");
        return 1;
    }
    if (!ce_ast_serialize(root, wire, sizeof(wire), &wire_size)) {
        printf("ERR|serialize failed\n");
        return 1;
    }
    ast_free(root);
    decoded = ce_ast_deserialize(wire, wire_size);
    if (!decoded) {
        printf("ERR|deserialize failed\n");
        return 1;
    }

    memset(&resp, 0, sizeof(resp));
    if (s_print_ans) g_last_answer = -12345;
    ce_processor_run_preparsed(input, decoded, had_equals, &resp);
    if (resp.ok) {
        fputs("OK|", stdout);
        print_escaped(resp.result);
    } else {
        fputs("ERR|", stdout);
        print_escaped(resp.error);
    }
    if (s_print_ans) printf("|ANS=%g", (double)resp.last_answer);
    putchar('\n');
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    int rc = 0;

    mem_init();
    for (i = 1; i < argc; i++) {
        /* "--deg" / "--rad" switch the angle mode for later expressions. */
        if (strcmp(argv[i], "--deg") == 0) { g_settings.angle_mode = ANGLE_DEGREES; continue; }
        if (strcmp(argv[i], "--rad") == 0) { g_settings.angle_mode = ANGLE_RADIANS; continue; }
        if (strcmp(argv[i], "--ans") == 0) { s_print_ans = 1; continue; }
        rc |= run_one(argv[i]);
    }
    return rc;
}
