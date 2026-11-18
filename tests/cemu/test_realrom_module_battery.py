"""Opt-in real-ROM replay of tests/cemu/edge-cases/module-battery.json.

Skips unless all of these exist (never in hosted CI; the ROM is never committed):
  ALGEBRIX_ROM       owned TI-84 Plus CE ROM (OS 5.8.x for bundled arTIfiCE 2.1)
  CEMU_HEADLESS      cemu-headless built with tools/emulator/linux-realrom/cemu-headless-debug.patch
  bin/*/*.8xp        a fresh `make all` of this checkout

Baselined cases must reproduce their CRC on a stable screen without a reset.
Known failures must not reset the calculator; if one now matches nothing, that is
expected (the screen is wrong until its card lands). A matching CRC proves only
that the screen is unchanged: rebaseline only after inspecting the pixels.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MODULES = sorted(ROOT.glob("bin/*/*.8xp"))
CASES = json.loads((ROOT / "tests/cemu/edge-cases/module-battery.json").read_text(encoding="utf-8"))["cases"]

pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file() and len(MODULES) >= 35),
    reason="real-ROM lane needs ALGEBRIX_ROM, a debug CEMU_HEADLESS and a built bin/",
)


@pytest.fixture(scope="module")
def battery(tmp_path_factory) -> dict[str, dict]:
    import realrom

    out = tmp_path_factory.mktemp("realrom-battery")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    results = realrom.run_battery(CEMU, str(out / "launched.ce"), out / "cases", CASES)
    return {r["name"]: r for r in results}


@pytest.mark.parametrize("case", [c for c in CASES if not c.get("known_failure")], ids=lambda c: c["name"])
def test_baselined_case_reproduces_its_screen(battery, case) -> None:
    r = battery[case["name"]]
    assert not r["reset"], "calculator reset"
    assert r["stable"], "screen still changing after the result delay"
    assert r["crc"] in case["expected_crcs"], (
        f"{case['input_expression']}: CRC {r['crc']} not in {case['expected_crcs']}; "
        f"inspect {r['bmp']} (expected display {case['expected_display']!r})")


@pytest.mark.parametrize("case", [c for c in CASES if c.get("known_failure")], ids=lambda c: c["name"])
def test_known_failure_does_not_reset(battery, case) -> None:
    r = battery[case["name"]]
    if r.get("untypeable"):
        pytest.xfail(f"{case['known_failure']['card']}: {r['untypeable']}")
    assert not r["reset"], "calculator reset"
