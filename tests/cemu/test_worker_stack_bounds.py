"""CE worker recursion regressions, using real module flags on the host.

Host stack size is not CE stack evidence: the opt-in real-ROM companion test
paints and scans the actual emulated OS stack. Here we pin results, refusal,
allocation cleanup, and that a depth error cannot poison the next request.
"""
from pathlib import Path
import subprocess

import pytest

import test_module_flag_parity as parity


def longest_chain(template, op):
    n = 1
    while len(template(op.join(["x"] * (n + 1)))) <= 127:
        n += 1
    return template(op.join(["x"] * n))


@pytest.fixture(scope="module")
def workers(tmp_path_factory):
    out = tmp_path_factory.mktemp("stack_workers")
    built = {}
    for name in ("algsolve", "algsolve.adv", "calc.lim.base", "calc.lim.adv", "calc.lim.cont", "calc.lim.rel", "base"):
        makefile = parity.SPLIT / f"Makefile.{name}"
        values = parity._makefile_vars(makefile)
        # These modules have custom TI-only entry points. The processor driver
        # replaces just the entry point, not the processor or math sources.
        entries = {s for s in values["PROGRAM_SOURCES"].split()
                   if s.startswith("src/modules/") and s.endswith("main.c")}
        if name != "base":
            assert parity._flag(values["PROGRAM_CFLAGS"], "CE_EVAL_MAX_DEPTH") == "12"
            assert parity._flag(values["PROGRAM_CFLAGS"], "CE_POLY_MAX_DEPTH") == "8"
        with pytest.MonkeyPatch.context() as mp:
            mp.setattr(parity, "_HOST_EXCLUDED_SOURCES",
                       parity._HOST_EXCLUDED_SOURCES | entries)
            built[name] = parity._build_module_driver(makefile, out)
    return built


def checked(exe: Path, exprs):
    rows = parity._run_stats(exe, exprs)
    for expr, (_, _, leaked, double_free) in zip(exprs, rows):
        assert leaked == double_free == 0, (expr, leaked, double_free)
    return [(ok, text) for ok, text, _, _ in rows]


def test_long_solve_sums_keep_the_answer(workers):
    expressions = ["solve(" + "+".join(["x"] * 55) + "-1,x)",
                   longest_chain(lambda s: f"solve({s}-1,x)", "+"),
                   longest_chain(lambda s: f"solve({s}-1,x)", "-")]
    for name in ("algsolve", "algsolve.adv"):
        rows = checked(workers[name], expressions)
        for expr, (ok, text) in zip(expressions, rows):
            assert ok, (name, expr, text)
            value = float(text.split(" = ")[1].split("\\n")[0])
            coefficient = expr.count("x") - 1  # exclude the argument variable
            if "-x" in expr:
                coefficient = 2 - coefficient
            assert value == pytest.approx(1 / coefficient, rel=2e-5)


def test_cebase_ordinary_arithmetic_retains_its_depth(workers):
    expression = "+".join(str(i) for i in range(1, 14))
    row = checked(workers["base"], [expression])[0]
    assert row[0] and row[1].split("\\n")[0] == "= 91", row


def test_order_signs_and_polynomial_power(workers):
    expressions = ["solve(x-(2x-3)-5,x)", "solve(10000000-10000000+1+x,x)",
                   "solve(x^2-5x+6,x)", "solve(abs(x)-2,x)"]
    for name in ("algsolve", "algsolve.adv"):
        rows = checked(workers[name], expressions)
        assert rows[0][0] and rows[0][1].startswith("x = -2"), rows
        assert rows[1][0] and rows[1][1].startswith("x = -1"), rows
        assert rows[2][0] and "x = 3 or x = 2" in rows[2][1], rows
    assert checked(workers["algsolve"], [expressions[-1]])[0][0]


def test_depth_refusal_is_not_no_solution_and_resets(workers):
    tree = "x*(" * 20 + "x" + ")" * 20
    deep = f"solve({tree}-1,x)"
    abs_deep = f"solve(abs({tree})-1,x)"
    for name in ("algsolve", "algsolve.adv"):
        rows = checked(workers[name], [deep, "solve(x+2,x)", deep, "solve(x-3,x)"])
        assert not rows[0][0] and "too deep" in rows[0][1], rows
        assert rows[1][0] and rows[1][1].startswith("x = -2"), rows
        assert not rows[2][0] and "too deep" in rows[2][1], rows
        assert rows[3][0] and rows[3][1].startswith("x = 3"), rows
    row = checked(workers["algsolve.adv"], [abs_deep])[0]
    assert not row[0] and "too deep" in row[1], row


