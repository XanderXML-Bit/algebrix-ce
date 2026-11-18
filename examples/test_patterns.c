/*
 * test_patterns.c — Example: Test harness patterns for CalcEngine
 *
 * Demonstrates the custom assertion macros, test helpers, and organization
 * patterns used in the CalcEngine test suite.
 *
 * THIS IS A REFERENCE EXAMPLE — see tests/test_main.c for the full test suite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "ast.h"
#include "parser.h"
#include "eval.h"
#include "simplify.h"

/* ── Global settings (defined here since test_main.c replaces main.c) */
settings_t g_settings = { ANGLE_RADIANS, true, 6 };

/* ── Test infrastructure ─────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; \
           printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
} while(0)

#define ASSERT_EQ_INT(a, b, msg) ASSERT_TRUE((a) == (b), msg)
#define ASSERT_EQ_FLOAT(a, b, tol, msg) ASSERT_TRUE(fabsf((a)-(b)) < (tol), msg)
#define ASSERT_STR_CONTAINS(haystack, needle, msg) \
    ASSERT_TRUE(strstr(haystack, needle) != NULL, msg)

/* ── Test helpers ────────────────────────────────────────── */

/* Helper: parse an expression string into an AST */
static ast_node_t *parse_str(const char *input)
{
    static tokenizer_t tok;
    static parser_t    par;

    tok_init(&tok, input);
    if (tok_tokenize(&tok) != CE_OK) return NULL;
    parser_init(&par, &tok);
    return parser_parse(&par);
}

/* Helper: parse and evaluate an expression numerically */
static ce_float_t eval_str(const char *input, ce_error_t *err)
{
    ast_node_t *root;
    eval_ctx_t ctx;
    ce_float_t val;

    *err = CE_OK;
    root = parse_str(input);
    if (!root) { *err = CE_ERR_SYNTAX; return 0; }

    eval_init(&ctx);
    val = eval_expr(&ctx, root);
    *err = eval_error(&ctx);
    ast_free(root);
    return val;
}

/* Helper: parse, evaluate with x bound */
static ce_float_t eval_str_at(const char *input, char var,
                               ce_float_t x, ce_error_t *err)
{
    ast_node_t *root;
    eval_ctx_t ctx;
    ce_float_t val;

    *err = CE_OK;
    root = parse_str(input);
    if (!root) { *err = CE_ERR_SYNTAX; return 0; }

    eval_init(&ctx);
    eval_set_var(&ctx, var, x);
    val = eval_expr(&ctx, root);
    *err = eval_error(&ctx);
    ast_free(root);
    return val;
}

/* ═══════════════════════════════════════════════════════════ */
/*                    EXAMPLE: MEMORY POOL TESTS              */
/* ═══════════════════════════════════════════════════════════ */

static void test_memory(void)
{
    ast_node_t *nodes[10];
    int i;

    printf("=== Memory Pool ===\n");

    mem_init();
    ASSERT_EQ_INT(mem_nodes_used(), 0, "init: 0 used");
    ASSERT_EQ_INT(mem_nodes_free(), NODE_POOL_SIZE, "init: all free");

    /* Allocate a few nodes */
    for (i = 0; i < 10; i++) {
        nodes[i] = mem_alloc_node();
        ASSERT_TRUE(nodes[i] != NULL, "alloc succeeds");
    }
    ASSERT_EQ_INT(mem_nodes_used(), 10, "10 nodes used");

    /* Free them */
    for (i = 0; i < 10; i++)
        mem_free_node(nodes[i]);
    ASSERT_EQ_INT(mem_nodes_used(), 0, "all freed");

    mem_free_all();
}

/* ═══════════════════════════════════════════════════════════ */
/*                    EXAMPLE: EVALUATOR TESTS                */
/* ═══════════════════════════════════════════════════════════ */

static void test_evaluator(void)
{
    ce_error_t err;

    printf("=== Evaluator ===\n");

    mem_free_all(); /* Always reset pool before test group */

    ASSERT_EQ_FLOAT(eval_str("2+3", &err), 5.0f, 1e-5f, "2+3 = 5");
    ASSERT_EQ_INT(err, CE_OK, "no error");

    ASSERT_EQ_FLOAT(eval_str("2^10", &err), 1024.0f, 1e-5f, "2^10 = 1024");

    /* Test with variable binding */
    ASSERT_EQ_FLOAT(eval_str_at("x^2", 'x', 3.0f, &err), 9.0f, 1e-5f, "x^2 at x=3");

    mem_free_all();
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    mem_init();

    test_memory();
    test_evaluator();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
