"""Opt-in OS 5.8 CEmu stack regressions for the CELSCX worker, NOT hardware.

The launcher routes list stores, stores of a value with a list or complex
part, top-level list ops, polar( and rect( to CELSCX, so anything typed
before such a ->A is evaluated there. Its eval_node frame carried a 201 B
list_t (277 B of locals), stacked once per nested sum/prod/int/lim level, and
on main 2df0aca nested sums met the eval stack backstop with only 350 B left
(internal review). The fix moves the list case out of line (eval_node 104 B) and
gives CELSCX a 960 B backstop reserve; the worst alignment the sweep found
then is a nested int.

Since an internal review a scalar store of a plain real value (`2+2->A`, `sin(0)->A`)
goes to CEBASE instead, whose checked arithmetic refuses tiny values (#140).
Those inputs stay here as CEBASE cases (same margin bar, CEBASE's depth cap
16), and the int sweep keeps a CELSCX twin that ends in `+dim(L1)` (a list
part, so it still routes to CELSCX).

Margin = min(painted low-water, every breakpoint SP), measured as in
test_realrom_cebase_stack.py. Braces cannot be typed on the CE keypad: cases
with { } are typed with ( ) and the launcher input buffer is patched before
ENTER (INJECTED). Requires ALGEBRIX_ROM (owned ROM), debug CEMU_HEADLESS and a
fresh make all. Never commit ROMs or emulator state files.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))
import realrom

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file()
         and len(list(ROOT.glob("bin/*/*.8xp"))) >= 35),
    reason="requires owned ROM, debug CEmu and fresh make all",
)

MIN_MARGIN = 600
FUNCS = {
    "CALCENG": ("bin/launcher/CALCENG.map", ["parse_expr", "encode_node"]),
    "CELSCX": ("bin/lists_complex/CELSCX.map",
               ["eval_expr", "eval_node", "eval_list_op", "ast_print", "decode_node"]),
    "CEBASE": ("bin/base/CEBASE.map",
               ["eval_expr", "eval_node", "ast_print", "decode_node"]),
}


def nest(op: str, k: int, leaf: str, tail: str, chain: int = 0, suffix: str = "") -> str:
    return f"{op}(" * k + leaf + tail * k + "+2" * chain + suffix + "->A"


X, B = "CELSCX", "CEBASE"


def cases() -> list[tuple[str, str, str, int, str]]:
    """(expression, kind, expected, wait_ms, program); kind is value or error.
    program is the worker the launcher routes the input to."""
    out = [
        # Scalar stores of a plain real value: CEBASE (cap 16).
        ("5->A", "value", "= 5", 8000, B),
        ("2" + "+2" * 46 + "->A", "error", "Eval recursion limit", 8000, B),
        ("2" + "+2" * 48 + "->A", "error", "Eval recursion limit", 8000, B),
        ("2" + "+2" * 6 + "->A", "value", "= 14", 8000, B),
        ("2" + "+2" * 7 + "->A", "value", "= 16", 8000, B),
        ("2" + "+2" * 14 + "->A", "value", "= 30", 8000, B),
        ("2" + "+2" * 15 + "->A", "error", "Eval recursion limit", 8000, B),
        ("2" + "+2" * 49 + "->A", "error", "Eval recursion limit", 8000, B),
        ("2" + "+2" * 61 + "->A", "error", "Eval recursion limit", 8000, B),
        ("2" + "*2" * 61 + "->A", "error", "Eval recursion limit", 8000, B),
        ("sin(" * 24 + "0" + ")" * 24 + "->A", "error", "Eval recursion limit", 8000, B),
        ("sin(" * 6 + "0" + ")" * 6 + "->A", "value", "= 0", 8000, B),
        ("sin(" * 7 + "0" + ")" * 7 + "->A", "value", "= 0", 8000, B),
        ("sin(" * 14 + "0" + ")" * 14 + "->A", "value", "= 0", 8000, B),
        ("sin(" * 15 + "0" + ")" * 15 + "->A", "error", "Eval recursion limit", 8000, B),
        (nest("sum", 10, "x", ",x,1,2)", 6), "error", "Eval recursion limit", 8000, B),
        # CELSCX: top-level sums, polar/rect, lists (fixed depth-eight cap).
        (nest("sum", 6, "x", ",x,1,2)"), "value", "= 96", 8000, X),
        (nest("sum", 7, "x", ",x,1,2)"), "error", "Eval recursion limit", 8000, X),
        ("polar(2,1)", "value", "x = 1.0806", 8000, X),
        # Complex conversion reports its existing generic evaluation error.
        ("rect(1," + "sin(" * 22 + "0" + ")" * 22 + ")", "error", "Eval error", 8000, X),
        (nest("sum", 10, "x", ",x,1,2)"), "error", "Eval recursion limit", 8000, X),
        # INJECTED braces: the list case at depth, through eval_list_op.
        (nest("sum", 8, "dim({x^0.5,2})", ",x,1,2)"), "error", "Eval recursion limit", 8000, X),
        # INJECTED nested list ops. Main 36f82d7 answered up to 7 levels
        # (321 B each); eval_expr now calls eval_list_op directly (256 B).
        ("dim({" * 6 + "2" + "})" * 6 + "->A", "value", "= 1", 8000, X),
        ("dim({" * 7 + "2" + "})" * 7 + "->A", "error", "Eval recursion limit", 8000, X),
        ("dim({" * 9 + "2" + "})" * 9 + "->A", "error", "Eval recursion limit", 8000, X),
        # Retain historical deeper probes as fixed-cap refusals.
        ("dim({" * 10 + "2" + "})" * 10 + "->A", "error", "Eval recursion limit", 8000, X),
        ("dim({" * 12 + "2" + "})" * 12 + "->A", "error", "Eval recursion limit", 8000, X),
    ]
    # Nested int reaches the backstop. The chain shifts the nest 21 B per
    # term; with the 832 B reserve chain 7 was the worst alignment (525 B).
    # Chain 0 is a top-level int (CELSCX); with a chain the store is plain
    # real (CEBASE). The `+dim(L1)` twin keeps every shift in CELSCX.
    # On the CE every one is refused by the eval stack backstop or the cap
    # (the desktop, which has no backstop, says "no convergence" for some).
    # The CEBASE store margins equal those of the same nest without the
    # store, because eval_expr evaluates a store's value (internal review).
    for chain in range(0, 9):
        prog = X if chain == 0 else B
        out.append((nest("int", 9, "x", ",x,0,1)", chain), "error",
                    "Eval recursion limit", 20000, prog))
        out.append((nest("int", 9, "x", ",x,0,1)", chain, "+dim(L1)"), "error",
                    "Eval recursion limit", 20000, X))
    assert all(len(e) <= 127 for e, _, _, _, _ in out)
    return out


def _keys(text: str) -> list[str]:
    out, i = [], 0
    while i < len(text):
        if text.startswith("->", i):
            out.append("sto")
            i += 2
        else:
            out += realrom.keys_for(text[i])
            i += 1
    return out


def _syms(prog: str) -> dict[int, str]:
    mp, names = FUNCS[prog]
    out = {}
    for n in names:
        try:
            out[realrom.map_symbol(ROOT / mp, "_" + n)] = n
        except KeyError:
            continue
    if prog == "CELSCX":
        missing = {"eval_expr", "eval_node", "eval_list_op"} - set(out.values())
        assert not missing, f"CELSCX.map lacks {missing}"
    return out


@pytest.fixture(scope="module")
def launched(tmp_path_factory):
    for binary, sources in (("bin/lists_complex/CELSCX.8xp",
                             ["src/eval.c", "src/list.c", "src/split/processor.c", "src/ast.c",
                              "src/split/astwire.c", "src/eval.h", "src/calcengine.h",
                              "src/list.h", "split/Makefile.lists_complex",
                              "split/program.mk"]),
                            ("bin/base/CEBASE.8xp",
                             ["src/eval.c", "src/split/processor.c", "src/ast.c",
                              "src/split/astwire.c", "src/eval.h", "src/calcengine.h",
                              "split/Makefile.base", "split/program.mk"]),
                            ("bin/launcher/CALCENG.8xp",
                             ["src/parser.c", "src/core/launcher/main.c",
                              "src/split/classify.c"])):
        newest = max((ROOT / s for s in sources), key=lambda p: p.stat().st_mtime_ns)
        assert (ROOT / binary).stat().st_mtime_ns >= newest.stat().st_mtime_ns, (
            f"stale {binary}: newer input {newest}; rebuild make all")
    out = tmp_path_factory.mktemp("realrom_celscx_stack")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return out


def test_deep_celscx_input_keeps_stack_margin(launched):
    resp_addr = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    input_addr = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_s_input")
    syms = {p: _syms(p) for p in FUNCS}
    rows = []
    for index, (expr, kind, expected, wait_ms, program) in enumerate(cases()):
        log_path = launched / f"case-{index}.log"
        typed = expr.replace("{", "(").replace("}", ")")
        with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                         stderr_path=log_path) as e:
            e.run(300)
            regs = e.cmd("regs")
            sp = int(re.search(r"spl=([0-9A-F]+)", regs).group(1), 16)
            size = sp - 32 - realrom.STACK_LIMIT
            assert 600 < size < 4096, regs
            e.cmd(f"fill {realrom.STACK_LIMIT:x} {size} a5")
            e.cmd(f"fill {resp_addr:x} 2 0")
            e.cmd(f"bp {realrom.USER_MEM:06X}")
            for addr in set(syms["CALCENG"]) | set(syms[program]):
                e.cmd(f"bp {addr:06X}")
            # Assumes STO types exactly the two characters "->" into
            # _s_input (checked below when braces are injected).
            for k in _keys(typed):
                e.key(k, 120)
            if typed != expr:
                cur = realrom._peek(e, input_addr, len(typed)).decode("latin-1")
                assert cur.lower() == typed.lower(), (cur, typed)
                for i, ch in enumerate(expr):
                    if ch in "{}":
                        e.cmd(f"fill {input_addr + i:x} 1 {ord(ch):02x}")
            e.key("enter", wait_ms)
            e.shot(launched / f"case-{index}.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            raw = realrom._peek(e, resp_addr, 64 + 512 + 128)
            log = e.quit()
        trace = log_path.read_text(errors="replace")
        low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
        prog, deepest, hops, all_sp = "CALCENG", {}, [], []
        for line in trace.splitlines():
            m = re.match(r"\[HOP\] pc=([0-9A-F]+) spl=([0-9A-F]+) op1=(\S*)", line)
            if not m:
                continue
            pc, spl = int(m.group(1), 16), int(m.group(2), 16)
            all_sp.append(spl)
            if pc == realrom.USER_MEM:
                prog = m.group(3)
                hops.append(prog)
            elif pc in syms.get(prog, {}):
                key = f"{prog}.{syms[prog][pc]}"
                deepest[key] = min(deepest.get(key, spl), spl)
        margins = {k: v - realrom.STACK_LIMIT for k, v in deepest.items()}
        margin = min([low] + [s - realrom.STACK_LIMIT for s in all_sp])
        # A real prefix ("= ", "x = "): an empty one matches the header.
        prefix = expected[:expected.index("=") + 2] if kind == "value" else "= "
        response = realrom.parse_response(raw, prefix, realrom.ipc_version())
        row = dict(expression=expr, program=program, kind=kind, margin=margin, paint_margin=low,
                   function_margins=margins, hops=hops, reset=realrom.reset_seen(log),
                   nmi="stack limit" in trace, fresh=response["fresh"], ok=response["ok"],
                   injected=typed != expr)
        rows.append(row)
        (launched / "celscx-high-water.json").write_text(json.dumps(rows, indent=2))
        assert not row["reset"] and not row["nmi"], row
        assert margin >= MIN_MARGIN, row
        assert hops == [program, "CALCENG"], row
        assert response["fresh"], (row, "no new valid CE_RESP")
        assert f"{program}.eval_expr" in margins, (row, f"{program} did not evaluate")
        if kind == "value":
            assert response["ok"], (row, raw)
            assert response["text"].split("\n", 1)[0] == expected, (row, response["text"])
        else:
            assert not response["ok"], (row, raw)
            assert b"\0" + expected.encode("ascii") + b"\0" in raw, (row, raw)
    assert len(rows) == len(cases())
