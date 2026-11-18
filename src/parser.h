/*
 * parser.h — Recursive-descent (Pratt) parser
 * Token stream → AST.
 */
#ifndef PARSER_H
#define PARSER_H

#include "calcengine.h"
#include "token.h"
#include "ast.h"

typedef struct {
    const tokenizer_t *tokens;
    uint16_t           pos;        /* current token index            */
    uint8_t            depth;      /* recursion depth counter        */
    ce_error_t         error;
    char               error_msg[64];
} parser_t;

#define PARSER_MAX_DEPTH 50  /* prevent stack overflow on TI-84 (4KB stack) */

/* Initialize with a tokenized input */
void parser_init(parser_t *p, const tokenizer_t *tokens);

/* Parse the full expression.  Returns root node (NULL on error). */
ast_node_t *parser_parse(parser_t *p);

/* Error accessors */
ce_error_t  parser_error(const parser_t *p);
const char *parser_error_msg(const parser_t *p);

#endif /* PARSER_H */
