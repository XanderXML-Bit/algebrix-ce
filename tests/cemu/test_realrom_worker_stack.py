"""Opt-in OS 5.8 CEmu high-water regressions, NOT hardware verification.

Requires ALGEBRIX_ROM (owned ROM), debug CEMU_HEADLESS and a fresh make all.
Paint the free stack, not merely program-entry SP: transient recursive frames
are gone by the next module hop. Never commit ROMs or emulator state files.
"""
from __future__ import annotations

import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))
import realrom
from ce_source_graph import ce_module_sources, _assignment

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file()
         and len(list(ROOT.glob("bin/*/*.8xp"))) >= 35),
    reason="requires owned ROM, debug CEmu and fresh make all",
)


def chain(template, op):
    n = 1
    while len(template(op.join(["x"] * (n + 1)))) <= 127:
        n += 1
    return template(op.join(["x"] * n))


def cases():
    result: list[tuple[str, bool | None | str, str]] = [
        ("lim((" + "+".join(["x"] * 55) + ")/x,x,0)", True, "= 55"),
        ("solve(" + "+".join(["x"] * 55) + "-1,x)", True, "X = 0.0181818"),
    ]
    for op in "+-*/^":
        # 126-byte expressions plus a harmless leading blank are not
        # equivalent on this keypad. Use a leading unary minus for 127 B.
        for prefix in ("", "-"):
            lim = chain(lambda s: f"lim(({prefix}{s})/x,x,0)", op)
            solve = chain(lambda s: f"solve({prefix}{s}-1,x)", op)
            # The parser intentionally rejects a 57-level right-associative
            # power chain. Pin that safe refusal separately, never as worker
            # coverage merely because it did not reset.
            if op == "^":
                result.extend([(lim, "syntax", ""), (solve, "syntax", "")])
            elif op in "*/":
                result.extend([(lim, False, "Could not evaluate limit"),
                               (solve, False, "Expression too deep for solve")])
            else:
                # Linear chains retain their exact coefficient, including
                # the leading unary minus. An erroneous default zero fails.
                n = lim.count("x") - 2
                coeff = n if op == "+" else 2 - n
                if prefix:
                    coeff -= 2
                result.append((lim, True, f"= {coeff}"))
                result.append((solve, True, "X = "))
    result.extend([
        ("lim(" + "sin(" * 10 + "x" + ")" * 10 + "+1,x,0)", True, "= 1"),
        ("lim(" + "sin(" * 21 + "x" + ")" * 21 + "+1,x,0)", False,
         "Could not evaluate limit"),
        ("solve(x-(1-(2-(3-(4-(5-(6-(7-(1e38*x)))))))),x)", False, "Expression too deep for solve"),
        ("solve(" + "0+(" * 7 + "1e-6*x-0.001" + ")" * 7 + ",x)", False, "Expression too deep for solve"),
        ("solve(" + "0+(" * 7 + "1e-10*x-1e-9" + ")" * 7 + ",x)", False, "Expression too deep for solve"),
        ("solve(" + "0+(" * 7 + "1e-6*(x-0.0001)^2+1e-18" + ")" * 7 + ",x)", False, "Expression too deep for solve"),
        ("solve(" + "0+(" * 7 + "1e-10*(x-1000)/(x-0.000010000001)" + ")" * 7 + ",x)", False, "Expression too deep for solve"),
        ("lim((x-sin(x))/x^3,x,0)", True, "= 0.166667"),
        ("continuity((x-sin(x))/x^3,x,0)", True, "Removable discontinuity"),
        ("continuity(1/x+" + "x/(" * 20 + "x" + ")" * 20 + ",x,0)", True, "Continuity: could not determine"),
        ("lim(" + "+".join(["x"] * 20) + ",x,0)", True, "= 0"),
        ("lim(" + "+".join(["x"] * 55) + ",x,0)", False, "Could not evaluate limit"),
        ("continuity(" + "+".join(["x"] * 13) + ",x,0)", True, "Continuity: could not determine"),
        ("solve(x-(1-(2-(3-(4-(5-(6-(7-x))))))),x)", False, "Expression too deep for solve"),
        ("solve(1+x*(2+x*(3+x*(4+x*(5+x)))),x)", False, "Expression too deep for solve"),
        ("+".join(str(i) for i in range(1, 14)), True, "91"),
        ("solve(" + "^".join(["x"] * 13) + ",x)", False,
         "Expression too deep for solve"),
        ("lim(" + "sin(" * 10 + "x" + ")" * 10 + "/x,x,0)", True, "= 1"),
        ("lim(" + "sin(" * 21 + "x" + ")" * 21 + ",x,0)", False,
         "Could not evaluate limit"),
        ("solve(" + "sin(" * 20 + "x" + ")" * 20 + ",x)", False,
         "Expression too deep for solve"),
        ("solve(abs(" + "x*(" * 20 + "x" + ")" * 20 + ")-1,x)", False,
         "Expression too deep for solve"),
        ("lim(x^2,x,2)", True, "= 4"),
        ("solve(x+2,x)", True, "X = -2"),
    ])
    for begin, end in [("lim(", ",x,0)"), ("solve(", "-1,x)"),
                       ("solve(sin(x)+", "-1,x)")]:
        for unit in ("x*(", "x-(", "x/(", "sin(", "-"):
            close = ")" if "(" in unit else ""
            n = 0
            while len(begin + unit * (n + 1) + "x" + close * (n + 1) + end) <= 127:
                n += 1
            result.append((begin + unit * n + "x" + close * n + end,
                           "parser-or-worker", ""))
    assert all(len(expr) <= 127 for expr, _, _ in result)
    return result


