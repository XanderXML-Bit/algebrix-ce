/*
 * solver.c — Solve orchestrator with step-by-step checkpoints
 *
 * Unified solver that dispatches to the appropriate engine module
 * (eval, deriv, integrate, limits, etc.) and captures operation-level
 * checkpoints as SolveStep objects.
 */
#include "solver.h"
#include "ast.h"
#include "eval.h"
#include "simplify.h"
#include "deriv.h"
#include "integrate.h"
#include "limits.h"
#include "series.h"
#include "algebra.h"
#include "steps.h"
#include "parser.h"
#include "token.h"
#include "memory.h"

#include <string.h>
#include <stdio.h>

/* ── Method name table ─────────────────────────────────────── */
static const char *method_names[SOLVE_METHOD_COUNT] = {
    "Auto",
    "Direct Evaluation",
    "Simplify",
    "Differentiate",
    "Integrate (Auto)",
    "Integrate (u-sub)",
    "Integrate (by parts)",
    "Integrate (trig sub)",
    "Integrate (partial fractions)",
    "Limit",
    "Series",
    "Solve Equation",
    "Factor",
    "Expand",
    "Tangent Line",
    "Taylor Series"
};

const char *solver_method_name(solve_method_t method)
{
    if (method >= 0 && method < SOLVE_METHOD_COUNT)
        return method_names[method];
    return "Unknown";
}

void solver_init(solve_result_t *result)
{
    memset(result, 0, sizeof(*result));
}

/* ── Node type detection helpers ───────────────────────────── */

static bool is_deriv_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_DERIV;
}

static bool is_integral_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_INTEGRAL;
}

static bool is_limit_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_LIMIT;
}

static bool is_sum_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_SUM;
}

static bool is_taylor_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_TAYLOR;
}

static bool is_solve_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_SOLVE;
}

static bool is_factor_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_FACTOR;
}

static bool is_expand_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_EXPAND;
}

static bool is_tangent_node(const ast_node_t *expr)
{
    return expr && expr->type == NODE_TANGENT;
}

static bool is_evaluable(const ast_node_t *expr)
{
    if (!expr) return false;
    switch (expr->type) {
        case NODE_NUMBER:
        case NODE_CONSTANT:
            return true;
        case NODE_BINOP:
            return is_evaluable(expr->d.binop.left) &&
                   is_evaluable(expr->d.binop.right);
        case NODE_UNOP:
            return is_evaluable(expr->d.unop.operand);
        case NODE_FUNC:
            return is_evaluable(expr->d.func.arg);
        default:
            return false;
    }
}

/* ── Detect applicable method from AST ─────────────────────── */

static solve_method_t detect_method(const ast_node_t *expr)
{
    if (!expr) return SOLVE_METHOD_DIRECT_EVAL;

    if (is_deriv_node(expr))     return SOLVE_METHOD_DIFFERENTIATE;
    if (is_integral_node(expr))  return SOLVE_METHOD_INTEGRATE_AUTO;
    if (is_limit_node(expr))     return SOLVE_METHOD_LIMIT;
    if (is_sum_node(expr))       return SOLVE_METHOD_SERIES;
    if (is_taylor_node(expr))    return SOLVE_METHOD_TAYLOR;
    if (is_solve_node(expr))     return SOLVE_METHOD_SOLVE_EQUATION;
    if (is_factor_node(expr))    return SOLVE_METHOD_FACTOR;
    if (is_expand_node(expr))    return SOLVE_METHOD_EXPAND;
    if (is_tangent_node(expr))   return SOLVE_METHOD_TANGENT;
    if (is_evaluable(expr))      return SOLVE_METHOD_DIRECT_EVAL;

    return SOLVE_METHOD_SIMPLIFY;
}

static integ_strategy_t solve_method_to_strategy(solve_method_t method)
{
    switch (method) {
        case SOLVE_METHOD_INTEGRATE_PARTS:        return INTEG_STRATEGY_PARTS;
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:      return INTEG_STRATEGY_TRIGSUB;
        case SOLVE_METHOD_INTEGRATE_USUB:         return INTEG_STRATEGY_USUB;
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC: return INTEG_STRATEGY_PARTIAL_FRAC;
        default:                                  return INTEG_STRATEGY_AUTO;
    }
}

