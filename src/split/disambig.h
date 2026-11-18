/*
 * disambig.h — Smart disambiguation for ambiguous expressions
 *
 * Detects when an input expression could lead to genuinely different
 * outcomes depending on which mathematical operation the user intends,
 * and returns a list of candidate actions for the user to choose from.
 *
 * Three-tier logic (per user clarification):
 *   1. Unambiguous expression (explicit op, pure numeric)
 *      → returns 0; caller evaluates normally.
 *   2. Expression with variables that could be factored OR solved OR
 *      pfrac'd (genuinely different goals)
 *      → returns N > 0; caller shows disambiguation dialog.
 *   3. show_method_choices = true
 *      → also surface same-result method choices (e.g. simplify vs eval
 *        for plain expressions).
 *
 * The disambiguation dialog ONLY appears when two or more possible
 * operations yield DIFFERENT answers/forms (not just different algorithms
 * for the same result).
 */
#ifndef DISAMBIG_H
#define DISAMBIG_H

#include "../calcengine.h"
#include "../ast.h"

/* Maximum disambiguation options returned by ce_disambig_detect */
#define DISAMBIG_MAX_OPTS 6

/* ── Action enum ─────────────────────────────────────────────── */
typedef enum {
    DISAMBIG_EVAL = 0,   /* evaluate numerically                  */
    DISAMBIG_SIMPLIFY,   /* simplify algebraically                */
    DISAMBIG_FACTOR,     /* factor the polynomial                 */
    DISAMBIG_ROOTS,      /* find roots  (solve expr = 0)          */
    DISAMBIG_PFRAC,      /* partial fraction decomposition        */
    DISAMBIG_EXPAND      /* expand / multiply out                 */
} ce_disambig_action_t;

/* ── Option struct ───────────────────────────────────────────── */
typedef struct {
    const char           *label;   /* short description for menu item */
    ce_disambig_action_t  action;  /* action to execute when selected */
} ce_disambig_option_t;

/* ── API ─────────────────────────────────────────────────────── */

/*
 * Analyse root for ambiguity.
 * Fills opts[0 .. return_value - 1] with candidate actions.
 * Returns 0  → expression is unambiguous; caller should just evaluate.
 * Returns N > 0 → show the disambiguation dialog.
 *
 * show_method_choices: if true, also offer choices when operations yield
 *   the same numerical answer but different symbolic forms (e.g. eval vs
 *   simplify on a plain expression with variables).  Default is false.
 */
int ce_disambig_detect(const ast_node_t     *root,
                       ce_disambig_option_t *opts,
                       int                   max_opts,
                       bool                  show_method_choices);

/*
 * Build the expression string to submit for a given action.
 *
 * input_buf : original user expression (null-terminated)
 * var       : first variable found in root  (0 if none)
 * action    : chosen action
 * out_buf   : output buffer
 * out_size  : size of out_buf (including '\0')
 *
 * Returns true on success.
 */
bool ce_disambig_build_expr(const char           *input_buf,
                             char                  var,
                             ce_disambig_action_t  action,
                             char                 *out_buf,
                             int                   out_size);

#endif /* DISAMBIG_H */
