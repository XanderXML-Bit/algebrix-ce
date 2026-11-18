"""Pin every ALPHA letter table to the TI-84 Plus CE printed keycaps.

The CE launcher's table was shifted by one key from M onward until 2026-11
(ALPHA+LOG typed M, ALPHA+÷ typed nothing), so `lim`, `solve`, `sum` and
`mean` could not be typed from the keycaps. The emulator harnesses carried two
other wrong tables. This test reads each table straight from source and
compares it with one reference layout.

Reference: TI "Keyboard Mapping for the TI-84 Plus CE online calculator"
(education.ti.com eGuide; computer letter a-z presses the key that carries that
alpha letter), cross-checked on OS 5.8.0.0022 in CEmu, where ALPHA-lock plus
math..÷ types A..M and log..2 types N..Z.
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Letter -> physical key, in keypad row order.
PRINTED = {
    "A": "math", "B": "apps", "C": "prgm",
    "D": "recip", "E": "sin", "F": "cos", "G": "tan", "H": "power",
    "I": "square", "J": "comma", "K": "lparen", "L": "rparen", "M": "div",
    "N": "log", "O": "7", "P": "8", "Q": "9", "R": "mul",
    "S": "ln", "T": "4", "U": "5", "V": "6", "W": "sub",
    "X": "sto", "Y": "1", "Z": "2",
}

# Harness key names -> physical key.
HARNESS_KEY = {
    "math": "math", "apps": "apps", "prgm": "prgm", "-1": "recip", "sin": "sin",
    "cos": "cos", "tan": "tan", "^": "power", "^2": "square", ",": "comma",
    "(": "lparen", ")": "rparen", "/": "div", "log": "log", "7": "7", "8": "8",
    "9": "9", "*": "mul", "ln": "ln", "4": "4", "5": "5", "6": "6", "-": "sub",
    "sto": "sto", "1": "1", "2": "2",
}

# CEdev sk_* / kb_* names -> physical key.
C_KEY = {
    "Math": "math", "Apps": "apps", "Prgm": "prgm", "Recip": "recip", "Sin": "sin",
    "Cos": "cos", "Tan": "tan", "Power": "power", "Square": "square",
    "Comma": "comma", "LParen": "lparen", "RParen": "rparen", "Div": "div",
    "Log": "log", "7": "7", "8": "8", "9": "9", "Mul": "mul", "Ln": "ln",
    "4": "4", "5": "5", "6": "6", "Sub": "sub", "Store": "sto", "Sto": "sto",
    "1": "1", "2": "2", "0": "0", "GraphVar": "graphvar",
}


def _read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


def _launcher_alpha_block() -> str:
    src = _read("src/core/launcher/main.c")
    start = src.index("/* ── Alpha modifier")
    end = src.index("/* ── Normal (no modifier)", start)
    return src[start:end]


def test_launcher_alpha_table_matches_printed_keycaps() -> None:
    block = _launcher_alpha_block()
    pairs = re.findall(r"case sk_(\w+):\s*c = '(.)';", block)
    table = {C_KEY[k]: ch for k, ch in pairs}
    expected = {key: letter for letter, key in PRINTED.items()}
    expected["graphvar"] = "X"  # X,T,θ,n
    expected["0"] = " "  # printed: space
    assert table == expected


def test_launcher_unsupported_printed_symbols_are_noops() -> None:
    block = _launcher_alpha_block()
    # θ (3), " (+), : (.), ? ((-)) have no Algebrix token: fall to the no-op.
    noop = block[block.index("case sk_3:"):block.index("return 0; /* unmapped alpha key")]
    for key in ("sk_3", "sk_Add", "sk_DecPnt", "sk_Chs"):
        assert f"case {key}:" in noop
        assert not re.search(rf"case {key}:\s*c =", block)


def test_legacy_ui_alpha_table_matches_printed_keycaps() -> None:
    src = _read("src/ui.c")
    block = src[src.index("if (alpha_held) {\n        char ac = 0;"):]
    block = block[:block.index("if (ac) {")]
    pairs = re.findall(r"if \(g\d & kb_(\w+)\)\s+ac = '(.)';", block)
    table = {C_KEY[k]: ch for k, ch in pairs}
    expected = {key: letter.lower() for letter, key in PRINTED.items()}
    expected["0"] = " "
    assert table == expected


def _py_map(rel: str) -> dict[str, str]:
    src = _read(rel)
    body = re.search(r"ALPHA_KEY_MAP[^=]*=\s*\{(.*?)\n\}", src, re.S)
    assert body, rel
    return {k.upper(): v for k, v in re.findall(r'"(\w)":\s*"([^"]+)"', body.group(1))}


def _as_physical(mapping: dict[str, str]) -> dict[str, str]:
    return {letter: HARNESS_KEY[key] for letter, key in mapping.items()}


def test_python_harness_maps_match_printed_keycaps() -> None:
    for rel in (
        "tests/cemu/cemu_hardening.py",
        "scripts/cemu/keypress_sequences.py",
        "scripts/cemu/artifice_jailbreak.py",
        "validation/run_validation.py",
    ):
        assert _as_physical(_py_map(rel)) == PRINTED, rel


def test_typescript_harness_map_matches_printed_keycaps() -> None:
    src = _read("src/testing/cemu-interaction.ts")
    body = re.search(r"const ALPHA_KEY_MAP: Record<string, string> = \{(.*?)\n\}", src, re.S)
    assert body
    mapping = {k.upper(): v for k, v in re.findall(r'(\w):\s*"([^"]+)"', body.group(1))}
    assert _as_physical(mapping) == PRINTED


def test_realrom_driver_map_matches_printed_keycaps() -> None:
    src = _read("tools/emulator/linux-realrom/realrom.py")
    body = re.search(r"ALPHA = dict\((.*?)\)\n", src, re.S)
    assert body
    mapping = {k.upper(): v for k, v in re.findall(r'(\w)="([^"]+)"', body.group(1))}
    assert _as_physical(mapping) == PRINTED


def test_powershell_artifice_map_matches_printed_keycaps() -> None:
    src = _read("tools/emulator/CalcEmuArtificePipeline.psm1")
    body = re.search(r"\$alphaMap = @\{(.*?)\n    \}", src, re.S)
    assert body
    mapping = dict(re.findall(r'"(\w)" = "([^"]+)"', body.group(1)))
    assert _as_physical(mapping) == PRINTED


def _native_key_names(*, include_aliases: bool) -> set[str]:
    """Key names accepted by tools/emulator/cemu-headless native mode.

    Without aliases this is the upstream CEmu autotester vocabulary, which is
    what the committed ``key|...`` sequences are replayed against.
    """
    src = _read("tools/emulator/cemu-headless/main.c")
    table = src[src.index("g_native_key_map[] = {"):]
    table = table[:table.index("};")]
    names = set()
    for line in table.splitlines():
        m = re.search(r'\{ "([^"]+)",\s*\{', line)
        if m and (include_aliases or "/* alias */" not in line):
            names.add(m.group(1))
    return names


def test_every_alpha_map_key_is_accepted_by_the_native_backend() -> None:
    # A map value the backend rejects (e.g. "sto>") silently breaks real-CEmu
    # lanes while the fake backend keeps passing.
    upstream = _native_key_names(include_aliases=False)
    maps = {rel: _py_map(rel) for rel in (
        "tests/cemu/cemu_hardening.py",
        "scripts/cemu/keypress_sequences.py",
        "scripts/cemu/artifice_jailbreak.py",
        "validation/run_validation.py",
    )}
    ts = _read("src/testing/cemu-interaction.ts")
    ts_body = re.search(r"const ALPHA_KEY_MAP: Record<string, string> = \{(.*?)\n\}", ts, re.S)
    maps["src/testing/cemu-interaction.ts"] = dict(re.findall(r'(\w):\s*"([^"]+)"', ts_body.group(1)))
    rr = _read("tools/emulator/linux-realrom/realrom.py")
    rr_body = re.search(r"ALPHA = dict\((.*?)\)\n", rr, re.S)
    maps["realrom.py"] = dict(re.findall(r'(\w)="([^"]+)"', rr_body.group(1)))
    ps = _read("tools/emulator/CalcEmuArtificePipeline.psm1")
    ps_body = re.search(r"\$alphaMap = @\{(.*?)\n    \}", ps, re.S)
    maps["CalcEmuArtificePipeline.psm1"] = dict(re.findall(r'"(\w)" = "([^"]+)"', ps_body.group(1)))
    for name, mapping in maps.items():
        bad = {k: v for k, v in mapping.items() if v not in upstream}
        assert not bad, f"{name}: keys the backend rejects: {bad}"


def test_committed_alpha_sequences_use_backend_key_names() -> None:
    import json
    import subprocess

    upstream = _native_key_names(include_aliases=False)
    files = [
        f for f in subprocess.check_output(
            ["git", "ls-files", "tests/emulator/*.json", "validation/hardware-ready/*.json"],
            cwd=REPO, text=True,
        ).split()
        if "/outputs/" not in f
    ]
    checked = 0
    for rel in files:
        data = json.loads((REPO / rel).read_text(encoding="utf-8-sig"))
        seq = data.get("sequence") if isinstance(data, dict) else None
        if not isinstance(seq, list):
            continue
        for i, step in enumerate(seq[:-1]):
            if step == "key|alpha" and seq[i + 1].startswith("key|"):
                key = seq[i + 1][4:]
                assert key in upstream, f"{rel}[{i + 1}]: alpha+{key!r} is not a backend key"
                checked += 1
    assert checked > 500
