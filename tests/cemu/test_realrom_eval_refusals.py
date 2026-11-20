"""Opt-in OS 5.8 CEmu regressions: an eval refusal is never shown as math.

NOT hardware verification. Requires ALGEBRIX_ROM (owned ROM), debug
CEMU_HEADLESS and a fresh make all (the launched fixture refuses stale
binaries). Never commit ROMs or emulator state files.

Previously, the 50-level eval cap ("Eval recursion limit") reached
callers that turned the error into a value: fourier() of a 55-term sum
answered "Fourier_2 = 0", nlsolve() listed its Newton starts as roots,
recseq() called its last good term s(k), and the volume workers printed
"= nan". The cap now marks the run like the eZ80
stack backstop does (g_eval_refused), and those callers fail on a failed
sample. Shallow inputs keep their answers.
"""
from __future__ import annotations

import json

# Imported first: it puts tools/emulator/linux-realrom on sys.path.
from test_realrom_worker_stack import CEMU, ROOT, launched, pytestmark  # noqa: F401
import realrom  # noqa: E402

S55 = "+".join(["x"] * 55)
S51 = "+".join(["x"] * 51)
N51 = "+".join(["n"] * 51)

# Refusals: (input, the whole CE_RESP error text).
ERRORS = [
    # The reported case: 55 terms are deeper than the 50-level cap.
    (f"fourier({S55},x,2)", "Eval recursion limit"),
    (f"nlsolve({S51}-1,y-x,x,y)", "Eval recursion limit"),
    (f"sum({N51},n,1,10)", "Eval recursion limit"),
    # A failed sample (here a domain error for every x < 0) is no
    # coefficient or volume. (fourier(1/x) fails on the desktop only: in CE
    # float no Simpson node lands exactly on 0.)
    ("fourier(ln(x),x,2)", "Could not compute Fourier series"),
    ("disk(sqrt(x),x,-1,1)", "Volume (disk): computation failed"),
    # ln(0) at s(2): main showed "s(1) = 0" as if the sequence ended there.
    ("recseq(ln(x),x,1,5)", "recseq: s(2) could not be evaluated"),
    # CECIDF has no limits at infinity: it refuses, never prints a value.
    ("int(e^(-x),x,0,inf)", "Could not decide integral"),
]

# Shallow inputs keep their answers: (input, first result line or its
# prefix, text the first line must not contain).
ANSWERS = [
    ("fourier(x,x,2)", "Fourier_2 = 8.5036e-07+2sin(1X)+", "Fourier_2 = 0"),
    ("fourier(x^2,x,3)", "Fourier_3 = 3.28986+(-4cos(1X))+", "Fourier_3 = 0"),
    # The CE prints variables upper case.
    ("nlsolve(sqrt(x)+y-3,y-1,x,y)", "(X, Y) = (4, 1)", ";"),
    ("nlsolve(x^2+y^2-25,y-x,x,y)", "(X, Y) = (3.5355, 3.5355); (X, Y) = (-3.5355, -3.5355)", "nan"),
    ("disk(sqrt(x),x,0,4)", "Volume (disk) = 25.1327", "nan"),
    ("recseq(2*x,x,1,5)", "s(5) = 32", "nan"),
    ("solve(x^2-5x+6,x)", "X = ", "nan"),
    ("int(x^2,x,0,3)", "= 9", "nan"),
]


def _run(launched, index, expr):
    address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                     stderr_path=launched / f"refusal-{index}.log") as e:
        e.run(300)
        e.cmd(f"fill {address:x} 2 0")
        e.type(expr)
        e.key("enter", 4000)
        # nlsolve's 225 Newton starts take about 14 s in CEmu: poll for a
        # fresh CE_RESP for up to a minute rather than one fixed wait.
        for _ in range(28):
            raw = realrom._peek(e, address, 64 + 512 + 128)
            if realrom.parse_response(raw, "", realrom.ipc_version())["fresh"]:
                break
            e.run(2000)
        e.run(1000)
        e.shot(launched / f"refusal-{index}.bmp")
        raw = realrom._peek(e, address, 64 + 512 + 128)
        log = e.quit()
    return raw, realrom.reset_seen(log)


def test_refusals_are_errors_and_shallow_answers_stay(launched):
    version = realrom.ipc_version()
    rows = []
    for index, (expr, message) in enumerate(ERRORS):
        assert len(expr) <= 127, expr
        raw, reset = _run(launched, index, expr)
        response = realrom.parse_response(raw, "", version)
        rows.append(dict(expression=expr, reset=reset, fresh=response["fresh"],
                         ok=response["ok"], want=message))
        (launched / "refusals.json").write_text(json.dumps(rows, indent=2))
        assert not reset, rows[-1]
        assert response["fresh"] and not response["ok"], (expr, raw)
        assert b"\0" + message.encode("ascii") + b"\0" in raw, (expr, raw)
    for index, (expr, first, never) in enumerate(ANSWERS, len(ERRORS)):
        raw, reset = _run(launched, index, expr)
        response = realrom.parse_response(raw, first, version)
        line = response["text"].split("\n", 1)[0]
        rows.append(dict(expression=expr, reset=reset, fresh=response["fresh"],
                         ok=response["ok"], text=line))
        (launched / "refusals.json").write_text(json.dumps(rows, indent=2))
        assert not reset, rows[-1]
        assert response["fresh"] and response["ok"], (expr, raw)
        assert line.startswith(first), (expr, line)
        if first.endswith(")") or first[-1].isdigit():
            assert line == first, (expr, line)  # the whole answer, nothing more
        assert never not in line and "nan" not in line and "inf" not in line, (expr, line)
