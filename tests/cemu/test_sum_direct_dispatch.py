"""Exercise both production sum call sites and the actual evaluator wrapper."""
from pathlib import Path
import re
import subprocess

import pytest
import test_module_flag_parity as parity

ROOT = Path(__file__).resolve().parents[2]


def wrapper_source():
    source = (ROOT / "src/eval.c").read_text()
    start = source.index("ce_float_t eval_expr(eval_ctx_t *ctx, const ast_node_t *node)\n{")
    return source[start:source.index("\n}\n", start) + 3]


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef float ce_float_t;
typedef struct { int error, eval_depth; } eval_ctx_t;
typedef struct ast_node ast_node_t;
struct ast_node { int type; union {
 float number;
 struct { const ast_node_t *left, *right; } binop;
 struct { const ast_node_t *operand; } unop;
 struct { const ast_node_t *arg; } func;
} d; };
enum { NODE_NUMBER, NODE_SUM, NODE_BINOP, NODE_UNOP, NODE_FUNC };
enum { CE_OK, CE_ERR_INTERNAL, CE_ERR_DEPTH };
#define CE_EVAL_MAX_DEPTH 50
#define CE_EVAL_SUM_COMPENSATED 1
#define CE_EVAL_ENABLE_NODE_LIST 0
#define _EZ80 1
#define EVAL_STACK_LOW(probe) ((void)(probe), low_stack)
static bool low_stack, g_eval_refused;
static float g_eval_refused_ans, g_last_answer = 55;
static int node_calls, helper_calls, observed_depth, fail_helper;
static void set_err(eval_ctx_t *c, int err, const char *msg) {
 (void)msg; c->error = err;
}
static float eval_sum_compensated(eval_ctx_t *c, const ast_node_t *n) {
 (void)n; helper_calls++; observed_depth = c->eval_depth;
 if (fail_helper) c->error = CE_ERR_INTERNAL;
 return fail_helper ? 0 : 17;
}
static float eval_node(eval_ctx_t *c, const ast_node_t *n, float l, float r) {
 (void)l; (void)r; node_calls++;
 return n->type == NODE_SUM ? eval_sum_compensated(c,n) : 0;
}
WRAPPER
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
int main(void) {
 ast_node_t sum = { .type = NODE_SUM }, number = { .type = NODE_NUMBER, .d.number = 3 };
 eval_ctx_t c = {0, 7};
 CHECK(eval_expr(&c,&sum) == 17);
 CHECK(c.eval_depth == 7 && observed_depth == 8 && helper_calls == 1);
 CHECK(node_calls == (CE_EVAL_SUM_DIRECT_DISPATCH ? 0 : 1));
 fail_helper = 1;
 CHECK(eval_expr(&c,&sum) == 0 && c.error == CE_ERR_INTERNAL && c.eval_depth == 7);
 CHECK(helper_calls == 2);
 CHECK(eval_expr(&c,&sum) == 0 && helper_calls == 2);
 c.error = 0; c.eval_depth = 50;
 CHECK(eval_expr(&c,&sum) == 0 && c.error == CE_ERR_DEPTH && helper_calls == 2);
 CHECK(g_eval_refused && g_eval_refused_ans == 55 && c.eval_depth == 50);
 c.error = 0; c.eval_depth = 7; low_stack = true;
 CHECK(eval_expr(&c,&sum) == 0 && c.error == CE_ERR_DEPTH && helper_calls == 2);
 c.error = 0;
 CHECK(eval_expr(&c,&number) == 3 && c.eval_depth == 7);
 low_stack = false; fail_helper = 0;
 CHECK(eval_expr(&c,&sum) == 17 && c.eval_depth == 7 && helper_calls == 3);
 c.error = 0;
 CHECK(eval_expr(&c,0) == 0 && c.error == CE_ERR_INTERNAL);
 return 0;
}
'''


def build_wrapper(tmp_path, direct, wrapper=None):
    source = tmp_path / f"wrapper{direct}.c"
    source.write_text(HARNESS.replace("WRAPPER", wrapper_source() if wrapper is None else wrapper))
    exe = source.with_suffix("")
    cc = parity._host_cc()
    if cc is None:
        pytest.skip("needs a host C compiler")
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                    f"-DCE_EVAL_SUM_DIRECT_DISPATCH={direct}", str(source), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    return subprocess.run([str(exe)], capture_output=True).returncode


@pytest.mark.parametrize("direct", [0, 1])
def test_actual_wrapper_depth_checks_and_call_site(tmp_path, direct):
    assert build_wrapper(tmp_path, direct) == 0


@pytest.mark.parametrize("mutation", ["dispatch", "increment", "decrement", "depth", "stack", "error"])
def test_wrapper_negative_controls(tmp_path, mutation):
    wrapper = wrapper_source()
    if mutation == "dispatch":
        broken = wrapper.replace("if (node->type == NODE_SUM)", "if (0)")
    elif mutation == "increment":
        broken = wrapper.replace("ctx->eval_depth++;", "", 1)
    elif mutation == "decrement":
        broken = wrapper.replace("ctx->eval_depth--;", "", 1)
    elif mutation == "depth":
        broken = wrapper.replace("if (ctx->eval_depth >= CE_EVAL_MAX_DEPTH) goto refuse;", "")
    elif mutation == "stack":
        broken = wrapper.replace("if (EVAL_STACK_LOW(&ctx)) goto refuse;", "(void)low_stack;")
    else:
        broken = wrapper.replace("if (ctx->error != CE_OK) return 0;", "")
    assert broken != wrapper
    assert build_wrapper(tmp_path, 1, broken) != 0


def test_direct_flag_scope():
    source = (ROOT / "src/eval.c").read_text()
    assert re.search(r"#ifndef CE_EVAL_SUM_DIRECT_DISPATCH\s+#define CE_EVAL_SUM_DIRECT_DISPATCH 0\s+#endif", source)
    for makefile in parity._built_module_makefiles():
        flags = parity._makefile_vars(makefile)["PROGRAM_CFLAGS"]
        expected = "1" if makefile.name == "Makefile.lists_complex" else "0"
        assert (parity._flag(flags, "CE_EVAL_SUM_DIRECT_DISPATCH") or "0") == expected


def test_both_production_call_sites_agree(tmp_path, monkeypatch):
    monkeypatch.setattr(parity, "_HOST_EXCLUDED_SOURCES",
                        parity._HOST_EXCLUDED_SOURCES | {"src/modules/lists_complex/main.c"})
    makefile = ROOT / "split/Makefile.lists_complex"
    drivers = [parity._build_module_driver(makefile, tmp_path,
               extra_cflags=(f"-DCE_EVAL_SUM_DIRECT_DISPATCH={flag}",), tag=f"_{flag}")
               for flag in (0, 1)]
    expressions = [
        "sum(n,n,1,10000)->A", "sum(n^2,n,1,1000)->A",
        "sum(0.1,n,1,10000)->A", "sum(1/n,n,1,10000)->A",
        "sum(sum(k,k,1,n),n,1,100)->A", "sum(n,n,10,1)->A",
        "sum(n,n,1,10002)->A", "sum(1/(n-3),n,1,5)->A",
        "sum(q,n,1,5)->A", "sum(n,n,0,1e9)->A",
        "sum(1/n^2,n,1,inf)->A", "2+3->A",
    ]
    rows = []
    for driver in drivers:
        completed = subprocess.run([str(driver), *expressions], capture_output=True, check=True, timeout=120)
        rows.append([line.split("\t") for line in completed.stdout.decode("latin-1").splitlines()])
    assert rows[0] == rows[1]
    assert rows[0][0][0] == "1" and rows[0][0][1].split(r"\n")[0] == "= 50005000"
    assert rows[0][1][0] == "1" and rows[0][1][1].split(r"\n")[0] == "= 333833504"
    assert all(row[-2:] == ["0", "0"] for row in rows[0])
