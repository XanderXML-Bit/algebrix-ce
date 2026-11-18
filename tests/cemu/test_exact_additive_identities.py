"""Typed tiny constants must survive the simplifier's ADD/SUB identities.

Before an internal review the identities c + x -> x, x + c -> x, x - c -> x and
c - x -> -x used ast_is_num(n, 0), which accepts |c| < 1e-10.  The threshold
is absolute, so it dropped a constant the user typed whenever x is
comparable to it: CECALC answered d/dx(ln(x+5e-11)) with 1/x, which is 2x
too large at x = 5e-11 and undefined at x = 0, where the function is
defined.  CECODE's integrating factor for y' = y/(x+5e-11) lost the shift.

The oracle evaluates the displayed answer exactly in Decimal arithmetic and
compares it with a Decimal central difference of the input, at points where
x is comparable to the constant (including x = 0 and x = c).  A float check
at ordinary x such as x = 1 cannot see the bug, because there the broken
answer is within about 1e-10 relative of the true one.

The drivers use the real split Makefile flags with ASan/UBSan.  This is
host evidence for the listed literal inputs, not a hardware claim.
"""
import re
from decimal import Decimal, localcontext

import pytest

from test_module_flag_parity import SPLIT, _build_module_driver, _run_stats


@pytest.fixture(scope="module")
def cecalc(tmp_path_factory):
    return _build_module_driver(SPLIT / "Makefile.calculus",
                                tmp_path_factory.mktemp("exact_add_calc"),
                                sanitize=True)


@pytest.fixture(scope="module")
def cecode(tmp_path_factory):
    return _build_module_driver(SPLIT / "Makefile.calc.ode",
                                tmp_path_factory.mktemp("exact_add_ode"),
                                sanitize=True)


_TOKEN = re.compile(
    r"(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[A-Za-z_]\w*|\S")
_FUNCS = {"exp", "ln", "sqrt", "abs"}


def _decimal_value(body: str, x: Decimal) -> Decimal:
    """Evaluate displayed (or input) text exactly; literals become Decimal.

    Raises ZeroDivisionError / decimal.InvalidOperation where the text is
    undefined, which the oracle treats as a wrong answer when the true
    derivative is defined there."""
    parts: list[str] = []
    previous_operand = False
    for token in _TOKEN.findall(body):
        starts_operand = token[0].isalnum() or token[0] in "._("
        if previous_operand and starts_operand:
            parts.append("*")  # implicit product: 2x, 5e-11x, 2sqrt(x)
        if token[0].isdigit() or token[0] == ".":
            parts.append(f"D({token!r})")
        elif token == "^":
            parts.append("**")
        else:
            assert token in {"x", "+", "-", "*", "/", "(", ")"} | _FUNCS, \
                (token, body)
            parts.append(token)
        # A function name is followed by its call, not an implicit product.
        previous_operand = token not in _FUNCS and (token[0].isalnum() or
                                                    token in ".)")
    names = {"D": Decimal, "x": x, "exp": lambda v: v.exp(),
             "ln": lambda v: v.ln(), "sqrt": lambda v: v.sqrt(),
             "abs": abs}
    return eval("".join(parts), {"__builtins__": {}}, names)  # noqa: S307


def _central(fn, x: Decimal, scale: Decimal) -> Decimal:
    h = scale * Decimal("1e-60")
    return (fn(x + h) - fn(x - h)) / (2 * h)


def _derivative_check(func: str, shown: str, points, scale: Decimal) -> None:
    """shown(x) must equal f'(x) to 1e-30 relative at every point; `scale`
    is the size of the constant, so h stays far below it."""
    with localcontext() as ctx:
        ctx.prec = 200
        for x in points:
            true = _central(lambda t: _decimal_value(func, t), x, scale)
            got = _decimal_value(shown, x)
            assert abs(got - true) <= Decimal("1e-30") * max(abs(true),
                                                            Decimal("1e-300")), \
                (func, shown, x, got, true)


