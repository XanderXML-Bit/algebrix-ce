"""Opt-in OS 5.8 CEmu proof that eval's native stack backstop fires, NOT hardware.

The shipped build never reaches the backstop on 127-char input: its depth
caps stop eval first (tests/cemu/test_realrom_cebase_stack.py). So this test
builds diagnostic CEBASE and CECSER copies with effective caps of 200 and
CE_EVAL_STACK_RESERVE raised to 2,400 B. That makes the
backstop the first limit to trip. The test checks that:

* the refusal comes from the backstop, not the cap, at the reserve's edge,
  with no NMI or reset, and with the "Eval recursion limit" text;
* the refusal does not leak into the next program run: in one emulator
  session, "2+3" still works right after a refused input;
* a refusal inside fourier() cannot report success. Its Simpson helper
  used to return 0 on any eval error ("Fourier_2 = 0"); it now fails on a
  refused sample, and the g_eval_refused check in ce_response_from_runtime
  backs up any caller that still swallows one.

Requires ALGEBRIX_ROM (owned ROM), debug CEMU_HEADLESS, the CEdev toolchain on
PATH (cedev-config) and a fresh make all. Never commit ROMs or state files.
"""
from __future__ import annotations

import os
import re
import shutil
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
         and shutil.which("cedev-config")
         and len(list(ROOT.glob("bin/*/*.8xp"))) >= 35),
    reason="requires owned ROM, debug CEmu, CEdev on PATH and fresh make all",
)

RESERVE = 2400
HW_EXTRA = ("arTIfiCE.8xp", "fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv")


def _module_makefile(name: str) -> Path:
    for mk in ROOT.glob("split/Makefile.*"):
        if re.search(rf"^NAME\s*=\s*{name}\s*$", mk.read_text(errors="replace"), re.M):
            return mk
    raise AssertionError(f"no makefile for {name}")


def _patch(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    assert text.count(old) == 1, (path, old)
    path.write_text(text.replace(old, new))


def _raise_cebase_diagnostic_cap(path: Path) -> None:
    text = path.read_text()
    definitions = list(re.finditer(
        r"(?<!\S)-D\s*CE_EVAL_MAX_DEPTH(?:=[^\s\\]*)?(?=\s|\\|$)", text))
    assert len(definitions) == 1, (path, "expected one depth definition")
    definition = definitions[0]
    assert definition.group() == "-DCE_EVAL_MAX_DEPTH=16", (path, definition.group())
    path.write_text(text[:definition.start()] + "-DCE_EVAL_MAX_DEPTH=200"
                    + text[definition.end():])


# The fixture deploys the built bin/ with only CEBASE and CECSER rebuilt, so
# every other module must already match these shared sources.
SHARED_INPUTS = ["src/eval.c", "src/eval.h", "src/calcengine.h", "src/split/ipc.c",
                 "src/split/ipc.h", "src/split/runtime_globals.c", "src/split/processor.c"]


@pytest.fixture(scope="module")
def diag(tmp_path_factory):
    newest = max((ROOT / s for s in SHARED_INPUTS), key=lambda p: p.stat().st_mtime_ns)
    stale = sorted(str(b.relative_to(ROOT)) for b in ROOT.glob("bin/*/*.8xp")
                   if b.stat().st_mtime_ns < newest.stat().st_mtime_ns)
    assert not stale, f"stale binaries older than {newest}; rebuild make all: {stale}"
    tree = tmp_path_factory.mktemp("eval_backstop")
    for d in ("src", "split", "bin"):
        shutil.copytree(ROOT / d, tree / d)
    shutil.copy2(ROOT / "Makefile", tree / "Makefile")
    (tree / "deploy/hardware-ready").mkdir(parents=True)
    for n in HW_EXTRA:
        shutil.copy2(ROOT / "deploy/hardware-ready" / n, tree / "deploy/hardware-ready" / n)
    # Diagnostic only: the backstop must trip before the depth cap. (A module
    # that sets -DCE_EVAL_STACK_RESERVE in its makefile, such as CELSCX,
    # would ignore this source patch.)
    _patch(tree / "src/eval.c", "#define CE_EVAL_STACK_RESERVE 832u",
           f"#define CE_EVAL_STACK_RESERVE {RESERVE}u")
    _patch(tree / "src/eval.h", "#define CE_EVAL_MAX_DEPTH 50", "#define CE_EVAL_MAX_DEPTH 200")
    # CEBASE's scalar cap overrides eval.h. Raise it only in this disposable
    # diagnostic build so the reserve-edge test still exercises the backstop.
    _raise_cebase_diagnostic_cap(tree / "split/Makefile.base")
    for name in ("CEBASE", "CECSER"):
        mk = _module_makefile(name).relative_to(ROOT)
        assert not re.search(r"-DCE_EVAL_STACK_RESERVE\b", (tree / mk).read_text()), (
            name, "diagnostic source reserve would be overridden by module flags")
        subprocess.run(["make", "-f", str(mk), "STACK_CHECKER=true"], cwd=tree, check=True,
                       timeout=600, stdout=subprocess.DEVNULL)
    out = tree / "rr"
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(tree), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return tree, out / "launched.ce"


def _run(tree: Path, state: Path, expr: str, log: Path) -> dict:
    base_map = tree / "bin/base/CEBASE.map"
    eval_addr = realrom.map_symbol(base_map, "_eval_expr")
    resp_addr = realrom.map_symbol(tree / "bin/launcher/CALCENG.map", "_g_response")
    with realrom.Emu(binary=CEMU, image=str(state), stderr_path=log) as e:
        e.run(300)
        e.cmd(f"fill {resp_addr:x} 2 0")
        e.cmd(f"bp {realrom.USER_MEM:06X}")
        e.cmd(f"bp {eval_addr:06X}")
        e.type(expr)
        e.key("enter", 8000)
        raw = realrom._peek(e, resp_addr, 64 + 512 + 128)
        text = e.quit()
    prog, eval_sp, hops = "CALCENG", [], []
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"\[HOP\] pc=([0-9A-F]+) spl=([0-9A-F]+) op1=(\S*)", line)
        if not m:
            continue
        pc, spl = int(m.group(1), 16), int(m.group(2), 16)
        if pc == realrom.USER_MEM:
            prog = m.group(3)
            hops.append(prog)
        elif prog == "CEBASE" and pc == eval_addr:
            eval_sp.append(spl)
    trace = log.read_text(errors="replace")
    r = realrom.parse_response(raw, "", realrom.ipc_version())
    return dict(raw=raw, fresh=r["fresh"], ok=r["ok"], hops=hops,
                reset=realrom.reset_seen(text), nmi="stack limit" in trace,
                eval_margin=(min(eval_sp) - realrom.STACK_LIMIT) if eval_sp else None)


