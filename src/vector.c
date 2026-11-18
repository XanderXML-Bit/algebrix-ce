/*
 * vector.c — Vector calculus operations
 */
#include "vector.h"
#include "deriv.h"
#include "simplify.h"
#include "eval.h"
#include "ast.h"
#include "memory.h"
#include "steps.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Numerical helpers ───────────────────────────────────── */

ce_float_t vector_dot(const ce_float_t *v1, const ce_float_t *v2, int n)
{
    ce_float_t sum = 0;
    int i;
    for (i = 0; i < n; i++) sum += v1[i] * v2[i];
    return sum;
}

void vector_cross(const ce_float_t *a, const ce_float_t *b, ce_float_t *out)
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

/* ── Pair chain utilities ────────────────────────────────── */

int vector_eval_pair_chain(const ast_node_t *pair_chain,
                            ce_float_t *out, int max_n,
                            eval_ctx_t *ctx)
{
    int count = 0;
    const ast_node_t *p = pair_chain;
    while (p && count < max_n) {
        const ast_node_t *elem = NULL;
        if (p->type == NODE_PAIR) {
            elem = p->d.pair.head;
            p    = p->d.pair.tail;
        } else {
            elem = p;
            p    = NULL;
        }
        if (!elem) break;
        out[count] = eval_expr(ctx, elem);
        if (eval_error(ctx) != CE_OK) return -1;
        count++;
    }
    return count;
}

int vector_collect_pair_exprs(const ast_node_t *pair_chain,
                               const ast_node_t **out, int max_n)
{
    int count = 0;
    const ast_node_t *p = pair_chain;
    while (p && count < max_n) {
        if (p->type == NODE_PAIR) {
            out[count++] = p->d.pair.head;
            p            = p->d.pair.tail;
        } else {
            out[count++] = p;
            p            = NULL;
        }
    }
    return count;
}

/* ── Symbolic gradient ───────────────────────────────────── */

bool vector_gradient(const ast_node_t *f, const char *vars, int n,
                     char *result_buf, int buf_size)
{
    int i, sc;
    int pos = 0;
    char comp_str[96];

    if (!f || !vars || n < 1 || !result_buf || buf_size <= 0) return false;

    steps_add("Gradient: ∇f = (∂f/∂x, ∂f/∂y[, ∂f/∂z])");

    pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), "∇f = (");
    if (pos >= buf_size - 1) return false;

    for (i = 0; i < n; i++) {
        ast_node_t *partial;
        sc = steps_count();
        partial = deriv_diff_quiet(f, vars[i]);
        steps_restore(sc);

        if (!partial) {
            snprintf(result_buf, (size_t)buf_size, "Cannot compute ∂f/∂%c", vars[i]);
            return false;
        }

        partial = ast_simplify(partial);
        ast_to_display_str(partial, comp_str, sizeof(comp_str));
        steps_addf("∂f/∂%c = %s", vars[i], comp_str);

        if (i > 0) {
            pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), ", ");
        }
        pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), "%s", comp_str);
        ast_free(partial);

        if (pos >= buf_size - 2) break;
    }

    if (pos < buf_size - 2)
        pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), ")");

    return true;
}

/* ── Symbolic divergence ─────────────────────────────────── */

bool vector_divergence(const ast_node_t *const *fields,
                       const char *vars, int n,
                       char *result_buf, int buf_size)
{
    int i, sc;
    int pos = 0;
    char comp_str[96];

    if (!fields || !vars || n < 1 || !result_buf || buf_size <= 0) return false;

    steps_add("Divergence: div(F) = ∂F1/∂x + ∂F2/∂y [+ ∂F3/∂z]");

    pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), "div(F) = ");
    if (pos >= buf_size - 1) return false;

    for (i = 0; i < n; i++) {
        ast_node_t *partial;
        if (!fields[i]) continue;

        sc = steps_count();
        partial = deriv_diff_quiet(fields[i], vars[i]);
        steps_restore(sc);

        if (!partial) {
            snprintf(result_buf, (size_t)buf_size,
                     "Cannot compute ∂F%d/∂%c", i+1, vars[i]);
            return false;
        }

        partial = ast_simplify(partial);
        ast_to_display_term(partial, comp_str, sizeof(comp_str));
        steps_addf("∂F%d/∂%c = %s", i+1, vars[i], comp_str);

        if (i > 0) {
            pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), " + ");
        }
        pos += snprintf(result_buf + pos, (size_t)(buf_size - pos), "%s", comp_str);
        ast_free(partial);

        if (pos >= buf_size - 10) break;
    }

    return true;
}

