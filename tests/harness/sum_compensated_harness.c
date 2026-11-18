/* Compile the unmodified production helper with term-array evaluator doubles.
 * The Python fixture extracts its whole definition, never its algorithm. */
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "sum_ce_add.h"
typedef float ce_float_t;
typedef struct ast_node ast_node_t;
struct ast_node {
    int bound;
    union { struct { const ast_node_t *lo, *hi, *expr; char var; } sum; } d;
};
#define EVAL_VAR_SAME(a, b) ((a) == (b))
#define MAX_VARS 1
#define CE_OK 0
#define CE_ERR_TOO_COMPLEX 1
#define CE_ERR_OVERFLOW 2
#define CE_ERR_CANCELLED 3
#define CE_SAFE_FLOAT 1
#define CE_EVAL_RECORD_STEPS 0
#define CE_DISABLE_STEP_RECORDING 1
#define CE_SUM_FADD(a,b) sum_checked_add((a),(b))
#define ce_isfinite(x) isfinite(x)
#define steps_addf(...) ((void)0)
/* eval.c's non-TINY sample hooks (CE_SAFE_FLOAT_TINY is 0 here) */
#define FT_SAMPLE_(c, e, w) eval_expr((c), (e))
#define FT_MASS_RESET_() ((void)0)
#define FT_DROPS_OK_(scale) 1
typedef struct { int error; struct { bool bound; char name; float value; } vars[1]; } eval_ctx_t;
static struct { bool show_steps; } g_settings;
static const float *terms;
static int nread, fail_at, cancel_at, cancel_calls;
static void set_err(eval_ctx_t *c, int e, const char *s) { (void)s; if (!c->error) c->error=e; }
static bool eval_expr_as_int(eval_ctx_t *c, const ast_node_t *n, int *i, const char *s)
{ (void)c; (void)s; *i=n->bound; return true; }
static float lookup_var(eval_ctx_t *c, char v)
{ (void)v; if (!c->vars[0].bound) { c->error=1; return 0; } return c->vars[0].value; }
static bool eval_set_var(eval_ctx_t *c, char v, float x)
{ c->vars[0].bound=true; c->vars[0].name=v; c->vars[0].value=x; return true; }
static void eval_clear_var(eval_ctx_t *c, char v) { (void)v; c->vars[0].bound=false; }
static bool check_cancel(void) { return ++cancel_calls == cancel_at; }
static void progress_check(void) {}
static float eval_expr(eval_ctx_t *c, const ast_node_t *n)
{ (void)n; if (nread==fail_at) { nread++; set_err(c,7,"term error"); return 0; } return terms[nread++]; }
#include "sum_production.inc"
void sum_run(const float *ts, int n, int cancel, int fail, unsigned *out)
{
    ast_node_t lo={0}, hi={0}, term={0}, sum={0};
    eval_ctx_t c={0};
    float value;
    hi.bound=n-1; sum.d.sum.lo=&lo; sum.d.sum.hi=&hi;
    sum.d.sum.expr=&term; sum.d.sum.var='n';
    c.vars[0].bound=true; c.vars[0].name='n'; c.vars[0].value=37;
    terms=ts; nread=0; fail_at=fail; cancel_at=cancel; cancel_calls=0;
    value=eval_sum_compensated(&c,&sum);
    out[0]=sum_bits(value); out[1]=c.error; out[2]=nread;
    out[3]=cancel_calls; out[4]=sum_bits(c.vars[0].value); out[5]=c.vars[0].bound;
}
unsigned sum_add_bits(unsigned a, unsigned b) { return sum_bits(sum_ce_add(sum_float(a),sum_float(b))); }
