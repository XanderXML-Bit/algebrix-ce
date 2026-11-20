"""Build a CE module's processor on the desktop with the module's own flags.

The desktop suite (tests/Makefile) builds the processor with every feature on
(CE_PROC_CALC=1, simplify.c linked).  The CE modules are built from
split/Makefile.<module> with their own PROGRAM_CFLAGS and PROGRAM_SOURCES, so a
module can silently lose a feature the desktop suite still exercises.  That is
how CECALC shipped d/dx results such as ``((3*(X^(3-1)))+(2*1))`` (canonical
format; result lines are now printed as ``3X^2+2``): it was built
with ``-DCE_DERIV_USE_SIMPLIFY=0`` and without src/simplify.c, while the desktop
CLI printed ``((3*(x^2))+2)``.

These tests read a module's flags and sources from its makefile, compile them
with the host C compiler together with tests/module_parity/module_driver.c,
and check the processor output.  The driver follows the launcher -> worker
path: parse, astwire serialize, deserialize, ce_processor_run_preparsed().
Formatting goes through src/ce_fmt.c with 32-bit floats, as on the CE.

The integration strategy workers (CEIPRT, CEITRG, CEIUSB, CEIPFR) have no
processor.  For them tests/module_parity/strategy_driver.c runs the worker's
own main() from src/modules/calc_int/strategy_main.c (renamed with
-Dmain=ce_worker_main) and stands in for src/split/module_runtime.c.

Integration workers are too close to the 51,200 B ceiling for simplify.c
(about 11 KB), so they fold the finished antiderivative with src/fold_lite.c
(CE_INTEG_USE_FOLD_LITE=1).  Its output is checked exactly, and checked to be
numerically equal to the unfolded result of the same module.
"""
from __future__ import annotations

import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SPLIT = ROOT / "split"
DRIVER = ROOT / "tests" / "module_parity" / "module_driver.c"
STRATEGY_DRIVER = ROOT / "tests" / "module_parity" / "strategy_driver.c"

# Module entry points that need the TI runtime; the processor driver replaces
# them.  (The strategy driver runs strategy_main.c itself.)
_HOST_EXCLUDED_SOURCES = {
    "src/split/module_main.c",
    "src/modules/calc_int/simple_main.c",
    "src/modules/matrix/main.c",
}
# Added on the host:
# - module_runtime.c and ce_fmt.c, which split/program.mk links into every
#   module;
# - token.c and parser.c, which produce the AST.  On the CE they run in the
#   launcher (split/Makefile.launcher); here they are built with the module's
#   flags.  The launcher only adds CE_LAUNCHER_TINY_ERRORS, which changes
#   error text, not the parsed tree;
# - the file I/O mock that ipc.c and module_runtime.c call into.
_HOST_EXTRA_SOURCES = [
    "src/split/module_runtime.c",
    "src/ce_fmt.c",
    "src/token.c",
    "src/parser.c",
    "tests/mock/fileioc_mock.c",
]