/* ── Symbolic curl ───────────────────────────────────────── */

bool vector_curl(const ast_node_t *F1, const ast_node_t *F2,
                 const ast_node_t *F3,
                 char xv, char yv, char zv,
                 char *result_buf, int buf_size)
{
    /*
     * curl(F) = (∂F3/∂y - ∂F2/∂z,
     *            ∂F1/∂z - ∂F3/∂x,
     *            ∂F2/∂x - ∂F1/∂y)
     */
    ast_node_t *d[6];  /* d[0]=∂F3/∂y, d[1]=∂F2/∂z, d[2]=∂F1/∂z,
                           d[3]=∂F3/∂x, d[4]=∂F2/∂x, d[5]=∂F1/∂y */
    /* eZ80 stack ~4KB: s[6][80]=480B + diff chain ~4KB = overflow.
     * Static is safe: single-threaded; g_diff_depth/g_simplify_depth
     * guards prevent re-entrant calls before this function returns. */
    static char s[6][80];
    int i, sc;
    const ast_node_t *srcs[6];
    char svars[6];

    if (!F1 || !F2 || !F3 || !result_buf || buf_size <= 0) return false;

    srcs[0] = F3; svars[0] = yv;
    srcs[1] = F2; svars[1] = zv;
    srcs[2] = F1; svars[2] = zv;
    srcs[3] = F3; svars[3] = xv;
    srcs[4] = F2; svars[4] = xv;
    srcs[5] = F1; svars[5] = yv;

    steps_add("Curl: ∇×F = (∂F3/∂y-∂F2/∂z, ∂F1/∂z-∂F3/∂x, ∂F2/∂x-∂F1/∂y)");

    for (i = 0; i < 6; i++) {
        sc = steps_count();
        d[i] = deriv_diff_quiet(srcs[i], svars[i]);
        steps_restore(sc);
        if (!d[i]) {
            int j;
            for (j = 0; j < i; j++) ast_free(d[j]);
            snprintf(result_buf, (size_t)buf_size,
                     "Cannot differentiate F%d w.r.t. %c", (i/2)+1, svars[i]);
            return false;
        }
        d[i] = ast_simplify(d[i]);
        /* "%s - %s" below: the subtrahend must be a safe term. */
        if (i & 1) ast_to_display_term(d[i], s[i], sizeof(s[i]));
        else       ast_to_display_str(d[i], s[i], sizeof(s[i]));
    }

    /* curl_x = d[0] - d[1] = ∂F3/∂y - ∂F2/∂z */
    /* curl_y = d[2] - d[3] = ∂F1/∂z - ∂F3/∂x */
    /* curl_z = d[4] - d[5] = ∂F2/∂x - ∂F1/∂y */

    steps_addf("curl_x = ∂F3/∂%c - ∂F2/∂%c = %s - %s", yv, zv, s[0], s[1]);
    steps_addf("curl_y = ∂F1/∂%c - ∂F3/∂%c = %s - %s", zv, xv, s[2], s[3]);
    steps_addf("curl_z = ∂F2/∂%c - ∂F1/∂%c = %s - %s", xv, yv, s[4], s[5]);

    snprintf(result_buf, (size_t)buf_size,
             "curl(F) =\n"
             "  i: %s - %s\n"
             "  j: %s - %s\n"
             "  k: %s - %s",
             s[0], s[1], s[2], s[3], s[4], s[5]);

    for (i = 0; i < 6; i++) ast_free(d[i]);
    return true;
}
