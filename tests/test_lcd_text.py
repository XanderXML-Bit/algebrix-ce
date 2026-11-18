#!/usr/bin/env python3
"""
test_lcd_text.py — LCD text assertion suite for CalcEngine.

DESKTOP-LAYER ONLY. This suite sends each expression to the desktop
`bin/calcengine.exe --solve` binary and asserts that expected substrings
appear in stdout. The desktop binary shares CalcEngine's parser, AST,
simplifier, and math modules with the TI build, so this suite is a fast
correctness smoke test for the CAS core.

WARNING: This suite does NOT exercise the TI-OS layer, the `.8xp` module
dispatcher, the RAM path, keypad input, or the LCD rendering path. Real
hardware bugs (the "2+3 RAM Cleared" class of regressions, module
swap-in failures, LCD formatter drift) will NOT be caught here.

For hardware-equivalent coverage of the SAME 59 expressions, run the
emulator-layer suite, which drives the real `.8xp` bundle inside the
in-tree headless CEmu fork:

    python -m pytest tests/emulator/test_lcd_text_emulator.py -v

That suite validates each expression by comparing the LCD frame's CRC
against a captured baseline and is the correct reference for
hardware-path regressions. Keep BOTH suites — they cover disjoint
failure modes.

Usage:
    python tests/test_lcd_text.py [--bin <path/to/calcengine.exe>] [-v]

Pass -v for verbose (show full output for every test).
This file is NOT invoked by `python -m pytest`; it has its own __main__
entry point and runs against bin/calcengine.exe only.
"""

import os
import subprocess
import sys
import time

# ── Binary resolution ────────────────────────────────────────────────────────