@pytest.fixture(scope="module")
def launched(tmp_path_factory):
    # File count is only the skip gate. Refuse stale build inputs before
    # transferring: follow each module's quoted-header closure too.
    for module, sources in ce_module_sources(ROOT).items():
        pending, seen = set(sources), set()
        while pending:
            name = pending.pop()
            if name in seen:
                continue
            seen.add(name)
            source = ROOT / name
            for inc in re.findall(r'^\s*#\s*include\s*"([^"]+)"', source.read_text(), re.M):
                header = next((p.resolve() for p in (source.parent / inc, ROOT / "src" / inc)
                               if p.is_file()), None)
                assert header is not None, (source, inc)
                pending.add(header.relative_to(ROOT.resolve()).as_posix())
        mk = ROOT / f"split/Makefile.{module}"
        text = mk.read_text()
        binary = ROOT / _assignment(text, "BINDIR") / (_assignment(text, "NAME") + ".8xp")
        inputs = [ROOT / name for name in seen] + [mk, ROOT / "split/program.mk", ROOT / "Makefile"]
        assert binary.is_file(), binary
        newest = max(inputs, key=lambda p: p.stat().st_mtime_ns)
        assert binary.stat().st_mtime_ns >= newest.stat().st_mtime_ns, (
            f"stale {binary}: newer input {newest}; rebuild make all")
    out = tmp_path_factory.mktemp("realrom_stack")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return out