def test_backstop_refuses_at_its_reserve_in_cebase(diag, tmp_path):
    tree, state = diag
    r = _run(tree, state, "+".join(["2"] * 64), tmp_path / "sum64.log")
    assert not r["reset"] and not r["nmi"], r
    assert r["hops"] == ["CEBASE", "CALCENG"] and r["fresh"] and not r["ok"], r
    assert b"\0Eval recursion limit\0" in r["raw"], r
    # The cap is 200 here, so only the backstop can stop a 64-term chain,
    # and it stops it within one eval frame of the reserve.
    assert RESERVE - 64 <= r["eval_margin"] < RESERVE, r


def test_refusal_does_not_leak_into_the_next_run(diag, tmp_path):
    # One emulator session: the refused run, then a plain one. Each worker
    # run is a fresh program load, so the refusal flag must not carry over.
    tree, state = diag
    resp_addr = realrom.map_symbol(tree / "bin/launcher/CALCENG.map", "_g_response")
    version = realrom.ipc_version()
    seen = []
    log = tmp_path / "session.log"
    with realrom.Emu(binary=CEMU, image=str(state), stderr_path=log) as e:
        e.run(300)
        e.cmd(f"bp {realrom.USER_MEM:06X}")
        for expr in ("+".join(["2"] * 64), "2+3"):
            e.cmd(f"fill {resp_addr:x} 2 0")
            e.type(expr)
            e.key("enter", 8000)
            raw = realrom._peek(e, resp_addr, 64 + 512 + 128)
            seen.append((raw, realrom.parse_response(raw, "= ", version)))
            e.key("clear", 300)
        text = e.quit()
    assert not realrom.reset_seen(text)
    hops = [m.group(1) for m in re.finditer(
        rf"\[HOP\] pc={realrom.USER_MEM:06X} spl=[0-9A-F]+ op1=(\S*)", log.read_text(errors="replace"))]
    # Both inputs ran in CEBASE: the refused one, then the plain one.
    assert hops == ["CEBASE", "CALCENG", "CEBASE", "CALCENG"], hops
    (raw0, r0), (raw1, r1) = seen
    assert r0["fresh"] and not r0["ok"] and b"\0Eval recursion limit\0" in raw0, raw0
    assert r1["fresh"] and r1["ok"] and r1["text"].split("\n", 1)[0] == "= 5", raw1


@pytest.mark.parametrize("terms,want_ok", [(10, True), (45, False)])
def test_swallowed_refusal_never_reports_success(diag, tmp_path, terms, want_ok):
    tree, state = diag
    expr = "fourier(" + "+".join(["x"] * terms) + ",x,2)"
    r = _run(tree, state, expr, tmp_path / f"fourier{terms}.log")
    assert not r["reset"] and not r["nmi"], r
    assert r["hops"] == ["CECSER", "CALCENG"] and r["fresh"], r
    assert r["ok"] == want_ok, r
    if want_ok:
        assert b"Fourier_2 = " in r["raw"] and b"Fourier_2 = 0\0" not in r["raw"], r
    else:
        assert b"\0Eval recursion limit\0" in r["raw"] and b"Fourier_2" not in r["raw"], r