def _makefile_vars(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8").replace("\\\n", " ")
    values: dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r"^([A-Z_]+)\s*[:?]?=\s*(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2).strip()
    return values


def _built_module_makefiles() -> list[Path]:
    top = (ROOT / "Makefile").read_text(encoding="utf-8").replace("\\\n", " ")
    names: list[str] = []
    for var in ("SPLIT_MODULES", "WORKER_MODULES"):
        match = re.search(rf"^{var}\s*:=\s*(.*)$", top, re.M)
        assert match, f"{var} not found in Makefile"
        names.extend(match.group(1).split())
    return [SPLIT / f"Makefile.{name}" for name in names]


def _flag(cflags: str, name: str) -> str | None:
    # The last -D wins, as it does for the compiler.
    matches = re.findall(rf"-D{name}=(\S+)", cflags)
    return matches[-1] if matches else None


def _effective(cflags: str, name: str, *fallbacks: str) -> str:
    """Resolve a flag the way src/split/processor.h does: an unset
    CE_PROC_CALC_DERIV defaults to CE_PROC_CALC_BASE, which defaults to
    CE_PROC_CALC, which defaults to 0."""
    for flag in (name, *fallbacks):
        value = _flag(cflags, flag)
        if value is not None:
            return value
    return "0"


def test_finite_sum_flag_scope():
    """Only CELSCX uses direct dispatch to preserve its backstop answers."""
    enabled = {"base", "calc.seq", "lists_complex"}
    files = _built_module_makefiles()
    assert len(files) == 35
    for path in files:
        flags = _makefile_vars(path).get("PROGRAM_CFLAGS", "")
        expected = "1" if path.name.removeprefix("Makefile.") in enabled else "0"
        assert (_flag(flags, "CE_EVAL_SUM_COMPENSATED") or "0") == expected, path.name
    source = (ROOT / "src/eval.c").read_text()
    assert re.search(r"#ifndef CE_EVAL_SUM_COMPENSATED\s+#ifdef __ez80__\s+"
                     r"#define CE_EVAL_SUM_COMPENSATED 0\s+#else\s+"
                     r"#define CE_EVAL_SUM_COMPENSATED 1\s+#endif\s+#endif", source)


def test_finite_sum_flag_negative_control(tmp_path, monkeypatch):
    """Removing a flag in a copied makefile must fail the scope guard."""
    (tmp_path / "split").mkdir()
    (tmp_path / "src").mkdir()
    shutil.copy2(ROOT / "Makefile", tmp_path / "Makefile")
    shutil.copy2(ROOT / "src/eval.c", tmp_path / "src/eval.c")
    for path in _built_module_makefiles():
        shutil.copy2(path, tmp_path / "split" / path.name)
    target = tmp_path / "split/Makefile.calc.seq"
    flags = target.read_text()
    assert flags.count("-DCE_EVAL_SUM_COMPENSATED=1") == 1
    target.write_text(flags.replace("-DCE_EVAL_SUM_COMPENSATED=1", ""))
    monkeypatch.setitem(globals(), "ROOT", tmp_path)
    monkeypatch.setitem(globals(), "SPLIT", tmp_path / "split")
    with pytest.raises(AssertionError, match="Makefile.calc.seq"):
        test_finite_sum_flag_scope()


def _handles_derivatives(cflags: str) -> bool:
    deriv = _effective(cflags, "CE_PROC_CALC_DERIV",
                       "CE_PROC_CALC_BASE", "CE_PROC_CALC")
    tangent = _flag(cflags, "CE_PROC_CALC_TANGENT") or deriv
    return deriv != "0" or tangent != "0"


def _host_cc() -> str | None:
    # The module build needs a GNU-style driver (-include, -lm).  On Windows
    # only MinGW gcc is accepted; clang/cc there may target MSVC.
    if sys.platform == "win32":
        return shutil.which("gcc")
    return shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _gc_flags() -> list[str]:
    # Module sources reference functions that only other modules define
    # (integrate.c's definite/polar paths).  The CE build drops them through
    # LTO; on the host, drop unreferenced sections instead.
    if sys.platform == "darwin":
        return ["-Wl,-dead_strip"]
    return ["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]


def _build_module_driver(makefile: Path, out_dir: Path, *,
                         strategy: bool = False,
                         extra_cflags: tuple[str, ...] = (),
                         sanitize: bool = False,
                         tag: str = "") -> Path:
    cc = _host_cc()
    if cc is None:
        pytest.skip("no host C compiler")
    values = _makefile_vars(makefile)
    if strategy:
        driver = STRATEGY_DRIVER
        sources = values["PROGRAM_SOURCES"].split()
        extra = [s for s in _HOST_EXTRA_SOURCES
                 if s != "src/split/module_runtime.c"]
        main_flags = ["-Dmain=ce_worker_main"]
    else:
        driver = DRIVER
        sources = [s for s in values["PROGRAM_SOURCES"].split()
                   if s not in _HOST_EXCLUDED_SOURCES]
        extra = _HOST_EXTRA_SOURCES
        main_flags = []
    exe = out_dir / f"driver_{makefile.name.replace('.', '_')}{tag}"
    san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] \
        if sanitize else []
    cmd = [
        cc, "-std=c99", "-O0", "-w",
        "-I", str(ROOT / "src"), "-I", str(ROOT / "tests" / "mock"),
        # DESKTOP_BUILD only stubs the keypad scan in runtime_globals.c and
        # the program hand-off in module_runtime.c; no math source tests it.
        # -w: the module sources are warning-checked by the CE build itself.
        "-DDESKTOP_BUILD",
        # Freed pool nodes are overwritten, so use-after-free shows up.
        "-DMEM_POISON_ON_FREE",
        *values["PROGRAM_CFLAGS"].split(),
        # Later -D wins, so extra_cflags can override a makefile flag.
        *extra_cflags,
        *main_flags,
        "-DCE_FMT_REDIRECT", "-DCE_FMT_REAL=float",
        "-include", str(ROOT / "src" / "ce_fmt.h"),
        *san,
        str(driver),
        *[str(ROOT / s) for s in sources + extra],
        *_gc_flags(),
        "-lm", "-o", str(exe),
    ]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if sanitize and proc.returncode != 0:
        pytest.skip("host compiler cannot build with ASan/UBSan")
    assert proc.returncode == 0, proc.stderr[-4000:]
    return exe


def _run_stats(exe: Path, exprs: list[str]
               ) -> list[tuple[bool, str, int | None, int | None]]:
    """Run the driver; per expression return (ok, text, leaked nodes,
    double frees).  The counters are None when the driver did not measure
    them (the expression never reached the module).  Arguments starting
    with --free= are driver options."""
    env = dict(os.environ,
               ASAN_OPTIONS="detect_leaks=0",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    proc = subprocess.run([str(exe), *exprs], capture_output=True, text=True,
                          timeout=120, env=env)
    assert proc.returncode == 0, proc.stderr[-4000:]
    rows = proc.stdout.splitlines()
    want = len([e for e in exprs if not e.startswith(
        ("--free=", "--steps=", "--alloc-stats="))])
    assert len(rows) == want, proc.stdout
    out = []
    for row in rows:
        parts = row.split("\t")
        assert len(parts) >= 4, f"driver row without counters: {row!r}"
        leaked, dfree = (None if v == "-" else int(v) for v in parts[-2:])
        parts = parts[:-2]
        out.append((parts[0] == "1", "\t".join(parts[1:]), leaked, dfree))
    return out


def _run(exe: Path, exprs: list[str]) -> list[tuple[bool, str]]:
    return [(ok, text) for ok, text, _, _ in _run_stats(exe, exprs)]


@pytest.fixture(scope="module")
def cecalc(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _build_module_driver(SPLIT / "Makefile.calculus",
                                tmp_path_factory.mktemp("cecalc"))


# Expected strings are what the full-feature desktop CLI prints
# (bin/calcengine --solve '<expr>').
# Result lines use the display printer (ast_to_display_str):
# minimal parentheses and implicit products after a numeric literal.
CECALC_CASES = [
    ("d/dx(x^3+2x)", "d/dx = 3x^2+2"),
    ("d/dx(x^4-3x^2)", "d/dx = 4x^3-6x"),
    ("nderiv(x^2,x,1)", "d/dx = 2x"),
    ("nderiv(x^3,x,2)", "d/dx = 6x"),
    ("d/dx(sin(x)*x^2)", "d/dx = cos(x)*x^2+sin(x)*(2x)"),
    ("d/dx(sqrt(x))", "d/dx = 1/(2sqrt(x))"),
    ("d/dx(1/x)", "d/dx = -1/x^2"),
    ("d/dx(ln(x^2+1))", "d/dx = 2x/(x^2+1)"),
    ("d/dx(5)", "d/dx = 0"),
    ("d/dx(x)", "d/dx = 1"),
    ("tangent(x^2,x,1)", "y = 2*x - 1"),
]

def test_cecalc_nested_definite_integral_is_refused_not_summed(cecalc: Path) -> None:
    """CECALC builds with CE_EVAL_ENABLE_NUMERIC_INT=0 (size, PR #99): a
    definite integral nested in a tangent is refused instead of summed
    without the convergence checks. handle_tangent reports it with its own
    message. Plain tangents are unaffected."""
    rows = _run(cecalc, ["tangent(x*int(t,t,0,1),x,2)",
                         "tangent(x^2,x,int(1,x,0,1))", "tangent(x^2,x,1)"])
    assert not rows[0][0] and "Cannot evaluate" in rows[0][1], rows[0]
    assert not rows[1][0] and "Cannot evaluate" in rows[1][1], rows[1]
    assert rows[2] == (True, "y = 2*x - 1"), rows[2]


def test_cecalc_records_steps_and_clears_each_request(cecalc: Path) -> None:
    expr = "d/dx(x^3+2x)"
    rows = _run_stats(cecalc, ["--steps=1", expr, "d/dx(x)"])
    ok, text, leaked, dfree = rows[0]
    assert ok and leaked == dfree == 0
    result, *steps = text.split("|STEP|")
    assert result == CECALC_CASES[0][1]
    assert steps[0] == "d/dx( ((x^3)+(2*x)) )"
    assert "Sum rule on ((x^3)+(2*x))" in steps
    assert "Power rule on (x^3)" in steps
    assert "Const-mult rule on (2*x)" in steps
    assert steps[-1] == "= ((3*(x^2))+2)"
    assert rows[1] == (True, "d/dx = 1|STEP|d/dx( x )|STEP|= 1", 0, 0)


@pytest.mark.parametrize("expr, rule", [
    ("d/dx(sin(cos(x)))", "Chain rule on sin(cos(x))"),
    ("d/dx(ln(x))", "Chain rule on ln(x)"),
    ("d/dx(e^sin(x))", "Exp rule on (e^sin(x))"),
    ("d/dx(2^sin(x))", "Exp rule on (2^sin(x))"),
    ("nderiv(x^3,x,2)", "Derivative #2 of (3*(x^2))"),
])
def test_cecalc_nested_rule_text_uses_the_right_input(cecalc: Path,
                                                    expr: str, rule: str) -> None:
    ok, text, leaked, dfree = _run_stats(cecalc, ["--steps=1", expr])[0]
    assert ok and leaked == dfree == 0
    assert rule in text.split("|STEP|"), text
    if expr == "d/dx(sin(cos(x)))":
        assert "Chain rule on cos(x)" in text.split("|STEP|")


def test_cecalc_deep_step_text_is_explicitly_abbreviated(cecalc: Path) -> None:
    expr = "d/dx(" + "+".join(["x"] * 20) + ")"
    ok, text, leaked, dfree = _run_stats(cecalc, ["--steps=1", expr])[0]
    assert ok and leaked == dfree == 0
    result, *steps = text.split("|STEP|")
    assert result == "d/dx = 20", "only explanatory text is capped, not the computation"
    assert steps[0] == "d/dx( (... deep expression) )"
    assert "... nested rules omitted" in steps
    assert steps[-1] == "= 20"


def test_cecalc_reports_omission_when_unary_nodes_skip_boundary(cecalc: Path) -> None:
    expr = "d/dx(" + "-(" * 9 + "x+x" + ")" * 9 + ")"
    ok, text, leaked, dfree = _run_stats(cecalc, ["--steps=1", expr])[0]
    assert ok and leaked == dfree == 0
    result, *steps = text.split("|STEP|")
    assert result == "d/dx = -2"
    assert steps.count("... nested rules omitted") == 1
    assert steps[-1] == "= -2"


# Signs that the power/product rule output was never simplified, in the
# display format: a parenthesised literal pair (x^(3-1)), a product or
# quotient of two literals that is not part of a power (2*3x), or a factor or
# exponent of 1 (x*1, x^1).
_UNFOLDED = re.compile(
    r"\(\d+(?:\.\d+)?[-+*/]\d+(?:\.\d+)?\)"
    r"|(?<![\w.^])\d+(?:\.\d+)?[*/]\d+(?:\.\d+)?(?![\d.^])"
    r"|\*1(?![\d.])|\^1(?![\d.])")


def test_unfolded_pattern_matches_display_format() -> None:
    for bad in ("d/dx = 3x^(3-1)", "d/dx = 2*3x", "d/dx = x*1", "d/dx = x^1+2",
                "d/dx = 6/2x"):
        assert _UNFOLDED.search(bad), bad
    for good in ("d/dx = 3x^2+2", "d/dx = 4x^3-6x", "d/dx = x^10*10",
                 "d/dx = 2x/(x^2+1)", "d/dx = 1/(2sqrt(x))", "d/dx = x^2*3",
                 "d/dx = 0.5x^(-1)", "d/dx = 2*3^x"):
        assert not _UNFOLDED.search(good), good


def test_cecalc_derivatives_are_simplified_with_module_flags(cecalc: Path) -> None:
    results = _run(cecalc, [expr for expr, _ in CECALC_CASES])
    for (expr, expected), (ok, text) in zip(CECALC_CASES, results):
        assert ok, f"{expr}: {text}"
        assert text == expected, f"{expr}: got {text!r}, want {expected!r}"


def test_cecalc_derivative_output_has_no_unfolded_constants(cecalc: Path) -> None:
    # Pin every scanned answer. Display text can expose an unfolded sum
    # without parentheses (1+1), which the supplementary regex cannot detect.
    cases = [
        ("d/dx(x^5+x^3-7x)", "d/dx = 5x^4+3x^2-7"),
        ("d/dx(3x^2*x)", "d/dx = 6x*x+3x^2"),
        ("d/dx(cos(2x))", "d/dx = -sin(2x)*2"),
        ("d/dx(e^(3x))", "d/dx = e^(3x)*3"),
        ("d/dx(ln(x^2))", "d/dx = 2x/x^2"),
        ("d/dx(sqrt(x))", "d/dx = 1/(2sqrt(x))"),
        ("nderiv(x^4,x,3)", "d/dx = 24x"),
        ("d/dx(x+x)", "d/dx = 2"),
    ]
    exprs = [expr for expr, _ in cases]
    for (expr, expected), (ok, text) in zip(cases, _run(cecalc, exprs)):
        assert ok, f"{expr}: {text}"
        assert text == expected, f"{expr}: got {text!r}, want {expected!r}"
        assert not _UNFOLDED.search(text), f"{expr}: unsimplified {text!r}"


def test_cecalc_deep_expression_still_returns(cecalc: Path) -> None:
    """A tree deeper than the module's SIMPLIFY_MAX_DEPTH (24 in CECALC) must
    still produce a result: the deep part is left unsimplified, never an
    error or a crash."""
    values = _makefile_vars(SPLIT / "Makefile.calculus")
    depth = int(_flag(values["PROGRAM_CFLAGS"], "SIMPLIFY_MAX_DEPTH") or 50)
    terms = depth + 8
    exprs = [
        "d/dx(" + "+".join(["x"] * terms) + ")",
        "d/dx(" + "sin(" * 24 + "x" + ")" * 24 + ")",
    ]
    for expr in exprs:
        assert len(expr) < 128, "launcher input limit is 127 characters"
    for expr, (ok, text) in zip(exprs, _run(cecalc, exprs)):
        assert ok, f"{expr}: {text}"
        assert text.startswith("d/dx = "), text
        assert "()" not in text, text


def test_cecalc_pool_exhaustion_is_a_clean_error(cecalc: Path) -> None:
    """When the 512-node pool runs out part-way through a derivative, the
    module must report an error, not crash in simplify (NULL operand) and not
    print a tree with holes such as "(0+(0+))" or "cos()"."""
    exprs = [
        "nderiv(x^2*sin(x)*cos(x)*e^x,x,9)",
        "nderiv(sin(x)*cos(x)*tan(x),x,6)",
        "nderiv(x^x,x,5)",
        "nderiv(1/(1+x^2),x,8)",
    ]
    for expr, (ok, text, leaked, dfree) in zip(exprs, _run_stats(cecalc, exprs)):
        assert not ok, f"{expr}: expected an error, got {text!r}"
        assert "()" not in text and "+)" not in text, text
        # On main, ast_clone freed a failed ast_binop's operands twice
        # (15 double frees for the first expression).
        assert leaked == 0 and dfree == 0, (expr, leaked, dfree)


def test_every_built_derivative_module_links_simplify() -> None:
    """Static guard: a built module that handles d/dx must simplify the result."""
    checked = []
    for makefile in _built_module_makefiles():
        assert makefile.exists(), makefile
        values = _makefile_vars(makefile)
        cflags = values.get("PROGRAM_CFLAGS", "")
        if not _handles_derivatives(cflags):
            continue
        checked.append(makefile.name)
        assert _flag(cflags, "CE_DERIV_USE_SIMPLIFY") != "0", (
            f"{makefile.name}: handles d/dx but builds with "
            "-DCE_DERIV_USE_SIMPLIFY=0, so results print as x^(3-1), 2*1")
        assert "src/simplify.c" in values["PROGRAM_SOURCES"].split(), (
            f"{makefile.name}: handles d/dx but does not link src/simplify.c")
    assert "Makefile.calculus" in checked


def test_flag_resolution_follows_processor_defaults() -> None:
    assert _handles_derivatives("-DCE_PROC_CALC_BASE=1")
    assert _handles_derivatives("-DCE_PROC_CALC=1")
    assert _handles_derivatives("-DCE_PROC_CALC_DERIV=0 -DCE_PROC_CALC_TANGENT=1")
    assert not _handles_derivatives("-DCE_PROC_CALC=1 -DCE_PROC_CALC_BASE=0")
    assert not _handles_derivatives("-DCE_PROC_CALC=1 -DCE_PROC_CALC_DERIV=0 "
                                    "-DCE_PROC_CALC_TANGENT=0")
    assert not _handles_derivatives("")


# ── Integration workers ─────────────────────────────────────────────
#
# Routing on the CE: int() goes CECINT -> CECISM, which handles the basic
# rules and forwards the rest to CECIAD, which tries the strategy workers
# CEIPFR, CEITRG, CEIUSB and CEIPRT in turn.  intparts/trigsub/usub go
# straight to their worker, and ode_sep/ode_linear/diffeq go to CECODE.

INT_PROCESSOR_MODULES = ["Makefile.calc.int.simple", "Makefile.calc.ode"]
INT_STRATEGY_MODULES = [
    "Makefile.calc.int.parts",
    "Makefile.calc.int.pfrac",
    "Makefile.calc.int.trig",
    "Makefile.calc.int.usub",
]


def _int_driver(name: str, tmp_path_factory: pytest.TempPathFactory,
                **kwargs) -> Path:
    return _build_module_driver(SPLIT / name,
                                tmp_path_factory.mktemp(name.replace(".", "_")),
                                strategy=name in INT_STRATEGY_MODULES,
                                **kwargs)


@pytest.fixture(scope="module")
def cecism(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _int_driver("Makefile.calc.int.simple", tmp_path_factory)


@pytest.fixture(scope="module")
def cecism_unfolded(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """CECISM with the fold turned off: what the CE showed before."""
    return _int_driver("Makefile.calc.int.simple", tmp_path_factory,
                       extra_cflags=("-DCE_INTEG_USE_FOLD_LITE=0",),
                       tag="_unfolded")


@pytest.fixture(scope="module")
def int_workers(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Path]:
    return {name: _int_driver(name, tmp_path_factory)
            for name in INT_STRATEGY_MODULES + ["Makefile.calc.ode"]}


# Exact CE output.  The first three are the cases from the bug report; on
# main CECISM printed ((x^(2+1))/(2+1)) for int(x^2,x).  fold_lite cancels
# coefficients (3*((x^3)/3) -> x^3), which the desktop simplifier does not
# do, so a few strings are tidier than the desktop CLI's.
CECISM_CASES = [
    ("int(x^2,x)", "int = ((x^3)/3) + C"),
    ("int(3x^2+2x,x)", "int = ((x^3)+(x^2)) + C"),
    ("int(sin(x),x)", "int = (-cos(x)) + C"),
    ("int(x^3-4x+1,x)", "int = ((((x^4)/4)-(2*(x^2)))+x) + C"),
    ("int(4x^3,x)", "int = (x^4) + C"),
    ("int(-3x^2,x)", "int = (-(x^3)) + C"),
    ("int(1/x,x)", "int = ln(abs(x)) + C"),
    # Constant -1 exponents (review rounds 12 and 14): NEG 1 is matched
    # directly and (0-1) is folded on a copy, never x^0/0.
    ("int(x^(-1),x)", "int = ln(abs(x)) + C"),
    ("int(x^(0-1),x)", "int = ln(abs(x)) + C"),
    ("int((2-1)^x,x)", "int = x + C"),
    # CECISM builds with FOLD_LITE_SIGN_RULES=0 (size), so no b-a reorder.
    ("int(sin(x)+cos(x),x)", "int = ((-cos(x))+sin(x)) + C"),
    ("int(-sin(x),x)", "int = cos(x) + C"),
    ("int(sqrt(x),x)", "int = ((2/3)*(x^(3/2))) + C"),
    ("int(5,x)", "int = (5*x) + C"),
    ("int(x,x)", "int = ((x^2)/2) + C"),
]


def test_cecism_integrals_are_folded_with_module_flags(cecism: Path) -> None:
    results = _run(cecism, [expr for expr, _ in CECISM_CASES])
    for (expr, expected), (ok, text) in zip(CECISM_CASES, results):
        assert ok, f"{expr}: {text}"
        assert text == expected, f"{expr}: got {text!r}, want {expected!r}"


@pytest.mark.parametrize("expr, expected_steps", [
    ("int(x^2,x)", ["Power rule: int(x^n) = x^(n+1)/(n+1)"]),
    ("int(sin(2x),x)", ["u-sub: int(sin(u)) = -cos(u)/a, a=2"]),
    ("int(cos(3x),x)", ["u-sub: int(cos(u)) = sin(u)/a, a=3"]),
    ("int(2^x,x)", ["int(2^x) = 2^x/ln(2)"]),
    ("int(3^(2x),x)", ["u-sub: int(3^u) = 3^u/(a*ln(3))"]),
    ("int(x^2+sin(2x),x)", ["Power rule: int(x^n) = x^(n+1)/(n+1)",
                            "u-sub: int(sin(u)) = -cos(u)/a, a=2"]),
])
def test_cecism_shared_step_buffer_preserves_rule_text(
        cecism: Path, expr: str, expected_steps: list[str]) -> None:
    """Power, linear-function and exponential rules reuse integ_text's static buffer to
    fit the CE module; each step must consume its text before the next print.
    A second request with a different coefficient also checks stale text."""
    rows = _run_stats(cecism, ["--steps=1", expr, "int(sin(7x),x)"])
    for ok, text, leaked, dfree in rows:
        assert ok and leaked == dfree == 0, (ok, text, leaked, dfree)
    steps = rows[0][1].split("|STEP|")[1:]
    for expected in expected_steps:
        assert expected in steps, (expr, steps)
    assert "u-sub: int(sin(u)) = -cos(u)/a, a=7" in rows[1][1].split("|STEP|")


# The CE keypad types upper-case letters.  With no variable
# argument the parser defaulted to lower-case x, so int(X^2) integrated X^2
# as a constant and the CE showed "((X^2)*x) + C".  The default now follows
# the input's case, so each input must match its explicit-variable form.
# The driver parses with the launcher's parser and sends the tree through
# astwire, as CALCENG does.
CECISM_DEFAULT_VAR_CASES = [
    ("int(X^2)", "int(X^2,X)", "int = ((X^3)/3) + C"),
    ("int(X^2)dX", "int(X^2,X)", "int = ((X^3)/3) + C"),
    ("int(X^2)dx", "int(X^2,X)", "int = ((X^3)/3) + C"),
    ("int(3X^2+2X)", "int(3X^2+2X,X)", "int = ((X^3)+(X^2)) + C"),
    ("int(sin(X))", "int(sin(X),X)", "int = (-cos(X)) + C"),
    ("int(T)", "int(T,x)", "int = (T*x) + C"),
    ("int(x^2)", "int(x^2,x)", "int = ((x^3)/3) + C"),
    ("int(5)", "int(5,x)", "int = (5*x) + C"),
]


def test_cecism_default_variable_follows_input_case(cecism: Path) -> None:
    exprs = [e for row in CECISM_DEFAULT_VAR_CASES for e in row[:2]]
    rows = _run_stats(cecism, exprs)
    for i, (bare, explicit, expected) in enumerate(CECISM_DEFAULT_VAR_CASES):
        for expr, (ok, text, leaked, dfree) in ((bare, rows[2 * i]),
                                                (explicit, rows[2 * i + 1])):
            assert ok and leaked == dfree == 0, (expr, ok, text, leaked, dfree)
            assert text == expected, f"{expr}: got {text!r}, want {expected!r}"


def test_cecism_nested_int_without_variable_is_not_a_constant(cecism: Path) -> None:
    """int(int(f)) on the CE used to clone the inner integral through the
    constant rule (and, with an 84-deep inner tree, overflow the stack while
    printing it).  It must now integrate in X like int(int(f,X),X)."""
    pairs = [("int(int(--2X+X+X))", "int(int(--2X+X+X,X),X)"),
             ("int(int(X))", "int(int(X,X),X)")]
    rows = _run_stats(cecism, [e for pair in pairs for e in pair])
    for i, (bare, explicit) in enumerate(pairs):
        (ok_b, text_b, leak_b, df_b) = rows[2 * i]
        (ok_e, text_e, leak_e, df_e) = rows[2 * i + 1]
        assert leak_b == df_b == leak_e == df_e == 0, rows
        assert (ok_b, text_b) == (ok_e, text_e), (bare, rows[2 * i], rows[2 * i + 1])
        assert "*x)" not in text_b and "int(" not in text_b, (bare, text_b)


CECALC_DEFAULT_VAR_CASES = [
    ("deriv(X^3)", "d/dX(X^3)", "d/dX = 3X^2"),
    ("nderiv(X^3)", "nderiv(X^3,X)", "d/dX = 6X"),
    ("deriv(x^3)", "d/dx(x^3)", "d/dx = 3x^2"),
]


def test_cecalc_default_variable_follows_input_case(cecalc: Path) -> None:
    exprs = [e for row in CECALC_DEFAULT_VAR_CASES for e in row[:2]]
    rows = _run_stats(cecalc, exprs)
    for i, (bare, explicit, expected) in enumerate(CECALC_DEFAULT_VAR_CASES):
        for expr, (ok, text, leaked, dfree) in ((bare, rows[2 * i]),
                                                (explicit, rows[2 * i + 1])):
            assert ok and leaked == dfree == 0, (expr, ok, text, leaked, dfree)
            assert text == expected, f"{expr}: got {text!r}, want {expected!r}"


# Only the case of the default letter is inferred, so on the CE an
# upper-case input must answer like its lower-case form does on the desktop
# (review, PR #124).  The one exception, a second nlsolve/graphimpl default
# that would equal an explicit first variable, has no case here.  The CE keypad types every letter upper-case
# (keywords are matched case-insensitively), as in INT(X^2) and D/DX(X^3).


MIRROR_CECISM = ["int(x^2)", "int(3x^2+2x)", "int(sin(x))", "int(x^2)dx",
                 "int(t^2)", "int(a*x)", "int(a*b)", "int(5)", "int(x) d y",
                 "int(int(x))", "int(int(--2x+x+x))", "int(sum(k,k,1,3))",
                 "int(e^x)", "int(1/x)", "int(x*e^x)"]
MIRROR_CECALC = ["deriv(x^3)", "nderiv(x^3)", "deriv(t^2)", "deriv(a*x^2)",
                 "deriv(sum(k,k,1,3))", "d/dx(x^3)", "tangent(x^2)",
                 "tangent(x^2,x,1)", "deriv(sin(x)*x)"]


@pytest.mark.parametrize("module, exprs", [("cecism", MIRROR_CECISM),
                                           ("cecalc", MIRROR_CECALC)])
def test_upper_case_input_answers_like_lower_case(
        request: pytest.FixtureRequest, module: str, exprs: list[str]) -> None:
    exe = request.getfixturevalue(module)
    lower = _run(exe, exprs)
    upper = _run(exe, [e.upper() for e in exprs])
    for expr, lo, up in zip(exprs, lower, upper):
        assert (up[0], up[1].lower()) == (lo[0], lo[1].lower()), (expr, lo, up)


def test_cecism_unfolded_build_reproduces_the_bug(cecism_unfolded: Path) -> None:
    """Guards the guard: without the fold the module prints the raw power
    rule, so the exact-output test above really depends on fold_lite."""
    [(ok, text)] = _run(cecism_unfolded, ["int(x^2,x)"])
    assert ok and text == "int = ((x^(2+1))/(2+1)) + C", text


# Strategy workers and CECODE.  CEIPRT, CEIUSB and CECODE link the full
# simplifier (the desktop path); CEIPFR and CEITRG use fold_lite.
INT_WORKER_CASES = {
    "Makefile.calc.int.parts": [
        ("int(x*sin(x),x)", "int = x*(-cos(x))+sin(x) + C"),
        ("intparts(x*e^x,x)", "int = x*e^x-e^x + C"),
    ],
    "Makefile.calc.int.usub": [
        # u-substitution compares du with factors of the integrand, which
        # only works with simplify linked: on main every case failed with
        # "u-substitution not applicable".
        ("usub(2x*cos(x^2),x,x^2)", "int = sin(x^2) + C"),
        ("usub(3x^2*sin(x^3),x,x^3)", "int = -cos(x^3) + C"),
        ("int(2x*(x^2+1)^3,x)", "int = (x^2+1)^4/4 + C"),
    ],
    "Makefile.calc.int.trig": [
        ("trigsub(sqrt(1-x^2),x)",
         "int = x/2*sqrt(1-x^2)+0.5asin(x) + C"),
        ("int(1/(1+x^2),x)", "int = atan(x) + C"),
    ],
    "Makefile.calc.int.pfrac": [
        ("int(1/(x*(x+1)),x)", "int = (ln(abs(x))-ln(abs((x+1)))) + C"),
    ],
    "Makefile.calc.ode": [
        ("ode_sep(x^2,x,y)", "y = x^3/3 + C"),
        ("ode_sep(x*y,x,y)", "ln(abs(y)) = x^2/2 + C"),
        ("diffeq(x-y,x,y)", "y = (exp(x)*(x-1) + C)/(exp(x))"),
        ("ode_linear(x-y,x,y)", "y = (exp(x)*(x-1) + C)/(exp(x))"),
        ("diffeq(x^2,x,y)", "y = x^3/3 + C"),
    ],
}


@pytest.mark.parametrize("name", sorted(INT_WORKER_CASES))
def test_integration_worker_results_with_module_flags(
        name: str, int_workers: dict[str, Path]) -> None:
    cases = INT_WORKER_CASES[name]
    results = _run(int_workers[name], [expr for expr, _ in cases])
    for (expr, expected), (ok, text) in zip(cases, results):
        assert ok, f"{name} {expr}: {text}"
        assert text == expected, (
            f"{name} {expr}: got {text!r}, want {expected!r}")


# Signs of rule output that was never folded: (2+1), (1/1), *1), 1*x, ^1)
_INT_UNFOLDED = re.compile(
    r"\(-?\d+[-+*/]\(?-?\d+\)?\)|\*1\)|\(1\*|\^1\)|/1\)")

INT_FOLD_EXPRS = [
    "int(x^2,x)", "int(3x^2+2x,x)", "int(x^3-4x+1,x)", "int(7x^6,x)",
    "int(x^10,x)", "int(6x^5-2,x)", "int(2x+1,x)", "int(x^2+x+1,x)",
    "int(-x,x)", "int(8x^3,x)", "int(1/x,x)", "int(cos(2x),x)",
    "int(x^-2,x)", "int(-3x^2+4,x)", "int(5x^4-3x^2+2,x)",
]


def test_cecism_output_has_no_unfolded_constants(cecism: Path) -> None:
    for expr, (ok, text) in zip(INT_FOLD_EXPRS, _run(cecism, INT_FOLD_EXPRS)):
        assert ok, f"{expr}: {text}"
        assert not _INT_UNFOLDED.search(text), f"{expr}: unfolded {text!r}"


def _eval_result(text: str, x: float) -> float:
    """Evaluate an "int = ... + C" string at x (C = 0)."""
    body = text.removeprefix("int = ").removesuffix(" + C")
    # Tokenize scientific literals before inserting implicit products.
    tokens = re.findall(r"(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[A-Za-z_]\w*|\S", body)
    parts: list[str] = []
    previous_number = False
    for token in tokens:
        if previous_number and (token[0].isalpha() or token == "("):
            parts.append("*")
        parts.append("**" if token == "^" else token)
        previous_number = token[0].isdigit() or token[0] == "."
    body = "".join(parts)
    names = {
        "x": x, "e": math.e, "pi": math.pi,
        "sin": math.sin, "cos": math.cos, "tan": math.tan,
        "asin": math.asin, "acos": math.acos, "atan": math.atan,
        "ln": math.log, "abs": abs, "sqrt": math.sqrt, "exp": math.exp,
    }
    return float(eval(body, {"__builtins__": {}}, names))  # noqa: S307


@pytest.mark.parametrize("body, expected", [
    ("2x", 2 * 0.2),
    ("2(x+1)", 2 * (0.2 + 1)),
    ("1e+06x", 1e6 * 0.2),
    ("1.5e-05x", 1.5e-5 * 0.2),
    ("0.5asin(x)", 0.5 * math.asin(0.2)),
    ("2*e^x", 2 * math.exp(0.2)),
    ("x/(2sqrt(x))", 0.2 / (2 * math.sqrt(0.2))),
])
def test_fold_result_evaluator_accepts_display_products(body: str,
                                                       expected: float) -> None:
    assert _eval_result(f"int = {body} + C", 0.2) == pytest.approx(expected)


@pytest.fixture(scope="module")
def fold_pairs(tmp_path_factory: pytest.TempPathFactory) -> dict[str, tuple[Path, Path]]:
    """(folded, unfolded) drivers for every fold_lite module."""
    pairs = {}
    for name in FOLD_MODULES:
        pairs[name] = (
            _int_driver(name, tmp_path_factory),
            _int_driver(name, tmp_path_factory,
                        extra_cflags=("-DCE_INTEG_USE_FOLD_LITE=0",),
                        tag="_unfolded"))
    return pairs


FOLD_MODULES = ["Makefile.calc.int.simple", "Makefile.calc.int.trig",
                "Makefile.calc.int.pfrac"]

# Expressions and sample points (inside each result's domain) per module.
FOLD_EQUIVALENCE = {
    "Makefile.calc.int.simple": (INT_FOLD_EXPRS + [
        "int(sin(x)+cos(x),x)", "int(-sin(x),x)", "int(x^(1/2),x)",
        "int(x/3,x)", "int(x^2/2,x)", "int(2*e^(3x),x)", "int(0,x)",
        "int(-(x^3),x)", "int(3-x,x)", "int(-5,x)", "int(x^-3,x)",
        "int(-6x^5,x)", "int(12x^3-x,x)",
    ], (0.37, 1.3, 2.9)),
    "Makefile.calc.int.trig": ([
        "trigsub(sqrt(1-x^2),x)", "trigsub(1/sqrt(4-x^2),x)",
        "trigsub(1/(x^2+9),x)", "int(1/(1+x^2),x)", "int(1/(4+x^2),x)",
        "trigsub(sqrt(9-x^2),x)",
    ], (0.2, 0.5, 0.8)),
    "Makefile.calc.int.pfrac": ([
        "int(1/(x^2-1),x)", "int(1/(x*(x+1)),x)", "int(1/(x^2-4),x)",
        "int(2/(x^2-1),x)", "int(1/((x-1)*(x+2)),x)",
        "int(3/(x*(x-3)),x)",
    ], (3.3, 4.7, 6.1)),
}


@pytest.mark.parametrize("name", FOLD_MODULES)
def test_fold_preserves_the_value_of_every_result(
        name: str, fold_pairs: dict[str, tuple[Path, Path]]) -> None:
    """fold_lite must only rewrite, never change, the antiderivative: compare
    each module's folded and unfolded output numerically."""
    folded_exe, raw_exe = fold_pairs[name]
    exprs, points = FOLD_EQUIVALENCE[name]
    folded = _run(folded_exe, exprs)
    raw = _run(raw_exe, exprs)
    checked = 0
    for expr, (ok_f, f), (ok_r, r) in zip(exprs, folded, raw):
        assert ok_f == ok_r, f"{name} {expr}: {f!r} / {r!r}"
        if not ok_f:
            continue
        checked += 1
        for x in points:
            want = _eval_result(r, x)
            got = _eval_result(f, x)
            assert math.isclose(got, want, rel_tol=1e-5, abs_tol=1e-6), (
                f"{name} {expr} at x={x}: folded {f!r} = {got}, "
                f"raw {r!r} = {want}")
    assert checked >= len(exprs) // 2, f"{name}: too few results to compare"


_HOLE = re.compile(r"\(\)|[-+*/^]\)|\([+*/^]|= +\+ C$")


@pytest.mark.parametrize("name", ["Makefile.calc.int.simple"]
                         + INT_STRATEGY_MODULES + ["Makefile.calc.ode"])
def test_integration_pool_exhaustion_is_clean(
        name: str, tmp_path_factory: pytest.TempPathFactory) -> None:
    """Run each worker with only N pool nodes free, for every N from 0 to
    120: the result must be complete or a clean error, never a tree with a
    hole such as "(0+)" or "cos()", and never a crash, a leaked node or a
    double free.  Built with ASan and UBSan when the host compiler supports
    them.  (On main, ast_clone freed the operands of a failed ast_binop a
    second time.)"""
    exprs = {
        "Makefile.calc.int.simple": ["int(3x^2+2x,x)", "int(x^3-4x+1,x)",
                                      "int(sin(x)+cos(x),x)"],
        "Makefile.calc.int.parts": ["int(x*sin(x),x)", "intparts(x*e^x,x)"],
        "Makefile.calc.int.pfrac": ["int(1/(x*(x+1)),x)",
                                     "int(1/(x^2-1),x)"],
        "Makefile.calc.int.trig": ["trigsub(sqrt(1-x^2),x)",
                                    "int(1/(1+x^2),x)"],
        "Makefile.calc.int.usub": ["usub(2x*cos(x^2),x,x^2)",
                                    "int(2x*(x^2+1)^3,x)"],
        "Makefile.calc.ode": ["ode_sep(x^2,x,y)", "ode_sep(x*y,x,y)"],
    }[name]
    exe = _int_driver(name, tmp_path_factory, sanitize=True, tag="_asan")
    args: list[str] = []
    for free in range(0, 121):
        args.append(f"--free={free}")
        args.extend(exprs)
    results = _run_stats(exe, args)
    assert len(results) == 121 * len(exprs)
    completed = 0
    for (ok, text, leaked, dfree) in results:
        # The pool is a static array, so ASan cannot see a bad free there:
        # the driver counts nodes left in use and frees of free nodes.
        assert leaked == 0, f"{name}: {leaked} nodes leaked ({text!r})"
        assert dfree == 0, f"{name}: {dfree} double frees ({text!r})"
        if ok:
            completed += 1
            assert not _HOLE.search(text), f"{name}: hole in {text!r}"
        else:
            assert text, f"{name}: empty error"
    # With 120 nodes free every case completes; the sweep is not vacuous.
    assert completed >= len(exprs), f"{name}: nothing completed"


def test_numeric_int_dropped_only_where_no_definite_integral_is_answered() -> None:
    """CE_EVAL_ENABLE_NUMERIC_INT=0 (size) makes eval refuse NODE_INTEGRAL.
    Only workers that never answer int(f,x,a,b) may set it: CECINT sends
    every bounded integral to CECIDF, which must keep it."""
    router = (ROOT / "src/modules/calc_int/main.c").read_text()
    assert '"CECIDF"' in router
    dropped = []
    for makefile in _built_module_makefiles():
        cflags = _makefile_vars(makefile).get("PROGRAM_CFLAGS", "")
        if _flag(cflags, "CE_EVAL_ENABLE_NUMERIC_INT") != "0":
            continue
        dropped.append(makefile.name)
        assert _flag(cflags, "CE_PROC_CALC_INT_DEFINITE") != "1", makefile.name
        assert makefile.name != "Makefile.calc.int.def"
    assert set(dropped) == {"Makefile.calc.int.simple", "Makefile.calc.int.pfrac",
                            "Makefile.calc.int.trig", "Makefile.calculus"}, dropped


def test_numeric_lim_tangent_dropped_only_in_the_definite_worker() -> None:
    """CE_EVAL_ENABLE_NUMERIC_LIM_TAN=0 (size, PR #99) makes eval refuse
    NODE_LIMIT and NODE_TANGENT. Only CECIDF may set it: it answers definite
    integrals only, with limits and tangents off in its processor."""
    dropped = []
    for makefile in _built_module_makefiles():
        cflags = _makefile_vars(makefile).get("PROGRAM_CFLAGS", "")
        if _flag(cflags, "CE_EVAL_ENABLE_NUMERIC_LIM_TAN") != "0":
            continue
        dropped.append(makefile.name)
        assert _flag(cflags, "CE_PROC_CALC_LIMIT") == "0", makefile.name
        assert _flag(cflags, "CE_PROC_CALC_TANGENT") == "0", makefile.name
    assert dropped == ["Makefile.calc.int.def"], dropped


# CECIDF links integrate.c only for F(b) - F(a): it shows the value, never the
# antiderivative, and simplify/fold_lite do not fit its 51,200 B budget.
VALUE_ONLY_INTEGRATION_MODULES = {"Makefile.calc.int.def"}


def test_every_built_integration_module_simplifies_or_folds() -> None:
    """Static guard: a built module that links integrate.c must either link
    simplify.c with CE_INTEG_USE_SIMPLIFY on, or fold with fold_lite.c."""
    checked = []
    for makefile in _built_module_makefiles():
        values = _makefile_vars(makefile)
        sources = values.get("PROGRAM_SOURCES", "").split()
        if "src/integrate.c" not in sources:
            continue
        cflags = values.get("PROGRAM_CFLAGS", "")
        checked.append(makefile.name)
        if makefile.name in VALUE_ONLY_INTEGRATION_MODULES:
            # Prints only the definite value: integrate.h turns
            # CE_INTEG_DEFINITE_PRINT_F off when neither simplify nor
            # fold_lite is on, and processor.c's INT_SHOW_ANTIDERIV follows
            # it (which also keeps ast_print out of the module).
            assert _flag(cflags, "CE_INTEG_USE_SIMPLIFY") == "0"
            assert _flag(cflags, "CE_INTEG_USE_FOLD_LITE") in (None, "0")
            assert _flag(cflags, "CE_INTEG_DEFINITE_PRINT_F") is None
            hdr = (ROOT / "src/integrate.h").read_text()
            assert "!(defined(CE_INTEG_USE_FOLD_LITE) && CE_INTEG_USE_FOLD_LITE)" in hdr
            assert "#define CE_INTEG_DEFINITE_PRINT_F 0" in hdr
            proc = (ROOT / "src/split/processor.c").read_text()
            assert "#define INT_SHOW_ANTIDERIV CE_INTEG_DEFINITE_PRINT_F" in proc
            continue
        if _flag(cflags, "CE_INTEG_USE_SIMPLIFY") != "0":
            assert "src/simplify.c" in sources, (
                f"{makefile.name}: CE_INTEG_USE_SIMPLIFY is on but "
                "src/simplify.c is not linked")
            continue
        assert _flag(cflags, "CE_INTEG_USE_FOLD_LITE") == "1", (
            f"{makefile.name}: links integrate.c without simplify or "
            "-DCE_INTEG_USE_FOLD_LITE=1, so int(x^2,x) prints "
            "((X^(2+1))/(2+1))")
        assert "src/fold_lite.c" in sources, (
            f"{makefile.name}: CE_INTEG_USE_FOLD_LITE=1 but src/fold_lite.c "
            "is not linked")
    assert set(INT_PROCESSOR_MODULES + INT_STRATEGY_MODULES) <= set(checked)


FOLD_DRIVER = ROOT / "tests" / "module_parity" / "fold_lite_driver.c"


def test_fold_cancellation_past_the_depth_cap(
        tmp_path_factory: pytest.TempPathFactory) -> None:
    """2*(((x/1)/1).../1) with 200 divisions, under k negations.  For k = 39
    the division chain lies past FOLD_LITE_MAX_DEPTH (40) and was never
    folded; the cancellation must still strip it, iteratively.  For k >= 40
    the product itself is past the cap and stays as it is."""
    cc = _host_cc()
    if cc is None:
        pytest.skip("no host C compiler")
    exe = tmp_path_factory.mktemp("fold") / "fold_driver"
    base = [cc, "-std=c99", "-O0", "-w", "-I", str(ROOT / "src"),
            "-DDESKTOP_BUILD", "-DMEM_POISON_ON_FREE",
            "-DCE_AST_ENABLE_STAT_NAMES=0"]
    srcs = [str(FOLD_DRIVER)] + [str(ROOT / "src" / f) for f in
                                 ("fold_lite.c", "ast.c", "memory.c")]
    proc = subprocess.run(base + ["-fsanitize=address,undefined", "-g"]
                          + srcs + ["-lm", "-o", str(exe)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        proc = subprocess.run(base + srcs + ["-lm", "-o", str(exe)],
                              capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr[-4000:]
    ks = [0, 1, 38, 39, 40, 41, 100]
    run = subprocess.run([str(exe), *map(str, ks)], capture_output=True,
                         text=True, timeout=60,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    assert run.returncode == 0, run.stderr[-4000:]
    rows = [r.split("\t") for r in run.stdout.splitlines()]
    assert [int(r[0]) for r in rows] == ks
    by_k = {int(r[0]): r for r in rows}
    for k in (0, 38):
        assert by_k[k][1] == "(2*x)", by_k[k]
    for k in (1, 39):
        assert by_k[k][1] == "(-(2*x))", by_k[k]
    for k in (40, 41, 100):
        assert "/1)" in by_k[k][1], by_k[k]       # past the cap: unfolded
    for r in rows:
        assert r[2] == "0" and r[3] == "0", f"leak/double free: {r[0]} {r[2:]}"


def test_fold_lite_recursion_is_only_the_depth_capped_walk() -> None:
    """Static guard: only fl_fold may recurse (it carries the depth cap).
    A recursive rule could descend through a subtree past the cap.  Only
    direct self-calls are checked; today's call graph (fl_binop ->
    fl_negate -> fl_unop) has no cycle, so keep it that way."""
    src = (ROOT / "src" / "fold_lite.c").read_text(encoding="utf-8")
    bodies = re.split(r"\n(?=static |ast_node_t \*ast_fold_lite)", src)
    for body in bodies:
        m = re.match(r"static (?:FL_NOINLINE )?[\w ]+?\*?(\w+)\(", body)
        if not m or m.group(1) == "fl_fold":
            continue
        fn = m.group(1)
        calls = len(re.findall(rf"\b{fn}\(", body))
        assert calls == 1, f"fold_lite.c: {fn} calls itself"


# --- CEMAT: eigenval / eigenvec ---------------------------
#
# classify.c put eigenval()/eigenvec() in CE_DOMAIN_MATRIX, but the launcher's
# route_polar_program() sent them to CEBASE, and the only handler in
# processor.c sat inside #ifndef HW_BUILD_SPLIT, so no .8xp had it.  On the
# real OS 5.8 ROM both answered "Polar/advanced op (use at top level)".
# CEMAT now runs them (processor.c handle_eigen) and the launcher routes them
# there.  The expected eigenvalues/eigenvectors were checked against
# numpy.linalg.eig in double precision.

ROUTE_HARNESS = ROOT / "tests" / "harness" / "launcher_route_harness.c"


@pytest.fixture(scope="module")
def cemat(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _build_module_driver(SPLIT / "Makefile.matrix",
                                tmp_path_factory.mktemp("cemat"))


CEMAT_EIGEN_CASES = [
    ("eigenval(mat(2))", "lambda = {2}"),
    ("eigenvec(mat(2))", "v1(lambda=2)=[1]"),
    ("eigenval(mat(2,1;1,2))", "lambda = {3, 1}"),
    ("eigenvec(mat(2,1;1,2))", "v1(lambda=3)=[1,1]\nv2(lambda=1)=[-1,1]"),
    ("eigenval(mat(1,2;3,4))", "lambda = {5.37228, -0.372281}"),
    ("eigenvec(mat(4,1;2,3))", "v1(lambda=5)=[1,1]\nv2(lambda=2)=[-0.5,1]"),
    ("eigenvec(mat(1,0;0,1))", "v1(lambda=1)=[1,0]\nv2(lambda=1)=[0,1]"),
    # The CE keypad has no ';', so mat2(a,b,c,d) is how a 2x2 is typed there.
    ("eigenval(mat2(2,1,1,2))", "lambda = {3, 1}"),
    ("eigenvec(mat2(4,1,2,3))", "v1(lambda=5)=[1,1]\nv2(lambda=2)=[-0.5,1]"),
    ("eigenval(mat(5,0,0;0,3,0;0,0,1))", "lambda = {5, 3, 1}"),
    ("eigenvec(mat(2,0,0;0,3,0;0,0,5))",
     "v1(lambda=5)=[0,0,1]\nv2(lambda=3)=[0,1,0]\nv3(lambda=2)=[1,0,0]"),
    # Double root 3: two independent eigenvectors, not [0,0,1] twice.
    ("eigenval(mat(2,1,0;1,2,0;0,0,3))", "lambda = {3, 3, 1}"),
    ("eigenvec(mat(2,1,0;1,2,0;0,0,3))",
     "v1(lambda=3)=[1,1,0]\nv2(lambda=3)=[0,0,1]\nv3(lambda=1)=[-1,1,0]"),
    ("eigenval(mat(3,0,0;0,3,0;0,0,3))", "lambda = {3, 3, 3}"),
    ("eigenvec(mat(3,0,0;0,3,0;0,0,3))",
     "v1(lambda=3)=[1,0,0]\nv2(lambda=3)=[0,1,0]\nv3(lambda=3)=[0,0,1]"),
    ("eigenval(mat(2,1,0;1,2,1;0,1,2))", "lambda = {3.41421, 2, 0.585786}"),
    ("eigenval(mat(1,2,3;4,5,6;7,8,10))",
     "lambda = {16.7075, 0.198247, -0.905739}"),
    # One real eigenvalue and a complex pair: only the real one is listed.
    ("eigenval(mat(0,-1,0;1,0,0;0,0,2))", "lambda = {2} + complex pair"),
    ("eigenvec(mat(0,-1,0;1,0,0;0,0,2))", "v1(lambda=2)=[0,0,1]\n+ complex pair"),
    # q = 0 but p > 0: 1 and 1 +- 2i, not a triple root.
    ("eigenval(mat(-3,-6,-2;5,4,1;-5,3,2))", "lambda = {1} + complex pair"),
    # Defective (Jordan block): eigenvalue 1 three times, one eigenvector.
    # The missing ones are named, not printed again as if new.
    ("eigenval(mat(1,1,0;0,1,1;0,0,1))", "lambda = {1, 1, 1}"),
    ("eigenvec(mat(1,1,0;0,1,1;0,0,1))",
     "v1(lambda=1)=[1,0,0]\nv2(lambda=1)=same as v1\nv3(lambda=1)=same as v1"),
    ("eigenvec(mat2(1,1,0,1))", "v1(lambda=1)=[1,0]\nv2(lambda=1)=same as v1"),
    # Review counterexamples (PR #117 r1).  Double roots whose float32
    # discriminant came out about -1e-6:
    ("eigenval(mat(23,20,10;15,10,6;10,8,2))", "lambda = {39, -2, -2}"),
    ("eigenval(mat(6,-4,4;-4,0,0;2,-4,4))", "lambda = {10, 0, 0}"),
    ("eigenval(mat(6,0,5;-1,-3,5;-2,-2,2))", "lambda = {2, 2, 1}"),
    ("eigenval(mat(4,2,-4;0,2,4;-5,0,-6))", "lambda = {4, 4, -8}"),
    # Close distinct roots in triangular input are its diagonal, exactly.
    ("eigenval(mat(1,0,0;0,1.001,0;0,0,3))", "lambda = {3, 1.001, 1}"),
    ("eigenval(mat(100,0,0;0,1,0;0,0,1.05))", "lambda = {100, 1.05, 1}"),
    # Review r2: eigenvalues closer than the pivot tolerance got each
    # other's vectors; the smallest-residual candidate is kept now.
    ("eigenvec(mat(1000,0,0;0,1.05,0;0,0,1))",
     "v1(lambda=1000)=[1,0,0]\nv2(lambda=1.05)=[0,1,0]\nv3(lambda=1)=[0,0,1]"),
    # Review r2: exact small coefficients (p = +-5e-5, q = 0) are not a
    # triple root: 0 and +-141.4i, then +-141.4 and 0.
    ("eigenval(mat(0,20000,0;-1,0,0;0,0,0))", "lambda = {0} + complex pair"),
    ("eigenval(mat(0,20000,0;1,0,0;0,0,0))", "lambda = {141.421, 0, -141.421}"),
    # Review r2: tiny matrices used to hit absolute floors.
    ("eigenval(mat(0,1,0;0.0001,0,0;0,0,0))", "lambda = {0.01, 0, -0.01}"),
    # Review r3: tiny and huge eigenvalues are shown with an exponent and 5
    # significant digits (they printed as 0, -0, >4e9 or 0.000123); vector
    # components stay fixed.
    ("eigenval(mat2(1e-8,2e-8,2e-8,1e-8))", "lambda = {3e-8, -1e-8}"),
    ("eigenvec(mat2(1e-8,2e-8,2e-8,1e-8))",
     "v1(lambda=3e-8)=[1,1]\nv2(lambda=-1e-8)=[-1,1]"),
    ("eigenvec(mat(4e30,1e30;2e30,3e30))",
     "v1(lambda=5e30)=[1,1]\nv2(lambda=2e30)=[-0.5,1]"),
    ("eigenval(mat(0.000123456))", "lambda = {1.2346e-4}"),
    ("eigenval(mat(-1234567))", "lambda = {-1.2346e6}"),
    ("eigenval(mat(0.05))", "lambda = {0.05}"),
    ("eigenval(mat(9e30))", "lambda = {9e30}"),
    ("eigenval(mat(6e-30))", "lambda = {6e-30}"),
    # Review r3: exact +-2^-24 after scaling must not become 0.
    ("eigenval(mat2(0,16777216,1/16777216,0))", "lambda = {1, -1}"),
    # Review r4: a singular 3x3 with trace 0 printed its zero
    # eigenvalue as ~9e-8 rounding noise, and the small 2x2 eigenvalue
    # s - sqrt(disc) lost most digits (-2.2054e-6 for -2.24999e-6).
    ("eigenval(mat(1,2,3;4,-5,1;5,-3,4))", "lambda = {6.40312, 0, -6.40312}"),
    ("eigenval(mat(-5,9,-7;-1,-6,18;-6,3,11))", "lambda = {13.3417, 0, -13.3417}"),
    ("eigenval(mat2(1,0.0015,0.0015,0))", "lambda = {1, -2.25e-6}"),
    ("eigenval(mat2(1,0.01,0.01,0))", "lambda = {1.0001, -9.999e-5}"),
    ("eigenval(mat2(1,2,3,6))", "lambda = {7, 0}"),
    ("eigenval(mat2(0.3,0.7,0.9,2.1))", "lambda = {2.4, 0}"),
    # Review r5: tiny but representable input is kept (an absolute
    # 1e-30 floor answered 0), and a resolvable small 2x2 eigenvalue is not
    # snapped to 0 (a 16-ulp determinant snap did).
    ("eigenval(mat(1e-31))", "lambda = {1e-31}"),
    ("eigenval(mat2(1e-31,0,0,2e-31))", "lambda = {2e-31, 1e-31}"),
    ("eigenval(mat2(1,1,-0.990099,-0.9901))", "lambda = {0.010001, -1.0132e-4}"),
    ("eigenval(mat(0,0;0,0))", "lambda = {0, 0}"),
    # Review r6: a singular 3x3 with a zero diagonal (sa = 0).
    ("eigenval(mat(0,0.3,0.9;0.1,0,-0.5;-0.3,-0.5,0))", "lambda = {0.1, 0, -0.1}"),
    # Review r3: a pair 0.21% of max|A - s*I| apart is resolved
    # (EIG_F_TOL 2e-6 merged it to 1.00124 twice, both vectors not found).
    ("eigenval(mat(0.943914,0.318236,-0.024651;0.318236,-0.794477,0.134135;"
     "-0.024651,0.134135,0.992103))", "lambda = {1.00247, 1, -0.86094}"),
    # A - lambda*I came out full rank, so the old code printed [0,0,1].
    ("eigenvec(mat(4,6,-4;4,1,-4;3,-5,-2))",
     "v1(lambda=7.63435)=[1,0.604254,-0.002207]\n"
     "v2(lambda=0.509314)=[0.988787,0.091409,1]\n"
     "v3(lambda=-5.14366)=[0.01786,0.639449,1]"),
]

CEMAT_EIGEN_ERRORS = [
    ("eigenval(mat(1,2))", "Matrix must be square"),
    ("eigenvec(mat(1,2,3;4,5,6))", "Matrix must be square"),
    ("eigenval(mat(0,-1;1,0))", "No real eigenvalues"),
    # 2x2 complex pair close to the real axis is still complex.
    ("eigenval(mat(1,0.001;-0.001,1))", "No real eigenvalues"),
    ("eigenval(mat(1,0,0,0;0,1,0,0;0,0,1,0;0,0,0,1))", "Eigen: up to 3x3 only"),
    # A wrapped matrix is rejected; the old handler silently used the inner
    # literal, so eigenval(inv(A)) answered with the eigenvalues of A.
    ("eigenval(inv(mat(1,2;3,4)))", "Expected mat(...) argument"),
    ("eigenval(5)", "Expected mat(...) argument"),
    ("eigenval(mat(1/0))", "Eval: Division by zero"),
    # The trace of 2e38*I overflowed before scaling (PR #117 r1).
    ("eigenval(mat(2*10^37,0,0;0,2*10^37,0;0,0,2*10^37))",
     "Eigen: entry out of range"),
]


def test_cemat_answers_eigenval_and_eigenvec(cemat: Path) -> None:
    exprs = [e for e, _ in CEMAT_EIGEN_CASES]
    for (expr, want), (ok, text, leaked, dfree) in zip(
            CEMAT_EIGEN_CASES, _run_stats(cemat, exprs)):
        assert ok, f"{expr}: {text}"
        assert text.replace("\\n", "\n") == want, expr
        assert "use at top level" not in text
        assert (leaked, dfree) == (0, 0), expr


def test_cemat_eigen_errors_are_clean(cemat: Path) -> None:
    exprs = [e for e, _ in CEMAT_EIGEN_ERRORS]
    for (expr, want), (ok, text, leaked, dfree) in zip(
            CEMAT_EIGEN_ERRORS, _run_stats(cemat, exprs)):
        assert not ok and text == want, f"{expr}: {text}"
        assert (leaked, dfree) == (0, 0), expr


def _parse_matrix(expr: str) -> list[list[float]]:
    body = re.search(r"mat\((.*)\)\)$", expr).group(1)
    return [[float(x) for x in row.split(",")] for row in body.split(";")]


# Eigenpairs checked in double precision against the input matrix.
EIGEN_RESIDUAL_MATRICES = [
    "mat(4,1;2,3)", "mat(1,2;3,4)", "mat(6,-2;2,1)", "mat(-3,5;5,7)",
    "mat(2,1,0;1,2,1;0,1,2)", "mat(1,2,3;4,5,6;7,8,10)",
    "mat(4,-2,1;-2,4,-2;1,-2,4)", "mat(2,1,0;1,2,0;0,0,3)",
    # A large common diagonal: unshifted float Newton found six "roots".
    "mat(-4000.123,1,2;3,-4000.456,5;6,7,-4000.789)",
    "mat(0.001,0.002,0;0.002,0.001,0;0,0,0.004)",
    "mat(4,6,-4;4,1,-4;3,-5,-2)", "mat(23,20,10;15,10,6;10,8,2)",
    "mat(0.943914,0.318236,-0.024651;0.318236,-0.794477,0.134135;"
    "-0.024651,0.134135,0.992103)",
    "mat(1,2,3;4,-5,1;5,-3,4)", "mat(-5,9,-7;-1,-6,18;-6,3,11)",
    "mat(1,0.0015;0.0015,0)",
    # Review r2: the same matrices at tiny and huge scale.
    "mat(1e-8,2e-8;2e-8,1e-8)", "mat(2e-30,1e-30,0;1e-30,2e-30,0;0,0,3e-30)",
    "mat(4e30,1e30;2e30,3e30)",
]


def test_cemat_eigenpairs_satisfy_av_equals_lambda_v(cemat: Path) -> None:
    exprs = []
    for m in EIGEN_RESIDUAL_MATRICES:
        exprs += [f"eigenval({m})", f"eigenvec({m})"]
    rows = _run(cemat, exprs)
    for k, m in enumerate(EIGEN_RESIDUAL_MATRICES):
        a = _parse_matrix(f"eigenval({m})")
        n = len(a)
        norm = max(abs(x) for row in a for x in row) or 1.0
        ok_val, vals_text = rows[2 * k]
        ok_vec, vecs_text = rows[2 * k + 1]
        assert ok_val and ok_vec, (m, vals_text, vecs_text)
        vals = [float(x) for x in
                re.fullmatch(r"lambda = \{(.*)\}", vals_text).group(1).split(", ")]
        # Every matrix above has n real eigenvalues; trace = sum of them.
        assert len(vals) == n, (m, vals_text)
        trace = sum(a[i][i] for i in range(n))
        assert abs(sum(vals) - trace) <= 1e-4 * norm * n, (m, vals_text)
        lines = vecs_text.split("\\n")
        assert len(lines) == n, (m, vecs_text)
        # Residuals against the centred matrix: a large common diagonal
        # (-4000 below) must not loosen the check.
        cn = max(abs(a[i][j] - (trace / n if i == j else 0))
                 for i in range(n) for j in range(n))
        for line in lines:
            mm = re.fullmatch(r"v\d\(lambda=([^)]*)\)=\[(.*)\]", line)
            assert mm, line
            lam = float(mm.group(1))
            v = [float(x) for x in mm.group(2).split(",")]
            assert len(v) == n and abs(max(abs(x) for x in v) - 1) < 1e-4, line
            for i in range(n):
                av = sum(a[i][j] * v[j] for j in range(n))
                assert abs(av - lam * v[i]) <= 2e-3 * cn + 1e-5 * norm, \
                    (m, line, i)


def _matmul(a: list[list[int]], b: list[list[int]]) -> list[list[int]]:
    return [[sum(a[i][k] * b[k][j] for k in range(len(b)))
             for j in range(len(b[0]))] for i in range(len(a))]


def _conjugated_cases(seed: int = 2026, count: int = 240,
                      bound: int = 30) -> list[tuple[list[list[int]], list[int]]]:
    """Integer matrices P*D*P^-1 with known eigenvalues: D is diagonal or a
    Jordan form (repeated, defective and distinct eigenvalues), P a product
    of integer shears, so P^-1 is an integer matrix too."""
    import random
    rng = random.Random(seed)
    out = []
    while len(out) < count:
        n = rng.choice([2, 3, 3, 3])
        a, b, c = (rng.randint(-6, 6) for _ in range(3))
        if n == 2:
            d, ev = rng.choice([([[a, 0], [0, a]], [a, a]),
                                ([[a, 1], [0, a]], [a, a]),
                                ([[a, 0], [0, b]], [a, b])])
        else:
            d, ev = rng.choice([
                ([[a, 0, 0], [0, a, 0], [0, 0, b]], [a, a, b]),
                ([[a, 1, 0], [0, a, 0], [0, 0, b]], [a, a, b]),
                ([[a, 1, 0], [0, a, 1], [0, 0, a]], [a, a, a]),
                ([[a, 0, 0], [0, b, 0], [0, 0, c]], [a, b, c])])
        p = [[int(i == j) for j in range(n)] for i in range(n)]
        pi = [row[:] for row in p]
        for _ in range(rng.randint(2, 5)):
            i, j = rng.sample(range(n), 2)
            k = rng.choice([-2, -1, 1, 2])
            e = [[int(r == s) for s in range(n)] for r in range(n)]
            ei = [row[:] for row in e]
            e[i][j], ei[i][j] = k, -k
            p, pi = _matmul(p, e), _matmul(ei, pi)
        m = _matmul(_matmul(p, d), pi)
        if max(abs(x) for row in m for x in row) <= bound:
            out.append((m, sorted(ev, reverse=True)))
    return out


def test_cemat_eigenvalues_of_conjugated_integer_matrices(cemat: Path) -> None:
    """Repeated (double, triple, defective) and distinct eigenvalues hidden
    by an integer change of basis: the count and the values must be right.
    (The PR #117 r1 solver split or lost about 1% of such double roots.)"""
    cases = _conjugated_cases()
    exprs = [f"eigenval(mat({';'.join(','.join(map(str, r)) for r in m)}))"
             for m, _ in cases]
    rows = _run(cemat, exprs)
    for (m, ev), expr, (ok, text) in zip(cases, exprs, rows):
        assert ok, (expr, text)
        vals = [float(x) for x in
                re.fullmatch(r"lambda = \{(.*)\}", text).group(1).split(", ")]
        assert len(vals) == len(ev), (expr, text, ev)
        assert max(abs(x - y) for x, y in zip(vals, ev)) <= 1e-3, \
            (expr, text, ev)


def test_cemat_eigen_with_an_exhausted_pool(cemat: Path) -> None:
    """The eigen handler allocates no nodes after the AST arrives, so an
    empty pool must not matter."""
    exprs = ["--free=0", "eigenvec(mat(2,1,0;1,2,0;0,0,3))",
             "eigenval(mat(1,2;3,4))", "eigenval(mat(1,2))"]
    for ok, text, leaked, dfree in _run_stats(cemat, exprs):
        assert (leaked, dfree) == (0, 0), text
        assert ok or text == "Matrix must be square", text


def test_cemat_eigen_under_asan(tmp_path_factory: pytest.TempPathFactory) -> None:
    exe = _build_module_driver(SPLIT / "Makefile.matrix",
                               tmp_path_factory.mktemp("cemat_san"),
                               sanitize=True, tag="_san")
    cases = CEMAT_EIGEN_CASES + CEMAT_EIGEN_ERRORS
    rows = _run(exe, [e for e, _ in cases])
    for (expr, want), (_, text) in zip(cases, rows):
        assert text.replace("\\n", "\n") == want, expr


def _route_harness(tmp_path: Path) -> Path:
    """Build tests/harness/launcher_route_harness.c with split/Makefile.launcher's
    flags."""
    cc = _host_cc()
    if cc is None or sys.platform == "win32":
        pytest.skip("needs a POSIX host C compiler")
    flags = _makefile_vars(SPLIT / "Makefile.launcher")["PROGRAM_CFLAGS"].split()
    exe = tmp_path / "launcher_route_harness"
    cmd = [cc, "-std=gnu99", "-O0", "-w",
           "-I", str(ROOT / "src"),
           "-I", str(ROOT / "tests" / "harness" / "launcher_mock"),
           "-I", str(ROOT / "tests" / "mock"),
           "-DDESKTOP_BUILD", "-DCE_DISABLE_STEP_RECORDING=1", *flags,
           str(ROUTE_HARNESS),
           *[str(ROOT / s) for s in (
               "src/split/runtime_globals.c", "src/split/ipc.c",
               "src/split/astwire.c", "src/split/classify.c",
               "src/memory.c", "src/token.c", "src/ast.c", "src/parser.c",
               "src/split/module_runtime.c", "tests/mock/fileioc_mock.c")],
           "-lm", "-o", str(exe)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr[-4000:]
    return exe


def _route_rows(exe: Path, exprs: list[str]) -> list[list[str]]:
    run = subprocess.run([str(exe), *exprs], capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 0, run.stderr[-4000:]
    rows = [r.split("\t") for r in run.stdout.splitlines()]
    assert len(rows) == len(exprs), run.stdout
    return rows


def test_launcher_routes_eigen_to_cemat(tmp_path: Path) -> None:
    """The launcher, built with split/Makefile.launcher's flags, must send
    eigenval()/eigenvec() to CEMAT, the module that handles them, and agree
    with classify.c's domain."""
    exe = _route_harness(tmp_path)
    exprs = ["eigenval(mat(2))", "eigenvec(mat(2))",
             "eigenval(mat(1,2;3,4))", "eigenvec(mat2(4,1,2,3))", "eigenvec(mat(2,0,0;0,3,0;0,0,5))",
             "det(mat(1,2;3,4))"]
    assert _route_rows(exe, exprs) == [["CEMAT", "CEMAT"]] * len(exprs)


# A scalar store of a plain real value goes to CEBASE, whose
# checked arithmetic (#140) refuses a tiny value before it is stored. CELSCX
# has no such check: `1E-30*1E-30->X` stored 0 there. List stores, and values
# with a list or complex part, stay in CELSCX (known limitation: CELSCX's
# own arithmetic is unchecked).
SCALAR_STORE_ROUTES = [
    ("5->X", "CEBASE"),
    ("1E-30*1E-30->X", "CEBASE"),
    ("X+1->X", "CEBASE"),
    ("2^-30*2^-30*2^-30*2^-30*2^-6->Z", "CEBASE"),
    ("2+sum(k,k,1,3)->Y", "CEBASE"),
    ("5->L1", "CELSCX"),
    ("{1,2,3}->L1", "CELSCX"),
    ("imag(2)+1->X", "CELSCX"),
    ("3+I->X", "CEBASE"),   # complex mode off: I is a variable
    ("real(2)->X", "CELSCX"),
    ("L1(2)+1->X", "CELSCX"),
    ("2*dim(L1)->X", "CELSCX"),
    ("sum(k,k,1,3)->X", "CELSCX"),
    ("int(x,x,0,1)->X", "CELSCX"),
    # Root sampler stores stay in CELSCX (known limitation); split_vars_test
    # pins the same inputs against CEBASE directly, at module level only.
    ("lim(1E-37*x/9,x,1)->X", "CELSCX"),
    ("int(x*1E-37,x,0,1)->X", "CELSCX"),
    # A nested derivative or limit under a BINOP classifies CORE; CEBASE
    # answers it as CELSCX did on 967d5b9 (owned ROM, rom/nested-*).
    ("1+d/dx(X^2)->Y", "CEBASE"),
    ("2*lim(x,x,1)->Y", "CEBASE"),
    # Statistics and polar operands classify CORE, so their stores reach
    # CEBASE, which refuses them as CELSCX did (split_vars_test).
    ("1+nCr(5,2)->X", "CEBASE"),
    ("1+normalcdf(0,1)->X", "CEBASE"),
    ("1+abs(polar(1,2))->X", "CEBASE"),
]


def test_launcher_routes_scalar_real_store_to_cebase(tmp_path: Path) -> None:
    exe = _route_harness(tmp_path)
    rows = _route_rows(exe, [e for e, _ in SCALAR_STORE_ROUTES])
    for (expr, want), row in zip(SCALAR_STORE_ROUTES, rows):
        assert row[0] == want, (expr, row)


# ── Integration recursion bound ─────────────────────────────
# On main these inputs reset the CE (stack-limit NMI at 0xD19881, "RAM
# Cleared"): integ() recursed once per term of a sum with an 113 B frame.
# The linear rules now recurse through a small frame and integ() fails
# cleanly past INTEG_MAX_DEPTH.  The stack margin itself can only be measured
# on the real ROM (stack high-water probe); these tests pin the behaviour.

def _sum(term: str, sep: str, n: int) -> str:
    return sep.join([term] * n)


# (input, value of the antiderivative at x = 1.5 with C = 0)
CECISM_LONG_CASES = [
    ("int(" + _sum("x", "+", 58) + ",x)", 58 * 1.5 ** 2 / 2),
    ("int(" + _sum("x^2", "+", 30) + ",x)", 30 * 1.5 ** 3 / 3),
    ("int(" + _sum("3x^2", "-", 24) + ",x)", (1 - 23) * 1.5 ** 3),
    ("int(x" + "*2" * 59 + ",x)", 2.0 ** 59 * 1.5 ** 2 / 2),
    ("int(x" + "/2" * 59 + ",x)", 2.0 ** -59 * 1.5 ** 2 / 2),
    # 24 nested negations is as deep as the parser goes (PARSER_MAX_DEPTH).
    ("int(" + "-(" * 24 + "x" + ")" * 24 + ",x)", 1.5 ** 2 / 2),
]


def test_cecism_long_linear_inputs_integrate(cecism: Path) -> None:
    """The longest sums and constant-factor chains that fit the launcher's
    127-character input integrate, with no leak or double free.  A long
    antiderivative is cut to the processor's result buffer before " + C" is
    appended, so the value is checked whenever the printed tree is whole."""
    cases = CECISM_LONG_CASES + [
        ("int(" + _sum("x", "+", 6) + ",x)", 6 * 1.5 ** 2 / 2),
    ]
    exprs = [e for e, _ in cases]
    for expr in exprs:
        assert len(expr) <= 127, (len(expr), expr)
    checked = 0
    for (expr, want), (ok, text, leaked, dfree) in zip(
            cases, _run_stats(cecism, exprs)):
        assert ok, f"{expr}: {text}"
        assert text.startswith("int = ") and text.endswith(" + C"), text
        assert leaked == 0 and dfree == 0, (expr, leaked, dfree)
        if text.count("(") == text.count(")"):
            got = _eval_result(text, 1.5)
            assert math.isclose(got, want, rel_tol=1e-4), (expr, text, got, want)
            checked += 1
    assert checked >= 2, "no long result was printed whole"


@pytest.fixture(scope="module")
def capped_workers(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Path]:
    """CECISM and CECODE with INTEG_MAX_DEPTH lowered to 8, so short inputs
    can probe both sides of the cap."""
    return {name: _int_driver(name, tmp_path_factory,
                              extra_cflags=("-DINTEG_MAX_DEPTH=8",),
                              tag="_cap8")
            for name in INT_PROCESSOR_MODULES}


def test_integration_depth_cap_is_a_clean_error(
        capped_workers: dict[str, Path]) -> None:
    deep = "x" + "*2" * 8           # 9 nested integ() calls: one past the cap
    at_cap = "x" + "*2" * 7         # 8 nested calls: at the cap
    rows = _run_stats(capped_workers["Makefile.calc.int.simple"],
                      [f"int({deep},x)", f"int({at_cap},x)",
                       "int(" + "-(" * 9 + "x" + ")" * 9 + ",x)"])
    (ok, text, leaked, dfree) = rows[0]
    # Answered in CECISM: not forwarded to the strategy workers (CEIADVREQ),
    # which would only hit the same cap.
    assert not ok and text == "Expression too deep", text
    assert leaked == 0 and dfree == 0, (leaked, dfree)
    ok, text, leaked, dfree = rows[1]
    assert ok, text
    assert math.isclose(_eval_result(text, 1.5), 2 ** 7 * 1.5 ** 2 / 2,
                        rel_tol=1e-5), text
    assert leaked == 0 and dfree == 0, (leaked, dfree)
    ok, text, leaked, dfree = rows[2]
    assert not ok and text == "Expression too deep", text
    assert leaked == 0 and dfree == 0, (leaked, dfree)

    [(ok, text, leaked, dfree)] = _run_stats(
        capped_workers["Makefile.calc.ode"], [f"ode_sep({deep},x,y)"])
    assert not ok and "Expression too deep" in text, text
    assert leaked == 0 and dfree == 0, (leaked, dfree)


def test_shipped_depth_cap_is_reachable_and_clean(cecism: Path) -> None:
    """With the shipped INTEG_MAX_DEPTH (64), a run of unary minus signs (one
    character per level) in front of a sum reaches the cap inside the
    127-character line.  It must give the depth error from CECISM itself."""
    deep = "int(" + "-" * 48 + "x" + "+x" * 35 + ",x)"      # 84 nested calls
    under = "int(" + "-" * 30 + "x^2" + "+x" * 33 + ",x)"   # 64: at the cap
    assert len(deep) <= 127 and len(under) <= 127
    (ok, text, leaked, dfree), (ok2, text2, leaked2, dfree2) = \
        _run_stats(cecism, [deep, under])
    assert not ok and text == "Expression too deep", text
    assert ok2 and text2.startswith("int = "), text2
    assert (leaked, dfree, leaked2, dfree2) == (0, 0, 0, 0)


@pytest.mark.parametrize("api", [0, 1])
def test_cecode_pool_failure_reason_is_request_local(
        tmp_path: Path, api: int) -> None:
    exe = _build_module_driver(
        SPLIT / "Makefile.calc.ode", tmp_path,
        extra_cflags=(f"-DCE_INTEG_ENABLE_STRATEGY_API={api}",))
    rows = _run_stats(exe, ["--alloc-stats=1", "--free=7",
                            "ode_sep(x^2,x,y)", "--free=-1",
                            "ode_sep(sin(x^2),x,y)", "ode_sep(x^2,x,y)"])
    ok, text, leaked, dfree = rows[0]
    reason, allocs = text.split("|ALLOC|")
    assert not ok and int(allocs) > 0, text
    assert "Out of memory" in reason and "No symbolic" not in reason, reason
    ok, text, _, _ = rows[1]
    assert not ok and text.endswith("|ALLOC|0"), text
    assert "No symbolic antiderivative" in text and "Out of memory" not in text
    assert rows[2] == (True, "y = x^3/3 + C|ALLOC|0", 0, 0)
    assert all(leaked == dfree == 0 for _, _, leaked, dfree in rows)


@pytest.mark.parametrize("op", ["ode_sep", "diffeq"])
@pytest.mark.parametrize("terms", [20, 48, 55, 57])
def test_cecode_repeated_sum_is_complete(
        int_workers: dict[str, Path], op: str, terms: int) -> None:
    expr = f"{op}(" + _sum("x", "+", terms) + ",x,y)"
    assert len(expr) <= 127
    [(ok, text, leaked, dfree)] = _run_stats(
        int_workers["Makefile.calc.ode"], ["--alloc-stats=1", expr])
    assert ok and text == f"y = {terms}(x^2/2) + C|ALLOC|0", text
    assert leaked == dfree == 0


@pytest.mark.parametrize("expr, expected", [
    ("ode_sep(z+z+z,z,y)", "y = 3(z^2/2) + C"),
    ("ode_sep(x+x+1,x,y)", "y = 2(x^2/2)+x + C"),
    ("ode_sep(x-x+x,x,y)", "y = x^2/2 + C"),
    ("ode_sep(x+(x+x),x,y)", "y = 3(x^2/2) + C"),
])
def test_cecode_sum_reduction_preserves_other_shapes(
        int_workers: dict[str, Path], expr: str, expected: str) -> None:
    assert _run_stats(int_workers["Makefile.calc.ode"], [expr]) == [
        (True, expected, 0, 0)]


def test_cecode_sum_reduction_pool_sweep(tmp_path: Path) -> None:
    exe = _build_module_driver(SPLIT / "Makefile.calc.ode", tmp_path,
                               sanitize=True)
    args = []
    for free in range(20):
        args.extend([f"--free={free}", "ode_sep(x+x+x,x,y)"])
    for ok, text, leaked, dfree in _run_stats(exe, args):
        assert leaked == dfree == 0, (text, leaked, dfree)
        if ok:
            assert text == "y = 3(x^2/2) + C", text


def test_cecode_long_sums_answer_cleanly(int_workers: dict[str, Path]) -> None:
    """Repeated x terms reduce before integration. Mixed sums still obey
    INTEG_MAX_DEPTH (48), and y+x+... obeys DIFF_MAX_DEPTH (36)."""
    exe = int_workers["Makefile.calc.ode"]
    exprs = ["diffeq(" + _sum("x", "+", 20) + ",x,y)",
             "diffeq(" + _sum("x", "+", 54) + "+1,x,y)",
             "diffeq(y+" + _sum("x", "+", 57) + ",x,y)",
             "diffeq(" + _sum("x", "*", 55) + ",x,y)"]
    for expr in exprs:
        assert len(expr) <= 127, (len(expr), expr)
    rows = _run_stats(exe, exprs)
    ok, text, _, _ = rows[0]
    assert ok and text.startswith("y = ") and text.endswith(" + C"), text
    ok, text, _, _ = rows[1]
    assert not ok and "(Expression too deep)" in text, text
    ok, text, _, _ = rows[2]
    assert not ok and text.startswith("Not linear in y"), text
    ok, text, _, _ = rows[3]
    assert not ok and text.startswith("Linear ODE failed: could not integrate"), text
    for expr, (ok, text, leaked, dfree) in zip(exprs, rows):
        assert "CEIADVREQ" not in text, (expr, text)
        assert leaked == 0 and dfree == 0, (expr, leaked, dfree)


def test_integration_recursion_is_bounded() -> None:
    """Static guard for the stack layout the real-ROM margins depend on."""
    src = (ROOT / "src" / "integrate.c").read_text(encoding="utf-8")
    cap = re.search(r"#define INTEG_MAX_DEPTH (\d+)", src)
    assert cap, "integrate.c must define INTEG_MAX_DEPTH"
    # x*2*...*2 or x+x+...+x in 127 characters nests at most 61 integ()
    # calls; the default cap must not reject them.
    assert int(cap.group(1)) >= 61, cap.group(0)
    noinline = r"__attribute__\(\(noinline\)\)\s*\n#endif\s*\n"
    # The large rule frame stays out of the recursive spine.
    assert re.search(noinline + r"static ast_node_t \*integ_rules\(", src), \
        "integ_rules() must stay noinline"
    # Step text: one out-of-line printer with a static buffer and a depth
    # probe, so no print buffer sits in a recursive frame and no step print
    # walks a deep tree.
    m = re.search(noinline + r"static const char \*integ_text\(.*?\n}\n",
                  src, re.S)
    assert m, "integ_text() must stay noinline"
    assert "static char buf[120];" in m.group(0)
    assert 'return "...";' in m.group(0)
    for fn in ("integ_const", "integ_node", "integ_power_rule", "integ_func_linear", "try_exp_rule"):
        body = _c_function_body(src, f"static ast_node_t *{fn}(")
        assert not re.search(r"\bchar \w+\[", body), f"{fn} has a local buffer"
        assert "ast_to_str(" not in body, f"{fn} prints without integ_text()"
    # integ() is the only recursive entry: integ_node()/integ_rules() never
    # call themselves.
    for fn in ("integ_node", "integ_rules"):
        body = _c_function_body(src, f"static ast_node_t *{fn}(")
        assert f"{fn}(" not in body, f"{fn} calls itself"
    values = _makefile_vars(SPLIT / "Makefile.calc.ode")
    diff_cap = _flag(values["PROGRAM_CFLAGS"], "DIFF_MAX_DEPTH")
    assert diff_cap is not None and int(diff_cap) <= 36, diff_cap
    integ_cap = _flag(values["PROGRAM_CFLAGS"], "INTEG_MAX_DEPTH")
    assert integ_cap is not None and int(integ_cap) <= 48, integ_cap


@pytest.fixture(scope="module")
def cecseq(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _build_module_driver(SPLIT / "Makefile.calc.seq",
                                tmp_path_factory.mktemp("cecseq"))


# CECSEQ answers sum(f,n,a,b).  Only an inf upper bound is a
# series; on main every upper bound ran the convergence test, so
# sum(n,n,1,10) printed "Convergence: diverges (nth-term)" and
# sum(1/n^2,n,1,10) printed the infinite sum.  The CE types upper case.
CECSEQ_FINITE_CASES = [
    ("sum(n,n,1,10)", "= 55"),
    ("SUM(N,N,1,10)", "= 55"),
    ("sum(1/n^2,n,1,10)", "= 1.54977"),
    ("SUM(1/N^2,N,1,10)", "= 1.54977"),
    ("sum(k^2,k,1,4)", "= 30"),
    ("sum(k,k,5,1)", "= 0"),
    ("sum(1,k,8388607,8388607)", "= 1"),
    ("sum(1,k,8388607,-8388607)", "= 0"),
    ("sum(2^k,k,0,10)", "= 2047"),
]
CECSEQ_FAIL_CASES = [
    ("sum(1/(n-3),n,1,5)", "Division by zero"),
    ("sum(n,n,1,20000)", "Sum too many terms"),
    ("sum(10^38,k,1,10)", "Sum overflow"),
    ("SUM(K^2,K,1,N)", "N has no value"),
]


def test_cecseq_finite_sum_is_the_partial_sum(cecseq: Path) -> None:
    assert _flag(_makefile_vars(SPLIT / "Makefile.calc.seq")["PROGRAM_CFLAGS"],
                 "CE_SAFE_FLOAT") == "1", "finite sums must not use wrapping CE ops"
    exprs = [e for e, _ in CECSEQ_FINITE_CASES + CECSEQ_FAIL_CASES] + [
        "sum(1/n^2,n,1,inf)", "SUM(1/N^2,N,1,INF)", "sum(n,n,1,inf)"]
    rows = _run_stats(cecseq, exprs)
    n_ok = len(CECSEQ_FINITE_CASES)
    for (expr, want), (ok, text, _, _) in zip(CECSEQ_FINITE_CASES, rows):
        assert ok and text == want, (expr, ok, text)
    for (expr, want), (ok, text, _, _) in zip(CECSEQ_FAIL_CASES, rows[n_ok:]):
        assert not ok and want in text, (expr, ok, text)
    # The infinite series keeps its convergence answer and sum estimate.
    for ok, text, _, _ in rows[-3:-1]:
        assert ok and text.startswith("Convergence: converges"), text
        assert "Sum ~= 1.6" in text, text
    assert rows[-1][0] and rows[-1][1].startswith("Convergence: diverges"), rows[-1]
    for expr, (_, _, leaked, dfree) in zip(exprs, rows):
        assert leaked in (0, None) and dfree in (0, None), (expr, leaked, dfree)


# ── Definite-integral stack bound ───────────────────────────
# CECIDF (#99) evaluates the antiderivative at both bounds and walks it for
# the FTC rounding guard, integ_err_scale().  That walk recursed once per
# node, about 125 B of CE stack per level, so int(x+x+...,x,0,1) with 20
# terms reset the calculator (stack-limit NMI); since #125's eval backstop
# the same inputs refuse instead.  The walk is now iterative, so they answer.
# The margins are measured on the real ROM (test_realrom_worker_stack.py);
# these tests pin the layout, the answers and the pool accounting with
# module flags.

def test_definite_error_walk_is_iterative() -> None:
    src = (ROOT / "src" / "integrate.c").read_text(encoding="utf-8")
    walk = _c_function_body(src, "static ce_float_t integ_err_scale(")
    node = _c_function_body(src, "static CE_INTEG_NOINLINE ce_float_t integ_err_node(")
    # The walk keeps its open nodes in a static array, never on the stack,
    # and neither function calls the walk or itself.
    assert re.search(r"static struct \w+ \{[^}]*\} s_open\[INTEG_ERR_MAX_DEPTH\];", walk), walk
    assert "integ_err_scale(" not in walk and "integ_err_node(" not in node
    assert "#define INTEG_ERR_MAX_DEPTH CE_EVAL_MAX_DEPTH" in src


@pytest.fixture(scope="module")
def cecidf(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _build_module_driver(SPLIT / "Makefile.calc.int.def",
                                tmp_path_factory.mktemp("cecidf_depth"),
                                sanitize=True, tag="_asan")


def test_cecidf_long_definite_inputs_answer_or_refuse(cecidf: Path) -> None:
    """Long sums keep their FTC value; inputs deeper than the eval cap
    refuse with a reason, never a wrong value, a leak or a double free.
    The host has no stack backstop, so this pins values and pool
    accounting, not stack use."""
    import math

    def terms(t: str, n: int, op: str = "+") -> str:
        return f"int({op.join([t] * n)},x,0,1)"
    cases = [  # (input, exact value or None when it must refuse)
        (terms("x", 13), 6.5), (terms("x", 20), 10.0), (terms("x", 30), 15.0),
        (terms("x", 45), 22.5), (terms("x^2", 12), 4.0),
        (terms("sin(x)", 13), 13 * (1 - math.cos(1))),
        ("int(" + "-" * 14 + "x,x,0,1)", 0.5), ("int(" + "-" * 15 + "x,x,0,1)", -0.5),
        ("int(x" + "*2" * 20 + ",x,0,1)", 2.0 ** 19),
        # Deeper than CE_EVAL_MAX_DEPTH (50): refused.
        (terms("x", 58), None), ("int(x" + "*2" * 57 + ",x,0,1)", None),
        (terms("x", 58, "*"), None),
        # The #99 precision guard still refuses the cancellation case.
        ("int(x-100000,x,100000,100001)", None),
    ]
    # "Eval recursion limit" alone is #128's reason for any refused eval.
    refusals = ("Could not decide integral", "Eval recursion limit",
                "Expression too deep")
    rows = _run_stats(cecidf, [expr for expr, _ in cases])
    for (expr, want), (ok, text, leaked, dfree) in zip(cases, rows):
        assert leaked == 0 and dfree == 0, (expr, leaked, dfree)
        if want is None:
            assert not ok and any(r in text for r in refusals), (expr, text)
            continue
        # The whole first line is "= <value>": no prefix match ("= 7" vs
        # "= 70"), and no numeric fallback for these FTC cases.
        line = text.split("\\n")[0].split()
        assert ok and len(line) == 2 and line[0] == "=", (expr, text)
        assert float(line[1]) == pytest.approx(want, rel=2e-6), (expr, text)

def _c_function_body(src: str, head: str) -> str:
    """Body of the C function whose definition starts with head (the
    definition, not a prototype ending in ';'), up to its closing brace at
    column 0.  Fails if the definition is not found."""
    for m in re.finditer(re.escape(head), src):
        rest = src[m.start():]
        sig_end = rest.index(")")
        after = rest[sig_end + 1:].lstrip()
        if after.startswith("{"):
            body = rest[sig_end + 1:]
            return body.split("\n}\n", 1)[0]
    raise AssertionError(f"definition of {head!r} not found")
