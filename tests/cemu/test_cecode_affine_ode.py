"""Exercise the bounded affine integrating-factor rule with CECODE flags."""
import pytest

from test_module_flag_parity import (
    SPLIT, _build_module_driver, _eval_result, _flag, _makefile_vars, _run_stats,
)


@pytest.fixture(scope="module")
def cecode(tmp_path_factory):
    return _build_module_driver(SPLIT / "Makefile.calc.ode",
                                tmp_path_factory.mktemp("affine_ode"),
                                sanitize=True)


def test_cecode_keeps_expensive_strategies_disabled():
    flags = _makefile_vars(SPLIT / "Makefile.calc.ode")["PROGRAM_CFLAGS"]
    for name in ("PARTS", "NONLINEAR_USUB", "CYCLIC_PARTS"):
        assert _flag(flags, f"CE_INTEG_ENABLE_{name}") == "0"
    assert _flag(flags, "INTEG_MAX_DEPTH") == "48"
    assert _flag(flags, "DIFF_MAX_DEPTH") == "36"


@pytest.mark.parametrize("rhs,p,a,b", [
    ("x-y", 1, 1, 0),
    ("3x+2-2y", 2, 3, 2),
    ("x+3+y", -1, 1, 3),
    ("x-3-2y", 2, 1, -3),
    ("x-2x-2y", 2, -1, 0),
    ("x-y+y-y", 1, 1, 0),
    ("x-y+x-x", 1, 1, 0),
])
def test_cecode_affine_solution_satisfies_ode(cecode, rhs, p, a, b):
    [(ok, text, leaked, dfree)] = _run_stats(cecode, [f"diffeq({rhs},x,y)"])
    assert ok and text.startswith("y = "), text
    assert leaked == dfree == 0
    # Check the actual displayed solution, including its arbitrary constant.
    for constant in (-2, 0, 3):
        body = text.removeprefix("y = ").replace("C", str(constant))
        for x in (-0.8, 0.2, 1.1):
            def value(t):
                return _eval_result("int = " + body, t)
            h = 1e-5
            derivative = (value(x + h) - value(x - h)) / (2 * h)
            assert derivative == pytest.approx(a * x + b - p * value(x),
                                                rel=2e-5, abs=2e-5), text


def test_cecode_affine_pool_sweep_is_complete(cecode):
    args = []
    for free in range(121):
        args.extend([f"--free={free}", "diffeq(x-y,x,y)"])
    rows = _run_stats(cecode, args)
    assert any(ok for ok, _, _, _ in rows)
    assert any(not ok for ok, _, _, _ in rows)
    for ok, text, leaked, dfree in rows:
        assert leaked == dfree == 0, (ok, text, leaked, dfree)
        if ok:
            assert text == "y = (exp(x)*(x-1) + C)/(exp(x))"
        else:
            assert "failed" in text or "Not linear" in text, text


def test_cecode_affine_fallback_preserves_refusals_and_controls(cecode):
    exprs = ["ode_linear(x^2-y,x,y)", "ode_linear(sin(x)-y,x,y)",
             "ode_linear(x*y+x,x,y)", "diffeq(x^2,x,y)",
             "diffeq(x*y,x,y)", "ode_linear(x-x-y,x,y)",
             "ode_linear(x,x,y)", "ode_linear(z-y,z,y)"]
    rows = _run_stats(cecode, exprs)
    assert all(not ok and "could not integrate mu*Q" in text
               for ok, text, _, _ in rows[:3]), rows[:3]
    assert rows[3] == (True, "y = x^3/3 + C", 0, 0)
    assert rows[4] == (True, "ln(abs(y)) = x^2/2 + C", 0, 0)
    assert rows[5] == (True, "y = (0 + C)/(exp(x))", 0, 0)
    assert rows[6] == (True, "y = (exp(0)*(x^2/2) + C)/(exp(0))", 0, 0)
    assert rows[7] == (True, "y = (exp(z)*(z-1) + C)/(exp(z))", 0, 0)
    assert all(leaked == dfree == 0 for _, _, leaked, dfree in rows)
