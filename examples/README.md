# Examples

This directory contains small, representative code patterns from the engine. Read them before adding a feature so new code follows the same conventions.

## Files

### `memory_pool.c` / `memory_pool.h`
Demonstrates the static pool allocator pattern. All AST nodes come from this fixed 512-node pool. Shows:
- Static array + free stack for O(1) allocation
- Bitmap-based in-use tracking for double-free protection
- Diagnostics functions (used/free counts)
- **Key pattern**: Never use malloc/free. All memory comes from the pool.

### `simplify_example.c`
Demonstrates the algebraic simplification engine. Shows:
- Bottom-up recursive tree walk pattern
- Pattern matching on AST node types (constant folding, identity elimination)
- Structural equality checking (`ast_equal`)
- Proper node memory management (free nodes when removing from tree)
- Static helper functions for internal logic

### `test_patterns.c`
Demonstrates the test harness patterns. Shows:
- Custom assertion macros (ASSERT_TRUE, ASSERT_EQ_INT, ASSERT_EQ_FLOAT)
- Helper functions (parse_str, eval_str, eval_str_at)
- Test organization by module with printf section headers
- Memory pool cleanup between test groups (mem_free_all)
