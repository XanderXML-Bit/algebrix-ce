/* Exercise both polynomial converters without processor formatting/routing. */
#include <stdio.h>
#include "memory.h"
#include "token.h"
#include "parser.h"
#include "algebra.h"

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        static tokenizer_t tok;
        static parser_t parser;
        ast_node_t *expr;
        poly_t p;
        int k;
        mem_free_all();
        tok_init(&tok, argv[i]);
        if (tok_tokenize(&tok) != CE_OK) return 2;
        parser_init(&parser, &tok);
        expr = parser_parse(&parser);
        if (!expr) return 3;
        if (!algebra_ast_to_poly(expr, 'x', &p)) {
            puts("refused");
        } else {
            printf("%d", p.degree);
            for (k = 0; k <= p.degree; k++) printf(" %.9g", (double)p.coeff[k]);
            putchar('\n');
        }
        ast_free(expr);
        if (mem_nodes_used() || mem_double_frees()) return 4;
    }
    return 0;
}
