/*
 * eval.h — Numerical expression evaluator
 * Tree-walk evaluator with variable bindings.
 */
#ifndef EVAL_H
#define EVAL_H

#include "calcengine.h"
#include "ast.h"
#include "ce_float_ops.h"

/* Stack-tight workers select a measured bound in their makefile. Other
 * evaluators retain the existing 50-level behavior. */
#ifndef CE_EVAL_MAX_DEPTH
#define CE_EVAL_MAX_DEPTH 50
#endif

#define MAX_VARS 16

typedef struct {
    char       name;
    ce_float_t value;
    bool       bound;
} var_binding_t;

typedef struct {
    var_binding_t vars[MAX_VARS];
    ce_error_t    error;
    char          error_msg[64];
    uint8_t       eval_depth;
    char          undefined_var; /* latest unbound NODE_VARIABLE read, or 0;
                                    valid only while error == CE_ERR_UNDEFINED */
} eval_ctx_t;

/* Initialize evaluation context (clears all bindings) */
void eval_init(eval_ctx_t *ctx);

/* Bind / clear a variable.  Returns false if all slots full. */
bool eval_set_var(eval_ctx_t *ctx, char name, ce_float_t value);
void eval_clear_var(eval_ctx_t *ctx, char name);

/* Evaluate an AST numerically.  Check ctx->error afterwards. */
ce_float_t eval_expr(eval_ctx_t *ctx, const ast_node_t *node);

/* Convenience: evaluate expression at a single variable binding */
ce_float_t eval_at(const ast_node_t *expr, char var,
                   ce_float_t x, ce_error_t *err);

/* Error accessors */
ce_error_t  eval_error(const eval_ctx_t *ctx);
const char *eval_error_msg(const eval_ctx_t *ctx);

/* Stored variables (internal review).  With CE_EVAL_USER_VARS=1 a NODE_VARIABLE
 * read that has no local binding falls back to g_user_vars, and a variable
 * store (`expr->X`) also writes it.  Local bindings (d/dx, sum, int, ...)
 * always win.  The module runtime copies g_user_vars from and to CE_STATE.
 * Default 0: the module never touches the table. */
#ifndef CE_EVAL_USER_VARS
#define CE_EVAL_USER_VARS 0
#endif

#if CE_EVAL_USER_VARS
extern ce_user_vars_t g_user_vars;
#endif

#endif /* EVAL_H */
