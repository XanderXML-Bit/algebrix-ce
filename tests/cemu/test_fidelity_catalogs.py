from __future__ import annotations

import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from keypress_sequences import normalize_key


KEYPAD_FIDELITY_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.keypad-fidelity.json"
LCD_FIDELITY_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.lcd-fidelity.json"


def _load_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def test_keypress_sequences_support_graph_row_aliases() -> None:
    assert normalize_key("y=") == "yequ"
    assert normalize_key("YEQU") == "yequ"
    assert normalize_key("window") == "window"
    assert normalize_key("zoom") == "zoom"
    assert normalize_key("trace") == "trace"
    assert normalize_key("graph") == "graph"


def test_keypad_fidelity_catalog_covers_required_physical_paths() -> None:
    payload = _load_json(KEYPAD_FIDELITY_PATH)

    sequence = payload["sequence"]
    hashes = payload["hashes"]

    assert isinstance(sequence, list) and sequence
    assert isinstance(hashes, dict) and hashes

    required_hashes = {
        "kf27_mode_settings_open",
        "kf29_catalog_menu_open",
        "kf30_catalog_browse_down",
        "kf32_store_arrow_input",
        "kf33_yequ_entry_state",
        "kf35_graph_screen_drawn",
        "kf36_graph_trace_toggle",
        "kf37_graph_window_editor_open",
        "kf38_graph_zoom_menu_open",
    }
    assert required_hashes.issubset(hashes)
    assert all(f"hash|{name}" in sequence for name in required_hashes)

    catalog_hash_index = sequence.index("hash|kf29_catalog_menu_open")
    assert sequence[catalog_hash_index - 3 : catalog_hash_index + 1] == [
        "key|2nd",
        "key|0",
        "delay|1200",
        "hash|kf29_catalog_menu_open",
    ]
    assert "key|mode" in sequence
    assert "key|yequ" in sequence
    assert "key|graph" in sequence
    assert "key|trace" in sequence
    assert "key|window" in sequence
    assert "key|zoom" in sequence


def test_lcd_fidelity_catalog_covers_required_visual_paths() -> None:
    payload = _load_json(LCD_FIDELITY_PATH)

    sequence = payload["sequence"]
    hashes = payload["hashes"]

    assert isinstance(sequence, list) and sequence
    assert isinstance(hashes, dict) and hashes

    required_hashes = {
        "lf16_mode_screen_open",
        "lf18_catalog_menu_open",
        "lf21_mathprint_expression_input",
        "lf22_mathprint_result_layout",
        "lf24_graph_entry_screen",
        "lf26_graph_screen_pixels",
        "lf27_window_editor_screen",
        "lf28_zoom_menu_screen",
        "lf29_trace_screen",
    }
    assert required_hashes.issubset(hashes)
    assert all(f"hash|{name}" in sequence for name in required_hashes)

    assert "key|mode" in sequence
    assert "key|2nd" in sequence
    assert "key|0" in sequence
    assert "key|yequ" in sequence
    assert "key|graph" in sequence
    assert "key|window" in sequence
    assert "key|zoom" in sequence
    assert "key|trace" in sequence
    assert "key|^2" in sequence