static int integration_method_base_score(const ast_node_t *expr,
                                         solve_method_t method)
{
    const ast_node_t *inner;
    char var;
    int score;

    if (!is_integral_node(expr) || !expr->d.integral.expr) return 0;

    inner = expr->d.integral.expr;
    var = expr->d.integral.var;

    switch (method) {
        case SOLVE_METHOD_INTEGRATE_AUTO:         score = 45; break;
        case SOLVE_METHOD_INTEGRATE_USUB:         score = 25; break;
        case SOLVE_METHOD_INTEGRATE_PARTS:        score = 20; break;
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:      score = 15; break;
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC: score = 18; break;
        default:                                  score = 0;  break;
    }

    if (inner->type == NODE_BINOP && inner->d.binop.op == OP_MUL) {
        if (method == SOLVE_METHOD_INTEGRATE_USUB)  score += 18;
        if (method == SOLVE_METHOD_INTEGRATE_PARTS) score += 22;
    }
    if (inner->type == NODE_BINOP && inner->d.binop.op == OP_DIV) {
        if (method == SOLVE_METHOD_INTEGRATE_USUB)         score += 14;
        if (method == SOLVE_METHOD_INTEGRATE_TRIGSUB)      score += 10;
        if (method == SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC) score += 24;
    }
    if (inner->type == NODE_FUNC && inner->d.func.arg &&
        ast_contains_var(inner->d.func.arg, var) &&
        !(inner->d.func.arg->type == NODE_VARIABLE &&
          inner->d.func.arg->d.variable == var) &&
        method == SOLVE_METHOD_INTEGRATE_USUB) {
        score += 20;
    }

    return score;
}

static bool probe_integration_method(const ast_node_t *expr,
                                     solve_method_t method,
                                     int *score_out)
{
    const ast_node_t *inner;
    char var;
    ast_node_t *probe = NULL;
    int saved_steps;
    int score;

    if (!is_integral_node(expr) || !expr->d.integral.expr) return false;

    inner = expr->d.integral.expr;
    var = expr->d.integral.var;
    score = integration_method_base_score(expr, method);

    if (method == SOLVE_METHOD_INTEGRATE_AUTO) {
        if (score_out) *score_out = score + 35;
        return true;
    }

    saved_steps = steps_save();
    probe = integrate_with_strategy(inner, var, solve_method_to_strategy(method), NULL);
    steps_restore(saved_steps);
    if (!probe) {
        if (score_out) *score_out = score;
        return false;
    }

    ast_free(probe);
    score += 36;
    if (score > 99) score = 99;
    if (score_out) *score_out = score;
    return true;
}

static void format_invalid_method_message(const ast_node_t *expr,
                                          solve_method_t attempted,
                                          char *buf,
                                          size_t buf_size)
{
    method_report_t report;
    size_t used;
    int i;
    bool appended = false;

    if (!buf || buf_size == 0) return;
    snprintf(buf, buf_size, "Cannot apply %s here. Switch to Auto",
             solver_method_name(attempted));

    if (!expr) return;

    solver_get_methods(expr, &report);
    used = strlen(buf);
    for (i = 0; i < report.count; i++) {
        const method_info_t *opt = &report.options[i];
        if (!opt->applicable ||
            opt->method == SOLVE_METHOD_AUTO ||
            opt->method == attempted) {
            continue;
        }

        if (!appended) {
            used += (size_t)snprintf(buf + used,
                                     used < buf_size ? buf_size - used : 0,
                                     " or select: ");
            appended = true;
        } else {
            used += (size_t)snprintf(buf + used,
                                     used < buf_size ? buf_size - used : 0,
                                     ", ");
        }
        used += (size_t)snprintf(buf + used,
                                 used < buf_size ? buf_size - used : 0,
                                 "%s",
                                 solver_method_name(opt->method));
    }
}

