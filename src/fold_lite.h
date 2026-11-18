/*
 * fold_lite.h — small exact constant fold for CE integration workers.
 *
 * The full simplifier (simplify.c, about 11 KB on eZ80) does not fit in the
 * CE integration workers (CECISM, CEIPFR, CEITRG are within a few KB of the
 * 51,200 B module ceiling).  Without it the power rule printed
 * ((X^(2+1))/(2+1)) on target.  This pass folds what the integration rules
 * leave behind: exact integer arithmetic on small integers, the identities
 * E+0, 0+E, E-0, E*1, 1*E, -1*E, E/1, E^1, 0*E, -(-E), and coefficient
 * cancellation such as 3*((x^3)/3) -> x^3.  It never rounds: an inexact quotient such as 2/3 is
 * left as it is, and any value outside +-2047 is left unfolded.
 *
 * It only frees and reuses nodes, never allocates, so it cannot run the
 * pool dry.  The input must be a complete tree (no NULL operands);
 * integrate.c checks that before calling it.
 */
#ifndef FOLD_LITE_H
#define FOLD_LITE_H

#include "ast.h"

/* Fold `node` in place and return the folded tree (takes ownership). */
ast_node_t *ast_fold_lite(ast_node_t *node);

#endif /* FOLD_LITE_H */
