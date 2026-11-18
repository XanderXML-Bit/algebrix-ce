/*
 * memory_pool.h — Example: Pool allocator for AST nodes
 * Fixed-size block pool — no malloc/free churn.
 * Designed for 256KB RAM constraint.
 *
 * THIS IS A REFERENCE EXAMPLE — see src/memory.h for the actual implementation.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include "calcengine.h"

/* Max AST nodes that can be alive simultaneously */
#define NODE_POOL_SIZE  512

/* Initialize the pool — call once at startup */
void mem_init(void);

/* Allocate one zeroed AST node. Returns NULL if pool exhausted. */
ast_node_t *mem_alloc_node(void);

/* Return a single node to the pool */
void mem_free_node(ast_node_t *node);

/* Free every node (reset the entire pool) */
void mem_free_all(void);

/* Diagnostics */
uint16_t mem_nodes_used(void);
uint16_t mem_nodes_free(void);

#endif /* MEMORY_H */
