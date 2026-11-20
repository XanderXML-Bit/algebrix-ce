"""Opt-in CE float regression on an owned ROM in CEmu, never hardware proof.

Requires ALGEBRIX_ROM, debug CEMU_HEADLESS, and all 35 freshly built modules.
The launcher map must come from that same build. No ROM or state is committed.
"""
from __future__ import annotations

import os
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
def float_state(tmp_path_factory):
    out = tmp_path_factory.mktemp("realrom_float")
    subprocess.run(
        [sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
         "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
        check=True, timeout=600,
    )
    return str(out / "launched.ce")


@pytest.mark.parametrize("expr,ok,expected", [
    ("1E38*10", False, "Overflow"),
    ("3E38*2", False, "Overflow"),
    ("3E38+3E38", False, "Overflow"),
    ("-3E38-3E38", False, "Overflow"),
    ("1E38/0.1", False, "Overflow"),
    ("exp(100)", False, "exp overflow"),
    ("10^39", False, "Power overflow"),
    ("cosh(100)", False, "cosh overflow"),
    ("sinh(90)", False, "sinh overflow"),
    ("2^30", True, "= 1073741824"),
    ("2^24-1", True, "= 16777215"),
    ("65536*256-1", True, "= 16777215"),
    ("3^15", True, "= 14348907"),
    ("(-1)^8388609", True, "= -1"),
    # Underflow and tiny operands are refused, not shown as 0 or as a
    # same-value ratio of a non-IEEE difference;
    # tests/cemu/test_realrom_tiny.py has the full set.
    ("10^(-40)", False, "Tiny value unsupported"),
    ("10^(-44)", False, "Tiny value unsupported"),
    ("(2.9843E-38-2.0235E-38)/(2.9843E-38-2.0235E-38)", False, "Tiny value unsupported"),
    ("-(2.9843E-38-2.0235E-38)/(2.9843E-38-2.0235E-38)", False, "Tiny value unsupported"),
    ("2+3", True, "= 5"),
])
def test_float_result_on_real_rom(float_state, expr, ok, expected, tmp_path):
    import realrom

    address = realrom.map_symbol(MAP, "_g_response")
    with realrom.Emu(binary=CEMU, image=float_state) as emu:
        emu.run(300)
        emu.cmd(f"fill {address:06X} 2 0")
        emu.type(expr)
        emu.key("enter", 9000)
        # The packed header precedes result[512] and error[128]. Read both
        # fields, since an error response intentionally has an empty result.
        raw = realrom._peek(emu, address, realrom.RESULT_WINDOW + 128)
        response = realrom.parse_response(raw, expected if ok else "= ", realrom.ipc_version())
        emu.shot(tmp_path / "result.bmp")
        log = emu.quit()
    assert not realrom.reset_seen(log), expr
    assert response["fresh"], (expr, response)
    assert response["ok"] == ok, (expr, response)
    if ok:
        assert response["text"].partition("\n")[0] == expected, (expr, response)
    else:
        assert expected.encode() + b"\0" in raw[4:], (expr, raw)
