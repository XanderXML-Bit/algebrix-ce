"""Opt-in OS 5.8 CEmu regression for tiny CE arithmetic, NOT hardware proof.

On main 1dbddef the owned ROM showed:

* ``2.9843E-38-2.0235E-38`` stored the non-IEEE 00513E68 and printed 0;
* its ``^0.5`` stored the normal 1FE771A2 (9.8e-20) but printed 0, while
  ``sqrt()`` and ``^1`` of it stored 0, its ``ln`` read the bytes with a
  hidden bit, ``1E-30/`` it gave 134029272 and it divided by itself to 1;
* ``10^(-40)``, ``10^(-44)``, ``1E20^(-2)`` and ``2^(-127)*2^127`` gave 0;
* the literal ``1.3E-38`` (a normal float) read as 0;
* small normal answers such as 1E-30 printed 0.

CEBASE (with CE_SAFE_FLOAT_TINY) now answers "Tiny value
unsupported" for a nonzero value below FLT_MIN, the launcher refuses such a
literal before anything runs, exact cancellation and both zeros still work,
and a nonzero answer below 1e-6 prints in scientific form. Every row reads
the fresh CE_RESP and the launcher's own Ans (g_last_answer); one session
checks that a refusal keeps the previous Ans.

Requires ALGEBRIX_ROM (owned ROM), debug CEMU_HEADLESS and a fresh make all.
Never commit ROMs or state files.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))
ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MAP = ROOT / "bin/launcher/CALCENG.map"
pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file()
         and len(list(ROOT.glob("bin/*/*.8xp"))) == 35 and MAP.is_file()),
    reason="needs owned ALGEBRIX_ROM, debug CEMU_HEADLESS, and fresh make all",
)

TINY = "Tiny value unsupported"
# Every input that decides these answers; a binary older than any of them
# would test the previous source.
INPUTS = ["src/eval.c", "src/eval.h", "src/ce_float_ops.h", "src/token.c", "src/token.h",
          "src/split/processor.c", "src/core/launcher/main.c", "split/Makefile.base",
          "split/Makefile.launcher"]
BINARIES = ["bin/base/CEBASE.8xp", "bin/launcher/CALCENG.8xp"]

# (input, first result line, Ans bits). Ans equals main's for every row:
# only the printed text changed (it was "= 0" on main for all but the last
# group), so these pin that the parse and arithmetic stayed the same.
ANSWERS = [
    ("2^-30*2^-30*2^-30*2^-30*2^-6", "= 1.17549e-38", "00800000"),   # exactly FLT_MIN
    ("2^-30*2^-30*2^-30*2^-30*2^-5-2^-30*2^-30*2^-30*2^-30*2^-6", "= 1.17549e-38", "00800000"),
    ("2^(-125)", "= 2.351e-38", "0100002D"),         # TI-OS powf, 45 ulp high as on main
    ("2E-38", "= 2e-38", "00D9C7EB"),
    ("2.9843E-38*1", "= 2.9843e-38", "01227B19"),   # CE atof reads it 7 ulp high
    ("1E-20*1E-17", "= 1e-37", "02081CF2"),
    ("EXP(-87)", "= 1.64582e-38", "00B336A1"),
    ("1E-30", "= 1e-30", "0DA24268"),
    ("SIN(1E-30)", "= 1e-30", "0DA24267"),
    ("SINH(1E-30)", "= 1e-30", "0DA24268"),
    ("TAN(1E-30)", "= 1e-30", "0DA24266"),
    ("ABS(-2E-38)", "= 2e-38", "00D9C7EB"),
    ("(1E-10)^2", "= 1e-20", "1E3CE50E"),
    ("1E-7", "= 1e-07", "33D6BF97"),
    ("0.0000005", "= 5e-07", "350637BE"),
    ("0.000001", "= 0.000001", "358637BE"),
    # Finite sums (CEBASE's compensated sum): a correction below FLT_MIN is
    # dropped, so normal sums of small terms still answer.
    ("0+SUM(1E-37/3*K,K,1,10)", "= 1.83334e-36", "041BF681"),
    ("0+SUM(1E-36*K/7,K,1,50)", "= 1.82143e-34", "07721C04"),
    ("0+SUM(1/K^2,K,1,1000)", "= 1.64393", "3FD26C72"),
    ("0+LIM((1E-30*X+2E-36/10)-1.5E-36/10,X,0)", "= 5e-38", "01881CEB"),   # round 9
    # review round 1: powers of 1, FLT_MIN itself, a Kahan step whose
    # term - correction cancels below FLT_MIN against a total of 1.5
    # (TI-OS atof reads 1.2E-38 as 0, so the launcher refuses 2^1.2E-38's
    # literal; this power's |expo*ln(base)| is 1.2e-42.)
    ("1.0000001^(1E-35)", "= 1", "3F800000"),
    ("0.5^126", "= 1.17549e-38", "00800001"),     # TI-OS powf: 1 ulp high
    ("0+GEOMSUM(2,0.5,40)", "= 4", "40800000"),
    ("0+DISTANCE(0,0,3,4)", "= 5", "40A00000"),
    ("0+GEOMSUM(3,0.1,40)", "= 3.33333", None),   # r^n below FLT_MIN (round 3)
    ("0+DISTANCE(0,0,1E-20,1)", "= 1", "3F800000"),
    # review round 4: samples whose own value is below FLT_MIN are dropped
    # when they cannot move the result by half an ulp (answers as on main)
    ("2*INT(EXP(-X^2),X,-10,10)", "= 3.5449", None),      # TI-OS expf
    ("1+INT(X*EXP(-X^2),X,-20,20)", "= 1", None),
    ("1+SUM(0.5^N,N,1,200)", "= 2", None),
    ("1+SUM(K*0.5^K,K,1,200)", "= 3", None),
    ("1+SUM(0.5^K/K,K,1,200)", "= 1.69315", None),
    ("3+SUM(-(0.5^K),K,1,200)", "= 2", None),
    ("2*NDERIV(X^4,X,4)", "= 48.0001", None),          # TI-OS powf
    # review round 5: the marked factor may come first
    ("2*INT(E^(-X^2)*COS(X),X,-15,15)", "= 2.76078", None),
    ("1+SUM(0.5^K/(K+1),K,1,200)", "= 1.38629", None),
    ("1+SUM(0.5^K+1,K,1,200)", "= 202", None),   # absorbed by the + 1
    ("0+SUM((K-2)*(K-3)*0.75+1E-37*(K-1)*(3-K)-1.0000001E-37*(K-1)*(K-2)/2,K,1,3)",
     "= 1.5", "3FC00000"),
    # exact cancellation and both zeros (the CE's (-1)*0 is +0)
    ("1E-30-1E-30", "= 0", "00000000"),
    ("2.9843E-38-2.9843E-38", "= 0", "00000000"),
    ("(-1)*0", "= 0", "00000000"),
    ("0/(-5)", "= 0", "00000000"),
    ("(-1)*0+5", "= 5", "40A00000"),
    ("SQRT((-1)*0)", "= 0", "00000000"),
    ("((-1)*0)^2", "= 0", "00000000"),
    ("0^0", "= 1", "3F800000"),
    # ordinary integer controls
    ("16777216-1", "= 16777215", "4B7FFFFF"),
    ("2^30", "= 1073741824", "4E800000"),
    ("2+3", "= 5", "40A00000"),
]

# Worker errors (the input reached CEBASE).
ERRORS = [
    ("2.9843E-38-2.0235E-38", TINY),
    ("(2.9843E-38-2.0235E-38)^0.5", TINY),
    ("SQRT(2.9843E-38-2.0235E-38)", TINY),
    ("LN(2.9843E-38-2.0235E-38)", TINY),
    ("1E-30/(2.9843E-38-2.0235E-38)", TINY),
    ("(2.9843E-38-2.0235E-38)/(2.9843E-38-2.0235E-38)", TINY),
    ("(2.9843E-38-2.0235E-38)^1", TINY),
    ("2^(-127)*2^127", TINY),
    ("10^(-40)", TINY),
    ("10^(-44)", TINY),
    ("1E20^(-2)", TINY),
    ("1E-20*1E-20", TINY),
    ("0.5^1000", TINY),
    ("EXP(-88)", TINY),
    ("ACOT(3E38)", TINY),
    ("0+SUM(1E-20*1E-20,K,1,2)", TINY),
    ("0+SUM(3E-38*K/7,K,1,50)", TINY),            # its first term is 4.3e-39
    ("0+SUM(EXP(-88),K,1,3)", TINY),
    ("SECH(88.5)", TINY),                         # main: = 0
    ("CSCH(-88.5)", TINY),
    # Root GEOMSUM/SLOPE/DISTANCE go to CECSEQ/CEALGEO; nested ones run here.
    ("0+GEOMSUM(SECH(88.5),0.5,40)", TINY),       # review round 2
    ("0+SLOPE(0,0,1E30,1E-10)", TINY),            # native dy/dx flushed to 0
    ("0+DISTANCE(0,0,1E-20,0)", TINY),            # main: = 0 (1e-40 squared)
    ("0+TANGENT(1E-36*(X+1),X,0)", TINY),         # fp - fm = 2e-39
    # review round 4: drops that are not provably harmless stay refused
    ("0+INT(EXP(-X^2),X,100,200)", TINY),         # every sample dropped
    ("1+LIM(X^7,X,0)", TINY),                     # 0 cannot be told from tiny
    ("1+SUM(SQRT(1E-20*1E-20)+K,K,1,3)", TINY),   # refused below the root
    ("0+SUM(1E36*0.5^K,K,126,130)", TINY),        # its dropped terms are 2e-2 of it
    ("0+SUM(E^(-100*K)*1E30*1E30+1E9,K,1,1)", TINY),  # the bound overflows (round 5)
    ("2^-30*2^-30*2^-30*2^-30*2^-6*0.5", TINY),
    ("2^-30*2^-30*2^-30*2^-30*2^-6/2", TINY),
    # CE __fsub gives 0 for 1.5*2^-126 - 2^-126 (main printed "= 0"): only
    # the scaled add sees the difference is nonzero.
    ("2^-30*2^-30*2^-30*2^-30*2^-6*1.5-2^-30*2^-30*2^-30*2^-30*2^-6", TINY),
    # 1/1E38 in the integer-power path (main printed "= 0").
    ("(1E19)^(-2)", TINY),
    # lim's extrapolation: 5e-39 from normal samples, which the CE's __fdiv
    # flushed to 0 (1a298dc printed "= 0"; review round 9).
    ("0+LIM((1E-30*X+2E-37/10)-1.5E-37/10,X,0)", TINY),
    # a function consuming a marked 0: the refusal, not 1/0 (round 9)
    ("1+SUM(1/GEOMSUM(0.5^K,1,3),K,127,127)", TINY),
    ("1/((-1)*0)", "Division by zero"),
    ("((-1)*0)^(-1)", "Division by zero"),
    ("0^(-1)", "Division by zero"),
    ("1E38*10", "Overflow"),
    ("10^39", "Power overflow"),
]

# Refused by the launcher's tokenizer: no worker runs, the input stays.
LITERALS = ["1.3E-38", "1E-38", "1E-40", "2+1E-45", "2^1.2E-38"]

# A literal whose exponent does not fit in a token stops in the launcher too
# (review round 9: 1.0000000000E-99 read as 1e-09, 1.000000000000000E-99 as 1).
LONG_LITERALS = ["1.0000000000E-99", "1.000000000000000E-99", "2+1.0000000000E+38",
                 "0.0000000000000001"]   # 1e-16: its only nonzero digit is cut


@pytest.fixture(scope="module")
def tiny_state(tmp_path_factory):
    newest = max((ROOT / s for s in INPUTS), key=lambda p: p.stat().st_mtime_ns)
    stale = [b for b in BINARIES if (ROOT / b).stat().st_mtime_ns < newest.stat().st_mtime_ns]
    assert not stale, f"binaries older than {newest}; rebuild make all: {stale}"
    out = tmp_path_factory.mktemp("realrom_tiny")
    subprocess.run(
        [sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
         "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
        check=True, timeout=600,
    )
    return str(out / "launched.ce")


def _shot_dir(tmp_path: Path) -> Path:
    d = os.environ.get("ALGEBRIX_TINY_SHOTS")
    if d:
        Path(d).mkdir(parents=True, exist_ok=True)
        return Path(d)
    return tmp_path


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("latin1")


def _evaluate(emu, expr: str) -> dict:
    import realrom

    resp = realrom.map_symbol(MAP, "_g_response")
    ans = realrom.map_symbol(MAP, "_g_last_answer")
    status = realrom.map_symbol(MAP, "_s_status")
    emu.cmd(f"fill {resp:06X} 2 0")
    emu.type(expr)
    emu.key("enter", 9000)
    raw = realrom._peek(emu, resp, realrom.RESULT_WINDOW + 128)
    fresh = ((raw[0] | (raw[1] << 8)) == realrom.CE_IPC_MAGIC
             and raw[2] == realrom.ipc_version())
    # Packed ce_response_t: 30-byte header (Ans at 26..29), result[512],
    # error[128] (src/split/ipc.h).
    return {
        "fresh": fresh, "ok": fresh and raw[3] == 1,
        "line1": _cstr(raw[30:542]).split("\n", 1)[0] if fresh else "",
        "error": _cstr(raw[542:670]) if fresh else "",
        "resp_ans": raw[26:30][::-1].hex().upper() if fresh else None,
        "ans": realrom._peek(emu, ans, 4)[::-1].hex().upper(),
        "status": _cstr(realrom._peek(emu, status, 27)),
    }


def _one(state: str, expr: str, shot: Path) -> dict:
    import realrom

    with realrom.Emu(binary=CEMU, image=state) as emu:
        emu.run(300)
        r = _evaluate(emu, expr)
        emu.shot(shot)
        log = emu.quit()
    r["reset"] = realrom.reset_seen(log)
    return r


@pytest.mark.parametrize("expr,line1,ans", ANSWERS)
def test_answer_text_and_ans(tiny_state, tmp_path, expr, line1, ans):
    r = _one(tiny_state, expr, _shot_dir(tmp_path) / f"answer-{ANSWERS.index((expr, line1, ans)):02d}.bmp")
    assert not r["reset"], (expr, r)
    assert r["fresh"] and r["ok"], (expr, r)
    assert r["line1"] == line1, (expr, r)
    if ans is None:     # TI-OS rounding not pinned; Ans must still be the answer
        assert r["resp_ans"] == r["ans"] and r["ans"] != "00000000", (expr, r)
    else:
        assert r["resp_ans"] == ans and r["ans"] == ans, (expr, r)


@pytest.mark.parametrize("expr,err", ERRORS)
def test_refusal_text(tiny_state, tmp_path, expr, err):
    r = _one(tiny_state, expr, _shot_dir(tmp_path) / f"error-{ERRORS.index((expr, err)):02d}.bmp")
    assert not r["reset"], (expr, r)
    assert r["fresh"] and not r["ok"], (expr, r)
    assert r["error"] == err and r["line1"] == "", (expr, r)


@pytest.mark.parametrize("expr", LITERALS)
def test_tiny_literal_stops_in_the_launcher(tiny_state, tmp_path, expr):
    r = _one(tiny_state, expr, _shot_dir(tmp_path) / f"literal-{LITERALS.index(expr):02d}.bmp")
    assert not r["reset"], (expr, r)
    assert not r["fresh"], ("no worker may run", expr, r)
    assert r["status"] == TINY, (expr, r)


@pytest.mark.parametrize("expr", LONG_LITERALS)
def test_long_literal_stops_in_the_launcher(tiny_state, tmp_path, expr):
    r = _one(tiny_state, expr, _shot_dir(tmp_path) / f"long-{LONG_LITERALS.index(expr):02d}.bmp")
    assert not r["reset"], (expr, r)
    assert not r["fresh"], ("no worker may run", expr, r)
    assert r["status"] == "Number too long", (expr, r)


def test_refusals_keep_the_previous_ans_in_one_session(tiny_state, tmp_path):
    import realrom

    seq = ["2+3", "10^(-40)", "1E-40", "2.9843E-38-2.0235E-38",
           "(2.9843E-38-2.0235E-38)^0.5", "2+4"]
    rows = []
    with realrom.Emu(binary=CEMU, image=tiny_state) as emu:
        emu.run(300)
        for i, expr in enumerate(seq):
            r = _evaluate(emu, expr)
            emu.shot(_shot_dir(tmp_path) / f"session-{i:02d}.bmp")
            rows.append((expr, r))
            emu.key("clear", 300)
            if r["status"]:
                emu.key("clear", 300)   # a local error keeps the input line
        log = emu.quit()
    assert not realrom.reset_seen(log)
    five, six = "40A00000", "40C00000"
    (_, r0), (_, r1), (_, r2), (_, r3), (_, r4), (_, r5) = rows
    assert r0["ok"] and r0["line1"] == "= 5" and r0["ans"] == five, rows[0]
    assert r1["fresh"] and r1["error"] == TINY and r1["ans"] == five == r1["resp_ans"], rows[1]
    assert not r2["fresh"] and r2["status"] == TINY and r2["ans"] == five, rows[2]
    assert r3["fresh"] and r3["error"] == TINY and r3["ans"] == five, rows[3]
    assert r4["fresh"] and r4["error"] == TINY and r4["ans"] == five, rows[4]
    assert r5["ok"] and r5["line1"] == "= 6" and r5["ans"] == six, rows[5]
