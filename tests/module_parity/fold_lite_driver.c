/*
 * fold_lite_driver.c — trees that 127-character input cannot build.
 *
 * tests/cemu/test_module_flag_parity.py compiles this with src/fold_lite.c,
 * src/ast.c and src/memory.c (ASan/UBSan when available, freed pool nodes
 * poisoned).  For each k in argv it builds
 *
 *     -(-(...-( 2 * (((x/1)/1).../1) )...))     k negations, CHAIN divisions
 *
 * so the product sits at depth k and, once k passes FOLD_LITE_MAX_DEPTH, its
 * division chain lies past the depth cap and was never folded.  The
 * cancellation of 2*(E/1) must then strip the whole chain without recursing
 * once per division (review of PR #108).  Prints one line per k:
 *
 *     <k>\t<result>\t<nodes in use after freeing the result>\t<double frees>
 */
#include <stdio.h>
#include <stdlib.h>

#include "calcengine.h"
#include "ast.h"
#include "memory.h"
#include "fold_lite.h"

#define CHAIN 200

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        static char buf[512];
        int k = atoi(argv[i]);
        int j;
        uint16_t dfree;
        ast_node_t *t;

        mem_free_all();
        dfree = mem_double_frees();
        t = ast_variable('x');
        for (j = 0; j < CHAIN && t; j++) t = ast_binop(OP_DIV, t, ast_number(1));
        if (t) t = ast_binop(OP_MUL, ast_number(2), t);
        for (j = 0; j < k && t; j++) t = ast_unop(UOP_NEG, t);
        if (!t || !ast_tree_complete(t)) {
            printf("%d\tbuild failed\t0\t0\n", k);
            continue;
        }
        t = ast_fold_lite(t);
        ast_to_str(t, buf, sizeof(buf));
        ast_free(t);
        printf("%d\t%s\t%u\t%u\n", k, buf, (unsigned)mem_nodes_used(),
               (unsigned)(uint16_t)(mem_double_frees() - dfree));
    }
    return 0;
}
