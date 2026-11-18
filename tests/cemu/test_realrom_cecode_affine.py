"""Opt-in owned-ROM ODE responses and painted stack, never hardware proof."""
import json
import re

from test_realrom_worker_stack import (
    CEMU, ROOT, launched, pytestmark, realrom,
)


CASES = [
    ("diffeq(x-y,x,y)", True, "Y = (exp(X)*(X-1) + C)/(exp(X))"),
    ("odelinear(x-y,x,y)", True, "Y = (exp(X)*(X-1) + C)/(exp(X))"),
    ("diffeq(x-y+y-y,x,y)", True, "Y = (exp(X)*(X-1) + C)/(exp(X))"),
    ("diffeq(x-y+x-x,x,y)", True, "Y = (exp(X)*(X-1) + C)/(exp(X))"),
    ("diffeq(x^2,x,y)", True, "Y = X^3/3 + C"),
    ("diffeq(x*y,x,y)", True, "ln(abs(Y)) = X^2/2 + C"),
    ("odelinear(3x+2-2y,x,y)", True,
     "Y = (exp(2X)*((3X+2)/2-0.75) + C)/(exp(2X))"),
    ("odelinear(x+3+y,x,y)", True,
     "Y = (exp(-X)*((X+3)/(-1)-1) + C)/(exp(-X))"),
    ("odelinear(x-x-y,x,y)", True, "Y = (0 + C)/(exp(X))"),
    ("odelinear(x^2-y,x,y)", False, "Linear ODE failed: could not integrate mu*Q"),
    ("odelinear(sin(x)-y,x,y)", False, "Linear ODE failed: could not integrate mu*Q"),
    ("diffeq(y+" + "+".join(["x"] * 57) + ",x,y)", False, "Not linear in Y"),
    ("diffeq(" + "+".join(["x"] * 54) + "+1,x,y)", False,
     "Linear ODE failed: could not integrate mu*Q"),
]


def test_cecode_affine_fresh_results_and_stack(launched):
    address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    rows = []
    for index, (expr, want_ok, expected) in enumerate(CASES):
        assert len(expr) <= 127
        with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                         stderr_path=launched / f"ode-{index}.log") as e:
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
            e.shot(launched / f"ode-{index}-home.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
            raw = realrom._peek(e, address, 64 + 512 + 128)
            prefix = expected.split(" = ")[0] + " = " if want_ok else expected.split(":")[0]
            response = realrom.parse_response(raw, prefix, realrom.ipc_version())
            if not want_ok:
                # Errors live after result[512], outside the result parser's
                # header window. Require the expected bytes in this response.
                start = raw.find(expected.encode("ascii"))
                response["text"] = (raw[start:].split(b"\0", 1)[0].decode("ascii")
                                    if start >= 0 else "")
            e.key("trace", 300)
            e.shot(launched / f"ode-{index}-trace.bmp")
            log = e.quit()
        rows.append(dict(expression=expr, margin=low, reset=realrom.reset_seen(log),
                         **response))
        (launched / "ode-high-water.json").write_text(json.dumps(rows, indent=2))
        assert not rows[-1]["reset"] and low >= 600, rows[-1]
        assert response["fresh"] and response["ok"] == want_ok, rows[-1]
        assert response["text"].startswith(expected), rows[-1]
        if want_ok:
            assert response["text"] == expected, rows[-1]
    assert len(rows) == len(CASES)
