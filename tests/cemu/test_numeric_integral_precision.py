"""Numeric fallback display regressions (host CE formatter and owned-ROM lane).

Three-digit expected text is independently rounded from analytic references
(log(sin(2)/sin(1)), 2/3, pi/4, 1/2+(sin(4)-sin(2))/4) and the
high-precision erfi(1)*sqrt(pi)/2 value for exp(x*x). These finite cases do
not certify arbitrary quadrature or its displayed precision universally.
"""
from pathlib import Path
import os
import subprocess
import sys

import pytest
from test_module_config_desktop import cecidf_driver, _run

ROOT = Path(__file__).resolve().parents[2]
CASES = [
    ("int(1/tan(x),x,1,2)", "0.0775"),
    ("int(x^(0*x+2),x,-1,1)", "0.667"),
    ("int(e^(x^2),x,0,1)", "1.46"),
    ("int(sqrt(1-x^2),x,0,1)", "0.785"),
    ("int(1/sqrt(sec(x)^4),x,1,2)", "0.0835"),
]

@pytest.mark.parametrize("expr,expected", CASES)
def test_numeric_integral_digits(cecidf_driver, expr, expected):
    assert _run(cecidf_driver, expr) == [f"OK|= {expected}  (numeric)"]


def test_symbolic_precision_unchanged(cecidf_driver):
    assert _run(cecidf_driver, "int(x^2,x,0,1)") == ["OK|= 0.333333"]


@pytest.mark.parametrize("expr,shown,reference", [
    ("int(e^(x^2),x,1,0)", "-1.46", -1.4626517459071816),
    ("int(0.000001*e^(x^2),x,0,1)", "1.46e-06", 1.4626517459071816e-6),
    ("int(1000000*e^(x^2),x,0,1)", "1.46e+06", 1.4626517459071816e6),
    ("int(x*e^(-x^2),x,-1,1)", "0", 0),
    ("int(int(1/x^2,x,1,2),x,0,3)", "1.5", 1.5),
])
def test_guard_digits_remain_in_ans(cecidf_driver, expr, shown, reference):
    (line,) = _run(cecidf_driver, "--ans", expr)
    display, ans = line.rsplit("|ANS=", 1)
    assert display == f"OK|= {shown}  (numeric)"
    assert float(ans) == pytest.approx(reference, rel=5e-5,
                                       abs=0 if reference else 1e-8)


ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
READY = (Path(ROM).is_file() and Path(CEMU).is_file()
         and len(list(ROOT.glob("bin/*/*.8xp"))) == 35)

@pytest.fixture(scope="module")
def numeric_launched(tmp_path_factory):
    if not READY:
        pytest.skip("requires owned ALGEBRIX_ROM, debug CEMU_HEADLESS and 35 fresh modules")
    sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))
    out = tmp_path_factory.mktemp("numeric_digits_rom")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return out

@pytest.mark.parametrize("expr,expected", CASES)
def test_numeric_integral_digits_owned_rom(numeric_launched, expr, expected):
    import realrom
    (r,) = realrom.run_result_text(CEMU, str(numeric_launched / "launched.ce"),
                                  ROOT / "bin/launcher/CALCENG.map", "= ", [expr], 180000)
    assert r["fresh"] and r["ok"] and not r["reset"], r
    assert r["text"] == f"= {expected}  (numeric)", r
