/*
 * solver.h — Solve orchestrator with step-by-step checkpoints
 *
 * Provides a unified solver that takes a parsed AST and produces
 * an array of SolveStep objects representing operation-level
 * checkpoints. Supports Full Auto and Step-by-Step modes, and
 * method selection (auto vs forced).
 */
#ifndef SOLVER_H
#define SOLVER_H

#include "calcengine.h"
#include "ast.h"
#include "steps.h"

/* ── Solve method identifiers ──────────────────────────────── */
typedef enum {
    SOLVE_METHOD_AUTO = 0,
    SOLVE_METHOD_DIRECT_EVAL,
    SOLVE_METHOD_SIMPLIFY,
    SOLVE_METHOD_DIFFERENTIATE,
    SOLVE_METHOD_INTEGRATE_AUTO,
    SOLVE_METHOD_INTEGRATE_USUB,
    SOLVE_METHOD_INTEGRATE_PARTS,
    SOLVE_METHOD_INTEGRATE_TRIGSUB,
    SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC,
    SOLVE_METHOD_LIMIT,
    SOLVE_METHOD_SERIES,
    SOLVE_METHOD_SOLVE_EQUATION,
    SOLVE_METHOD_FACTOR,
    SOLVE_METHOD_EXPAND,
    SOLVE_METHOD_TANGENT,
    SOLVE_METHOD_TAYLOR,
    SOLVE_METHOD_COUNT
} solve_method_t;

/* ── Solve step status ─────────────────────────────────────── */
typedef enum {
    STEP_STATUS_OK = 0,
    STEP_STATUS_ERROR,
    STEP_STATUS_INCOMPLETE,
    STEP_STATUS_SKIPPED
} step_status_t;

/* ── SolveStep: one operation-level checkpoint ─────────────── */
#define SOLVE_STEP_DESC_LEN 80
#define SOLVE_STEP_STATE_LEN 120
#define MAX_SOLVE_STEPS 24

typedef struct {
    char description[SOLVE_STEP_DESC_LEN];   /* e.g. "Apply chain rule" */
    char state_before[SOLVE_STEP_STATE_LEN]; /* expression before this step */
    char state_after[SOLVE_STEP_STATE_LEN];  /* expression after this step */
    solve_method_t method_used;
    step_status_t status;
    int step_index;                          /* index in steps buffer */
} solve_step_t;

/* ── Solve result ──────────────────────────────────────────── */
typedef struct {
    solve_step_t steps[MAX_SOLVE_STEPS];
    int step_count;
    int current_step;        /* for step-by-step mode: which step we're on */
    char final_result[MAX_RESULT_LEN];
    ce_error_t error;
    char error_msg[80];
    solve_method_t method_used;
    bool complete;           /* true when all steps are done */
} solve_result_t;

/* ── Method info (for method selection reporting) ──────────── */
typedef struct {
    solve_method_t method;
    const char *name;        /* human-readable name */
    bool applicable;         /* whether this method can be applied */
    int rank;                /* 0 = best, higher = worse */
} method_info_t;

#define MAX_METHOD_OPTIONS 8

typedef struct {
    method_info_t options[MAX_METHOD_OPTIONS];
    int count;
    solve_method_t recommended;
} method_report_t;

/* ── Solver API ────────────────────────────────────────────── */

/*
 * Initialize a solve result (zeroes everything).
 */
void solver_init(solve_result_t *result);

/*
 * Solve an expression fully (Full Auto mode).
 * Produces all SolveStep objects at once.
 * Uses g_settings.integ_method for integration method selection.
 * Uses g_settings.show_steps to control step recording.
 */
void solver_solve_full(const ast_node_t *expr, solve_result_t *result);

/*
 * Solve one step (Step-by-Step mode).
 * Call repeatedly until result->complete is true.
 * Each call advances result->current_step and fills the next step.
 */
void solver_solve_next_step(const ast_node_t *expr, solve_result_t *result);

/*
 * Auto-complete remaining steps from current position.
 * Used when user presses 2nd+Enter in step-by-step mode.
 */
void solver_complete_remaining(const ast_node_t *expr, solve_result_t *result);

/*
 * Report which methods are applicable to this expression.
 * Fills report with ranked method options.
 */
void solver_get_methods(const ast_node_t *expr, method_report_t *report);

/*
 * Get human-readable name for a solve method.
 */
const char *solver_method_name(solve_method_t method);

/*
 * Check if a specific method can be applied to this expression.
 * Returns true if applicable, false otherwise.
 */
bool solver_method_applicable(const ast_node_t *expr, solve_method_t method);

/*
 * Solve with a specific forced method.
 * If the method is not applicable, sets result->error = CE_ERR_DOMAIN
 * and fills result->error_msg with a helpful message.
 */
void solver_solve_with_method(const ast_node_t *expr,
                              solve_method_t method,
                              solve_result_t *result);

#endif /* SOLVER_H */
