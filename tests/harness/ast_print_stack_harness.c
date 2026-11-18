/* Instrument the production printer, including its private traversal function. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/ast.c"

/* The shape battery does not use statistics, but ast.c links this name API. */
const char *stat_fn_name(stat_fn_t fn)
{
    (void)fn;
    return "stat";
}

static unsigned active, high;
void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *fn, void *caller)
{
    (void)caller;
    if (fn == (void *)ast_print) {
        ++active;
        if (active > high) high = active;
    }
}
void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *fn, void *caller)
{
    (void)caller;
    if (fn == (void *)ast_print) --active;
}

/* Pin continuations that do not occur in the single-spine depth shapes. */
static void check_fixed(const ast_node_t *node, const char *expected, int term_wrap)
{
    char text[128], term[128];
    int mode, size;
    snprintf(term, sizeof(term), "(%s)", expected);
    for (mode = 0; mode < 3; ++mode) {
        int (*print)(const ast_node_t *, char *, int) = mode == 0
            ? ast_to_str : mode == 1 ? ast_to_display_str : ast_to_display_term;
        const char *want = mode == 2 && CE_AST_DISPLAY_PRINT && term_wrap ? term : expected;
        high = 0;
        assert(print(node, text, sizeof(text)) == (int)strlen(want));
        assert(strcmp(text, want) == 0);
        assert(high == 1);
        for (size = 0; size <= (int)strlen(want) + 2; ++size) {
            int got;
            memset(text, 0x5a, sizeof(text));
            got = print(node, text + 1, size);
            assert(text[0] == 0x5a && text[size + 1] == 0x5a);
            if (size) assert(got < size && text[got + 1] == '\0');
        }
    }
}

static void fixed_continuations(void)
{
    ast_node_t x = {0}, y = {0}, one = {0}, two = {0};
    ast_node_t p = {0}, q = {0}, root = {0};
    x.type = y.type = NODE_VARIABLE;
    x.d.variable = 'X'; y.d.variable = 'Y';
    one.type = two.type = NODE_NUMBER;
    one.d.number = 1; two.d.number = 2;
    p.type = q.type = NODE_PAIR;
    p.d.pair.head = &x; p.d.pair.tail = &q;
    q.d.pair.head = &y;
    check_fixed(&p, "X,Y", 1);
    p.d.pair.tail = &y;
    check_fixed(&p, "X,", 1);
    root.type = NODE_PIECEWISE;
    check_fixed(&root, "piecewise()", 0);
    root.d.piecewise.pieces = &x;
    check_fixed(&root, "piecewise()", 0);
    root.d.piecewise.pieces = &p;
    check_fixed(&root, "piecewise(X, )", 0);
    p.d.pair.tail = &q;
    check_fixed(&root, "piecewise(X, Y)", 0);
    memset(&root, 0, sizeof(root));
    root.type = NODE_STORE;
    root.d.store.expr = &x;
    check_fixed(&root, "X->L1", 1);
    memset(&root, 0, sizeof(root));
    root.type = NODE_POLAR;
    root.d.polar.op = POLOP_USUB;
    root.d.polar.var = 'X';
    root.d.polar.expr1 = &x; root.d.polar.expr2 = &y;
    root.d.polar.lo = &one; root.d.polar.hi = &two;
    check_fixed(&root, "usub(X,Y,X,1,2)", 0);
    root.d.polar.expr2 = NULL;
    check_fixed(&root, "usub(X,X,1,2)", 0);
    root.d.polar.expr2 = &y; root.d.polar.lo = NULL;
    check_fixed(&root, "usub(X,Y,X)", 0);
    memset(&root, 0, sizeof(root));
    root.type = NODE_TAYLOR;
    root.d.taylor.var = 'X'; root.d.taylor.expr = &x;
    root.d.taylor.center = &one; root.d.taylor.degree = &two;
    check_fixed(&root, "taylor(X,X,1,2)", 0);
    memset(&root, 0, sizeof(root));
    root.type = NODE_LIMIT;
    root.d.limit.var = 'X'; root.d.limit.expr = &x;
    root.d.limit.point = &one; root.d.limit.direction = -1;
    check_fixed(&root, "lim(X,X,1,-)", 0);
    root.d.limit.direction = 1;
    check_fixed(&root, "lim(X,X,1,+)", 0);
    check_fixed(NULL, "", 0);
}

