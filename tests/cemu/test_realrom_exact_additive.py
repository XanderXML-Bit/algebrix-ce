"""Opt-in owned-ROM check that typed tiny constants survive ADD/SUB
identities (internal review).  Emulator evidence, never hardware proof.

The displayed answers are checked with the same exact Decimal oracle as the
host regression, at points where x is comparable to the constant.
"""
import json
import re
from decimal import Decimal

from test_exact_additive_identities import _derivative_check, _ode_residual
from test_realrom_worker_stack import (
    CEMU, ROOT, launched, pytestmark, realrom,
)

C5 = Decimal("5e-11")

# (calculator input, response prefix, expected text, oracle)
CASES = [
    ("d/dx(ln(x+5E-11))", "d/dX = ", "d/dX = 1/(X+5e-11)",
     lambda t: _derivative_check("ln(x+5e-11)", t,
                                 [Decimal(0), C5, -C5 / 2], C5)),
    ("d/dx(1/(x+5E-11))", "d/dX = ", "d/dX = -1/(X+5e-11)^2",
     lambda t: _derivative_check("1/(x+5e-11)", t, [Decimal(0), C5], C5)),
    ("d/dx(sqrt(x+1E-30))", "d/dX = ", "d/dX = 1/(2sqrt(X+1e-30))",
     lambda t: _derivative_check("sqrt(x+1e-30)", t,
                                 [Decimal(0), Decimal("1e-30")],
                                 Decimal("1e-30"))),
    ("d/dx(x^2+5E-11x)", "d/dX = ", "d/dX = 2X+5e-11",
     lambda t: _derivative_check("x^2+5e-11x", t, [Decimal(0), C5], C5)),
    ("odelinear(y/(x+5E-11),x,y)", "Y = ",
     "Y = (0 + C)/(exp(-ln(abs(X+5e-11))))",
     lambda t: _ode_residual("y = " + t.removeprefix("y = "),
                             lambda x: 1 / (x + C5), lambda x: Decimal(0),
                             [Decimal(0), C5, -2 * C5], "1e-30")),
    # Control: ordinary input, unchanged by the fix.
    ("d/dx(ln(x+1))", "d/dX = ", "d/dX = 1/(X+1)", None),
]


def test_exact_additive_constants_fresh_results_and_stack(launched):
    address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    rows = []
    for index, (expr, prefix, want, oracle) in enumerate(CASES):
        assert len(expr) <= 127
        with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                         stderr_path=launched / f"exact-add-{index}.log") as e:
            e.run(300)
            regs = e.cmd("regs")
            match = re.search(r"spl=([0-9A-F]+)", regs)
            assert match, regs
            size = int(match.group(1), 16) - 32 - realrom.STACK_LIMIT
            assert 600 < size < 4096, regs
            e.cmd(f"fill {realrom.STACK_LIMIT:x} {size} a5")
            e.cmd(f"fill {address:x} 2 0")
            e.type(expr)
            e.key("enter", 6000)
            e.shot(launched / f"exact-add-{index}-home.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
            raw = realrom._peek(e, address, 64 + 512 + 128)
            response = realrom.parse_response(raw, prefix, realrom.ipc_version())
            log = e.quit()
        rows.append(dict(expression=expr, margin=low, reset=realrom.reset_seen(log),
                         **response))
        (launched / "exact-add-high-water.json").write_text(json.dumps(rows, indent=2))
        assert not rows[-1]["reset"] and low >= 600, rows[-1]
        assert response["fresh"] and response["ok"], rows[-1]
        assert response["text"] == want, rows[-1]
        if oracle:
            # The CE prints X and Y in upper case; the oracle reads lower case.
            oracle(response["text"].removeprefix("d/dX = ")
                   .replace("X", "x").replace("Y", "y"))
    assert len(rows) == len(CASES)