bool solver_method_applicable(const ast_node_t *expr, solve_method_t method)
{
    if (!expr) return false;

    switch (method) {
        case SOLVE_METHOD_AUTO:
            return true;
        case SOLVE_METHOD_DIRECT_EVAL:
            return is_evaluable(expr);
        case SOLVE_METHOD_SIMPLIFY:
            return true; /* simplify can always be attempted */
        case SOLVE_METHOD_DIFFERENTIATE:
            return is_deriv_node(expr) ||
                   ast_contains_var(expr, 'x'); /* can differentiate anything with a variable */
        case SOLVE_METHOD_INTEGRATE_AUTO:
            return is_integral_node(expr);
        case SOLVE_METHOD_INTEGRATE_USUB:
        case SOLVE_METHOD_INTEGRATE_PARTS:
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC:
            return probe_integration_method(expr, method, NULL);
        case SOLVE_METHOD_LIMIT:
            return is_limit_node(expr);
        case SOLVE_METHOD_SERIES:
            return is_sum_node(expr);
        case SOLVE_METHOD_SOLVE_EQUATION:
            return is_solve_node(expr);
        case SOLVE_METHOD_FACTOR:
            return is_factor_node(expr) || ast_contains_var(expr, 'x');
        case SOLVE_METHOD_EXPAND:
            return is_expand_node(expr) || ast_contains_var(expr, 'x');
        case SOLVE_METHOD_TANGENT:
            return is_tangent_node(expr);
        case SOLVE_METHOD_TAYLOR:
            return is_taylor_node(expr);
        default:
            return false;
    }
}

/* ── Record a SolveStep ────────────────────────────────────── */

static void record_step(solve_result_t *result,
                        const char *desc,
                        const char *before,
                        const char *after,
                        solve_method_t method,
                        step_status_t status)
{
    if (result->step_count >= MAX_SOLVE_STEPS) return;

    solve_step_t *s = &result->steps[result->step_count];
    strncpy(s->description, desc ? desc : "", SOLVE_STEP_DESC_LEN - 1);
    s->description[SOLVE_STEP_DESC_LEN - 1] = '\0';
    strncpy(s->state_before, before ? before : "", SOLVE_STEP_STATE_LEN - 1);
    s->state_before[SOLVE_STEP_STATE_LEN - 1] = '\0';
    strncpy(s->state_after, after ? after : "", SOLVE_STEP_STATE_LEN - 1);
    s->state_after[SOLVE_STEP_STATE_LEN - 1] = '\0';
    s->method_used = method;
    s->status = status;
    s->step_index = result->step_count;
    result->step_count++;
}

/* ── Core solve dispatch ───────────────────────────────────── */

static void solve_derivative(const ast_node_t *expr, solve_result_t *result)
{
    char before_buf[SOLVE_STEP_STATE_LEN];
    char after_buf[SOLVE_STEP_STATE_LEN];
    ast_node_t *diff_result;

    ast_to_str(expr, before_buf, SOLVE_STEP_STATE_LEN);

    record_step(result, "Begin differentiation", before_buf, "",
                SOLVE_METHOD_DIFFERENTIATE, STEP_STATUS_OK);

    steps_clear();
    diff_result = deriv_diff(expr->d.deriv.expr, expr->d.deriv.var);

    if (diff_result) {
        ast_to_str(diff_result, after_buf, SOLVE_STEP_STATE_LEN);
        record_step(result, "Apply differentiation rules", before_buf, after_buf,
                    SOLVE_METHOD_DIFFERENTIATE, STEP_STATUS_OK);

        /* Capture any intermediate steps from the steps buffer */
        int i;
        for (i = 0; i < steps_count() && result->step_count < MAX_SOLVE_STEPS; i++) {
            record_step(result, steps_get(i), "", "",
                        SOLVE_METHOD_DIFFERENTIATE, STEP_STATUS_OK);
        }

        strncpy(result->final_result, after_buf, MAX_RESULT_LEN - 1);
        result->final_result[MAX_RESULT_LEN - 1] = '\0';
        result->error = CE_OK;
        ast_free(diff_result);
    } else {
        record_step(result, "Differentiation failed", before_buf, "",
                    SOLVE_METHOD_DIFFERENTIATE, STEP_STATUS_ERROR);
        result->error = CE_ERR_TOO_COMPLEX;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Could not differentiate expression");
    }
    result->method_used = SOLVE_METHOD_DIFFERENTIATE;
    result->complete = true;
}

