"""Build-artifact guard for the evaluator's per-level stack cost (internal review).

eval_expr() is the only evaluator frame repeated once per AST level. When it
was an eval_expr -> eval_expr_inner pair, the real OS 5.8 ROM measured 131 B
per level, and CEBASE ran past the 0xD19881 stack limit on a 32-term sum.
The fix keeps the recursion in a small eval_expr() and moves the big switch
into the noinline eval_node(). This test reads the -fstack-usage reports of
every CE module that links src/eval.c and fails if the recursive frame grows
back or LTO inlines the switch into it. Requires a fresh `make all`.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
# Inputs whose change can move the frames: the evaluator, its headers, the
# shared program rules and the module's own makefile (CE_EVAL_* flags).
COMMON_INPUTS = [ROOT / "src/eval.c", ROOT / "src/eval.h", ROOT / "src/calcengine.h",
                 ROOT / "src/list.c", ROOT / "src/list.h", ROOT / "split/program.mk"]
# eval_expr locals measured at 9 B on CEdev v11.2 (-Oz). A few bytes of
# slack for compiler drift; the old pair spent 104 B of locals per level.
MAX_EVAL_EXPR_LOCALS = 16
# eval_node runs once below the deepest eval_expr, so the 832 B stack
# backstop in src/eval.c must cover it plus its libm/steps callees, and it
# is stacked once per nested sum/prod/int/lim/nderiv level. an internal review moved
# the list-op case (a 201 B list_t) into the noinline eval_list_op: CELSCX's
# eval_node fell from 277 B to 104 B, and its backstop-edge margin on the OS
# 5.8 ROM rose from 350 B. Largest now: 104 B (CELSCX), 102 B (CEBASE).
# (CEAPDV's 299 B was never linked: its map has no eval_expr.)
MAX_EVAL_NODE_LOCALS = 128
# eval_list_op holds the 201 B tmp_list; it must stay out of eval_node and
# is called by eval_expr itself, so a nested list level costs eval_expr +
# eval_list_op (223 B measured). list_median keeps its sort buffer static.
LIST_MODULES = {"CELSCX"}
MAX_EVAL_LIST_OP_LOCALS = 240
MAX_LIST_LEAF_LOCALS = 32


def _reports() -> dict[str, dict[str, int]]:
    out = {}
    for su in sorted(ROOT.glob("obj/*/lto.su")):
        frames: dict[str, int] = {}
        for line in su.read_text(errors="replace").splitlines():
            m = re.match(r".*:([\w.]+)\t(\d+)\t", line)
            if m:
                # LTO may suffix a clone (eval_node.llvm.N); keep the base name.
                name = m.group(1).split(".", 1)[0]
                frames[name] = max(frames.get(name, 0), int(m.group(2)))
        if "eval_expr" in frames:
            out[su.parent.name] = frames
    return out


def _linked_eval() -> set[str]:
    """Modules whose linked binary contains eval_expr (LTO drops it from
    routers that list src/eval.c but never evaluate)."""
    names = set()
    for mp in ROOT.glob("bin/*/*.map"):
        if re.search(r"^_eval_expr\s+=", mp.read_text(errors="replace"), re.M):
            names.add(mp.stem)
    return names


@pytest.fixture(scope="module")
def reports():
    reps = _reports()
    if not reps:
        pytest.skip("no CE -fstack-usage reports; run make all")
    makefiles = {}
    for mk in ROOT.glob("split/Makefile.*"):
        name = re.search(r"^NAME\s*=\s*(\S+)", mk.read_text(errors="replace"), re.M)
        if name:
            makefiles[name.group(1)] = mk
    stale = []
    for n in reps:
        inputs = COMMON_INPUTS + ([makefiles[n]] if n in makefiles else [])
        newest = max(p.stat().st_mtime_ns for p in inputs)
        if (ROOT / "obj" / n / "lto.su").stat().st_mtime_ns < newest:
            stale.append(n)
    assert not stale, f"stale stack reports (rebuild make all): {stale}"
    return reps


def test_every_eval_module_has_a_report(reports):
    linked = _linked_eval()
    assert "CEBASE" in linked and "CEBASE" in reports
    missing = sorted(linked - set(reports))
    assert not missing, f"modules link eval_expr but have no stack report: {missing}"


def test_eval_recursion_frame_stays_small(reports):
    bad = {n: f["eval_expr"] for n, f in reports.items()
           if f["eval_expr"] > MAX_EVAL_EXPR_LOCALS}
    assert not bad, f"eval_expr locals grew past {MAX_EVAL_EXPR_LOCALS} B: {bad}"


def test_eval_node_frame_stays_inside_the_backstop_budget(reports):
    bad = {n: f["eval_node"] for n, f in reports.items()
           if f.get("eval_node", 0) > MAX_EVAL_NODE_LOCALS}
    assert not bad, f"eval_node locals grew past {MAX_EVAL_NODE_LOCALS} B: {bad}"


def test_eval_switch_is_not_inlined_into_the_recursion(reports):
    for name, frames in reports.items():
        assert "eval_node" in frames, f"{name}: eval_node was inlined into eval_expr"
        assert "eval_expr_inner" not in frames, name


def test_list_ops_stay_out_of_the_eval_node_frame(reports):
    linked = _linked_eval()
    for name in sorted(LIST_MODULES):
        assert name in reports and name in linked, f"{name}: no eval stack report"
        assert "eval_list_op" in reports[name], (
            f"{name}: eval_list_op was inlined into eval_node (its 201 B list "
            "would sit in every eval_node frame again)")
        frames = reports[name]
        assert frames["eval_list_op"] <= MAX_EVAL_LIST_OP_LOCALS, (name, frames["eval_list_op"])
        # A leaf LTO inlines into eval_list_op reads as 0 here; the
        # eval_list_op bound above then covers it.
        for leaf in ("list_median", "list_stdev", "list_sum", "list_prod", "list_min",
                     "list_max", "list_mean"):
            assert frames.get(leaf, 0) <= MAX_LIST_LEAF_LOCALS, (name, leaf, frames.get(leaf))


def test_celscx_keeps_its_larger_stack_reserve():
    """CELSCX answers every "...->A" store, so nested int/sum/lim reach the
    eval backstop there. On the OS 5.8 ROM the default 832 B reserve left
    525 B at the worst measured alignment (21 B steps), so CELSCX builds
    with 960 B (test_realrom_celscx_stack.py checks >= 600 B)."""
    text = (ROOT / "split/Makefile.lists_complex").read_text(errors="replace")
    m = re.search(r"-DCE_EVAL_STACK_RESERVE=(\d+)u?\b", text)
    assert m and int(m.group(1)) >= 960, "CELSCX lost its CE_EVAL_STACK_RESERVE"
    assert "#ifndef CE_EVAL_STACK_RESERVE" in (ROOT / "src/eval.c").read_text(errors="replace")


def test_complex_evaluator_is_not_linked_into_any_module():
    """src/complex.c's ceval_expr recursion has neither the small frame nor
    the stack backstop of eval_expr. CELSCX lists complex.c, but nothing in
    a CE build calls ceval_expr, so the linker drops it (a CELSCX build
    without complex.c is the same size). Any module that starts calling it
    must first get the same treatment (internal review)."""
    # Source references first, so desktop CI (no maps) still checks them:
    # calls, function pointers or macros anywhere outside complex.[ch].
    refs = sorted(
        str(p.relative_to(ROOT)) for p in sorted((ROOT / "src").rglob("*.[ch]"))
        if p.name not in ("complex.c", "complex.h")
        and re.search(r"\bceval_(expr|inner)\b", p.read_text(errors="replace")))
    assert not refs, f"ceval_expr referenced from shipped sources: {refs}"
    maps = list(ROOT.glob("bin/*/*.map"))
    if not maps:
        pytest.skip("no CE link maps; run make all")
    # A CEdev map lists every linked label: a mutant CELSCX that calls
    # ceval_expr shows "_ceval_expr = D1E5D9" (an internal review control).
    linked = sorted(mp.stem for mp in maps
                    if re.search(r"^_ceval_(expr|inner)\b", mp.read_text(errors="replace"), re.M))
    assert not linked, f"complex.c's unguarded ceval_expr is linked into {linked}"
