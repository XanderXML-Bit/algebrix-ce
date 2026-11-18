#ifndef SPLIT_CLASSIFY_H
#define SPLIT_CLASSIFY_H

#include "split/ipc.h"
#include "ast.h"

ce_domain_t ce_classify_expression(const ast_node_t *root, bool input_had_equals);

#endif /* SPLIT_CLASSIFY_H */