static void solve_integral(const ast_node_t *expr, solve_result_t *result,
                           solve_method_t forced_method)
{
    char before_buf[SOLVE_STEP_STATE_LEN];
    char after_buf[SOLVE_STEP_STATE_LEN];
    ast_node_t *int_result = NULL;
    integ_strategy_t strategy = INTEG_STRATEGY_AUTO;

    ast_to_str(expr, before_buf, SOLVE_STEP_STATE_LEN);
    result->method_used = (forced_method != SOLVE_METHOD_AUTO)
                        ? forced_method
                        : SOLVE_METHOD_INTEGRATE_AUTO;

    /* Map forced method to integration strategy */
    switch (forced_method) {
        case SOLVE_METHOD_INTEGRATE_USUB:
            strategy = INTEG_STRATEGY_USUB;
            record_step(result, "Apply u-substitution", before_buf, "",
                        forced_method, STEP_STATUS_OK);
            break;
        case SOLVE_METHOD_INTEGRATE_PARTS:
            strategy = INTEG_STRATEGY_PARTS;
            record_step(result, "Apply integration by parts", before_buf, "",
                        forced_method, STEP_STATUS_OK);
            break;
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:
            strategy = INTEG_STRATEGY_TRIGSUB;
            record_step(result, "Apply trig substitution", before_buf, "",
                        forced_method, STEP_STATUS_OK);
            break;
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC:
            strategy = INTEG_STRATEGY_PARTIAL_FRAC;
            record_step(result, "Rewrite as partial fractions", before_buf, "",
                        forced_method, STEP_STATUS_OK);
            break;
        default:
            record_step(result, "Begin integration", before_buf, "",
                        SOLVE_METHOD_INTEGRATE_AUTO, STEP_STATUS_OK);
            break;
    }

    steps_clear();

    if (strategy != INTEG_STRATEGY_AUTO) {
        int_result = integrate_with_strategy(expr->d.integral.expr,
                                             expr->d.integral.var,
                                             strategy, NULL);
    } else {
        int_result = integrate(expr->d.integral.expr, expr->d.integral.var);
    }

    if (int_result) {
        ast_to_str(int_result, after_buf, SOLVE_STEP_STATE_LEN);

        /* Capture intermediate steps */
        int i;
        for (i = 0; i < steps_count() && result->step_count < MAX_SOLVE_STEPS; i++) {
            record_step(result, steps_get(i), "", "",
                        result->method_used, STEP_STATUS_OK);
        }

        record_step(result, "Integration complete", before_buf, after_buf,
                    forced_method != SOLVE_METHOD_AUTO ? forced_method : SOLVE_METHOD_INTEGRATE_AUTO,
                    STEP_STATUS_OK);

        /* Handle definite integral evaluation */
        if (expr->d.integral.lo && expr->d.integral.hi) {
            ce_error_t eval_err = CE_OK;
            char eval_buf[SOLVE_STEP_STATE_LEN];
            ce_float_t lo_val, hi_val, definite_val;

            lo_val = eval_at(expr->d.integral.lo, expr->d.integral.var, 0, &eval_err);
            hi_val = eval_at(expr->d.integral.hi, expr->d.integral.var, 0, &eval_err);

            if (eval_err == CE_OK) {
                ce_float_t f_hi = eval_at(int_result, expr->d.integral.var, hi_val, &eval_err);
                ce_float_t f_lo = eval_at(int_result, expr->d.integral.var, lo_val, &eval_err);
                definite_val = f_hi - f_lo;

                snprintf(eval_buf, sizeof(eval_buf), "F(%g) - F(%g) = %g",
                         (double)hi_val, (double)lo_val, (double)definite_val);
                record_step(result, "Evaluate definite integral bounds",
                            after_buf, eval_buf,
                            forced_method != SOLVE_METHOD_AUTO ? forced_method : SOLVE_METHOD_INTEGRATE_AUTO,
                            STEP_STATUS_OK);
                snprintf(result->final_result, MAX_RESULT_LEN, "%g", (double)definite_val);
            } else {
                strncpy(result->final_result, after_buf, MAX_RESULT_LEN - 1);
                result->final_result[MAX_RESULT_LEN - 1] = '\0';
            }
        } else {
            snprintf(result->final_result, MAX_RESULT_LEN, "%s + C", after_buf);
        }

        result->error = CE_OK;
        ast_free(int_result);
    } else {
        const char *reason = integrate_last_failure_reason();
        record_step(result, "Integration failed", before_buf, reason ? reason : "",
                    forced_method != SOLVE_METHOD_AUTO ? forced_method : SOLVE_METHOD_INTEGRATE_AUTO,
                    STEP_STATUS_ERROR);
        if (forced_method != SOLVE_METHOD_AUTO &&
            forced_method != SOLVE_METHOD_INTEGRATE_AUTO) {
            result->error = CE_ERR_DOMAIN;
            format_invalid_method_message(expr, forced_method,
                                          result->error_msg,
                                          sizeof(result->error_msg));
        } else {
            result->error = CE_ERR_TOO_COMPLEX;
            snprintf(result->error_msg, sizeof(result->error_msg),
                     "Could not integrate: %s", reason ? reason : "unknown");
        }
    }

    result->method_used = (forced_method != SOLVE_METHOD_AUTO) ? forced_method : SOLVE_METHOD_INTEGRATE_AUTO;
    result->complete = true;
}

