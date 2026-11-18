"""Tiny nonzero y-coefficients must survive CECODE's linear ODE path.

ast_is_num(n, 0) treats |n| < 1e-10 as zero.  Before this regression the
simplifier used that tolerance in annihilating rules, so P = -1e-30 lost
its sign (-(1e-30) -> 1e-30) and P*x lost x (1e-30*x -> 1e-30).  The
displayed solution then had a constant integrating factor exp(1e-30).

A floating-point residual check near x = 1 cannot see that: the broken
answer x^2/2 + C is within 1e-30 of a solution there.  The oracle below
evaluates the displayed text exactly in Decimal arithmetic and checks the
ODE where |c*x| is near 1, where the broken answer is wrong by about 30%.
This is evidence for the listed literal coefficients only, not a claim of
exact handling for arbitrary floating-point coefficients.
"""
import re
from decimal import Decimal, localcontext

import pytest

from test_module_flag_parity import SPLIT, _build_module_driver, _run_stats


@pytest.fixture(scope="module")
def cecode(tmp_path_factory):
    return _build_module_driver(SPLIT / "Makefile.calc.ode",
                                tmp_path_factory.mktemp("tiny_ode"),
                                sanitize=True)


_TOKEN = re.compile(
    r"(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[A-Za-z_]\w*|\S")


def _decimal_value(body: str, x: Decimal) -> Decimal:
    """Evaluate displayed text exactly: literals become Decimal."""
    parts: list[str] = []
    previous_operand = False
    for token in _TOKEN.findall(body):
        starts_operand = token[0].isalnum() or token[0] in "._("
        if previous_operand and starts_operand:
            parts.append("*")  # implicit product: 2x, 1e-30x, 2(x+1), x(...)
        if token[0].isdigit() or token[0] == ".":
            parts.append(f"D({token!r})")
        elif token == "^":
            parts.append("**")
        else:
            assert token in {"x", "exp", "+", "-", "*", "/", "(", ")"}, \
                (token, body)
            parts.append(token)
        # A function name is followed by its call, not by an implicit product.
        previous_operand = token != "exp" and (token[0].isalnum() or
                                               token in ".)")
    names = {"D": Decimal, "x": x, "exp": lambda v: v.exp()}
    return eval("".join(parts), {"__builtins__": {}}, names)  # noqa: S307


def _solution_body(text: str, constant: int) -> str:
    assert text.startswith("y = "), text
    assert text.count("C") == 1, text
    return text.removeprefix("y = ").replace("C", f"({constant})")


def _residual_check(text: str, coeff: Decimal, forcing, points,
                    homogeneous: bool = True) -> None:
    """y' = coeff*y + forcing(x) for C in {-2, 0, 3}, plus the homogeneous
    part y_C - y_0 against z' = coeff*z on its own scale."""
    with localcontext() as ctx:
        ctx.prec = 300
        for x in points:
            h = (abs(x) + 1) * Decimal("1e-60")

            def at(constant, t):
                return _decimal_value(_solution_body(text, constant), t)

            def deriv(constant):
                return (at(constant, x + h) - at(constant, x - h)) / (2 * h)

            for constant in (-2, 0, 3):
                y = at(constant, x)
                dy = deriv(constant)
                rhs = coeff * y + forcing(x)
                scale = max(abs(dy), abs(coeff * y), abs(forcing(x)), 1)
                assert abs(dy - rhs) <= Decimal("1e-4") * scale, \
                    (text, constant, x, dy, rhs)
            if not homogeneous:
                continue
            z = at(3, x) - at(0, x)
            dz = deriv(3) - deriv(0)
            assert z != 0, (text, x)
            assert abs(dz - coeff * z) <= \
                Decimal("1e-4") * max(abs(dz), abs(coeff * z)), \
                (text, x, dz, coeff * z)


# (literal coefficient c, ODE input y' = c*y + Q(x), Q, integrating factor)
TINY = [
    ("1e-30", "ode_linear(1e-30*y+x,x,y)", lambda x: x, "exp(-1e-30x)"),
    ("-1e-30", "ode_linear(-1e-30*y+x,x,y)", lambda x: x, "exp(1e-30x)"),
    ("1e-30", "diffeq(1e-30*y+x,x,y)", lambda x: x, "exp(-1e-30x)"),
    ("1e-15", "ode_linear(1e-15*y+x,x,y)", lambda x: x, "exp(-1e-15x)"),
    ("3e-11", "ode_linear(3e-11*y+x-1,x,y)", lambda x: x - 1,
     "exp(-3e-11x)"),
    ("-2e-20", "ode_linear(x-2e-20*y,x,y)", lambda x: x, "exp(2e-20x)"),
]


@pytest.mark.parametrize("coeff,expr,forcing,mu", TINY,
                         ids=[row[1] for row in TINY])
def test_cecode_keeps_tiny_ode_coefficient(cecode, coeff, expr, forcing, mu):
    [(ok, text, leaked, dfree)] = _run_stats(cecode, [expr])
    assert ok, text
    assert leaked == dfree == 0
    # The integrating factor must keep the coefficient's sign and x.
    assert text.endswith(f")/({mu})"), text
    c = Decimal(coeff)
    # Ordinary points plus points where |c*x| is about 0.5, 1 and 2.
    scale = 1 / abs(c)
    points = [Decimal("-0.8"), Decimal("1.1"),
              -scale / 2, scale, 2 * scale]
    _residual_check(text, c, lambda x: Decimal(forcing(x)), points)


def test_cecode_tiny_homogeneous_keeps_x(cecode):
    [(ok, text, leaked, dfree)] = _run_stats(
        cecode, ["ode_linear(1e-30*y,x,y)"])
    assert ok and leaked == dfree == 0, text
    assert text.endswith(")/(exp(-1e-30x))"), text
    c = Decimal("1e-30")
    _residual_check(text, c, lambda x: Decimal(0),
                    [Decimal("0.5"), Decimal("1e30"), Decimal("-2e30")])


def test_oracle_rejects_constant_integrating_factor():
    """The pre-fix answer's full residual passes near x = 1 but fails where
    c*x ~ 1; its homogeneous part (a constant) fails everywhere."""
    broken = "y = (exp(1e-30)*(x^2/2) + C)/(exp(1e-30))"
    c = Decimal("1e-30")
    _residual_check(broken, c, lambda x: x, [Decimal("1.1")],
                    homogeneous=False)
    with pytest.raises(AssertionError):
        _residual_check(broken, c, lambda x: x, [Decimal("1e30")],
                        homogeneous=False)
    with pytest.raises(AssertionError):
        _residual_check(broken, c, lambda x: x, [Decimal("1.1")])


def test_cecode_tiny_fix_keeps_exact_zero_and_controls(cecode):
    rows = _run_stats(cecode, [
        "ode_linear(x-y,x,y)", "ode_linear(x,x,y)", "ode_linear(x-x-y,x,y)",
        "ode_linear(0*y+x,x,y)", "diffeq(x^2,x,y)",
    ])
    assert rows == [
        (True, "y = (exp(x)*(x-1) + C)/(exp(x))", 0, 0),
        (True, "y = (exp(0)*(x^2/2) + C)/(exp(0))", 0, 0),
        (True, "y = (0 + C)/(exp(x))", 0, 0),
        (True, "y = (exp(0)*(x^2/2) + C)/(exp(0))", 0, 0),
        (True, "y = x^3/3 + C", 0, 0),
    ], rows