# (input, CECALC answer after "d/dx = ", function, points, constant scale)
C5 = Decimal("5e-11")
DERIVS = [
    ("d/dx(ln(x+5e-11))", "1/(x+5e-11)", "ln(x+5e-11)",
     [Decimal(0), C5, -C5 / 2, 2 * C5, Decimal(1)], C5),
    ("d/dx(ln(x+9e-11))", "1/(x+9e-11)", "ln(x+9e-11)",
     [Decimal(0), Decimal("9e-11"), Decimal(1)], Decimal("9e-11")),
    ("d/dx(1/(x+5e-11))", "-1/(x+5e-11)^2", "1/(x+5e-11)",
     [Decimal(0), C5, -C5 / 2, Decimal(1)], C5),
    ("d/dx(sqrt(x+1e-30))", "1/(2sqrt(x+1e-30))", "sqrt(x+1e-30)",
     [Decimal(0), Decimal("1e-30"), Decimal(1)], Decimal("1e-30")),
    ("d/dx(x^2+5e-11x)", "2x+5e-11", "x^2+5e-11x",
     [Decimal(0), -C5 / 2, C5, Decimal(1)], C5),
    ("d/dx((x+1e-30)^2)", "2x+2e-30", "(x+1e-30)^2",
     [Decimal(0), Decimal("-1e-30"), Decimal(1)], Decimal("1e-30")),
    ("d/dx((x-1e-30)^2)", "2x-2e-30", "(x-1e-30)^2",
     [Decimal(0), Decimal("1e-30"), Decimal(1)], Decimal("1e-30")),
    ("d/dx(ln(x-5e-11))", "1/(x-5e-11)", "ln(x-5e-11)",
     [2 * C5, Decimal("1.5") * C5, Decimal(1)], C5),
    ("d/dx(e^(x+1e-11))", "e^(x+1e-11)", "exp(x+1e-11)",
     [Decimal(0), Decimal(1)], Decimal("1e-11")),
    # Tiny constant on the left: the ADD-left and SUB-left identity sites.
    ("d/dx(ln(5e-11+x))", "1/(5e-11+x)", "ln(5e-11+x)",
     [Decimal(0), C5, -C5 / 2, Decimal(1)], C5),
    ("d/dx(ln(5e-11-x))", "-1/(5e-11-x)", "ln(5e-11-x)",
     [Decimal(0), -C5, C5 / 2, Decimal(-1)], C5),
]


@pytest.mark.parametrize("expr,answer,func,points,scale", DERIVS,
                         ids=[row[0] for row in DERIVS])
def test_cecalc_keeps_tiny_additive_constant(cecalc, expr, answer, func,
                                             points, scale):
    [(ok, text, leaked, dfree)] = _run_stats(cecalc, [expr])
    assert ok and leaked == dfree == 0, text
    assert text == f"d/dx = {answer}", text
    # e^ in the display is exp(); the oracle evaluates it exactly.
    shown = text.removeprefix("d/dx = ").replace("e^(", "exp(")
    _derivative_check(func, shown, points, scale)


def test_derivative_oracle_rejects_dropped_constant():
    """main's answers pass at x = 1 to float precision but fail where x is
    comparable to the constant, and are undefined at x = 0."""
    with localcontext() as ctx:
        ctx.prec = 200
        x = Decimal(1)
        true = _central(lambda t: _decimal_value("ln(x+5e-11)", t), x, C5)
        assert abs(_decimal_value("1/x", x) - true) <= Decimal("1e-9") * true
    for func, broken, point in [
            ("ln(x+5e-11)", "1/x", C5),
            ("ln(x+5e-11)", "1/x", -C5 / 2),
            ("1/(x+5e-11)", "-1/x^2", C5),
            ("x^2+5e-11x", "2x", Decimal(0)),
            ("ln(x-5e-11)", "1/x", 2 * C5)]:
        with pytest.raises(AssertionError):
            _derivative_check(func, broken, [point], C5)
    for func, broken in [("ln(x+5e-11)", "1/x"),
                         ("sqrt(x+1e-30)", "1/(2sqrt(x))"),
                         ("ln(5e-11-x)", "-1/(-x)")]:
        with pytest.raises(ZeroDivisionError):
            _derivative_check(func, broken, [Decimal(0)], Decimal("1e-30"))


def test_cecalc_exact_zero_and_ordinary_controls(cecalc):
    rows = _run_stats(cecalc, [
        "d/dx(x+0)", "d/dx(0+x^2)", "d/dx(x^2-0)", "d/dx(0-x^2)",
        "d/dx(ln(x+1))", "d/dx(x+1e-30)", "d/dx(1e-30+x^2)",
        "d/dx(x^2*(0.1*0.1-0.01)+x)",
    ])
    assert rows == [
        (True, "d/dx = 1", 0, 0),
        (True, "d/dx = 2x", 0, 0),
        (True, "d/dx = 2x", 0, 0),
        (True, "d/dx = -(2x)", 0, 0),
        (True, "d/dx = 1/(x+1)", 0, 0),
        (True, "d/dx = 1", 0, 0),
        (True, "d/dx = 2x", 0, 0),
        # An ordinary float32 folding residue is ~1e-9: above the old
        # 1e-10 threshold, so it was never hidden.  Unchanged.
        (True, "d/dx = 1.86265e-09x+1", 0, 0),
    ], rows


def test_cecalc_records_tiny_float_residue(cecalc):
    """Recorded behaviour, not a goal: folding tiny typed constants can
    leave a float32 residue below 1e-10, which the exact identity now keeps
    (main dropped it).  1e-5*1e-5 - 1e-10 is about -6.9e-18 in float32;
    the absolute error is negligible and the manual says so."""
    rows = _run_stats(cecalc, ["d/dx(x^2+(1e-5*1e-5-1e-10)x)",
                               "d/dx(x^2+(1e-6*1e-6-1e-12)x)"])
    assert rows == [
        (True, "d/dx = 2x+(-6.93889e-18)", 0, 0),
        (True, "d/dx = 2x", 0, 0),
    ], rows


