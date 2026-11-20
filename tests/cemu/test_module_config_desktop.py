"""Desktop checks of a split CE worker built with its *own* makefile flags.

The main desktop runner (tests/test_main.c) compiles processor.c with every
feature on, so it cannot see a worker whose PROGRAM_CFLAGS compile its job out.
That is how CECIDF (the definite-integral worker) shipped answering
"Feature is routed to another module" for every int(f,x,a,b).

This test reads PROGRAM_CFLAGS and PROGRAM_SOURCES from
split/Makefile.calc.int.def, swaps the CE entry point for
tests/module_config/module_driver.c, builds it with the host C compiler and
-DDESKTOP_BUILD, and runs real expressions through
ce_processor_run_preparsed() via the launcher's AST wire format.

Desktop glibc printf supports %g, so this cannot catch on-target formatting
bugs; the real-ROM lane (tools/emulator/linux-realrom) covers those.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DRIVER = REPO / "tests" / "module_config" / "module_driver.c"
MOCK_DIR = REPO / "tests" / "mock"

# Sources the driver needs on top of the worker's own list: the parser to
# turn input text into an AST, and the desktop AppVar mock that ipc.c uses.
EXTRA_SOURCES = ["src/token.c", "src/parser.c", "src/macros.c"]
MOCK_SOURCES = [
    "tests/mock/fileioc_mock.c",
]
# CE-only entry points that the driver replaces.
ENTRY_SOURCES = {
    "src/split/module_main.c",
    "src/split/module_runtime.c",
    "src/split/module_forward.c",
}


def _find_cc() -> str | None:
    for name in (os.environ.get("CC"), "gcc", "cc", "clang"):
        if name and shutil.which(name):
            return name
    return None


def _makefile_var(text: str, name: str) -> list[str]:
    """Return the whitespace-split value of `NAME = ...` with continuations.

    Only one plain `=` / `:=` assignment is supported. Fail loudly on `+=`,
    `?=` or `$(...)` so a future makefile edit cannot silently drop flags."""
    joined = text.replace("\\\n", " ")
    matches = re.findall(rf"^{name}\s*([:+?]?=)\s*(.*)$", joined, re.MULTILINE)
    if not matches:
        return []
    assert len(matches) == 1 and matches[0][0] in ("=", ":="), (
        f"{name}: only one plain assignment is supported by this test"
    )
    value = matches[0][1]
    assert "$(" not in value, f"{name}: make variable references are not expanded here"
    return shlex.split(value)


def _module_build_inputs(makefile: str) -> tuple[list[str], list[str]]:
    text = (REPO / "split" / makefile).read_text(encoding="utf-8")
    cflags = _makefile_var(text, "PROGRAM_CFLAGS")
    sources = [s for s in _makefile_var(text, "PROGRAM_SOURCES") if s not in ENTRY_SOURCES]
    sources += [s for s in _makefile_var(text, "PROGRAM_RUNTIME_SOURCES") if s not in ENTRY_SOURCES]
    for extra in EXTRA_SOURCES:
        if extra not in sources:
            sources.append(extra)
    return cflags, sources


@pytest.fixture(scope="module")
def cecidf_driver(tmp_path_factory) -> Path:
    if sys.platform == "win32":
        # The self-hosted Windows CEmu runner also runs tests/cemu; its
        # compilers may not take the GNU link flags below. Linux CI covers it.
        pytest.skip("module-config desktop build is exercised on Linux CI")
    cc = _find_cc()
    if cc is None:
        pytest.skip("no host C compiler (gcc/cc/clang) on PATH")
    cflags, sources = _module_build_inputs("Makefile.calc.int.def")
    assert "-DCE_PROC_CALC_INT_DEFINITE=1" in cflags, (
        "CECIDF must build with CE_PROC_CALC_INT_DEFINITE=1; with 0 it only "
        "answers 'Feature is routed to another module'"
    )
    out = tmp_path_factory.mktemp("cecidf") / "cecidf_driver"
    cmd = [
        cc, "-std=c99", "-O0", "-g", "-w",
        "-DDESKTOP_BUILD",
        f"-I{REPO / 'src'}", f"-I{MOCK_DIR}",
        # Format numbers with the CE's own printf (split/program.mk
        # force-includes ce_fmt.h and links ce_fmt.c), not host libc.
        "-include", str(REPO / "src" / "ce_fmt.h"), "-DCE_FMT_REDIRECT",
        *cflags,
        str(DRIVER),
        *[str(REPO / s) for s in sources],
        str(REPO / "src" / "ce_fmt.c"),
        *[str(REPO / s) for s in MOCK_SOURCES],
        # Mirror the CE link: LTO drops code the worker never reaches, so
        # unreferenced helpers (e.g. polar volume wrappers) need not link.
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-lm", "-o", str(out),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    assert proc.returncode == 0, f"build failed:\n{' '.join(cmd)}\n{proc.stderr[-4000:]}"
    return out


def _run(driver: Path, *exprs: str) -> list[str]:
    proc = subprocess.run([str(driver), *exprs], capture_output=True, text=True, timeout=60)
    lines = proc.stdout.strip().splitlines()
    n_expr = len([e for e in exprs if e not in ("--deg", "--rad", "--ans")])
    assert len(lines) == n_expr, proc.stdout + proc.stderr
    return lines


def _first_line(line: str) -> str:
    # The launcher transcript shows only a result's first line (core/launcher
    # main.c home_draw / state_store_last_result), so the value must be there.
    return line.split("\\n")[0]


def _assert_no_value(line: str) -> None:
    """CECIDF has no limits.c, so for a singular endpoint it must either say
    DIVERGES or fail with a message; it must never print a number."""
    head = _first_line(line)
    assert "converges" not in line, line
    assert not head.startswith("OK|="), line
    assert head.startswith("ERR|") or "DIVERGES" in head, line


def _first_number(line: str) -> float:
    # Results look like "OK|= 9" (FTC) or "OK|= 1.4627  (numeric)".
    head = _first_line(line)
    match = re.match(r"(?:OK\|)?=\s*(-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)", head)
    assert match, line
    return float(match.group(1))


@pytest.mark.parametrize(
    ("expr", "expected"),
    [
        ("int(x^2,x,0,3)", 9.0),
        ("int(x,x,0,1)", 0.5),
        ("int(x^3,x,0,1)", 0.25),
        ("int(sin(x),x,0,pi)", 2.0),
        ("int(3*x^2+2*x,x,1,2)", 10.0),
        ("int(x^2,x,3,0)", -9.0),
    ],
)
def test_cecidf_definite_integral_ftc(cecidf_driver: Path, expr: str, expected: float) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert line.startswith("OK|"), line
    assert "routed to another module" not in line
    # CECIDF neither simplifies nor folds, so it shows the value only;
    # "(numeric)" would mean the Simpson fallback answered instead of FTC.
    assert "(numeric)" not in line and "F(x) = " not in line, f"expected the FTC value only: {line}"
    assert _first_number(line) == pytest.approx(expected, abs=1e-4)


def test_cecidf_numeric_fallback_without_elementary_antiderivative(cecidf_driver: Path) -> None:
    # e^(x^2) has no elementary antiderivative; CECIDF falls back to Simpson.
    (line,) = _run(cecidf_driver, "--ans", "int(e^(x^2),x,0,1)")
    assert line.startswith("OK|"), line
    assert "(numeric)" in line, line
    assert float(line.rsplit("|ANS=", 1)[1]) == pytest.approx(1.462652, abs=1e-3)


@pytest.mark.parametrize(
    ("expr", "true_value"),
    [
        # Integrable endpoint singularities. CECIDF (no limits.c) refuses an
        # integral whose integrand fails at a bound, because no sample test
        # tells these from a masked pole (review round 5). A value, if one
        # is ever printed, must be right.
        ("int(x^(-1/2),x,0,1)", 2.0),
        ("int(x^(-1/2),x,1,0)", -2.0),
    ],
)
def test_cecidf_integrable_endpoint_refused_or_right(
    cecidf_driver: Path, expr: str, true_value: float
) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert "DIVERGES" not in line, line
    if _first_line(line).startswith("OK|="):
        assert _first_number(line) == pytest.approx(true_value, abs=1e-3), line


@pytest.mark.parametrize(
    "expr",
    [
        # Pole strictly inside the interval: F(1)-F(-1) = -2 would be wrong.
        "int(1/x^2,x,-1,1)",
        "int(1/x^2,x,1,-1)",
        # Poles at an endpoint.
        "int(1/x^2,x,0,1)",
        "int(1/x,x,0,1)",
        # Log divergence hidden by tiny or regular terms: samples of F look
        # converged (1e-6*ln(h) + 0.1*h).
        "int(0.000001/x,x,0,1)",
        "int(0.000001/x+0.1,x,0,1)",
        # A pole just past the float bound: F = -ln|cos| is finite only by
        # rounding at the float pi/2 (review round 4).
        "int(tan(x),x,0,pi/2)",
        # F overflows at the bounds; every Simpson node is finite but the
        # Simpson value (2875) is wrong (review round 4).
        "int(tanh(x),x,-1000000,1000001)",
        # Regular terms that mask the pole at the float pi/2 (round 5).
        "int(tan(x),x,1.5,pi/2)",
        "int(tan(x)+100,x,0,pi/2)",
        "int(tan(x)+1000,x,0,pi/2)",
        "int(tan(x)+1000,x,pi/2,0)",
        # Round 6: these pass the eval.c domain guards (the float bound is a
        # few ulps from the pole), so only the structural test catches them.
        "int(cot(x),x,6.2,2*pi)",
        "int(tan(x),x,7.6,5*pi/2)",
        "int(0.000001*cot(x),x,6,2*pi)",
        "int(cot(x)+1000000,x,6.2,2*pi)",
        "int(1/(x-2*pi),x,6,2*pi)",
        # Round 7: exponents the parser keeps unfolded.
        "int(sin(x)^(-3/2),x,2*pi,7)",
        "int((11*x-pi)^(-3/2),x,pi/11,1)",
        "int(sin(x)^(1-2),x,2*pi,7)",
    ],
)
def test_cecidf_singular_endpoint_never_reports_a_value(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    _assert_no_value(line)


@pytest.mark.parametrize(
    ("expr", "true_value"),
    [
        # Convergent, but sums of terms whose samples mimic a pole
        # (review rounds 2 and 3). DIVERGES is wrong; an honest failure or
        # the right value is fine.
        ("int(x^(-0.99)+ln(x),x,0,1)", 99.0),
        ("int(ln(x)+5.31,x,0,1)", 4.31),
        ("int(ln(x)+0.36/sqrt(x),x,0,1)", -0.28),
        ("int(10000*ln(x-1)+10000,x,1,2)", 0.0),
    ],
)
def test_cecidf_convergent_singular_endpoint_is_never_called_divergent(
    cecidf_driver: Path, expr: str, true_value: float
) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert "DIVERGES" not in line, line
    if line.startswith("OK|"):
        assert _first_number(line) == pytest.approx(true_value, rel=1e-2, abs=1e-2), line


@pytest.mark.parametrize(
    "expr",
    [
        # A pole strictly inside the interval that none of the
        # eight old probes lo + k(hi-lo)/8 landed on. FTC printed a value
        # (int(1/x^2,x,-1,2) = -1.5, int(tan(x),x,0,3) = 0.0100579).
        "int(1/(x-1)^2,x,0,3)",
        "int(1/(x-1),x,0,3)",
        "int(1/x^2,x,-1,2)",
        "int(1/x,x,-1,2)",
        "int(1/(x-0.3)^2,x,0,1)",
        "int(tan(x),x,0,3)",
        "int(tan(x)+1000,x,0,3)",
        "int(sec(x)^2,x,0,3)",
        "int(cot(x),x,3,4)",
        "int(csc(x)^2,x,-1,2)",
        "int((x-1)^(-2),x,0,3)",
        "int((x-1)^(0-2),x,0,3)",
        "int(0.000001/(x-1)^2,x,0,3)",
        "int(1/(x-pi),x,3,4)",
        "int(1/(x^2-2),x,0,3)",
        "int(x/(x^2-1),x,0,3)",
        "int(coth(x-1),x,0,3)",
        "int(csch(x),x,-1,2)",
        "int(1/abs(x-1),x,0,3)",
        "int(1/ln(x),x,0.5,2)",
        "int(tan(x),x,0,10)",       # several poles
        "int(1/sin(x),x,1,10)",
        "int(1/(x-1e16),x,0,3e16)",
        "int(1/(tan(x)-sec(x)),x,1,2)",  # the poles cancel to a zero
        "int(1/e^(tan(x)),x,1,2)",       # e^(-tan) blows up past pi/2
        # Review round 17: a double root between grid samples under a large
        # baseline (main printed 3e+09).
        "int(1e9+1/(x^2-2*x+1),x,0,3)",
        "int(1e9+1/(x-1.37)^2,x,0,3)",
        "int(1e9+1/((x-1.37)^2*(x+5)),x,0,3)",
        "int(1e9+1/((x^2-2*x+1)*(x^2+1)),x,0,3)",
        "int(1e9+1/(x^2-2*x+1)^0.5,x,0,3)",
        # Review round 18: an asymmetric double root, a tiny scale, a double
        # root in the last cell, and poles inside sum/prod/piecewise.
        "int(1e9+1/(x^3-3*x+2),x,-1,2)",
        "int(1e9+1e-20/(1e-20*x^2-2e-20*x+1e-20),x,0,3)",
        "int(1e9+1/(x^2-2*x+1),x,-2,1.01)",
        "int(1e9+1/(1-sin(x)),x,0,3)",
        "int(1e9+sum(1/(x-n)^2,n,1,1),x,0,3)",
        "int(1e9+sum((x-1.37)^n,n,-2*floor(x),0),x,0,3)",
        "int(1e9+prod(1/(x-n),n,1,1),x,0,3)",
        "int(1e9+piecewise(1/(x^2-2*x+1),1,0),x,0,3)",
        # Review round 20: a pole in a term past the 16th is still found.
        "int(1e9+sum(1/(x-n)^2,n,-16,1),x,0,3)",
        "int(1e9+prod(1/(x-n),n,-16,1),x,0,3)",
        # Review round 21: a pole in a body without the index, walked once.
        "int(1e9+sum(1/(x-1)^2,n,1,300),x,0,3)",
        "int(1e9+prod(1/(x-1),n,1,300),x,0,3)",
    ],
)
def test_cecidf_interior_pole_never_reports_a_value(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    _assert_no_value(line)


@pytest.mark.parametrize(
    ("expr", "expected"),
    [
        # The interior-pole scan must not refuse ordinary integrals whose
        # denominators come close to, but never reach, zero.
        ("int(1/(1+x^2),x,-1000,1000)", 3.1395927),
        ("int(1/(x+2),x,0,3)", 0.9162907),
        ("int(sec(x)^2,x,0,1)", 1.5574077),
        ("int(1/x^2,x,1,2)", 0.5),
        ("int(1/cos(x),x,0,1)", 1.2261343),
        ("int(tan(x),x,-1,1)", 0.0),
        # Review round 1: a constant exponent written with the variable.
        ("int(x^(0*x+2),x,-1,1)", 0.6666667),
        ("int((x-1)^(0*x+2),x,0,3)", 3.0),
        ("int(x^(x-2),x,0.5,2)", 1.6642846),
        # Review round 2: a tan/sec/cot/csc pole in a denominator.
        ("int(1/tan(x),x,1,2)", 0.0775207),
        ("int(1/sec(x),x,1,2)", 0.0678265),
        ("int(sin(x)/tan(x),x,1,2)", 0.0678265),
        ("int(x/tan(x),x,1,2)", 0.0279398),
        ("int(cos(x)/(1+tan(x)^2),x,1,2)", 0.0158252),
        # Review round 4: an inner denominator's zeros inside a denominator.
        ("int(1/(1/cos(x)),x,1,2)", 0.0678265),
        ("int(1/(1+1/cos(x)^2),x,1,2)", 0.0725089),
        ("int(1/(cos(x)^(-1)),x,1,2)", 0.0678265),  # review round 5
        ("int(1/sqrt(sec(x)^4),x,1,2)", 0.083475),  # review round 6
        # Review round 17: a positive minimum of the denominator between
        # (or on) grid samples is not a double root.
        ("int(1/(x^2+1),x,-999,1001)", 3.1395927),
        ("int(1/(x^2+0.01),x,-1,1)", 29.422553),
        ("int(1/(x^4+1),x,-5,5)", 2.2161107),
        ("int(1/(x^2+2*x+2),x,-3,3)", 2.4329664),
        ("int(1/(x^2+1),x,-1500,1500)", 3.1402593),
        ("int(1/(x^2+1),x,1500,-1500)", -3.1402593),  # round 19: reverse zoom
        ("int(1/(x^2+0.01),x,1,-1)", -29.422553),
        ("int(1/(1+e^(-x)),x,0,100)", 99.306853),  # flat samples far out
        ("int(1/x,x,0.000001,3)", 14.914123),        # steep at a bound
        # Review round 18: sums and products walked term by term, and an
        # inner integral over x itself.
        ("int(sum(x^(n+1),n,0,3),x,0,1)", 1.2833333),
        ("int(prod(x^(n+1),n,0,2),x,0,1)", 0.14285714),
        ("int(prod(1/(x^2+n),n,1,2),x,0,1)", 0.35018829),
        ("int(sum(1/(x^2+n),n,1,3),x,-1,2)", 3.8003982),
        ("int(sum(n*x^(n-1),n,1,3),x,0,2)", 14.0),
        ("int(sum(x^n,n,0,3),x,0,1)", 2.0833333),
        ("int(sum(x^n,n,0,40),x,0,0.5)", 0.69314718),
        ("int(prod(x+n,n,1,2),x,0,1)", 3.8333333),
        ("int(piecewise(x^2,1,0),x,0,3)", 9.0),
        ("int(int(1/x^2,x,1,2),x,0,3)", 1.5),
        ("int(x^x,x,0,1)", 0.78343051),
        # Review round 20: past 16 terms, sums still answer within the
        # shared 256-term budget, nested ones fall back to the unbound walk,
        # and the CE int range's lower end -2^23 is a valid index.
        ("int(sum(1/(x+n),n,1,16),x,0,1)", 2.8332133),
        ("int(sum(1/(x+n),n,1,17),x,0,1)", 2.8903718),
        ("int(sum(1/(x+n),n,1,256),x,0,1)", 5.5490761),
        ("int(sum(sum(x^m,m,0,16),n,1,16),x,0,1)", 55.03284),
        ("int(sum(x,n,-8388608,-8388608),x,0,1)", 0.5),
        # Review round 21: a body without the index is walked once and does
        # not use up the shared 256-term budget.
        ("int(sum(sum(1/(x+m),m,1,1),n,1,129),x,0,1)", 89.415986),
        ("int(prod(sum(1+0.001/(x+m),m,1,1),n,1,129),x,0,1)", 1.0936783),
    ],
)
def test_cecidf_interior_pole_scan_keeps_ordinary_integrals(
    cecidf_driver: Path, expr: str, expected: float
) -> None:
    (line,) = _run(cecidf_driver, "--ans", expr)
    assert line.startswith("OK|"), line
    # Display rounds numeric quadrature to three significant digits. Keep
    # this accuracy gate on the unrounded stored value, not looser tolerances.
    assert float(line.rsplit("|ANS=", 1)[1]) == pytest.approx(expected, rel=5e-4, abs=1e-6), line


def test_cecidf_deep_inputs_answer_or_refuse(cecidf_driver: Path) -> None:
    # With the eval guards from #125/#127, the pole scan adds walks over the
    # integrand, but long sums still answer (20 and 45 terms), inputs past
    # the eval cap refuse with a reason, and so does an antiderivative
    # deeper than the cap (review round 2). Real-ROM stack use is pinned by
    # tests/cemu/test_realrom_worker_stack.py.
    s20, s45, deep, deep_neg = _run(
        cecidf_driver,
        "int(" + "+".join(["x"] * 20) + ",x,0,1)",
        "int(" + "+".join(["x"] * 45) + ",x,0,1)",
        "int(" + "+".join(["x"] * 58) + ",x,0,1)",
        "int(" + "-" * 44 + "1/x,x,1,2)",
    )
    assert _first_number(s20) == pytest.approx(10.0), s20
    assert _first_number(s45) == pytest.approx(22.5), s45
    assert _first_number(deep_neg) == pytest.approx(0.6931472), deep_neg
    assert deep.startswith("ERR|") and any(
        r in deep for r in ("Expression too deep", "Could not decide",
                            "Eval recursion limit")), deep


def test_cecidf_float_overflow_is_not_a_value(cecidf_driver: Path) -> None:
    (line,) = _run(cecidf_driver, "int(1e30*x,x,0,1e5)")
    assert "inf" not in line.lower() and "nan" not in line.lower(), line


def test_cecidf_ftc_subtraction_overflow_is_not_a_value(cecidf_driver: Path) -> None:
    (line,) = _run(cecidf_driver, "int(1e38,x,-2,2)")
    assert "inf" not in line.lower() and "nan" not in line.lower(), line


@pytest.mark.parametrize(
    ("expr", "expected"),
    [
        # Ordinary integrals of pole-capable functions away from the pole.
        ("int(tan(x),x,0,1)", 0.6156264),
        ("int(1/x,x,1,2)", 0.6931472),
        ("int(1/(x+1)^2,x,0,1)", 0.5),
        ("int(x^(3/2),x,0,1)", 0.4),
        ("int(tan(x),x,-1,1)", 0.0),
    ],
)
def test_cecidf_ordinary_integrals_near_pole_capable_functions(
    cecidf_driver: Path, expr: str, expected: float
) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert line.startswith("OK|"), line
    assert _first_number(line) == pytest.approx(expected, rel=1e-3), line


@pytest.mark.parametrize(
    ("expr", "true_value"),
    [
        # CECIDF has no limits.c, and sampling F at infinity is fooled both
        # ways (review round 7), so infinite bounds are refused. A value, if
        # one is ever printed, must be right.
        ("int(e^(-x),x,0,inf)", 1.0),
        ("int(e^(-x),x,inf,0)", -1.0),
        ("int(e^(x),x,0,-inf)", -1.0),
        ("int(1/x^2,x,1,inf)", 1.0),
        ("int(e^(-x/1000),x,0,inf)", 1000.0),
    ],
)
def test_cecidf_infinite_bounds_refused_or_right(
    cecidf_driver: Path, expr: str, true_value: float
) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert "DIVERGES" not in line, line
    if _first_line(line).startswith("OK|="):
        assert _first_number(line) == pytest.approx(true_value, rel=1e-3), line


@pytest.mark.parametrize(
    "expr",
    [
        "int(e^(-x),x,0,-inf)",       # grows without bound
        "int(0.000000001/x,x,1,inf)",  # log divergence; F samples look flat
    ],
)
def test_cecidf_divergent_infinite_bounds_never_report_a_value(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    _assert_no_value(line)


def test_cecidf_degree_mode(cecidf_driver: Path) -> None:
    # integrate() antiderivatives assume radians. In degree mode a trig
    # integrand is refused (never the radian answer, never an unchecked
    # numeric sum); an angle-independent integrand keeps its exact value.
    lines = _run(
        cecidf_driver,
        "--deg",
        "int(cos(x),x,0,90)",
        "int(atan(x),x,0,1)",
        "int(cos(x),x,0,72000)",
        "int(1/x,x,0.001,1)",
        "int(e^(-10000*x),x,0,1)",
        "int(x^2,x,0,3)",
        "--rad",
        "int(cos(x),x,0,pi/2)",
    )
    for line in lines[:3]:
        assert line.startswith("ERR|") and "RADIAN" in line, line
    assert _first_number(lines[3]) == pytest.approx(6.907755, rel=1e-4), lines[3]
    assert _first_number(lines[4]) == pytest.approx(1e-4, rel=1e-3), lines[4]
    assert _first_number(lines[5]) == pytest.approx(9.0, abs=1e-3), lines[5]
    assert _first_number(lines[6]) == pytest.approx(1.0, abs=1e-4), lines[6]


@pytest.mark.parametrize(
    ("expr", "true_value"),
    [
        # No elementary antiderivative, so these reach the numeric path.
        # A fixed grid aliases them; the three-grid check must refuse
        # rather than print the aliased value.
        ("int(e^(-x^2),x,-1000,1000)", 1.7724539),
        ("int(e^(-x^2),x,-100,100)", 1.7724539),
        ("int(sin(x^2),x,0,30)", 0.6255437),  # Fresnel-type; heavy oscillation
        # Round 13: a constant baseline hid a visibly sampled, unresolved
        # peak (printed 1.00019); the tolerance scales with |f - mean|.
        ("int(1+e^(-1800000*(x-0.0011)^2),x,0,1)", 1.0012968),
        ("int(5+e^(-1800000*(x-0.0011)^2),x,0,1)", 5.0012968),
        # Cancellation: a sloped baseline that integrates to 0, plus the peak.
        ("int(x-0.5+0.2*e^(-1800000*(x-0.0011)^2),x,0,1)", 0.00025935),
        # Round 14: the noise floor hid a sampled peak under cancellation.
        ("int(x-0.5+0.03*e^(-3000000*(x-0.0011)^2),x,0,1)", 3.05916e-05),
        ("int(sqrt(4-x^2),x,0,2)", 3.1415927),  # must also be OK (value_display)
        # Two h^1.5 endpoints: refused today (follow-up card: order-aware
        # acceptance), but never a wrong value.
        ("int(sqrt(1-x^2),x,-1,1)", 1.5707963),
        ("int(sqrt(x-x^2),x,0,1)", 0.39269908),
        # Round 18: |f| sums overflowed to inf and every tolerance passed
        # (printed 0); |f| > 1e33 or a width > 1e4 is refused.
        ("int(4e36*(sin(x^2)-0.2),x,0,1)", 4.41073e35),
        ("int(sin(x^2),x,0,1e31)", 0.62665707),
        # Round 20: computed bounds that round to the same float32 are not a
        # zero-width interval (printed 0).
        ("int(e^(x^2),x,1,1+1/100000000)", 2.7182818e-08),
        ("int(100000000,x,1,1+1/100000000)", 1.0),
        # Round 21: bounds whose printed text collides at six digits, and a
        # nested integral with collapsed bounds.
        ("int(1000000000000,x,1+1/1000002,1+1/1000001)", 0.999997),
        ("int(1,x,1000000+100000000,1000001+100000000)", 1.0),
        ("int(int(100000000,t,1,1+1/100000000),x,0,1)", 1.0),
        ("int(int(t,t,x,1),x,0,1)", 0.33333333),
        # Round 19: a width under 2^-10 of the bounds is mostly bound rounding.
        ("int(e^(x^2),x,1,1+1/1000000)", 2.7182845e-06),
        # Round 16: every sample sits on the constant baseline (the peak
        # underflows to 0 at all 647 points); flat samples are refused.
        ("int(1+100*e^(-10000000000*(x-0.123456)^2),x,0,1)", 1.0017725),
        # Known fixed-grid limitation: a feature
        # narrower than every grid on a *varying* baseline is unseen, as in
        # any fixed-grid quadrature (TI-84 fnInt included). Pinned strict
        # xfail so a fix shows up.
        pytest.param("int(x+100*e^(-10000000000*(x-0.123456)^2),x,0,1)", 0.50177245,
                     marks=pytest.mark.xfail(strict=True, raises=AssertionError,
                                            reason="sub-grid feature on a varying baseline")),
        pytest.param("int(1+x+100*e^(-10000000000*(x-0.123456)^2),x,0,1)", 1.50177245,
                     marks=pytest.mark.xfail(strict=True, raises=AssertionError,
                                            reason="sub-grid feature on a varying baseline")),
    ],
)
def test_cecidf_numeric_fallback_refused_or_right(
    cecidf_driver: Path, expr: str, true_value: float
) -> None:
    (line,) = _run(cecidf_driver, "--ans", expr)
    assert "DIVERGES" not in line, line
    if line.startswith("OK|"):
        # Keep the old numerical accuracy requirement on stored Ans, even
        # when the display deliberately suppresses its guard digits.
        assert float(line.rsplit("|ANS=", 1)[1]) == pytest.approx(true_value, rel=5e-4), line


@pytest.mark.parametrize(
    "expr",
    [
        # Review round 9: every grid sample underflows to 0 (a missed peak),
        # or the peak is unresolved. Numeric quadrature cannot see a feature
        # narrower than its grid spacing; these must be refused, not "= 0".
        "int(e^(-x^2),x,-20,10000)",
        "int(e^(-x^2),x,-100,10000)",
        "int(10000*e^(-100000000*(x-0.1275)^2),x,0,1)",
        # FTC cancellation: F(b) - F(a) is swamped by float32 rounding.
        "int(x-100000,x,100000,100001)",
        "int(x,x,100000,100001)",
        # float(10.001) is off by 4e-7; cancellation turns that into a
        # 4th-digit error (it printed 22.0469, true 22.0375).
        "int(e^x,x,10,10.001)",
        # CEdev reduces trig arguments in float: on the real-ROM lane this
        # printed -0.661261 (true -0.660721), so it must be refused there.
        "int(cos(x),x,10000,10001)",
        # A relative slope step spanned a cosine period here (round 10).
        "int(sin(x+6434),x,0,0.1)",
        # Round 11: rounded products that land on integers are not exact.
        "int(1.1,x,8000000,8000001)",
        "int(3.3,x,3000000,3000001)",
        "int((2+1/4096)*x-2*x,x,3000,3002)",
        # Round 11: a narrow Gaussian the 100/146/200 grids agree on wrongly.
        "int(e^(-62078.3*(x-0.09897947)^2),x,0,1)",
        # Round 12: a baseline hid it from a share-of-mass trigger, and
        # near-equal grids line up near the bounds.
        "int(0.1+e^(-62078.3*(x-0.09897947)^2),x,0,1)",
        "int(10+100*e^(-62078.3*(x-0.09897947)^2),x,0,1)",
        "int(e^(-60000*(x-0.02)^2),x,0,1)",
        "int(e^(-60000*(x-0.981)^2),x,0,1)",
    ],
)
def test_cecidf_unresolvable_integrals_are_refused(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    _assert_no_value(line)


@pytest.mark.parametrize(
    ("expr", "shown"),
    [
        # Small values keep their significant digits (round 9: 1e-7
        # printed as "= 0"); large ones show six, not nine, digits.
        ("int(0.0000001,x,0,1)", "= 1e-07"),
        ("int(-0.0000001,x,0,1)", "= -1e-07"),
        ("int(x^2,x,0,0.01)", "= 3.33333e-07"),
        ("int(e^x,x,0,20)", "= 4.85165e+08"),
        # Exact zeros are not refused or shown as float noise.
        ("int(sin(x),x,0,2*pi)", "= 0"),
        ("int(x,x,-1,1)", "= 0"),
        ("int(x^3-x,x,-1,1)", "= 0"),
        # Ordinary integrals near the cancellation guard still evaluate.
        ("int(x,x,1000,1001)", "= 1000.5"),
        ("int(x^2,x,1,1)", "= 0"),
        ("int(cos(x),x,0,pi)", "= 0"),  # not -8.74228e-08
        ("int(x^3,x,999,1001)", "= 2.00001e+09"),
        # Review round 10.
        ("int(asin(x),x,0,1)", "= 0.570796"),   # slope probe inside asin's domain
        ("int(acos(x),x,-1,1)", "= 3.14159"),
        ("int(asin(x),x,-1,1)", "= 0"),
        ("int(sin(2*x),x,0,2*pi)", "= 0"),      # golden-ratio fmax points
        ("int(sin(x),x,-2*pi,2*pi)", "= 0"),
        ("int(sin(4*x),x,0,pi)", "= 0"),
        ("int(x^2,x,300,301)", None),            # unsimplified (2+1) is exact
        ("int(x/1e-30,x,0,1)", "= 5e+29"),       # no r*r underflow
        ("int(4321.5,x,0,1)", "= 4321.5"),       # six significant digits
        ("int(e^(x^2),x,0,3)", "= 1.44e+03  (numeric)"),  # steep, not a spike
        ("int(e^(-x^2),x,-20,20)", "= 1.77  (numeric)"),
        ("int(x*e^(-x^2),x,-1,1)", "= 0  (numeric)"),
        # Round 11.
        ("int(0.5,x,8000000,8000001)", "= 0.5"),       # power-of-two scaling is exact
        ("int(1/(1+x^2),x,0,20000)", "= 1.57075"),     # slope probe stays distinct
        # Round 12: -1 exponents the parser keeps as NEG 1 or unfolded.
        ("int(x^(-1),x,1,2)", "= 0.693147"),
        ("int((x+1)^(-1),x,0,1)", "= 0.693147"),
        ("int(x^(0-1),x,1,2)", "= 0.693147"),
        ("int(sqrt(1+x^3),x,0,2)", "= 3.24  (numeric)"),
        ("int(x^5,x,10,11)", None),
        ("int(x^4,x,100,101)", "= 1.0202e+08"),
        # Round 13: equal bounds are 0 on the numeric path too.
        ("int(e^(x^2),x,0,0)", "= 0  (numeric)"),
        ("int(e^(x^2),x,2.5,2.5)", "= 0  (numeric)"),
        ("int(sin(x^2),x,-1,-1)", "= 0  (numeric)"),
        # Round 14: an h^1.5 endpoint (f' unbounded at 1) converges slowly;
        # S100 gets 3x slack. 1^x is the constant 1, not 1^x/ln(1).
        ("int(sqrt(1-x^2),x,0,1)", "= 0.785  (numeric)"),
        ("int(sqrt(4-x^2),x,0,2)", None),
        ("int(1^x,x,0,1)", "= 1"),
        ("int((2-1)^x,x,0,1)", "= 1"),          # round 15: unfolded constant 1
        ("int(e^(x^2),x,1,1.001)", None),        # round 19: just above 2^-10
        ("int(x,x,pi,pi)", "= 0"),                # round 20: bounds typed alike
        ("int(x^2,x,-1,-1)", "= 0"),
        ("int(e^(x^2),x,2,2)", "= 0  (numeric)"),
        # Round 22: a nested integral whose inner bound meets the other at
        # an outer grid point (x = 0) is 0 there, not a refusal.
        ("int(int(1,t,0,x),x,0,1)", "= 0.5  (numeric)"),
    ],
)
def test_cecidf_value_display(cecidf_driver: Path, expr: str, shown: str | None) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert line.startswith("OK|"), line
    if shown is not None:
        assert _first_line(line) == "OK|" + shown, line


def test_cecidf_numeric_fallback_smooth_value(cecidf_driver: Path) -> None:
    (line,) = _run(cecidf_driver, "--ans", "int(e^(x^2),x,0,1)")
    assert line.startswith("OK|"), line
    assert float(line.rsplit("|ANS=", 1)[1]) == pytest.approx(1.462652, rel=1e-4), line


def test_cecidf_indefinite_integral_is_not_its_job(cecidf_driver: Path) -> None:
    # CECINT routes indefinite integrals to CECISM; CECIDF must refuse
    # clearly instead of pretending.
    (line,) = _run(cecidf_driver, "int(x^2,x)")
    assert line.startswith("ERR|"), line


# ── improper() and areabetween() ─────────────────────────────
# The launcher used to send both polar ops to CEBASE, which has no
# integrator and printed "Polar/advanced op (use at top level)" on the real
# OS 5.8 ROM. Now CALCENG routes them to CECINT, CECINT forwards them to
# CECIDF, and CECIDF (CE_PROC_CALC_INT_POLAR_LOWER=1) answers both through
# handle_integral(), so int()'s checks apply to them too.

def _c_function_body(path: Path, signature: str) -> str:
    text = path.read_text(encoding="utf-8")
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def _switch_case_target(body: str, case: str) -> str:
    """The `return X;` that a `case NAME:` label falls through to."""
    after = body[body.index(f"case {case}:"):]
    match = re.search(r"return\s+(\w+)\s*;", after)
    assert match, f"no return after case {case}"
    return match.group(1)


@pytest.mark.parametrize("op", ["POLOP_IMPROPER", "POLOP_AREABETWEEN"])
def test_improper_and_areabetween_route_to_the_definite_worker(op: str) -> None:
    launcher = _c_function_body(REPO / "src" / "core" / "launcher" / "main.c",
                                "static const char *route_polar_program(polar_op_t op)")
    assert _switch_case_target(launcher, op) == "CE_PRGM_CALCIN"
    cecint = _c_function_body(REPO / "src" / "modules" / "calc_int" / "main.c",
                              "static const char *ce_integral_program_for(const ast_node_t *root)")
    assert _switch_case_target(cecint, op) == "CE_PRGM_INT_DEF"
    assert '#define CE_PRGM_INT_DEF \"CECIDF\"' in (
        REPO / "src" / "modules" / "calc_int" / "main.c").read_text(encoding="utf-8")
    cflags, _ = _module_build_inputs("Makefile.calc.int.def")
    assert "-DCE_PROC_CALC_INT_POLAR_LOWER=1" in cflags


@pytest.mark.parametrize(
    ("expr", "shown"),
    [
        # improper(f,x,a,b) is int(f,x,a,b), signed, with the same paths.
        ("improper(x^2,x,0,3)", "= 9"),
        ("improper(x,x,0,1)", "= 0.5"),
        ("improper(x^2,x,3,0)", "= -9"),
        ("improper(e^(x^2),x,0,1)", "= 1.46  (numeric)"),
    ],
)
def test_cecidf_improper_is_the_definite_integral(cecidf_driver: Path, expr: str, shown: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert _first_line(line) == "OK|" + shown, line


@pytest.mark.parametrize(
    "expr",
    [
        # CECIDF has no limits.c: improper() refuses what int() refuses
        # (infinite bounds, singular endpoints), and never says DIVERGES.
        "improper(e^(-x),x,0,inf)",
        "improper(1/x^2,x,1,inf)",
        "improper(1/x^2,x,0,1)",
        "improper(1/sqrt(x),x,0,1)",
        "improper(1/x,x,-1,1)",
    ],
)
def test_cecidf_improper_refuses_what_int_refuses(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    _assert_no_value(line)
    assert "DIVERGES" not in line, line


def test_cecidf_improper_degree_mode(cecidf_driver: Path) -> None:
    lines = _run(cecidf_driver, "--deg", "improper(cos(x),x,0,90)", "improper(x^2,x,0,3)", "--rad")
    assert lines[0].startswith("ERR|") and "RADIAN" in lines[0], lines[0]
    assert _first_line(lines[1]) == "OK|= 9", lines[1]


@pytest.mark.parametrize(
    ("expr", "shown"),
    [
        ("areabetween(x^2,x,x,0,1)", "Area = 0.166667"),
        ("areabetween(x,x^2,x,0,1)", "Area = 0.166667"),       # f, g order
        # The crossing x = 1 sits exactly on a sample; the desktop
        # area_between_curves() misses a crossing there.
        ("areabetween(x^2,x,x,0,2)", "Area = 1"),
        ("areabetween(x^2,x,x,2,0)", "Area = 1"),               # an area is unsigned
        ("areabetween(sin(x),cos(x),x,0,pi)", "Area = 2.82843"),  # 2*sqrt(2), root pi/4
        ("areabetween(x^3,x,x,-1,1)", "Area = 0.5"),            # roots at -1, 0, 1
        ("areabetween(x^2,4,x,-3,3)", "Area = 15.3333"),        # 46/3
        ("areabetween(sqrt(x),x^2,x,0,1)", "Area = 0.333333"),
        ("areabetween(ln(x),0,x,0.5,2)", "Area = 0.539721"),
        ("areabetween(x,0,x,-1,1)", "Area = 1"),
        ("areabetween(cos(x),0,x,0,2*pi)", "Area = 4"),
        ("areabetween(e^(10*x),0,x,0,1)", "Area = 2202.55"),
        ("areabetween(x^2,0,x,0,1000)", "Area = 3.33333e+08"),
        # A tangency (sign does not change) at a sample is not a bound.
        ("areabetween((x-0.5)^2,0,x,0,1)", "Area = 0.0833333"),
        # sin(pi) is -8.7e-8 in float32: a root a few ulps from b is moved
        # onto b instead of leaving a sliver piece that int() refuses.
        ("areabetween(sin(x),0,x,0,pi)", "Area = 2"),
        ("areabetween(sin(10*x),0,x,0,pi)", "Area = 2"),
        ("areabetween(sin(30*x),0,x,0,pi)", "Area = 2"),
        # Identical curves: every sample is 0, so there is one piece.
        ("areabetween(x,x,x,0,1)", "Area = 0"),
        # Equal bounds typed alike are an empty interval.
        ("areabetween(x^2,x,x,1,1)", "Area = 0"),
        # No elementary antiderivative: the checked numeric fallback.
        ("areabetween(e^(x^2),0,x,0,1)", "Area = 1.46  (numeric)"),
        ("areabetween(e^(x^2),0,x,0,3)", "Area = 1.44e+03  (numeric)"),
    ],
)
def test_cecidf_areabetween_values(cecidf_driver: Path, expr: str, shown: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert _first_line(line) == "OK|" + shown, line


@pytest.mark.parametrize(
    "expr",
    [
        # Poles, at a sample or between samples (bisection walks to the
        # pole; int() then refuses the piece's endpoint).
        "areabetween(1/x,0,x,-1,1)",
        "areabetween(1/x,0,x,-1,1.1)",
        "areabetween(tan(x),0,x,0,pi/2)",
        "areabetween(ln(x),0,x,0,1)",
        # Infinite or collapsed bounds (as int()).
        "areabetween(x^2,x,x,0,inf)",
        "areabetween(x^2,x,x,-inf,0)",
        "areabetween(1,0,x,1,1+1/100000000)",
        # Crossings closer together than the 64-sample grid cancel inside a
        # piece; this printed "Area = 0.52" (true 2) before the trapezoid
        # cross-check.
        "areabetween(sin(100*x),0,x,0,pi)",
        "areabetween(sin(200*x),0,x,0,pi)",
        # A peak narrower than the grid.
        "areabetween(1+100*e^(-100000*(x-0.3)^2),1,x,0,1)",
        # float32 overflow is not a value.
        "areabetween(1e30*x,0,x,0,100000)",
    ],
)
def test_cecidf_areabetween_refuses_rather_than_guess(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    assert _first_line(line).startswith("ERR|"), line
    assert "inf" not in line.lower() and "nan" not in line.lower(), line


@pytest.mark.parametrize("op", ["areabetween({t},0,x,0,1)", "improper({t},x,0,1)",
                                "int({t},x,0,1)"])
def test_cecidf_polar_ops_name_the_eval_depth_cap(cecidf_driver: Path, op: str) -> None:
    # A 53-term sum passes eval's 50-level cap; every definite form says so,
    # as #128's refusals do (review round 3 nit).
    (line,) = _run(cecidf_driver, op.format(t="+".join(["x"] * 53)))
    assert line == "ERR|Eval recursion limit", line


@pytest.mark.parametrize(
    "expr",
    [
        "areabetween(ln(x),0,x,-2,-1)",
        "areabetween(sqrt(x),0,x,-2,-1)",
        "areabetween(x,ln(x),x,-1,1)",
        "areabetween(x/x,0,x,-1,1)",
        "areabetween(sqrt(x)*sqrt(x),0,x,-1,1)",
    ],
)
def test_cecidf_areabetween_failed_sample_is_an_error(cecidf_driver: Path, expr: str) -> None:
    # A sample of f-g that fails to evaluate is an error, never a skipped
    # sample (the desktop area_between_curves skipped
    # it, so areabetween(ln(x),0,x,-2,-1) could report 0). The sampling
    # loop itself refuses, before any piece reaches int(): with failed
    # samples skipped, these end in int()'s "Could not decide integral".
    (line,) = _run(cecidf_driver, expr)
    assert line == "ERR|Could not decide area", line


def test_cecidf_areabetween_needs_bounds_and_radians(cecidf_driver: Path) -> None:
    lines = _run(cecidf_driver, "areabetween(x^2,x)", "--deg",
                 "areabetween(sin(x),0,x,0,180)", "areabetween(x^2,x,x,0,1)", "--rad")
    assert lines[0] == "ERR|Use areabetween(f,g,x,a,b)", lines[0]
    assert lines[1].startswith("ERR|") and "RADIAN" in lines[1], lines[1]
    assert _first_line(lines[2]) == "OK|Area = 0.166667", lines[2]


def test_cecidf_areabetween_sets_ans(cecidf_driver: Path) -> None:
    lines = _run(cecidf_driver, "--ans", "areabetween(x,x^2,x,1,0)",
                 "areabetween(x^2,x,x,1,1)", "areabetween(x^2,x)")
    assert lines[0] == "OK|Area = 0.166667|ANS=0.166667", lines[0]
    assert lines[1] == "OK|Area = 0|ANS=0", lines[1]          # review round 1
    assert lines[2].endswith("|ANS=-12345"), lines[2]         # an error keeps Ans


@pytest.mark.parametrize(
    "expr",
    [
        # Review round 1 (P1): an even pole between samples, masked by
        # a constant baseline, passed the 25% trapezoid check and printed
        # "Area = 9995.24"; the area diverges. Every piece goes through
        # handle_integral(), so #134's structural interior-pole scan refuses it.
        # improper() inherits the same scan.
        "areabetween(10000+(x-0.3)^(-2),0,x,0,1)",
        "improper((x-0.3)^(-2),x,0,1)",
        "improper(10000+(x-0.3)^(-2),x,0,1)",
        "areabetween(1e9+1/(x^2-2*x+1),1e9,x,0,3)",
        # Refused today already (trapezoid check, numeric fallback).
        "areabetween((x-0.3)^(-2),0,x,0,1)",
        "improper(1/(x-1)^2,x,0,3)",
    ],
)
def test_cecidf_polar_ops_interior_pole_never_report_a_value(cecidf_driver: Path, expr: str) -> None:
    (line,) = _run(cecidf_driver, expr)
    _assert_no_value(line)


@pytest.mark.parametrize(
    ("expr", "shown"),
    [
        # A root inside the last sample interval but more than h/1024 from b
        # leaves a sliver piece whose F(b)-F(a) is below float32 resolution;
        # int() rightly refuses it, so the area is refused (review round 1,
        # P3). Refused, or exactly the right display, never a wrong value.
        ("areabetween(x^2,x,x,0,1.01)", "Area = 0.166717"),       # 0.16671700
        ("areabetween(sin(x),0,x,0,3.142)", "Area = 2"),           # 2.00000008
    ],
)
def test_cecidf_areabetween_sliver_refused_or_right(
    cecidf_driver: Path, expr: str, shown: str
) -> None:
    (line,) = _run(cecidf_driver, expr)
    head = _first_line(line)
    assert head.startswith("ERR|") or head == "OK|" + shown, line
