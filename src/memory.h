/*
 * memory.h — Pool allocator for AST nodes
 * Fixed-size block pool — no malloc/free churn.
 * Designed for 256KB RAM constraint.
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

/* Number of mem_alloc_node() calls that returned NULL.  Wraps; callers
 * compare two readings to learn whether an allocation failed in between. */
uint16_t mem_alloc_failures(void);

#ifdef DESKTOP_BUILD
/* Test hook: number of mem_free_node() calls on a node that was already
 * free (the guard ignores them).  Never reset. */
uint16_t mem_double_frees(void);
#endif

#endif /* MEMORY_H */
