/*
 * memory.c — Pool allocator implementation
 *
 * Static array of ast_node_t with a free-stack for O(1) alloc/free.
 * Total pool footprint: ~24 KB (512 nodes × 48 bytes), well within 50 KB usable RAM.
 */
#include "memory.h"
#include "ast.h"
#include <string.h>

/* ── Static storage ──────────────────────────────────────── */
static ast_node_t pool[NODE_POOL_SIZE];
static uint16_t   free_stack[NODE_POOL_SIZE];
static uint16_t   free_top;       /* next pop index (grows down) */
static uint16_t   used_count;
static uint8_t    in_use[NODE_POOL_SIZE / 8 + 1]; /* bitmap */
static uint16_t   alloc_failures; /* wraps; compare for change only */
#ifdef DESKTOP_BUILD
static uint16_t   double_frees;   /* test hook: frees of a free node */
#endif

/* ── Public API ──────────────────────────────────────────── */

void mem_init(void)
{
    uint16_t i;
    for (i = 0; i < NODE_POOL_SIZE; i++)
        free_stack[i] = i;
    free_top   = NODE_POOL_SIZE;
    used_count = 0;
    memset(in_use, 0, sizeof(in_use));
}

ast_node_t *mem_alloc_node(void)
{
    ast_node_t *n;
    uint16_t idx;
    if (free_top == 0) {
        ++alloc_failures;
        return NULL;                  /* pool exhausted */
    }
    --free_top;
    ++used_count;
    idx = free_stack[free_top];
    in_use[idx / 8] |= (uint8_t)(1 << (idx % 8));
    n = &pool[idx];
    memset(n, 0, sizeof(*n));
    return n;
}

void mem_free_node(ast_node_t *node)
{
    uint16_t idx;
    if (!node) return;
    idx = (uint16_t)(node - pool);
    if (idx >= NODE_POOL_SIZE) return; /* bad pointer guard */
    /* Guard against double-free */
    if (!(in_use[idx / 8] & (1 << (idx % 8)))) {
#ifdef DESKTOP_BUILD
        ++double_frees;
#endif
        return;
    }
    in_use[idx / 8] &= (uint8_t)~(1 << (idx % 8));
#if defined(DESKTOP_BUILD) && defined(MEM_POISON_ON_FREE)
    /* Test builds: a read through a freed node sees garbage pointers and
     * crashes under ASan instead of silently reading stale fields. */
    memset(node, 0xA5, sizeof(*node));
#endif
    free_stack[free_top++] = idx;
    --used_count;
}

void mem_free_all(void)
{
    uint16_t i;
    for (i = 0; i < NODE_POOL_SIZE; i++)
        free_stack[i] = i;
    free_top   = NODE_POOL_SIZE;
    used_count = 0;
    memset(in_use, 0, sizeof(in_use));
}

uint16_t mem_nodes_used(void) { return used_count; }
uint16_t mem_nodes_free(void) { return free_top;   }
uint16_t mem_alloc_failures(void) { return alloc_failures; }
#ifdef DESKTOP_BUILD
uint16_t mem_double_frees(void) { return double_frees; }
#endif
