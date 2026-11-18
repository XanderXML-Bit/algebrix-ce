"""Opt-in real-ROM regression for the module hand-off stack leak / reset.

Skips unless all of these exist (never in hosted CI; the ROM is never committed):
  ALGEBRIX_ROM       owned TI-84 Plus CE ROM (OS 5.8.x for bundled arTIfiCE 2.1)
  CEMU_HEADLESS      cemu-headless built with tools/emulator/linux-realrom/cemu-headless-debug.patch
  bin/*/*.8xp        a fresh `make all` of this checkout

Before the fix, int(x^2,x) looped CECINT 21 times and hit the stack limit, and
each NULL-callback hop leaked 209 bytes of the 4 KB OS stack.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MODULES = sorted(ROOT.glob("bin/*/*.8xp"))

pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file() and len(MODULES) >= 35),
    reason="real-ROM lane needs ALGEBRIX_ROM, a debug CEMU_HEADLESS and a built bin/",
)


@pytest.fixture(scope="module")
def launched_state(tmp_path_factory) -> str:
    import subprocess
    out = tmp_path_factory.mktemp("realrom")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return str(out / "launched.ce")


def test_module_calls_return_and_release_the_stack(launched_state: str) -> None:
    import realrom

    # int(1/(x^2-1),x) is a 3-deep chain CECINT -> CECISM -> CECIAD -> CEIPFR:
    # CECISM's forward overwrites CE_REQ.return_to, so only the re-entry flag
    # lets CECINT resume, and the child must launch from archive (no RAM copy).
    # int(x*sin(x),x) fails partial fractions first; CECIAD must go on to the
    # later strategies on re-entry and reach integration by parts (CEIPRT).
    exprs = ["int(x^2,x)", "solve(x+2,x)", "lim(x^2,x,2)", "factor(x^2-5x+6)",
             "int(1/(x^2-1),x)", "int(x*sin(x),x)"] * 2
    r = realrom.run_hops(CEMU, launched_state, exprs)
    assert r["hops"], "no [HOP] lines: CEMU_HEADLESS is not the debug-patched build"
    assert not r["reset"], "calculator reset during module hand-off"
    progs = [h["prog"] for h in r["hops"]]
    # Each evaluation must come back to CALCENG; 12 evaluations -> 12 returns.
    assert progs.count("CALCENG") == len(exprs), progs
    returns = [h["spl"] for h in r["hops"] if h["prog"] == "CALCENG"]
    assert len(set(returns)) == 1, f"stack drifts between evaluations: {[hex(s) for s in returns]}"
    # No self-relaunch loops: bounded program entries per evaluation.
    assert len(progs) <= 12 * len(exprs), len(progs)
    deep = ["CECINT", "CECISM", "CECIAD", "CEIPFR", "CECIAD", "CECISM", "CECINT", "CALCENG"]
    joined = " ".join(progs)
    assert " ".join(deep) in joined, f"3-deep chain did not unwind level by level: {progs}"
    seq = "CEIPFR CECIAD CEITRG CECIAD CEIUSB CECIAD CEIPRT CECIAD"
    assert seq in joined, f"strategy sequence did not continue in order on re-entry: {progs}"
    assert r["min_spl"] - realrom.STACK_LIMIT > 1024, "less than 1 KB of stack headroom left"


def test_strategy_report_ranks_all_four_children(launched_state: str) -> None:
    import realrom

    # intstrategy(...) runs CECINT -> CECIAD -> CEISTR, and CEISTR tries all four
    # strategy children. os_RunPrgm never returns on the CE, so before the fix
    # CEISTR reached only CEIPFR and handed its error straight up.
    exprs = ["intstrategy(x*sin(x),x)", "intstrategy(1/(x^2-1),x)"]
    r = realrom.run_hops(CEMU, launched_state, exprs)
    assert r["hops"], "no [HOP] lines: CEMU_HEADLESS is not the debug-patched build"
    assert not r["reset"], "calculator reset during the strategy report"
    progs = [h["prog"] for h in r["hops"]]
    assert progs.count("CALCENG") == len(exprs), progs
    seq = ("CECINT CECIAD CEISTR CEIPFR CEISTR CEITRG CEISTR CEIUSB CEISTR CEIPRT CEISTR "
           "CECIAD CECINT CALCENG")
    assert " ".join(progs) == " ".join([seq] * len(exprs)), progs
    child_spl = {h["spl"] for h in r["hops"] if h["prog"] in ("CEIPFR", "CEITRG", "CEIUSB", "CEIPRT")}
    assert len(child_spl) == 1, f"strategy children ran at different depths: {sorted(child_spl)}"
    returns = {h["spl"] for h in r["hops"] if h["prog"] == "CALCENG"}
    assert len(returns) == 1, returns

    # The home screen shows only line 1, so read the whole CE_RESP text from RAM.
    texts = realrom.run_result_text(CEMU, launched_state, ROOT / "bin/launcher/CALCENG.map",
                                    "Best: ", exprs)
    assert all(t["fresh"] and t["ok"] for t in texts), texts
    by_parts, pfrac = texts[0]["text"], texts[1]["text"]
    assert by_parts.startswith("Best: integration by parts\n"), by_parts
    assert "\n1) integration by parts (score 80): matched by CEIPRT" in by_parts, by_parts
    assert by_parts.count("\n") == 4, by_parts
    assert "Unsupported advanced integral request" not in by_parts, by_parts
    assert pfrac.startswith("Best: partial fractions\n"), pfrac
    assert "\n1) partial fractions (score 88): matched by CEIPFR" in pfrac, pfrac