static void solve_limit(const ast_node_t *expr, solve_result_t *result)
{
    char before_buf[SOLVE_STEP_STATE_LEN];
    char after_buf[SOLVE_STEP_STATE_LEN];

    ast_to_str(expr, before_buf, SOLVE_STEP_STATE_LEN);
    record_step(result, "Begin limit evaluation", before_buf, "",
                SOLVE_METHOD_LIMIT, STEP_STATUS_OK);

    steps_clear();

    ce_error_t err = CE_OK;
    ce_float_t pt = eval_at(expr->d.limit.point, expr->d.limit.var, 0, &err);
    if (err != CE_OK) pt = 0;

    ce_float_t val = eval_at(expr->d.limit.expr, expr->d.limit.var, pt, &err);

    if (err == CE_OK && ce_isfinite(val)) {
        snprintf(after_buf, sizeof(after_buf), "%g", (double)val);
        record_step(result, "Direct substitution", before_buf, after_buf,
                    SOLVE_METHOD_LIMIT, STEP_STATUS_OK);
        snprintf(result->final_result, MAX_RESULT_LEN, "%g", (double)val);
        result->error = CE_OK;
    } else {
        snprintf(after_buf, sizeof(after_buf), "Indeterminate form");
        record_step(result, "Direct substitution yields indeterminate form",
                    before_buf, after_buf, SOLVE_METHOD_LIMIT, STEP_STATUS_OK);
        record_step(result, "Apply L'Hopital's Rule or algebraic manipulation",
                    after_buf, "", SOLVE_METHOD_LIMIT, STEP_STATUS_INCOMPLETE);
        result->error = CE_ERR_TOO_COMPLEX;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Limit evaluation requires further analysis");
    }

    result->method_used = SOLVE_METHOD_LIMIT;
    result->complete = true;
}

static void solve_evaluate(const ast_node_t *expr, solve_result_t *result)
{
    char before_buf[SOLVE_STEP_STATE_LEN];
    char after_buf[SOLVE_STEP_STATE_LEN];

    ast_to_str(expr, before_buf, SOLVE_STEP_STATE_LEN);
    record_step(result, "Evaluate expression", before_buf, "",
                SOLVE_METHOD_DIRECT_EVAL, STEP_STATUS_OK);

    eval_ctx_t ctx;
    eval_init(&ctx);
    ce_float_t val = eval_expr(&ctx, expr);

    if (ctx.error == CE_OK) {
        snprintf(after_buf, sizeof(after_buf), "%g", (double)val);
        record_step(result, "Result", before_buf, after_buf,
                    SOLVE_METHOD_DIRECT_EVAL, STEP_STATUS_OK);
        snprintf(result->final_result, MAX_RESULT_LEN, "%g", (double)val);
        result->error = CE_OK;
    } else {
        record_step(result, "Evaluation error", before_buf, eval_error_msg(&ctx),
                    SOLVE_METHOD_DIRECT_EVAL, STEP_STATUS_ERROR);
        result->error = ctx.error;
        strncpy(result->error_msg, eval_error_msg(&ctx), sizeof(result->error_msg) - 1);
        result->error_msg[sizeof(result->error_msg) - 1] = '\0';
    }

    result->method_used = SOLVE_METHOD_DIRECT_EVAL;
    result->complete = true;
}

