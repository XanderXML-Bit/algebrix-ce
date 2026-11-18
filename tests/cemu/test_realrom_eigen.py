"""Opt-in real-ROM regression for eigenval()/eigenvec() in CEMAT (internal review).

Skips unless all of these exist (never in hosted CI; the ROM is never committed):
  ALGEBRIX_ROM       owned TI-84 Plus CE ROM (OS 5.8.x for bundled arTIfiCE 2.1)
  CEMU_HEADLESS      cemu-headless built with tools/emulator/linux-realrom/cemu-headless-debug.patch
  bin/*/*.8xp        a fresh `make all` of this checkout

Before the fix both ops answered "Polar/advanced op (use at top level)" from
CEBASE.  A later draft passed every desktop test but printed
"v1(lambda=5) not found" for eigenvec(mat2(4,1,2,3)) on the ROM: ez80-clang
re-read the exponent byte of a hoisted pivot from memory the loop had just
overwritten (src/matrix.c matrix_eigenvectors).  Only the eZ80 build shows
that, so this test reads the real CEMAT answers.

The keypad has no ';' key, so 3x3 input is typed with ',' and the row
separators are then written into the launcher's s_input with the debug
build's fill command before ENTER.  Everything after ENTER is the unmodified
launcher -> CEMAT -> launcher path.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MODULES = sorted(ROOT.glob("bin/*/*.8xp"))
CALCENG_MAP = ROOT / "bin/launcher/CALCENG.map"

pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file() and len(MODULES) >= 35),
    reason="real-ROM lane needs ALGEBRIX_ROM, a debug CEMU_HEADLESS and a built bin/",
)


@pytest.fixture(scope="module")
def launched_state(tmp_path_factory) -> str:
    import subprocess
    out = tmp_path_factory.mktemp("realrom_eigen")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return str(out / "launched.ce")


TYPED = [
    ("lambda", "eigenval(mat(2))", "lambda = {2}"),
    ("lambda", "eigenval(mat2(2,1,1,2))", "lambda = {3, 1}"),
    ("lambda", "eigenval(mat2(1,2,3,4))", "lambda = {5.37228, -0.372281}"),
    ("lambda", "eigenval(mat2(1,1,0,1))", "lambda = {1, 1}"),
    ("v1(", "eigenvec(mat(2))", "v1(lambda=2)=[1]"),
    ("v1(", "eigenvec(mat2(2,1,1,2))", "v1(lambda=3)=[1,1]\nv2(lambda=1)=[-1,1]"),
    ("v1(", "eigenvec(mat2(4,1,2,3))", "v1(lambda=5)=[1,1]\nv2(lambda=2)=[-0.5,1]"),
    ("v1(", "eigenvec(mat2(6,-2,2,1))", "v1(lambda=5)=[1,0.5]\nv2(lambda=2)=[0.5,1]"),
    ("v1(", "eigenvec(mat2(1,0,0,1))", "v1(lambda=1)=[1,0]\nv2(lambda=1)=[0,1]"),
    ("v1(", "eigenvec(mat2(1,1,0,1))", "v1(lambda=1)=[1,0]\nv2(lambda=1)=same as v1"),
    ("v1(", "eigenvec(mat2(1e-8,2e-8,2e-8,1e-8))",
     "v1(lambda=3e-8)=[1,1]\nv2(lambda=-1e-8)=[-1,1]"),
    # The CE soft-float truncates: one /10 step per digit gave 4.99999e30,
    # and 6 mantissa digits gave 6.00001e-30.
    ("v1(", "eigenvec(mat2(4e30,1e30,2e30,3e30))",
     "v1(lambda=5e30)=[1,1]\nv2(lambda=2e30)=[-0.5,1]"),
    ("lambda", "eigenval(mat(9e30))", "lambda = {9e30}"),
    ("lambda", "eigenval(mat(6e-30))", "lambda = {6e-30}"),
    ("lambda", "eigenval(mat(0.000123456))", "lambda = {1.2346e-4}"),
    ("lambda", "eigenval(mat2(0,16777216,1/16777216,0))", "lambda = {1, -1}"),
    # The small eigenvalue is det / lambda1, not s - sqrt(disc).
    ("lambda", "eigenval(mat2(1,0.0015,0.0015,0))", "lambda = {1, -2.25e-6}"),
    ("lambda", "eigenval(mat2(1,2,3,6))", "lambda = {7, 0}"),
    ("lambda", "eigenval(mat2(0.3,0.7,0.9,2.1))", "lambda = {2.4, 0}"),
    ("lambda", "eigenval(mat(1e-31))", "lambda = {1e-31}"),
    # matrix_rref / matrix_inverse have the same div = M[r][lead] loop shape
    # as the miscompiled eigenvector loop; pivots other than 1 cover them.
    ("[", "rref(mat2(2,4,1,3))", "[1 0]\n[0 1]"),
    ("[", "rref(mat2(2,4,1,2))", "[1 2]\n[0 0]"),
    ("[", "inv(mat2(2,0,0,4))", "[0.5 0]\n[0 0.25]"),
    ("[", "inv(mat2(4,7,2,6))", "[0.6 -0.7]\n[-0.2 0.4]"),
]


def test_typed_2x2_eigen_answers_on_the_rom(launched_state: str) -> None:
    import realrom
    for prefix in sorted({p for p, _, _ in TYPED}):
        cases = [(e, w) for p, e, w in TYPED if p == prefix]
        rows = realrom.run_result_text(CEMU, launched_state, CALCENG_MAP, prefix,
                                       [e for e, _ in cases])
        for (expr, want), r in zip(cases, rows):
            assert not r["reset"], f"{expr}: calculator reset"
            assert r["fresh"] and r["ok"], (expr, r)
            assert r["text"] == want, (expr, r["text"])


# Ground truth from numpy.linalg.eig in double precision; each vector is
# scaled so its largest component has magnitude 1, and compared up to sign.
INJECTED = [
    ("mat(2,1,0;1,2,0;0,0,3)", [3, 3, 1], None),
    ("mat(1,2,3;4,5,6;7,8,10)", [16.707493, 0.198247, -0.905740],
     [[0.267901, 0.604024, 1], [0.334553, -1, 0.577257],
      [1, -0.098922, -0.569299]]),
    ("mat(4,6,-4;4,1,-4;3,-5,-2)", [7.634352, 0.509314, -5.143665],
     [[1, 0.604254, -0.002207], [0.988787, 0.091409, 1],
      [0.017860, 0.639449, 1]]),
    ("mat(23,20,10;15,10,6;10,8,2)", [39, -2, -2], None),
    ("mat(-4000.123,1,2;3,-4000.456,5;6,7,-4000.789)",
     [-3992.830057, -4001.720515, -4006.817428],
     [[0.384903, 0.807075, 1], [1, -0.804370, -0.396573],
      [-0.195090, -0.693984, 1]]),
    ("mat(1000,0,0;0,1.05,0;0,0,1)", [1000, 1.05, 1],
     [[1, 0, 0], [0, 1, 0], [0, 0, 1]]),
    ("mat(0,20000,0;1,0,0;0,0,0)", [141.421356, 0, -141.421356], None),
    # Singular with trace 0: the zero eigenvalue is 0, not noise (r4).
    ("mat(1,2,3;4,-5,1;5,-3,4)", [6.403124, 0, -6.403124], None),
    # A close pair, 0.21% of max|A - s*I| apart (review r3).
    ("mat(0.943914,0.318236,-0.024651;0.318236,-0.794477,0.134135;"
     "-0.024651,0.134135,0.992103)", [1.002480, 1.0000005, -0.860940], None),
]


def _injected(state: str, expr: str, prefix: str) -> dict:
    import realrom
    s_input = realrom.map_symbol(CALCENG_MAP, "_s_input")
    g_resp = realrom.map_symbol(CALCENG_MAP, "_g_response")
    with realrom.Emu(binary=CEMU, image=state) as e:
        e.run(300)
        e.type(expr.replace(";", ","))
        for k, ch in enumerate(expr):
            if ch == ";":
                e.cmd(f"fill {s_input + k:06X} 1 3B")
        typed = realrom._peek(e, s_input, len(expr))
        assert typed.lower() == expr.lower().encode(), typed
        e.cmd(f"fill {g_resp:06X} 2 0")
        e.cmd("key enter")
        e.run(8000)
        r = realrom.parse_response(realrom._peek(e, g_resp, realrom.RESULT_WINDOW),
                                   prefix, realrom.ipc_version())
    r["reset"] = realrom.reset_seen(e.quit())
    return r


def test_injected_3x3_eigenpairs_on_the_rom(launched_state: str) -> None:
    for m, want_vals, want_vecs in INJECTED:
        rows = [[float(x) for x in row.split(",")] for row in m[4:-1].split(";")]
        n = len(rows)
        trace = sum(rows[i][i] for i in range(n))
        cn = max(abs(rows[i][j] - (trace / n if i == j else 0))
                 for i in range(n) for j in range(n))
        norm = max(abs(x) for row in rows for x in row)
        val = _injected(launched_state, f"eigenval({m})", "lambda")
        vec = _injected(launched_state, f"eigenvec({m})", "v1(")
        for r in (val, vec):
            assert not r["reset"] and r["fresh"] and r["ok"], (m, r)
        mv = re.fullmatch(r"lambda = \{(.*)\}", val["text"])
        assert mv, (m, val["text"])
        vals = [float(x) for x in mv.group(1).split(", ")]
        # The eigenvalues themselves, to 6 displayed digits.
        assert len(vals) == n, (m, val["text"])
        for got, want in zip(vals, want_vals):
            assert abs(got - want) <= 1e-5 * norm + 1e-4 * cn, (m, val["text"])
            if want == 0:
                assert got == 0, (m, val["text"])   # not rounding noise
        lines = vec["text"].split("\n")
        assert len(lines) == n, (m, vec["text"])
        for k, line in enumerate(lines):
            mm = re.fullmatch(r"v\d\(lambda=([^)]*)\)=\[(.*)\]", line)
            assert mm, (m, line)
            lam = float(mm.group(1))
            v = [float(x) for x in mm.group(2).split(",")]
            # n components, largest of magnitude 1 (so never all zeros).
            assert len(v) == n and abs(max(abs(x) for x in v) - 1) < 1e-4, (m, line)
            assert abs(lam - want_vals[k]) <= 1e-5 * norm + 1e-4 * cn, (m, line)
            if want_vecs is not None:
                sign = 1 if sum(a * b for a, b in zip(v, want_vecs[k])) >= 0 else -1
                assert max(abs(sign * a - b) for a, b in zip(v, want_vecs[k])) <= 1e-3, \
                    (m, line, want_vecs[k])
            for i in range(n):
                av = sum(rows[i][j] * v[j] for j in range(n))
                assert abs(av - lam * v[i]) <= 1e-4 * cn + 1e-5 * norm, (m, line, i)
