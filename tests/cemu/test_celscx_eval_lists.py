"""CELSCX (lists/complex worker) evaluator with the module's own flags, desktop.

an internal review moved eval_node's list-op case into the noinline eval_list_op():
its 201 B list_t sat in every eval_node frame, which on the OS 5.8 ROM cost
CELSCX about 322 B per nested sum/prod/int/lim level and left only 350 B
below the eval stack backstop. These tests build CELSCX's processor from
split/Makefile.lists_complex (through test_module_flag_parity's driver) and
pin the answers of the moved code and of deep stored nests, plus clean pool
accounting. The stack margins themselves are measured on the real ROM in
test_realrom_celscx_stack.py.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

import test_module_flag_parity as parity

# CELSCX has its own entry point; the parity driver supplies main().
_ENTRY_SOURCES = ("src/modules/lists_complex/main.c", "src/modules/common/module_entry.c")


def sum_nest(k: int, leaf: str, chain: int = 0) -> str:
    return "sum(" * k + leaf + ",x,1,2)" * k + "+2" * chain + "->A"


CASES = [
    # List literals go through eval_list_op (the CE keypad cannot type
    # braces, but the parser and worker accept them).
    ("dim({1,2,3})", True, "= 3"),
    ("dim({sin(2),2+3,4})->A", True, "= 3"),
    ("dim({" + ",".join(["2"] * 50) + "})", True, "= 50"),
    # (A 51st element is dropped silently at MAX_LIST_SIZE: pre-existing,
    # brace-only behaviour, deliberately not pinned here.)
    ("dim(L1)", True, "= 0"),
    # dim(5) silently returns 0 today: an unrelated known defect, not an oracle.
    ("dim({1/0})", False, "Division by zero"),
    ("dim({sqrt(-1)})", False, "sqrt domain"),
    ("cumsum({1,2,3})", False, "List op (not scalar)"),
    ("sortA({3,1,2})", False, "List op (not scalar)"),
    # median( parses as a statistics node, never a scalar list op, so
    # list_median is unreachable from typed input (see the parser guard).
    ("median({1,2,3})->A", False, "Statistics not enabled in this module"),
    # Stores and nests that run in CELSCX on the CE.
    ("5->A", True, "= 5"),
    ("2" + "+2" * 46 + "->A", False, "Eval recursion limit"),
    ("2" + "+2" * 6 + "->A", True, "= 14"),
    ("2" + "+2" * 7 + "->A", False, "Eval recursion limit"),
    ("2" + "+2" * 61 + "->A", False, "Eval recursion limit"),
    ("sin(" * 24 + "0" + ")" * 24 + "->A", False, "Eval recursion limit"),
    (sum_nest(1, "x"), True, "= 3"),
    (sum_nest(6, "x"), True, "= 96"),
    (sum_nest(7, "x"), False, "Eval recursion limit"),
    ("sin(" * 6 + "0" + ")" * 6 + "->A", True, "= 0"),
    ("sin(" * 7 + "0" + ")" * 7 + "->A", False, "Eval recursion limit"),
    (sum_nest(10, "x"), False, "Eval recursion limit"),
    (sum_nest(10, "x", 6), False, "Eval recursion limit"),
    (sum_nest(8, "dim({x^0.5,2})"), False, "Eval recursion limit"),
    # Nested list ops: each level is eval_expr + eval_list_op, no eval_node.
    *[("dim({" * k + "2" + "})" * k + "->A", k <= 6,
       "= 1" if k <= 6 else "Eval recursion limit") for k in (1, 2, 6, 7, 12, 17)],
    # ...and an element error from the innermost level comes back up.
    ("dim({" * 6 + "1/0" + "})" * 6 + "->A", False, "Eval recursion limit"),
    ("dim({" * 5 + "1/0" + "})" * 5 + "->A", False, "Division by zero"),
    ("dim({2,dim({3,4,5}),6})->A", True, "= 3"),
    ("polar(2,1)", True, "x = 1.0806"),
    ("rect(3,4)", True, "r = 5"),
    ("rect(1," + "sin(" * 22 + "0" + ")" * 22 + ")", False, "Eval error"),
]


@pytest.fixture(scope="module")
def celscx(tmp_path_factory: pytest.TempPathFactory) -> Path:
    src = parity.SPLIT / "Makefile.lists_complex"
    text = src.read_text(encoding="utf-8")
    for s in _ENTRY_SOURCES:
        assert s in text, s
        text = text.replace(s, "")
    out = tmp_path_factory.mktemp("celscx")
    mk = out / "Makefile.lists_complex"
    mk.write_text(text, encoding="utf-8")
    return parity._build_module_driver(mk, out)


def _run(exe: Path, exprs: list[str]) -> list[tuple[bool, str, int, int]]:
    # latin-1, not parity._run_stats' UTF-8: the "Parsed:" echo of a variable
    # store prints a raw byte today ("5->L" + chr(0xF2)), a separate defect.
    proc = subprocess.run([str(exe), *exprs], capture_output=True, timeout=120,
                          env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    assert proc.returncode == 0, proc.stderr[-4000:]
    rows = proc.stdout.decode("latin-1").splitlines()
    assert len(rows) == len(exprs), rows
    out = []
    for row in rows:
        parts = row.split("\t")
        out.append((parts[0] == "1", "\t".join(parts[1:-2]), int(parts[-2]), int(parts[-1])))
    return out


def test_celscx_list_ops_and_stored_nests(celscx: Path) -> None:
    rows = _run(celscx, [e for e, _, _ in CASES])
    for (expr, want_ok, want), (ok, text, leaked, dfree) in zip(CASES, rows):
        assert ok == want_ok, (expr, text)
        first = text.split("\\n", 1)[0]
        assert first == want, (expr, text)
        assert leaked == 0 and dfree == 0, (expr, leaked, dfree)


def test_list_ops_charge_and_restore_depth(celscx: Path) -> None:
    # Store + list + five binops reaches the depth-eight numeric leaf.
    # One more binop must refuse; dropping the list's depth charge would
    # incorrectly let it succeed. The desktop build isolates the depth cap
    # from the eZ80 native stack backstop.
    cases = [
        ("dim({2" + "+2" * 5 + "})->A", True, "= 1"),
        ("dim({2" + "+2" * 6 + "})->A", False, "Eval recursion limit"),
        ("dim({2" + "+2" * 47 + "})->A", False, "Eval recursion limit"),
        ("dim({2" + "+2" * 48 + "})->A", False, "Eval recursion limit"),
        # Sibling list calls below unary nesting must release their charges.
        ("-" * 4 + "dim({" + ",".join(["dim({2})"] * 5) + "})->A",
         True, "= 5"),
        ("-" * 44 + "dim({" + ",".join(["dim({2})"] * 5) + "})->A",
         False, "Eval recursion limit"),
    ]
    for (expr, want_ok, want), (ok, text, leaked, dfree) in zip(
            cases, _run(celscx, [e for e, _, _ in cases])):
        assert ok == want_ok and text.split("\\n", 1)[0] == want, (expr, text)
        assert leaked == 0 and dfree == 0, (expr, leaked, dfree)


def test_eval_list_op_is_out_of_line_in_the_source() -> None:
    src = (parity.ROOT / "src/eval.c").read_text(encoding="utf-8")
    body = parity._c_function_body(src, "static ce_float_t eval_node(")
    assert "list_t tmp_list" not in body
    head = src.split("static ce_float_t eval_list_op(eval_ctx_t *ctx, const ast_node_t *node)\n{", 1)[0][-200:]
    assert "__attribute__((noinline))" in head
    # eval_expr dispatches list ops itself (no eval_node frame per level).
    expr = parity._c_function_body(src, "ce_float_t eval_expr(")
    assert "ret = eval_list_op(ctx, node);" in expr


def test_parser_never_builds_the_scalar_list_ops() -> None:
    """eval_list_op's sum/prod/min/max/mean/median/stdev cases are reachable
    only if the parser builds NODE_LIST_OP with those fns. It builds only
    the ones below (median(...) etc. parse as statistics nodes), so the
    deepest list call below eval's stack backstop is eval_expr on an
    element. If this changes, measure the new path at the backstop edge on
    the real ROM (test_realrom_celscx_stack.py, an internal review)."""
    import re
    src = (parity.ROOT / "src/parser.c").read_text(encoding="utf-8")
    built = set(re.findall(r"\b(?:fn\s*=|list_op\.fn\s*=)\s*(LIST_[A-Z]+)", src))
    assert built == {"LIST_CUMSUM", "LIST_DELTALIST", "LIST_SORTA", "LIST_SORTD",
                     "LIST_DIM", "LIST_SEQ"}, built
    scalar = re.compile(r"\bLIST_(SUM|PROD|MIN|MAX|MEAN|MEDIAN|STDEV)\b")
    users = sorted(str(p.relative_to(parity.ROOT)) for p in (parity.ROOT / "src").rglob("*.c")
                   if p.name not in ("eval.c", "list.c") and scalar.search(p.read_text(errors="replace")))
    assert not users, f"scalar list ops built outside the parser: {users}"
