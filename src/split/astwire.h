#ifndef SPLIT_ASTWIRE_H
#define SPLIT_ASTWIRE_H

#include <stdint.h>
#include <stdbool.h>
#include "ast.h"

/* Serialize an AST into a compact wire format for cross-program IPC. */
bool ce_ast_serialize(const ast_node_t *root,
                      uint8_t *buf,
                      uint16_t cap,
                      uint16_t *out_size);

/* Deserialize an AST from wire format. Returns pool-allocated AST or NULL. */
ast_node_t *ce_ast_deserialize(const uint8_t *buf, uint16_t size);

#endif /* SPLIT_ASTWIRE_H */