/* Out-of-contract objects must not overrun the continuation arrays. */
static void over_pool_guard(void)
{
    static ast_node_t nodes[NODE_POOL_SIZE + 1];
    static char text[4096];
    int i, mode;
    nodes[0].type = NODE_VARIABLE;
    nodes[0].d.variable = 'X';
    for (i = 1; i <= NODE_POOL_SIZE; ++i) {
        nodes[i].type = NODE_FUNC;
        nodes[i].d.func.id = FN_SIN;
        nodes[i].d.func.arg = &nodes[i - 1];
    }
    for (mode = 0; mode < 3; ++mode) {
        int length;
        int (*print)(const ast_node_t *, char *, int) = mode == 0
            ? ast_to_str : mode == 1 ? ast_to_display_str : ast_to_display_term;
        high = 0;
        length = print(&nodes[NODE_POOL_SIZE], text, sizeof(text));
        assert(high == 1);
        assert(length == 4 * NODE_POOL_SIZE + 3);
        assert(strcmp(text + length - 3, "...") == 0);
        check_fixed(&nodes[0], "X", 0);
    }
}

int main(void)
{
    static ast_node_t nodes[NODE_POOL_SIZE];
    static char full[4096], clipped[4096];
    int shape, mode, count, size;
    fixed_continuations();
    over_pool_guard();
    for (shape = 0; shape < 5; ++shape) {
        memset(nodes, 0, sizeof(nodes));
        nodes[0].type = NODE_VARIABLE;
        nodes[0].d.variable = 'X';
        for (count = 1; count < NODE_POOL_SIZE; ++count) {
            ast_node_t *n = &nodes[count];
            if (shape == 0) {
                n->type = NODE_UNOP;
                n->d.unop.op = UOP_NEG;
                n->d.unop.operand = n - 1;
            } else if (shape == 1) {
                n->type = NODE_FUNC;
                n->d.func.id = FN_SIN;
                n->d.func.arg = n - 1;
            } else if (shape == 2 || shape == 3) {
                n->type = NODE_BINOP;
                n->d.binop.op = OP_ADD;
                /* NULL children exercise incomplete pool-exhausted trees. */
                if (shape == 2) n->d.binop.left = n - 1;
                else n->d.binop.right = n - 1;
            } else {
                n->type = NODE_PAIR;
                n->d.pair.head = n - 1;
            }
            for (mode = 0; mode < 3; ++mode) {
                int (*print)(const ast_node_t *, char *, int) = mode == 0
                    ? ast_to_str : mode == 1 ? ast_to_display_str : ast_to_display_term;
                int length;
                high = 0;
                length = print(n, full, sizeof(full));
                if (high != 1) {
                    fprintf(stderr, "recursive printer: shape=%d nodes=%d mode=%d depth=%u\n",
                            shape, count + 1, mode, high);
                    return 1;
                }
                assert(length == (int)strlen(full));
                {
                    int canonical = mode == 0 || !CE_AST_DISPLAY_PRINT;
                    int expected;
                    if (shape == 0) expected = canonical ? 3 * count + 1 : 3 * count - 1;
                    else if (shape == 1) expected = 5 * count + 1;
                    else if (shape == 2) expected = canonical ? 3 * count + 1 : count + 1;
                    else if (shape == 3) expected = canonical ? 3 * count + 1 : 3 * count - 1;
                    else expected = 1;
                    if (mode == 2 && !canonical && shape != 1)
                        expected += 2;
                    assert(length == expected);
                }
                for (size = 0; size <= length + 2; ++size) {
                    if (count != NODE_POOL_SIZE - 1 && size > 8 &&
                        size != 59 && size != 60 && size != 119 && size != 120 &&
                        size != 127 && size != 128 && size < length - 1) continue;
                    int got;
                    memset(clipped, 0x5a, sizeof(clipped));
                    got = print(n, clipped + 1, size);
                    assert(clipped[0] == 0x5a);
                    assert(clipped[size + 1] == 0x5a);
                    if (!size) { assert(got == 0); continue; }
                    assert(got >= 0 && got < size);
                    assert(clipped[got + 1] == '\0');
                    if (size > length) assert(strcmp(full, clipped + 1) == 0);
                    else if (!mode) assert(strncmp(full, clipped + 1, (size_t)got) == 0);
                }
            }
        }
    }
    puts("bounded printer: 5 shapes, 2..512 nodes, 3 APIs, boundary buffers and full-depth sweeps PASS");
    return 0;
}
