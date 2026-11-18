"""Static guards for tests/cemu/edge-cases/module-battery.json (always run, no emulator).

The battery's CRCs come from the Linux real-ROM lane (tools/emulator/linux-realrom,
arTIfiCE -> CALCENG) with the pixels inspected. A case whose screen is wrong carries
``known_failure`` with a card id and no CRC: a wrong screen is never baselined.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))

import realrom  # noqa: E402

BATTERY = ROOT / "tests/cemu/edge-cases/module-battery.json"
LAUNCHER = ROOT / "src/core/launcher/main.c"

# CRCs from the autotester-wrapper lane, which on OS 5.5+ never launches CALCENG and
# hashed the TI-OS home screen (proved by pixel inspection, an internal review). Never again.
RETIRED_TIOS_SCREEN_CRCS = {
    "B9B5D5E0", "96FD4303", "BB355886", "AF5B6E7B", "E5B90897",
    "EC70402F", "E0B3E73F", "9FBBCE5B", "22CEF46C", "31BE412A", "C43CFBBB",
}
CARD = re.compile(r"^\S.*$")  # every known failure names its tracking reference
CRC = re.compile(r"^[0-9A-F]{8}$")


def _data() -> dict:
    return json.loads(BATTERY.read_text(encoding="utf-8"))


def test_capture_provenance_is_real_rom_pixel_inspected_and_not_hardware() -> None:
    cap = _data()["capture"]
    assert cap["pixel_inspected"] is True
    assert cap["hardware_verified"] is False
    assert "linux-realrom" in cap["lane"]
    assert "CRC-32C" in cap["crc"]
    assert re.fullmatch(r"[0-9a-f]{40}", cap["source_commit"])


def test_every_case_is_either_baselined_or_a_carded_known_failure() -> None:
    cases = _data()["cases"]
    assert len(cases) >= 20
    for case in cases:
        crcs = case["expected_crcs"]
        known = case.get("known_failure")
        if known:
            assert crcs == [], f"{case['name']}: a known failure must not baseline a CRC"
            assert CARD.match(known["card"]), case["name"]
            assert known["reason"], case["name"]
            assert "observed_display" in known, case["name"]
        else:
            assert crcs and all(CRC.match(c) for c in crcs), case["name"]
            assert case["expected_display"] and "baseline" not in case["expected_display"].lower(), case["name"]
        assert not set(crcs) & RETIRED_TIOS_SCREEN_CRCS, f"{case['name']}: TI-OS home-screen CRC"


def test_baselined_crcs_are_unique_per_case() -> None:
    seen: dict[str, str] = {}
    for case in _data()["cases"]:
        for crc in case["expected_crcs"]:
            assert crc not in seen, f"{case['name']} and {seen[crc]} share CRC {crc}: a generic screen?"
            seen[crc] = case["name"]


def test_every_typeable_case_types_with_the_launcher_key_map() -> None:
    for case in _data()["cases"]:
        try:
            realrom.keys_for(case["input_expression"])
        except ValueError:
            known = case.get("known_failure") or {}
            assert "not typeable" in known.get("reason", ""), case["name"]


def test_semicolon_types_with_the_launcher_apps_key() -> None:
    # Matrix literals need ';'. The launcher types it from unmodified APPS
    # (internal review); realrom.py must press the same key.
    src = LAUNCHER.read_text(encoding="utf-8")
    normal = src[src.index("/* ── Normal (no modifier)"):]
    assert "case sk_Apps:    input_insert_char(';'); return 0;" in normal
    assert realrom.keys_for("1;2") == ["1", "apps", "2"]


def test_realrom_alpha_map_matches_the_launcher_alpha_table() -> None:
    src = LAUNCHER.read_text(encoding="utf-8")
    block = src[src.index("if (mod_alpha) {"):]
    block = block[: block.index("default:")]
    sk_to_key = {
        "Math": "math", "Apps": "apps", "Prgm": "prgm", "Recip": "-1", "Sin": "sin", "Cos": "cos",
        "Tan": "tan", "Power": "^", "Square": "^2", "Comma": ",", "LParen": "(", "RParen": ")",
        "Log": "log", "Ln": "ln", "Store": "sto", "DecPnt": ".",
        "Div": "/", "Mul": "*", "Sub": "-", "Add": "+",
        **{str(d): str(d) for d in range(10)},
    }
    table: dict[str, str] = {}
    for sk, letter in re.findall(r"case sk_(\w+):\s*c = '([A-Z])'", block):
        if sk == "GraphVar":  # X,T,θ,n is a second way to type X
            continue
        table[letter.lower()] = sk_to_key[sk]
    assert table == realrom.ALPHA


def test_autotester_crc_is_crc32c() -> None:
    # CRC-32C check value; zlib's CRC-32 of the same input is CBF43926.
    assert realrom.autotester_crc(b"123456789") == "E3069283"


def test_previously_inspected_wrong_screens_cannot_be_promoted():
    wrong = {"17B2609A", "71C3FE52", "82C0A40E", "A051D630", "7379CFC0",
             "68B7D37B", "E4ECC534", "F957DF48", "F3DEC624", "B0B58459",
             "F2B5109D", "B7906BA4", "867E918E",
             "D82A5735", "CBC1C708"}
    for case in _data()["cases"]:
        assert not wrong.intersection(case["expected_crcs"]), case["name"]