static void solve_simplify(const ast_node_t *expr, solve_result_t *result)
{
    char before_buf[SOLVE_STEP_STATE_LEN];
    char after_buf[SOLVE_STEP_STATE_LEN];

    ast_to_str(expr, before_buf, SOLVE_STEP_STATE_LEN);
    record_step(result, "Simplify expression", before_buf, "",
                SOLVE_METHOD_SIMPLIFY, STEP_STATUS_OK);

    ast_node_t *copy = ast_clone(expr);
    if (!copy) {
        record_step(result, "Memory exhausted", before_buf, "",
                    SOLVE_METHOD_SIMPLIFY, STEP_STATUS_ERROR);
        result->error = CE_ERR_MEMORY;
        result->complete = true;
        return;
    }

    ast_node_t *simplified = ast_simplify(copy);
    ast_to_str(simplified, after_buf, SOLVE_STEP_STATE_LEN);

    record_step(result, "Simplified", before_buf, after_buf,
                SOLVE_METHOD_SIMPLIFY, STEP_STATUS_OK);

    strncpy(result->final_result, after_buf, MAX_RESULT_LEN - 1);
    result->final_result[MAX_RESULT_LEN - 1] = '\0';
    result->error = CE_OK;
    result->method_used = SOLVE_METHOD_SIMPLIFY;
    result->complete = true;
    ast_free(simplified);
}

/* ── Public API ────────────────────────────────────────────── */

void solver_solve_full(const ast_node_t *expr, solve_result_t *result)
{
    solver_init(result);

    if (!expr) {
        result->error = CE_ERR_SYNTAX;
        snprintf(result->error_msg, sizeof(result->error_msg), "No expression");
        result->complete = true;
        return;
    }

    solve_method_t method = detect_method(expr);

    /* Apply forced integration method from settings */
    if (method == SOLVE_METHOD_INTEGRATE_AUTO && g_settings.integ_method != METHOD_AUTO) {
        switch (g_settings.integ_method) {
            case METHOD_USUB:         method = SOLVE_METHOD_INTEGRATE_USUB; break;
            case METHOD_PARTS:        method = SOLVE_METHOD_INTEGRATE_PARTS; break;
            case METHOD_TRIGSUB:      method = SOLVE_METHOD_INTEGRATE_TRIGSUB; break;
            case METHOD_PARTIAL_FRAC: method = SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC; break;
            default: break;
        }
    }

    switch (method) {
        case SOLVE_METHOD_DIFFERENTIATE:
            solve_derivative(expr, result);
            break;
        case SOLVE_METHOD_INTEGRATE_AUTO:
        case SOLVE_METHOD_INTEGRATE_USUB:
        case SOLVE_METHOD_INTEGRATE_PARTS:
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC:
            solve_integral(expr, result, method);
            break;
        case SOLVE_METHOD_LIMIT:
            solve_limit(expr, result);
            break;
        case SOLVE_METHOD_DIRECT_EVAL:
            solve_evaluate(expr, result);
            break;
        case SOLVE_METHOD_SIMPLIFY:
        default:
            solve_simplify(expr, result);
            break;
    }
}

void solver_solve_next_step(const ast_node_t *expr, solve_result_t *result)
{
    if (!result) return;

    /* First call: initialize and do full solve internally */
    if (result->step_count == 0) {
        solver_solve_full(expr, result);
        result->current_step = 0;
        result->complete = false; /* override — we're stepping */
        if (result->step_count <= 1) {
            result->complete = true;
        }
        return;
    }

    /* Advance to next step */
    if (result->current_step < result->step_count - 1) {
        result->current_step++;
    }

    if (result->current_step >= result->step_count - 1) {
        result->complete = true;
    }
}

void solver_complete_remaining(const ast_node_t *expr, solve_result_t *result)
{
    (void)expr;
    if (!result) return;
    result->current_step = result->step_count > 0 ? result->step_count - 1 : 0;
    result->complete = true;
}

