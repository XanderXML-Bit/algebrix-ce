"""Opt-in OS 5.8 CEmu stack regressions for plain CEBASE input, NOT hardware.

CEBASE stack regression: "2^2^...^2^1" (31 levels, 63 chars) reset the
calculator with the stack-limit NMI in CEBASE's evaluator. Worse, a 32-term
sum ran eval frames ~630 B below 0xD19881 WITHOUT the NMI: the OS only traps
a write to that one byte, and a frame can step over it. So the margin here is
the minimum of two measurements:

* paint: 0xD19881..SP-32 is filled with 0xA5 at the prompt and the deepest
  changed byte is found afterwards (same method as test_realrom_worker_stack);
* breakpoints: the debug cemu-headless logs SP at every entry of the
  recursive functions (eval_expr, eval_node, ast_print, decode_node in
  CEBASE; parse_expr in CALCENG). A frame below the limit shows up here even
  when it skipped the painted window. Both programs run from userMem, so a
  hit counts only for the program whose userMem entry precedes it.

Requires ALGEBRIX_ROM (owned ROM), debug CEMU_HEADLESS and a fresh make all.
Never commit ROMs or emulator state files.
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
    # eval_expr_inner is the former name of eval_node; a map may
    # lack either, so this test also measures (and fails) an old build.
    "CEBASE": ("bin/base/CEBASE.map", ["eval_expr", "eval_node", "eval_expr_inner",
                                       "ast_print", "decode_node"]),
}


def chain(leaf: str, op: str, terms: int) -> str:
    return op.join([leaf] * terms)


def nest(unit: str, n: int, mid: str, close: str) -> str:
    return unit * n + mid + close * n


def cases() -> list[tuple[str, str, str]]:
    """(expression, kind, expected): kind is value, error, syntax or fresh."""
    out = []
    for op, leaf, v16 in (("+", "2", "= 32"), ("-", "1", "= -14"),
                           ("*", "1", "= 1"), ("/", "1", "= 1")):
        out += [(chain(leaf, op, 16), "value", v16),
                (chain(leaf, op, 17), "error", "Eval recursion limit"),
                (chain(leaf, op, 32), "error", "Eval recursion limit"),
                (chain(leaf, op, 50), "error", "Eval recursion limit"),
                (chain(leaf, op, 64), "error", "Eval recursion limit")]
    out += [
        # Keep historical reset inputs, with the fixed scalar depth cap.
        ("2^" * 31 + "1", "error", "Eval recursion limit"),
        (chain("2", "^", 32), "error", "Eval recursion limit"),
        (chain("1", "^", 50), "error", "Eval recursion limit"),
        (chain("2", "^", 64), "syntax", ""),
        (nest("-(", 24, "2", ")"), "error", "Eval recursion limit"),
        (nest("-(", 15, "2", ")"), "value", "= -2"),
        (nest("-(", 16, "2", ")"), "error", "Eval recursion limit"),
        (nest("-(", 42, "2", ")"), "syntax", ""),
        (nest("-", 126, "2", ""), "syntax", ""),
        (nest("(", 49, "2", ")"), "value", "= 2"),
        (nest("sin(", 25, "0", ")"), "error", "Eval recursion limit"),
        (nest("abs(", 25, "-3", ")"), "error", "Eval recursion limit"),
        (nest("sin(", 15, "0", ")"), "value", "= 0"),
        (nest("sin(", 16, "0", ")"), "error", "Eval recursion limit"),
        (nest("abs(", 14, "-3", ")"), "value", "= 3"),
        (nest("abs(", 15, "-3", ")"), "error", "Eval recursion limit"),
        # Historical power leaf probes remain as clean refusals.
        ("2.5^0.5" + "+2" * 48, "error", "Eval recursion limit"),
        ("2.5^0.5" + "+2" * 49, "error", "Eval recursion limit"),
        # Exercise power operands immediately below and above depth 16.
        ("2.5^0.5" + "+2" * 14, "value", "= 29.5811"),
        ("2.5^0.5" + "+2" * 15, "error", "Eval recursion limit"),
    ]
    assert all(len(e) <= 127 for e, _, _ in out)
    return out


def _syms(prog: str) -> dict[int, str]:
    mp, names = FUNCS[prog]
    out = {}
    for n in names:
        try:
            out[realrom.map_symbol(ROOT / mp, "_" + n)] = n
        except KeyError:
            continue
    assert "eval_expr" in out.values() or prog != "CEBASE", "no _eval_expr in CEBASE.map"
    return out


@pytest.fixture(scope="module")
def launched(tmp_path_factory):
    # File count is only the skip gate: refuse binaries older than the
    # sources this test is about.
    for binary, sources in (("bin/base/CEBASE.8xp", ["src/eval.c", "src/split/processor.c",
                                                     "src/ast.c", "src/split/astwire.c"]),
                            ("bin/launcher/CALCENG.8xp", ["src/parser.c", "src/core/launcher/main.c"])):
        newest = max((ROOT / s for s in sources), key=lambda p: p.stat().st_mtime_ns)
        assert (ROOT / binary).stat().st_mtime_ns >= newest.stat().st_mtime_ns, (
            f"stale {binary}: newer input {newest}; rebuild make all")
    out = tmp_path_factory.mktemp("realrom_cebase_stack")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return out


def test_deep_cebase_input_keeps_stack_margin(launched):
    resp_addr = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    status_addr = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_s_status")
    syms = {p: _syms(p) for p in FUNCS}
    rows = []
    for index, (expr, kind, expected) in enumerate(cases()):
        log_path = launched / f"case-{index}.log"
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
            for addr in set(syms["CALCENG"]) | set(syms["CEBASE"]):
                e.cmd(f"bp {addr:06X}")
            e.type(expr)
            e.key("enter", 8000)
            e.shot(launched / f"case-{index}.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            raw = realrom._peek(e, resp_addr, 64 + 512 + 128)
            status = realrom._peek(e, status_addr, 32).split(b"\0", 1)[0]
            log = e.quit()
        trace = log_path.read_text(errors="replace")
        low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
        # Every breakpoint SP is a real stack depth, whichever program ran
        # it, so all of them bound the margin. Attribution (by the userMem
        # entry that precedes a hit) only labels hits and proves coverage.
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
        margin = min([low] + [sp_hit - realrom.STACK_LIMIT for sp_hit in all_sp])
        response = realrom.parse_response(raw, "= ", realrom.ipc_version())
        row = dict(expression=expr, kind=kind, margin=margin, paint_margin=low,
                   function_margins=margins, hops=hops, reset=realrom.reset_seen(log),
                   nmi="stack limit" in trace, fresh=response["fresh"], ok=response["ok"],
                   status=status.decode("latin-1"))
        rows.append(row)
        (launched / "cebase-high-water.json").write_text(json.dumps(rows, indent=2))
        assert not row["reset"] and not row["nmi"], row
        assert margin >= MIN_MARGIN, row
        if kind == "syntax":
            # The launcher refused it: CEBASE never ran.
            assert hops == [], row
            assert not response["fresh"] and status == b"Syntax error", row
            continue
        # CEBASE ran once and the launcher was reloaded through userMem; a
        # different hop pattern would make the attribution above unreliable.
        assert hops == ["CEBASE", "CALCENG"], row
        assert response["fresh"], (row, "no new valid CE_RESP")
        assert "CEBASE.eval_expr" in margins, (row, "CEBASE did not evaluate")
        if kind == "value":
            assert response["ok"], (row, raw)
            # The whole first result line, so "= 1" cannot match "= 100".
            assert response["text"].split("\n", 1)[0] == expected, (row, response["text"])
        elif kind == "error":
            assert not response["ok"], (row, raw)
            # The whole error field: NUL-padded on both sides.
            assert b"\0" + expected.encode("ascii") + b"\0" in raw, (row, raw)
    assert len(rows) == len(cases())
