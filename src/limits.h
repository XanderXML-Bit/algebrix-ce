/*
 * limits.h -- Symbolic limit engine
 */
#ifndef LIMITS_H
#define LIMITS_H

#include "calcengine.h"
#include "ast.h"

/* Limit result status */
typedef enum {
    LIM_OK,           /* finite result in *result             */
    LIM_POS_INF,      /* limit is +infinity                   */
    LIM_NEG_INF,      /* limit is -infinity                   */
    LIM_DNE,          /* limit does not exist                 */
    LIM_FAILED        /* could not determine                  */
} lim_status_t;

/*
 * Compute the symbolic limit of expr as var -> point.
 * direction: -1 = left, 0 = two-sided, +1 = right.
 * On LIM_OK, *result holds the finite value.
 * Records steps via the steps module.
 */
lim_status_t compute_limit(const ast_node_t *expr, char var,
                           const ast_node_t *point, int8_t direction,
                           ce_float_t *result);

/* ── Continuity analysis ────────────────────────────────── */

typedef enum {
    CONT_CONTINUOUS,   /* f(a) exists and equals the two-sided limit */
    CONT_REMOVABLE,    /* limit exists but f(a) differs or undefined */
    CONT_JUMP,         /* left and right limits exist but disagree   */
    CONT_INFINITE,     /* at least one side diverges to ±inf         */
    CONT_ESSENTIAL,    /* oscillation or DNE                         */
    CONT_FAILED        /* could not determine                        */
} continuity_t;

/*
 * Classify the continuity of expr at var = point.
 * Returns classification and records steps.
 */
continuity_t limits_continuity(const ast_node_t *expr, char var,
                               const ast_node_t *point);

#endif /* LIMITS_H */