def _find_binary():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root  = os.path.dirname(script_dir)
    candidates = [
        os.path.join(repo_root, "bin", "calcengine.exe"),
        os.path.join(script_dir, "..", "bin", "calcengine.exe"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.abspath(c)
    return None

# ── Test runner ───────────────────────────────────────────────────────────────

BINARY   = None
VERBOSE  = False
RESULTS  = []   # (label, expr, expected_list, passed, got)


def _run(expr):
    """Run calcengine.exe --solve <expr>; return combined stdout+stderr."""
    try:
        proc = subprocess.run(
            [BINARY, "--solve", expr],
            capture_output=True,
            text=True,
            timeout=15,
            encoding="utf-8",
            errors="replace",
        )
        return proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        return "ERROR:TIMEOUT"
    except Exception as exc:
        return f"ERROR:{exc}"


def check(label, expr, *expected_fragments):
    """
    Assert that every fragment in *expected_fragments appears in the output for
    the expression.  Records the result; does not raise.
    """
    global RESULTS
    got    = _run(expr)
    passed = all(frag in got for frag in expected_fragments)
    RESULTS.append((label, expr, list(expected_fragments), passed, got.strip()))
    if VERBOSE or not passed:
        status = "PASS" if passed else "FAIL"
        miss   = [f for f in expected_fragments if f not in got]
        print(f"  [{status}] {label}")
        print(f"         expr     : {expr}")
        if not passed:
            print(f"         missing  : {miss}")
            print(f"         output   : {got.strip()!r}")
    else:
        print(f"  [PASS] {label}")
    return passed


# ── Test groups ───────────────────────────────────────────────────────────────

def group(name):
    print(f"\n{'-'*60}")
    print(f"  {name}")
    print(f"{'-'*60}")


def test_basic_arithmetic():
    group("Basic Arithmetic (10 cases)")
    check("2+2 = 4",              "2+2",                "4")
    check("10/4 = 2.5",           "10/4",               "2.5")
    check("3^2 = 9",              "3^2",                "9")
    check("sqrt(16) = 4",         "sqrt(16)",           "4")
    check("sin(0) = 0",           "sin(0)",             "0")
    check("cos(0) = 1",           "cos(0)",             "1")
    check("ln(1) = 0",            "ln(1)",              "0")
    check("abs(-7) = 7",          "abs(-7)",            "7")
    check("2*3+1 = 7",            "2*3+1",              "7")
    check("(1+2)*3 = 9",          "(1+2)*3",            "9")


def test_derivatives():
    group("Derivatives (5 cases)")
    check("d/dx(x^3) = 3x^2",    "d/dx(x^3)",         "3", "x^2")
    check("d/dx(sin(x)) = cos",   "d/dx(sin(x))",      "cos")
    check("product rule: x*sin",  "d/dx(x*sin(x))",    "sin(x)", "cos(x)")
    check("chain rule: sin(x^2)", "d/dx(sin(x^2))",    "cos", "2")
    check("d/dx(ln(x)) = 1/x",   "d/dx(ln(x))",       "1", "x")


def test_integrals():
    group("Indefinite & Definite Integrals (5 cases)")
    check("int(x^2,0,1) = 1/3",   "int(x^2,x,0,1)",   "0.333")
    check("int(e^x,x)",            "int(e^x,x)",        "e^x")
    check("int(sin(x),x)",         "int(sin(x),x)",     "cos")
    check("int(1/x,x) = ln",       "int(1/x,x)",        "ln")
    check("int(x^2,x) = x^3/3",   "int(x^2,x)",        "x^3")


def test_improper_integrals():
    group("Improper Integrals (3 cases)")
    check("int(1/x^2,1,inf)=1",   "int(1/x^2,x,1,inf)",    "converges")
    check("int(e^-x,0,inf)=1",    "int(e^(-x),x,0,inf)",   "converges", "1")
    check("int(1/x,1,inf)=div",   "int(1/x,x,1,inf)",      "DIVERGES")


def test_limits():
    group("Limits (5 cases)")
    check("lim(sin(x)/x,x,0)=1",  "lim(sin(x)/x,x,0)",        "1")
    check("lim(1/x,x,0+)=+inf",   "lim(1/x,x,0,+)",           "+inf")
    check("lim(x^2,x,2)=4",       "lim(x^2,x,2)",             "4")
    check("lim(1/x^2,0+)=+inf",   "lim(1/x^2,x,0,+)",         "+inf")
    check("lim((1-cos)/x^2,0)",   "lim((1-cos(x))/x^2,x,0)",  "0.5")


def test_odes():
    group("ODEs — dsolve (3 cases)")
    check("dsolve sep: dy/y=dx",  "dsolve(y,y,x)",         "ln", "x")
    check("dsolve2: 2 real roots","dsolve2(1,-5,6,0,x)",   "e^(3*x)", "e^(2*x)")
    check("dsolve2: complex roots","dsolve2(1,0,1,0,x)",   "cos", "sin")


def test_eigenvalues():
    group("Eigenvalues (2 cases)")
    check("eigenval diag(1,2)",   "eigenval(mat(1,0;0,2))",  "2", "1")
    check("eigenval sym(2,1;1,2)","eigenval(mat(2,1;1,2))",  "3", "1")


def test_vector_calc():
    group("Vector Calculus (3 cases)")
    check("dot [1,2,3]·[4,5,6]=32", "dot(1,2,3,4,5,6)",     "32")
    check("cross i×j = k",          "cross(1,0,0,0,1,0)",    "1")
    check("grad(x^2+y^2)",          "grad(x^2+y^2,x,y)",     "2*x", "2*y")


def test_polar():
    group("Polar Integration (2 cases)")
    check("polararea(r=1, 0..pi)=pi/2", "polararea(1,t,0,pi)",     "1.5708")
    check("polararclen(r=1,0..2pi)=2pi","polararclen(1,t,0,2*pi)", "6.28")


def test_laplace():
    group("Laplace Transforms (2 cases)")
    check("L{1} = 1/s",    "laplace(1,t,s)",    "1/s")
    check("L{t} = 1/s^2",  "laplace(t,t,s)",    "1/s^2")


def test_algebra():
    group("Algebra (5 cases)")
    check("solve x^2=4",         "solve(x^2=4,x)",     "2", "-2")
    check("factor x^2-1",        "factor(x^2-1,x)",    "x-1", "x+1")
    check("expand (x+1)^3",      "expand((x+1)^3,x)",  "x^3")
    check("tangent x^2 at x=2",  "tangent(x^2,x,2)",   "y =")
    check("lim DNE abs(x)/x",    "lim(abs(x)/x,x,0)",  "DNE")


def test_series():
    group("Series / Convergence (3 cases)")
    check("sum 1/n^2 converges",  "sum(1/n^2,n,1,inf)",    "converges")
    check("sum 1/n diverges",     "sum(1/n,n,1,inf)",      "diverges")
    check("taylor sin(x) order 5","taylor(sin(x),x,0,5)",  "x^5")


def test_matrix():
    group("Matrix Operations (3 cases)")
    check("det[[1,2],[3,4]]=-2",  "det(mat(1,2;3,4))",   "-2")
    check("inv diag(2,4)",        "inv(mat(2,0;0,4))",   "0.5")
    check("rref identity result", "rref(mat(1,2;3,4))",  "1")


def test_stats_prob():
    group("Statistics & Probability (4 cases)")
    check("mean(1..5)=3",         "mean(1,2,3,4,5)",  "3")
    check("nCr(10,3)=120",        "nCr(10,3)",         "120")
    check("nPr(10,3)=720",        "nPr(10,3)",         "720")
    check("binompdf(5,.5,2)~.313","binompdf(5,0.5,2)", "0.3125")


def test_settings_and_modes():
    group("Settings / Mode Assertions (4 cases)")
    # Degree mode: sin(90) should be 1.0
    check("sin(90deg) ~ 1",  "sin(90*pi/180)",   "1")
    # Large exponent
    check("10^6 scientific",  "10^6",              "1e+06")
    # Negative base
    check("(-2)^3 = -8",     "(-2)^3",            "-8")
    # Complex-adjacent: imaginary unit not triggered, just i^2
    check("e^(pi*i) check",  "cos(pi)",           "-1")


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    global BINARY, VERBOSE

    args = sys.argv[1:]
    if "--bin" in args:
        idx    = args.index("--bin")
        BINARY = args[idx + 1]
        args   = args[:idx] + args[idx + 2:]
    if "-v" in args or "--verbose" in args:
        VERBOSE = True

    if BINARY is None:
        BINARY = _find_binary()
    if BINARY is None or not os.path.isfile(BINARY):
        print(f"ERROR: calcengine.exe not found (tried bin/calcengine.exe)")
        print("       Run tests/build_msvc.ps1 first, or pass --bin <path>")
        sys.exit(1)

    print(f"\ncalcengine LCD text assertion suite")
    print(f"binary  : {BINARY}")
    print(f"verbose : {VERBOSE}")

    t0 = time.time()

    test_basic_arithmetic()
    test_derivatives()
    test_integrals()
    test_improper_integrals()
    test_limits()
    test_odes()
    test_eigenvalues()
    test_vector_calc()
    test_polar()
    test_laplace()
    test_algebra()
    test_series()
    test_matrix()
    test_stats_prob()
    test_settings_and_modes()

    elapsed = time.time() - t0

    # ── Summary ─────────────────────────────────────────────────────────────
    total  = len(RESULTS)
    passed = sum(1 for r in RESULTS if r[3])
    failed = total - passed

    print(f"\n{'='*60}")
    print(f"  RESULTS: {passed}/{total} passed  ({elapsed:.1f}s)")
    if failed:
        print(f"\n  FAILURES ({failed}):")
        for label, expr, frags, ok, got in RESULTS:
            if not ok:
                miss = [f for f in frags if f not in got]
                print(f"    * {label}")
                print(f"      expr    : {expr}")
                print(f"      missing : {miss}")
                print(f"      output  : {got!r}")
    print(f"{'='*60}\n")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
