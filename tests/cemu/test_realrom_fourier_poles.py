"""Owned-ROM Fourier pole regressions. This does not verify physical hardware."""
from __future__ import annotations

import pytest

from test_realrom_worker_stack import launched, pytestmark  # noqa: F401
from test_realrom_eval_refusals import _run
import realrom


@pytest.mark.parametrize("index,expr", list(enumerate([
    "fourier(1/x,x,2)", "fourier(1/(x-1),x,2)", "fourier(tan(x),x,2)",
    "fourier(1/(1e-12+(x-1)^2),x,2)",
    "fourier(1/(x-0.03141594)^2,x,2)",
    "fourier(1/(x+3.1101768)^2,x,2)",
    "fourier(1/(x-1)^2-1e6*x,x,2)",
    "fourier(1/(x-1)+10000,x,2)",
    "fourier(1/(x-1)^2+1e9,x,2)",
    "fourier(x^2/x^3,x,2)",
    "fourier(abs(x)^(-2),x,2)",
    "fourier(1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10)+1/(x+11)+1/(x+12)+1/(x-1),x,2)",
    "fourier(1/(x-1)-1e7*x,x,2)",
    "fourier(abs(x-1)^(-x),x,2)",
    "fourier(1/abs(x-1)^0.9,x,2)",
    "fourier((x-1)/(x-1)+1/(x-1),x,2)",
    "fourier(1/(x-(-3.1)),x,2)",
    "fourier(1/(x-(-1)),x,2)",
    "fourier(1/(x-(0.39)),x,2)",
    "fourier(1/(x-(1)),x,2)",
    "fourier(1/(x-(3.1)),x,2)",
    "fourier((1/sqrt(abs(x-1)))^4,x,2)",
    "fourier((1/sqrt(abs(x-1)))^2*sign(x-1),x,2)",
])))
def test_fourier_poles_refuse(launched, index, expr):
    raw, reset = _run(launched, f"pole-{index}", expr)
    response = realrom.parse_response(raw, "", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and not response["ok"], (expr, raw)
    assert b"\0Could not compute Fourier series\0" in raw, (expr, raw)


@pytest.mark.parametrize("index,expr,answer", [
    (0, "fourier(x,x,2)",
     "Fourier_2 = 8.5036e-07+2sin(1X)+(-1sin(2X))"),
    (1, "fourier(abs(x),x,2)",
     "Fourier_2 = 1.57079+(-1.27324cos(1X))"),
    (2, "fourier(x^2,x,3)",
     "Fourier_3 = 3.28986+(-4cos(1X))+0.999998cos(2X)+(-0.444437cos(3X))"),
])
def test_fourier_integrable_answers_stay(launched, index, expr, answer):
    raw, reset = _run(launched, f"finite-{index}", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and response["ok"], (expr, raw)
    assert response["text"].split("\n", 1)[0] == answer, (expr, raw)


@pytest.mark.parametrize("index,expr", [
    (0, "fourier(1/sqrt(abs(x-0.39)),x,2)"),
    (1, "fourier(ln(abs(x-0.39)),x,2)"),
])
def test_fourier_integrable_singularities_are_not_probed_at_the_point(launched, index, expr):
    raw, reset = _run(launched, f"integrable-{index}", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and response["ok"], (expr, raw)
    # A series with its b1 term, not a bare constant (display text has no
    # outer parentheses since PR #112).
    assert response["text"].startswith("Fourier_2 = ") and "sin(1X)" in response["text"], (expr, raw)


@pytest.mark.parametrize("index,expr", [
    (0, "fourier(1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10)+1/(x+11)+1/(x+12),x,2)"),
    (1, "fourier(1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10)+1/(x+11)+1/(x+12)+(x-1)/(x-1),x,2)"),
])
def test_fourier_dedupe_store_overflow_keeps_smooth_answers(launched, index, expr):
    raw, reset = _run(launched, f"overflow-{index}", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and response["ok"], (expr, raw)
    # A series with its b1 term, not a bare constant (display text has no
    # outer parentheses since PR #112).
    assert response["text"].startswith("Fourier_2 = ") and "sin(1X)" in response["text"], (expr, raw)


@pytest.mark.parametrize("index,expr", [
    (0, "fourier(1/(0.01+(x-1)^2)+10.7*x,x,2)"),
    (1, "fourier((x-1.00056)^2*(x-1)/(x-1),x,2)"),
    (2, "fourier(abs(x)^(x+5),x,2)"),
    (3, "fourier(abs(x-1)^x,x,2)"),
    # Compatibility only: non-integrable known miss, not a valid series.
    pytest.param(4, "fourier(1/(x-1)+1e12,x,2)", id="known-miss-large-background"),
])
def test_fourier_trend_and_variable_power_controls(launched, index, expr):
    raw, reset = _run(launched, f"r2-control-{index}", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and response["ok"], (expr, raw)
    # A series with its b1 term, not a bare constant (display text has no
    # outer parentheses since PR #112).
    assert response["text"].startswith("Fourier_2 = ") and "sin(1X)" in response["text"], (expr, raw)


@pytest.mark.parametrize("index,expr", [
    (0, "fourier(1/(0.0001+(x-1)^2)+55800000*(x-1)^2,x,2)"),
    (1, "fourier((x-1.00299)^3*(x-1)/(x-1),x,2)"),
    (2, "fourier((x-1)/(x-1)*(abs(x-1.0001)+(x-1)^2),x,2)"),
    (3, "fourier((x-1)/(x-1)*(sign(x-1.0001)-(x-1)^2),x,2)"),
    (4, "fourier(atan(1/(x-1)),x,2)"),
    (5, "fourier(exp(-1/(x-1)^2),x,2)"),
    # Compatibility only: this tuned background hides a non-integrable pole.
    pytest.param(6, "fourier(1/(x-1)+1e10*(x-1)^2,x,2)", id="known-miss-tuned-background"),
    (7, "fourier((x-1.00299)^3*((x-1)/(x-1)),x,2)"),
])
def test_fourier_owner_and_double_ratio_controls(launched, index, expr):
    raw, reset = _run(launched, f"r3-control-{index}", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and response["ok"], (expr, raw)
    # A series with its b1 term, not a bare constant (display text has no
    # outer parentheses since PR #112).
    assert response["text"].startswith("Fourier_2 = ") and "sin(1X)" in response["text"], (expr, raw)


def test_fourier_failed_owner_probe_preserves_the_real_result(launched):
    expr = "fourier(piecewise((----------x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x)/(x-1),0,1),x,2)"
    raw, reset = _run(launched, "q-depth-failure", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, raw
    assert response["fresh"] and response["ok"], raw
    assert response["text"].split("\n", 1)[0] == "Fourier_2 = 1", raw


@pytest.mark.parametrize("index,expr", [
    (0, "fourier(piecewise(1/(----------x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x),0,1),x,2)"),
    (1, "fourier((x-1)^2*((x-1)^2-0.0179507*(x-1)+3.57854e-5)*(x-1)/(x-1),x,2)"),
    (2, "fourier(1/abs(x-0.39)^0.84,x,2)"),
    (3, "fourier(piecewise(x^(----------1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1),0,1),x,2)"),
])
def test_fourier_denominator_probe_and_refinement_controls(launched, index, expr):
    raw, reset = _run(launched, f"r4-control-{index}", expr)
    response = realrom.parse_response(raw, "Fourier_", realrom.ipc_version())
    assert not reset, (expr, raw)
    assert response["fresh"] and response["ok"], (expr, raw)
    if index in (0, 3):
        assert response["text"].split("\n", 1)[0] == "Fourier_2 = 1", raw
    else:
        # A series with its b1 term (display text, PR #112).
        assert response["text"].startswith("Fourier_2 = ") and "sin(1X)" in response["text"], raw
