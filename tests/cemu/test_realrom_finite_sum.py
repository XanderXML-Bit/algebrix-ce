"""Opt-in finite sum regression on an owned ROM in CEmu, never hardware proof.

Requires ALGEBRIX_ROM, debug CEMU_HEADLESS, and all 35 freshly built modules.
The launcher map must come from that same build. No ROM or state is committed.

CECSEQ answers sum(f,n,a,b). Before an internal review it ran the infinite-series
convergence test for every upper bound, so SUM(N,N,1,10) showed
"Convergence: diverges (nth-term)" and SUM(1/N^2,N,1,10) showed the infinite
sum 1.63993. Only an inf upper bound is a series now.
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))
ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MAP = ROOT / "bin/launcher/CALCENG.map"
pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file()
         and len(list(ROOT.glob("bin/*/*.8xp"))) == 35 and MAP.is_file()),
    reason="needs owned ALGEBRIX_ROM, debug CEMU_HEADLESS, and fresh make all",
)


@pytest.fixture(scope="module")
def sum_state(tmp_path_factory):
    out = tmp_path_factory.mktemp("realrom_sum")
    subprocess.run(
        [sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
         "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
        check=True, timeout=600,
    )
    return str(out / "launched.ce")


# (typed input, ok, first result line or error text, second result line)
@pytest.mark.parametrize("expr,ok,expected,line2", [
    ("sum(n,n,1,10)", True, "= 55", None),
    ("sum(1/n^2,n,1,10)", True, "= 1.54977", None),
    ("sum(k^2,k,1,4)", True, "= 30", None),
    ("sum(1/n^2,n,1,inf)", True, "Convergence: converges (telescoping)", "Sum ~= 1.63993"),
    ("sum(n,n,1,inf)", True, "Convergence: diverges (nth-term)", None),
    ("sum(1/(n-3),n,1,5)", False, "Division by zero", None),
    ("sum(n,n,1,20000)", False, "Sum too many terms", None),
    ("sum(k^2,k,1,n)", False, "N has no value", None),
    ("sum(10^38,k,1,10)", False, "Sum overflow", None),
    ("sum(1,k,8388607,8388607)", True, "= 1", None),
    ("sum(1,k,8388607,-8388607)", True, "= 0", None),
    ("sum(k,k,8388608,8388608)", False, "sum bounds must be integers", None),
    ("sum(k,k,-8388609,-8388609)", False, "sum bounds must be integers", None),
    ("sum(1,k,-8388608,-8388608)", True, "= 1", None),
    ("sum(1,k,0,10000)", True, "= 10001", None),
    ("sum(1,k,0,10001)", False, "Sum too many terms", None),
    ("sum(sum(k,k,1,2)+k,k,1,2)", True, "= 9", None),
])
def test_sum_result_on_real_rom(sum_state, expr, ok, expected, line2, tmp_path):
    import realrom

    address = realrom.map_symbol(MAP, "_g_response")
    with realrom.Emu(binary=CEMU, image=sum_state) as emu:
        emu.run(300)
        emu.cmd(f"fill {address:06X} 2 0")
        emu.type(expr)
        emu.key("enter", 9000)
        raw = realrom._peek(emu, address, realrom.RESULT_WINDOW + 128)
        prefix = expected if ok else "= "
        response = realrom.parse_response(raw, prefix, realrom.ipc_version())
        emu.shot(tmp_path / "result.bmp")
        log = emu.quit()
    assert not realrom.reset_seen(log), expr
    assert response["fresh"], (expr, response)
    assert response["ok"] == ok, (expr, response)
    if ok:
        lines = response["text"].split("\n")
        assert lines[0] == expected, (expr, response)
        if line2 is not None:
            assert len(lines) > 1 and lines[1] == line2, (expr, response)
        if not expr.endswith(",inf)"):
            # A finite sum is a number, not a series test.
            assert len(lines) == 1, (expr, response)
    else:
        assert expected.encode() + b"\0" in raw[4:], (expr, raw)


# Frozen contract section 7. Bits for rounded terms are measured on OS 5.8.
ACCURACY_ROWS = [
    ("sum(n,n,1,10000)", "50005000", 0x4C3EC102),
    ("sum(n,n,0,10000)", "50005000", 0x4C3EC102),
    ("sum(n,n,1,10001)", "50015000", 0x4C3ECAC6),
    ("sum(1,n,0,10000)", "10001", 0x461C4400),
    ("sum(n^2,n,1,1000)", "333833504", 0x4D9F2F29),
    ("sum(n^3,n,1,100)", "25502500", 0x4BC29192),
    ("sum(0.1,n,1,10000)", "1000", 0x4479FFFF),
    ("sum(1/n,n,1,10000)", "9.78761", 0x411C9A08),
    ("sum(1/n^2,n,1,10000)", "1.64483", 0x3FD289EC),
    ("sum((-1)^n/n,n,1,10000)", "-0.693097", 0xBF316ED2),
    ("sum((-1)^n*n,n,1,10000)", "5000", 0x459C4000),
    ("sum(n-5000,n,1,10000)", "5000", 0x459C4000),
    ("sum(sum(k,k,1,n),n,1,100)", "171700", 0x4827AD00),
    ("sum(8388608+n,n,0,1)", "16777216", 0x4B800000),
    ("sum(8388609+n,n,0,1)", "16777220", 0x4B800002),
    ("sum(-8388608-n,n,0,1)", "-16777216", 0xCB800000),
    ("sum(-8388609-n,n,0,1)", "-16777220", 0xCB800002),
]


def _route_sum(expr, route):
    return "0+" + expr if route == "CEBASE" else expr + "->A" if route == "CELSCX" else expr


def _submit_sum(emu, expr):
    import realrom

    address = realrom.map_symbol(MAP, "_g_response")
    ans = realrom.map_symbol(MAP, "_g_last_answer")
    emu.cmd(f"fill {address:06X} 2 0")
    if "->" in expr:
        left, right = expr.split("->")
        emu.type(left)
        emu.key("sto", 120)
        emu.type(right)
    else:
        emu.type(expr)
    emu.key("enter", 2000)
    for _ in range(120):
        raw = realrom._peek(emu, address, realrom.RESULT_WINDOW + 128)
        response = realrom.parse_response(raw, "= ", realrom.ipc_version())
        if response["fresh"]:
            break
        emu.run(2000)
    assert response["fresh"], expr
    emu.run(300)
    ans_bits = struct.unpack("<I", realrom._peek(emu, ans, 4))[0]
    return raw, response, ans_bits


@pytest.mark.parametrize("route", ["CEBASE", "CECSEQ"])
@pytest.mark.parametrize("expr,text,ans_bits", ACCURACY_ROWS)
def test_sum_accuracy_on_real_rom(sum_state, route, expr, text, ans_bits, tmp_path):
    import realrom

    with realrom.Emu(binary=CEMU, image=sum_state) as emu:
        emu.run(300)
        raw, response, ans = _submit_sum(emu, _route_sum(expr, route))
        emu.shot(tmp_path / "accuracy.bmp")
        log = emu.quit()
    assert not realrom.reset_seen(log), expr
    assert response["ok"], (expr, raw)
    assert response["text"].split("\n", 1)[0] == "= " + text
    assert ans == ans_bits, (expr, hex(ans), hex(ans_bits))


@pytest.mark.parametrize("route", ["CEBASE", "CECSEQ", "CELSCX"])
def test_sum_refusal_preserves_ans_and_recovers(sum_state, route, tmp_path):
    import realrom

    refusals = [
        ("sum(n,n,1,10002)", "Sum too many terms"),
        ("sum(1/(n-3),n,1,5)", "Division by zero"),
        ("sum(q,n,1,5)", "Q has no value"),
        ("sum(n,n,8388608,8388608)", "sum bounds must be integers"),
    ]
    if route != "CELSCX":
        refusals.append(("sum(10^38,n,1,10)", "Sum overflow"))
    with realrom.Emu(binary=CEMU, image=sum_state) as emu:
        emu.run(300)
        for index, (expr, reason) in enumerate(refusals):
            _, seeded, seed_bits = _submit_sum(emu, "2+3")
            assert seeded["ok"] and seed_bits == 0x40A00000
            raw, refused, ans = _submit_sum(emu, _route_sum(expr, route))
            assert not refused["ok"]
            # CEBASE/CELSCX append existing hints after the first error line.
            assert any(reason.encode() + end in raw for end in (b"\0", b"\n"))
            assert ans == seed_bits
            emu.shot(tmp_path / f"refusal-{index}.bmp")
            _, recovered, ans = _submit_sum(emu, "3+4")
            assert recovered["ok"] and ans == 0x40E00000
        log = emu.quit()
    assert not realrom.reset_seen(log)