def _ode_residual(text: str, coeff, forcing, points, tol: str) -> None:
    """y' = coeff(x)*y + forcing(x) for C in {-2, 0, 3}, plus the
    homogeneous part y_C - y_0 against z' = coeff(x)*z."""
    assert text.startswith("y = ") and text.count("C") == 1, text
    with localcontext() as ctx:
        ctx.prec = 200
        for x in points:
            def at(constant, t):
                body = text.removeprefix("y = ").replace("C", f"({constant})")
                return _decimal_value(body, t)
            h = (abs(x) + Decimal("1e-11")) * Decimal("1e-60")
            for constant in (-2, 0, 3):
                y = at(constant, x)
                dy = (at(constant, x + h) - at(constant, x - h)) / (2 * h)
                rhs = coeff(x) * y + forcing(x)
                scale = max(abs(dy), abs(coeff(x) * y), abs(forcing(x)),
                            Decimal(1))
                assert abs(dy - rhs) <= Decimal(tol) * scale, \
                    (text, constant, x, dy, rhs)
            z = at(3, x) - at(0, x)
            dz = ((at(3, x + h) - at(0, x + h)) -
                  (at(3, x - h) - at(0, x - h))) / (2 * h)
            assert z != 0, (text, x)
            assert abs(dz - coeff(x) * z) <= \
                Decimal(tol) * max(abs(dz), abs(coeff(x) * z)), \
                (text, x, dz, coeff(x) * z)


def test_cecode_integrating_factor_keeps_shift(cecode):
    [(ok, text, leaked, dfree)] = _run_stats(
        cecode, ["ode_linear(y/(x+5e-11),x,y)"])
    assert ok and leaked == dfree == 0, text
    assert text == "y = (0 + C)/(exp(-ln(abs(x+5e-11))))", text
    # y = C|x + 5e-11|: check on both sides of the singular point -5e-11
    # and where x is comparable to the shift.
    _ode_residual(text, lambda x: 1 / (x + C5), lambda x: Decimal(0),
                  [Decimal(0), C5, -C5 / 2, -2 * C5, Decimal(1)], "1e-30")


def test_cecode_affine_forcing_keeps_tiny_constant(cecode):
    [(ok, text, leaked, dfree)] = _run_stats(
        cecode, ["ode_linear(y+x+1e-30,x,y)"])
    assert ok and leaked == dfree == 0, text
    assert text == "y = (exp(-x)*((x+1e-30)/(-1)-1) + C)/(exp(-x))", text
    # Exact: the residual is far below 1e-30 even where x + 1 ~ 0.
    _ode_residual(text, lambda x: Decimal(1),
                  lambda x: x + Decimal("1e-30"),
                  [Decimal(-1), Decimal(0), Decimal("2.5")], "1e-40")


def test_ode_oracle_rejects_dropped_shift():
    broken = "y = (0 + C)/(exp(-ln(abs(x))))"
    for point in (C5, -C5 / 2, Decimal(1) / 10**9):
        with pytest.raises(AssertionError):
            _ode_residual(broken, lambda x: 1 / (x + C5),
                          lambda x: Decimal(0), [point], "1e-30")
    old = "y = (exp(-x)*(x/(-1)-1) + C)/(exp(-x))"
    with pytest.raises(AssertionError):
        _ode_residual(old, lambda x: Decimal(1),
                      lambda x: x + Decimal("1e-30"), [Decimal(-1)], "1e-40")


def test_cecode_exact_zero_and_accepted_controls(cecode):
    rows = _run_stats(cecode, [
        "ode_linear(y/(x+0),x,y)", "ode_linear(y+x+0,x,y)",
        "ode_linear(y/(x+1),x,y)",
        # x^tiny -> 1 stays an accepted approximation (internal review): the
        # factor is float-exact, so this still solves.
        "ode_linear(y+x^1e-30,x,y)",
    ])
    assert rows == [
        (True, "y = (0 + C)/(exp(-ln(abs(x))))", 0, 0),
        (True, "y = (exp(-x)*(x/(-1)-1) + C)/(exp(-x))", 0, 0),
        (True, "y = (0 + C)/(exp(-ln(abs(x+1))))", 0, 0),
        (True, "y = (exp(-x)/(-1) + C)/(exp(-x))", 0, 0),
    ], rows


def test_cecode_cubic_tiny_coefficient_refuses_honestly(cecode):
    """an internal review thread nit: this cubic tiny-coefficient ODE has no
    finite symbolic mu*Q antiderivative within the engine; it must refuse,
    not answer with a dropped coefficient.  The diffeq form is recorded
    too."""
    rows = _run_stats(cecode, [
        "ode_linear(x^3+x^2+x+1-1e-30*y,x,y)",
        "odelinear(x^3+x^2+x+1-1E-30*y,x,y)",
        "diffeq(x^3+x^2+x+1-1E-30*y,x,y)",
    ])
    for ok, text, leaked, dfree in rows:
        assert leaked == dfree == 0, text
    assert rows[0][:2] == (
        False, "Linear ODE failed: could not integrate mu*Q "
        "(No symbolic antiderivative found)"), rows[0]
    assert rows[1][:2] == rows[0][:2], rows[1]
    # Probe of the diffeq form: it routes to the same linear solver and
    # refuses with the same message.
    assert rows[2][:2] == rows[0][:2], rows[2]
