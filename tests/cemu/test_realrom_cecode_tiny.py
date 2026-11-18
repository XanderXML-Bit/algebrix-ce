"""Opt-in owned-ROM tiny-coefficient ODE responses, never hardware proof.

The CE answer must keep the tiny coefficient's sign and x in the
integrating factor (internal review).  The displayed text is checked with the
same exact Decimal residual oracle as the host regression.
"""
import json
import re
from decimal import Decimal

from test_cecode_tiny_coefficient_ode import _residual_check
from test_realrom_worker_stack import (
    CEMU, ROOT, launched, pytestmark, realrom,
)


# (calculator input, coefficient c in y' = c*y + Q, Q, integrating factor)
CASES = [
    ("odelinear(1E-30*y+x,x,y)", "1e-30", lambda x: x, "exp(-1e-30X)"),
    ("diffeq(1E-30*y+x,x,y)", "1e-30", lambda x: x, "exp(-1e-30X)"),
    ("odelinear(x-2E-20*y,x,y)", "-2e-20", lambda x: x, "exp(2e-20X)"),
    ("odelinear(1E-30*y,x,y)", "1e-30", lambda x: 0, "exp(-1e-30X)"),
    ("odelinear(x-y,x,y)", "-1", lambda x: x, "exp(X)"),
]


def test_cecode_tiny_coefficients_fresh_results_and_stack(launched):
    address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    rows = []
    for index, (expr, coeff, forcing, mu) in enumerate(CASES):
        assert len(expr) <= 127
        with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                         stderr_path=launched / f"tiny-{index}.log") as e:
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
            e.shot(launched / f"tiny-{index}-home.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
            raw = realrom._peek(e, address, 64 + 512 + 128)
            response = realrom.parse_response(raw, "Y = ", realrom.ipc_version())
            e.key("trace", 300)
            e.shot(launched / f"tiny-{index}-trace.bmp")
            log = e.quit()
        rows.append(dict(expression=expr, margin=low, reset=realrom.reset_seen(log),
                         **response))
        (launched / "tiny-high-water.json").write_text(json.dumps(rows, indent=2))
        assert not rows[-1]["reset"] and low >= 600, rows[-1]
        assert response["fresh"] and response["ok"], rows[-1]
        text = response["text"]
        assert text.startswith("Y = ") and text.endswith(f")/({mu})"), rows[-1]
        # The CE prints X and Y in upper case; the oracle reads lower case.
        c = Decimal(coeff)
        scale = 1 / abs(c)
        _residual_check(text.replace("X", "x").replace("Y", "y"), c,
                        lambda x: Decimal(forcing(x)),
                        [Decimal("-0.8"), Decimal("1.1"), scale, -scale / 2])
    assert len(rows) == len(CASES)