def test_worker_high_water_and_fresh_responses(launched):
    address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    status_address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_s_status")
    rows = []
    for index, (expr, want_ok, expected) in enumerate(cases()):
        with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                         stderr_path=launched / f"case-{index}.log") as e:
            e.run(300)
            regs = e.cmd("regs")
            match = re.search(r"spl=([0-9A-F]+)", regs)
            assert match, regs
            sp = int(match.group(1), 16)
            size = sp - 32 - realrom.STACK_LIMIT
            assert 600 < size < 4096, regs
            e.cmd(f"fill {realrom.STACK_LIMIT:x} {size} a5")
            e.cmd(f"fill {address:x} 2 0")
            e.type(expr)
            e.key("enter", 6000)
            e.shot(launched / f"case-{index}.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
            # Includes error[128] after result[512]; do not assume settings_t
            # size or mistake a syntax error/stale screen for a safe run.
            raw = realrom._peek(e, address, 64 + 512 + 128)
            response = realrom.parse_response(raw, expected or "= ", realrom.ipc_version())
            status_text = realrom._peek(e, status_address, 32).split(b"\0", 1)[0]
            log = e.quit()
        rows.append(dict(expression=expr, margin=low, reset=realrom.reset_seen(log),
                         fresh=response["fresh"], ok=response["ok"]))
        (launched / "high-water.json").write_text(json.dumps(rows, indent=2))
        assert not rows[-1]["reset"], rows[-1]
        assert low >= 600, rows[-1]
        if want_ok == "syntax" or (want_ok == "parser-or-worker" and not response["fresh"]):
            assert not response["fresh"] and status_text == b"Syntax error", (expr, raw, status_text)
            continue
        assert response["fresh"], (expr, "no new valid CE_RESP; parse/dispatch failure")
        if want_ok == "parser-or-worker":
            # All maximally nested forms either fail parsing or exceed a
            # worker bound. A fresh default-zero success is never acceptable.
            assert not response["ok"], (expr, raw)
        if isinstance(want_ok, bool):
            assert response["ok"] == want_ok, (expr, raw)
        if expected:
            assert expected.encode("ascii") in raw, (expr, raw)
        if not response["ok"]:
            assert any(message in raw for message in (b"Expression too deep", b"Could not evaluate")), (expr, raw)
        if response["ok"] and expr.startswith("solve("):
            import math
            answer = realrom.parse_response(raw, "X ", realrom.ipc_version())["text"].split("\n", 1)[0]
            roots = re.findall(r"X [=~] ([-+0-9.eE]+)", answer)
            assert roots and "nan" not in answer.lower() and "inf" not in answer.lower(), (expr, answer)
            # Only our fixed test-authored arithmetic/sin expressions reach
            # this oracle; no calculator or external text is evaluated.
            equation = expr[len("solve("):].rsplit(",x)", 1)[0].replace("^", "**")
            for token in roots:
                root = float(token)
                residual = eval(equation, {"__builtins__": {}}, {"x": root, "sin": math.sin, "abs": abs})
                assert math.isfinite(root) and math.isfinite(residual) and abs(residual) < 0.02, (expr, answer, residual)
                if "1e38" in expr:
                    assert root == pytest.approx(4e-38, rel=2e-4, abs=1e-42), (expr, answer)
    assert len(rows) == len(cases())


def definite_cases():
    """int(f,x,a,b) through CECIDF. Each value is checked against
    a Simpson oracle; a too-deep input must refuse, never reset or misanswer."""
    def fit(build):
        n = 1
        while len(build(n + 1)) <= 127:
            n += 1
        return build(n)

    def terms(t, n, op="+"):
        return f"int({op.join([t] * n)},x,0,1)"

    def nest(open_, n, close=")"):
        return "int(" + open_ * n + "x" + close * n + ",x,0,1)"

    result = [
        ("int(x^2,x,0,3)", "= 9"), ("int(x,x,0,1)", "= 0.5"),
        ("int(sin(x),x,0,3.14159)", "= 2"), ("int(2^x,x,0,1)", "= 1.4427"),
        ("int(x-100000,x,100000,100001)", "refuse"),  # #99 precision guard
        (terms("x", 13), "= 6.5"), (terms("x", 14), "= 7"),
    ]
    # 15/20/25/30 terms, then the longest chains that fit the input line.
    # The old recursive walk tripped the eval backstop added in #125.
    # These inputs must now return the definite integral.
    for n in (15, 20, 25, 30):
        result.append((terms("x", n), "= %g" % (n / 2)))
    for t, op in (("x", "+"), ("x", "-"), ("x", "*"), ("x^2", "+"), ("sin(x)", "+")):
        result.append((fit(lambda n: terms(t, n, op)), None))
    for build in (lambda n: "int(x*" + "*".join(["2"] * n) + ",x,0,1)",
                  lambda n: "int(x" + "/2" * n + ",x,0,1)",
                  lambda n: "int(x+" + "+".join(["2"] * n) + ",x,0,1)",
                  lambda n: "int(x^(" + "+".join(["2"] * n) + "),x,0,1)",
                  lambda n: "int(x*(" + "+".join(["2"] * n) + "),x,0,1)",
                  lambda n: "int((" + "+".join(["2"] * n) + ")^x,x,0,1)",
                  lambda n: "int(" + "-" * 40 + "x" + "+x" * n + ",x,0,1)"):
        result.append((fit(build), None))
    for n in (10, 15, 16):
        result.append((nest("sin(", n), None))
    result.append((fit(lambda n: nest("sin(", n)), None))
    result.append((fit(lambda n: "int(" + "-" * n + "x,x,0,1)"), "parser-or-worker"))
    result.append((fit(lambda n: nest("(", n)), "parser-or-worker"))
    result.append((fit(lambda n: "int(" + "x+(" * n + "x" + ")" * n + ",x,0,1)"), "parser-or-worker"))
    for n in (10, 16):
        result.append(("int(" + "-" * n + "x,x,0,1)", None))
    assert all(len(expr) <= 127 for expr, _ in result)
    return result


def _simpson(expr):
    import math
    body, lo, hi = expr[len("int("):].rsplit(",x,", 1)[0], *map(float, expr.rsplit(",x,", 1)[1][:-1].split(","))
    # Only the test-authored expressions above reach this oracle.
    f = lambda x: eval(body.replace("^", "**"), {"__builtins__": {}}, {"x": x, "sin": math.sin})
    n = 2000
    h = (hi - lo) / n
    return h / 3 * sum((1 if i in (0, n) else 4 if i % 2 else 2) * f(lo + i * h) for i in range(n + 1))


def test_definite_int_high_water(launched):
    """CECIDF evaluates the antiderivative and walks it for the FTC rounding
    guard (#99). Long sums reset the calculator there, and after #125's eval
    backstop they refused; every 127-character family must keep
    >= 600 B and answer correctly or refuse."""
    address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_response")
    ans_address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_g_last_answer")
    status_address = realrom.map_symbol(ROOT / "bin/launcher/CALCENG.map", "_s_status")
    # "Eval recursion limit" is the eval cap behind "Integral failed: ", or
    # the bare text when #125's backstop refusal marks the run as failed.
    refusals = (b"Could not decide integral", b"Eval recursion limit",
                b"Expression too deep")
    rows = []
    for index, (expr, want) in enumerate(definite_cases()):
        with realrom.Emu(binary=CEMU, image=str(launched / "launched.ce"),
                         stderr_path=launched / f"int-{index}.log") as e:
            e.run(300)
            regs = e.cmd("regs")
            match = re.search(r"spl=([0-9A-F]+)", regs)
            assert match, regs
            sp = int(match.group(1), 16)
            size = sp - 32 - realrom.STACK_LIMIT
            assert 600 < size < 4096, regs
            e.cmd(f"fill {realrom.STACK_LIMIT:x} {size} a5")
            e.cmd(f"fill {address:x} 2 0")
            e.type(expr)
            # Numeric fallbacks over deep nested sin take 9-12 s of emulated
            # time; poll for the fresh response for up to 40 s.
            e.key("enter", 4000)
            for _ in range(18):
                raw = realrom._peek(e, address, 64 + 512 + 128)
                response = realrom.parse_response(raw, "= ", realrom.ipc_version())
                status_text = realrom._peek(e, status_address, 32).split(b"\0", 1)[0]
                # A parse error never reaches a worker: nothing to wait for.
                if response["fresh"] or status_text == b"Syntax error":
                    break
                e.run(2000)
            e.run(500)
            e.shot(launched / f"int-{index}.bmp")
            painted = realrom._peek(e, realrom.STACK_LIMIT, size)
            low = next((i for i, byte in enumerate(painted) if byte != 0xA5), size)
            raw = realrom._peek(e, address, 64 + 512 + 128)
            response = realrom.parse_response(raw, "= ", realrom.ipc_version())
            status_text = realrom._peek(e, status_address, 32).split(b"\0", 1)[0]
            stored_answer = struct.unpack("<f", realrom._peek(e, ans_address, 4))[0]
            log = e.quit()
        rows.append(dict(expression=expr, margin=low, reset=realrom.reset_seen(log),
                         fresh=response["fresh"], ok=response["ok"]))
        (launched / "int-high-water.json").write_text(json.dumps(rows, indent=2))
        assert not rows[-1]["reset"], rows[-1]
        assert low >= 600, rows[-1]
        if want == "parser-or-worker" and not response["fresh"]:
            assert status_text == b"Syntax error", (expr, raw, status_text)
            continue
        assert response["fresh"], (expr, "no new valid CE_RESP; parse/dispatch failure")
        if not response["ok"]:
            assert want in (None, "refuse", "parser-or-worker"), (expr, raw)
            assert any(m in raw for m in refusals), (expr, raw)
            continue
        assert want not in ("refuse", "parser-or-worker"), (expr, raw)
        if want:
            assert want.encode("ascii") in raw, (expr, raw)
        value = re.search(rb"= ([-+0-9.eE]+)", raw)
        assert value, (expr, raw)
        # Display rounding may discard guard digits, not change the value.
        assert float(value.group(1)) == pytest.approx(stored_answer, rel=5e-3, abs=0), (expr, raw)
        # Relative only: x/2/.../2 integrates to 3.5e-18, and a wrong 0 must fail.
        # Numeric fallback displays only three significant digits. Preserve
        # the existing accuracy requirement on unrounded Ans, not by
        # weakening the numerical tolerance to fit shorter display text.
        assert stored_answer == pytest.approx(_simpson(expr), rel=1e-4, abs=0), (expr, raw, stored_answer)
    assert len(rows) == len(definite_cases())


def test_depth_refusal_then_success_in_one_session(launched):
    # run_result_text reuses one emulator, clears magic before every request,
    # and returns only fresh, valid response headers. This is distinct from
    # the isolated per-case high-water measurements above.
    expressions = ["solve(" + "x*(" * 20 + "x" + ")" * 20 + "-1,x)",
                   "solve(x+2,x)",
                   "lim(" + "sin(" * 21 + "x" + ")" * 21 + ",x,0)",
                   "solve(x-3,x)", "solve(x^3-1,x)"]
    rows = realrom.run_result_text(CEMU, str(launched / "launched.ce"),
                                  ROOT / "bin/launcher/CALCENG.map", "X ", expressions)
    assert len(rows) == len(expressions)
    assert all(r["fresh"] and not r["reset"] for r in rows), rows
    assert [r["ok"] for r in rows] == [False, True, False, True, True], rows
    assert rows[1]["text"].startswith("X = -2"), rows
    assert rows[3]["text"].startswith("X = 3"), rows
    assert rows[4]["text"].startswith("X = 1"), rows  # basic -> advanced routing