def test_limits_base_fails_safely_and_advanced_cancels(workers):
    expressions = ["lim((" + "+".join(["x"] * 55) + ")/x,x,0)",
                   longest_chain(lambda s: f"lim(({s})/x,x,0)", "+")]
    base = checked(workers["calc.lim.base"], expressions)
    assert all(not ok and "CELADVREQ" in text for ok, text in base), base
    advanced = checked(workers["calc.lim.adv"], expressions)
    assert all(ok and text.startswith("= " + str(expr.count("x") - 2))
               for expr, (ok, text) in zip(expressions, advanced)), advanced
    deep = "lim(" + "sin(" * 21 + "x" + ")" * 21 + ",x,0)"
    rows = checked(workers["calc.lim.adv"], [deep, "lim(x^2,x,2)"])
    assert not rows[0][0] and "Could not evaluate limit" in rows[0][1], rows
    assert rows[1][0] and rows[1][1].startswith("= 4"), rows


@pytest.mark.parametrize("source", ["src/algebra.c", "src/split/algebra_poly.c"])
def test_both_converters_clear_trimmed_coefficients_and_keep_degree8(source, tmp_path):
    cc = parity._host_cc()
    if not cc:
        pytest.skip("no host compiler")
    exe = tmp_path / "poly"
    sources = ["tests/module_parity/poly_bounds_driver.c", "src/memory.c",
               "src/token.c", "src/parser.c", "src/ast.c", "src/steps.c",
               "src/split/runtime_globals.c", source]
    proc = subprocess.run([cc, "-std=c99", "-O0", "-w", "-DDESKTOP_BUILD",
                           "-DCE_POLY_MAX_DEPTH=8", "-I", str(parity.ROOT / "src"),
                           "-I", str(parity.ROOT / "tests/mock"),
                           *[str(parity.ROOT / s) for s in sources],
                           *parity._gc_flags(), "-lm", "-o", str(exe)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    degree8 = "*".join(f"(x-{i})" for i in range(1, 9))
    proc = subprocess.run([str(exe),
                           ".000000005*x+1-.000000005*x-.00000001*x",
                           "1-.00000001*x", degree8], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    rows = proc.stdout.splitlines()
    assert rows[0] == rows[1] and rows[0].startswith("1 1 -"), rows
    coefficients = [float(v) for v in rows[2].split()[1:]]
    assert rows[2].startswith("8 ") and coefficients[-1] == 1, rows
    for x in range(1, 9):
        assert sum(c * x**i for i, c in enumerate(coefficients)) == pytest.approx(0, abs=1e-6)


def test_polynomial_depth_refusal_does_not_enter_heuristic_newton(workers):
    expressions = [
        "solve(x-(1-(2-(3-(4-(5-(6-(7-x))))))),x)",
        "solve(1+x*(2+x*(3+x*(4+x*(5+x)))),x)",
        "solve(x-(1-(2-(3-(4-(5-(6-(7-(1e38*x)))))))),x)",
        "solve(" + "0+(" * 7 + "1e-6*x-0.001" + ")" * 7 + ",x)",
        "solve(" + "0+(" * 7 + "1e-10*x-1e-9" + ")" * 7 + ",x)",
        "solve(" + "0+(" * 7 + "1e-6*(x-0.0001)^2+1e-18" + ")" * 7 + ",x)",
        "solve(" + "0+(" * 7 + "1e-10*(x-1000)/(x-0.000010000001)" + ")" * 7 + ",x)",
    ]
    for name in ("algsolve", "algsolve.adv"):
        rows = checked(workers[name], expressions + ["solve(x+2,x)"])
        assert all(not ok and text == "Expression too deep for solve" for ok, text in rows[:-1]), rows
        assert rows[-1][0] and rows[-1][1].startswith("x = -2"), rows


def test_plain_sum_limit_split_and_continuity_depth_refusal(workers):
    row = checked(workers["calc.lim.adv"], ["lim(" + "+".join(["x"] * 20) + ",x,0)"])[0]
    assert row[0] and row[1].startswith("= 0"), row
    expr = "continuity(" + "+".join(["x"] * 13) + ",x,0)"
    row = checked(workers["calc.lim.cont"], [expr])[0]
    assert "Removable" not in row[1] and "undefined" not in row[1], row
    assert row[1] == "Continuity: could not determine", row


def test_numeric_fallback_still_solves_shallow_nonpolynomials(workers):
    row = checked(workers["algsolve.adv"], ["solve(cos(x)-x,x)"])[0]
    assert row[0] and "0.739085" in row[1], row