void solver_get_methods(const ast_node_t *expr, method_report_t *report)
{
    int rank = 0;
    int i;
    memset(report, 0, sizeof(*report));

    if (!expr) {
        report->recommended = SOLVE_METHOD_AUTO;
        return;
    }

    report->recommended = detect_method(expr);

    /* Always add Auto */
    report->options[report->count].method = SOLVE_METHOD_AUTO;
    report->options[report->count].name = solver_method_name(SOLVE_METHOD_AUTO);
    report->options[report->count].applicable = true;
    report->options[report->count].rank = rank++;
    report->count++;

    if (is_integral_node(expr)) {
        typedef struct {
            solve_method_t method;
            int score;
            bool applicable;
        } ranked_method_t;
        ranked_method_t ranked[4];
        solve_method_t int_methods[] = {
            SOLVE_METHOD_INTEGRATE_USUB,
            SOLVE_METHOD_INTEGRATE_PARTS,
            SOLVE_METHOD_INTEGRATE_TRIGSUB,
            SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC
        };
        int best_score = -1;

        for (i = 0; i < 4; i++) {
            ranked[i].method = int_methods[i];
            ranked[i].applicable = probe_integration_method(expr, int_methods[i], &ranked[i].score);
            if (ranked[i].applicable && ranked[i].score > best_score) {
                best_score = ranked[i].score;
                report->recommended = int_methods[i];
            }
        }

        for (i = 0; i < 3; i++) {
            int j;
            for (j = i + 1; j < 4; j++) {
                bool swap = false;
                if (ranked[j].applicable != ranked[i].applicable) {
                    swap = ranked[j].applicable;
                } else if (ranked[j].score > ranked[i].score) {
                    swap = true;
                }
                if (swap) {
                    ranked_method_t tmp = ranked[i];
                    ranked[i] = ranked[j];
                    ranked[j] = tmp;
                }
            }
        }

        for (i = 0; i < 4 && report->count < MAX_METHOD_OPTIONS; i++) {
            report->options[report->count].method = ranked[i].method;
            report->options[report->count].name = solver_method_name(ranked[i].method);
            report->options[report->count].applicable = ranked[i].applicable;
            report->options[report->count].rank = rank++;
            report->count++;
        }
        return;
    }

    {
        solve_method_t detected = detect_method(expr);
        if (detected != SOLVE_METHOD_AUTO && report->count < MAX_METHOD_OPTIONS) {
            report->options[report->count].method = detected;
            report->options[report->count].name = solver_method_name(detected);
            report->options[report->count].applicable = solver_method_applicable(expr, detected);
            report->options[report->count].rank = rank++;
            report->count++;
        }
    }

    if (report->recommended != SOLVE_METHOD_SIMPLIFY &&
        report->count < MAX_METHOD_OPTIONS) {
        report->options[report->count].method = SOLVE_METHOD_SIMPLIFY;
        report->options[report->count].name = solver_method_name(SOLVE_METHOD_SIMPLIFY);
        report->options[report->count].applicable = true;
        report->options[report->count].rank = rank++;
        report->count++;
    }
}

void solver_solve_with_method(const ast_node_t *expr,
                              solve_method_t method,
                              solve_result_t *result)
{
    solver_init(result);

    if (!expr) {
        result->error = CE_ERR_SYNTAX;
        snprintf(result->error_msg, sizeof(result->error_msg), "No expression");
        result->complete = true;
        return;
    }

    if (method == SOLVE_METHOD_AUTO) {
        solver_solve_full(expr, result);
        return;
    }

    if (!solver_method_applicable(expr, method)) {
        result->error = CE_ERR_DOMAIN;
        format_invalid_method_message(expr, method,
                                      result->error_msg,
                                      sizeof(result->error_msg));
        result->complete = true;
        result->method_used = method;
        return;
    }

    switch (method) {
        case SOLVE_METHOD_DIFFERENTIATE:
            solve_derivative(expr, result);
            break;
        case SOLVE_METHOD_INTEGRATE_AUTO:
        case SOLVE_METHOD_INTEGRATE_USUB:
        case SOLVE_METHOD_INTEGRATE_PARTS:
        case SOLVE_METHOD_INTEGRATE_TRIGSUB:
        case SOLVE_METHOD_INTEGRATE_PARTIAL_FRAC:
            solve_integral(expr, result, method);
            break;
        case SOLVE_METHOD_LIMIT:
            solve_limit(expr, result);
            break;
        case SOLVE_METHOD_DIRECT_EVAL:
            solve_evaluate(expr, result);
            break;
        case SOLVE_METHOD_SIMPLIFY:
        default:
            solve_simplify(expr, result);
            break;
    }
}
