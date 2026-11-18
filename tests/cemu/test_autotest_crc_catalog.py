from __future__ import annotations

import json
from pathlib import Path

from pipeline_support import REPO_ROOT, load_transfer_manifest


CATALOG_JSON_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.crc-catalog.json"
CATALOG_MARKDOWN_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.crc-catalog.md"
BASELINED_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.baselined.json"
MANIFEST_PATH = REPO_ROOT / "tests" / "emulator" / "manifest.json"
ARTIFICE_CATALOG_JSON_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.artifice-smoke.crc-catalog.json"
ARTIFICE_CATALOG_MARKDOWN_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.artifice-smoke.crc-catalog.md"
ARTIFICE_BASELINED_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.artifice-smoke.baselined.json"


def _load_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def _normalized_path_parts(value: object) -> tuple[str, ...]:
    normalized = str(value).strip().replace("\\", "/")
    return tuple(part.lower() for part in normalized.split("/") if part and part != ".")


def _path_matches_repo_suffix(value: object, expected_path: Path) -> bool:
    expected_suffix = tuple(part.lower() for part in expected_path.relative_to(REPO_ROOT).parts)
    actual_parts = _normalized_path_parts(value)
    return len(actual_parts) >= len(expected_suffix) and actual_parts[-len(expected_suffix):] == expected_suffix


def _assert_catalog_entry_shape(entry: dict[str, object], order: int) -> None:
    assert entry["order"] == order
    assert isinstance(entry["hash_name"], str) and entry["hash_name"]
    assert isinstance(entry["section"], str) and entry["section"]
    assert isinstance(entry["description"], str) and entry["description"]
    assert isinstance(entry["expected_crcs"], list) and entry["expected_crcs"]
    assert all(isinstance(value, str) and value == value.upper() for value in entry["expected_crcs"])
    assert isinstance(entry["timeout_ms"], int) and entry["timeout_ms"] > 0
    assert isinstance(entry["start"], str) and entry["start"]
    assert isinstance(entry["size"], str) and entry["size"]


def _expected_catalog_section(hash_name: str) -> str:
    prefix = hash_name.split("_", 1)[0]
    return prefix.upper() if len(prefix) == 3 and prefix.startswith("s") and prefix[1:].isdigit() else "UNSPECIFIED"


def test_comprehensive_crc_catalog_matches_the_baselined_suite() -> None:
    baselined = _load_json(BASELINED_PATH)
    catalog = _load_json(CATALOG_JSON_PATH)

    expected_entries = []
    for order, (hash_name, payload) in enumerate(baselined["hashes"].items(), start=1):
        expected_entries.append(
                {
                    "order": order,
                    "hash_name": hash_name,
                    "section": _expected_catalog_section(hash_name),
                    "description": payload["description"],
                    "expected_crcs": [str(value).upper() for value in payload["expected_CRCs"]],
                    "timeout_ms": int(payload["timeout"]),
                "start": payload["start"],
                "size": payload["size"],
                }
            )

    assert catalog["suite"] == "autotest.comprehensive"
    assert _path_matches_repo_suffix(catalog["source_baselined_path"], BASELINED_PATH)
    assert isinstance(catalog["hash_count"], int) and catalog["hash_count"] > 0
    assert isinstance(catalog["entries"], list) and catalog["entries"]

    for order, entry in enumerate(catalog["entries"], start=1):
        _assert_catalog_entry_shape(entry, order)

    assert catalog["hash_count"] == len(catalog["entries"]) == len(expected_entries) == 211
    assert catalog["entries"] == expected_entries


def test_comprehensive_crc_catalog_verifies_all_35_manifest_modules() -> None:
    catalog = _load_json(CATALOG_JSON_PATH)
    module_entries, _runtime_entries, _readme_entry = load_transfer_manifest(MANIFEST_PATH)
    inventory = catalog.get("module_inventory")

    assert isinstance(inventory, dict)
    assert _path_matches_repo_suffix(inventory["manifest_path"], MANIFEST_PATH)
    assert inventory["expected_module_count"] == len(module_entries) == 35
    assert inventory["verified_module_count"] == len(module_entries)
    assert inventory["missing_modules"] == []
    assert inventory["unexpected_modules"] == []

    rows = inventory["modules"]
    assert len(rows) == len(module_entries)
    assert [row["name"] for row in rows] == [entry.name for entry in module_entries]

    for row, entry in zip(rows, module_entries):
        assert row["manifest_size_bytes"] == entry.size_bytes
        assert row["manifest_crc32"] == entry.crc32
        assert row["resolved_size_bytes"] == entry.size_bytes
        assert row["resolved_crc32"] == entry.crc32
        assert row["status"] == "matched"
        assert row["resolved_path"]
        assert _normalized_path_parts(row["resolved_path"])[-1] == entry.name.lower()


def test_comprehensive_crc_catalog_markdown_reports_hash_and_module_totals() -> None:
    catalog = _load_json(CATALOG_JSON_PATH)
    markdown = CATALOG_MARKDOWN_PATH.read_text(encoding="utf-8")

    assert markdown.startswith("# CalcEngine ")
    assert "CRC Catalog" in markdown
    assert "Hashes:" in markdown
    assert catalog["suite"] == "autotest.comprehensive"
    assert "# CalcEngine autotest.comprehensive CRC Catalog" in markdown
    assert "Hashes: 211" in markdown
    assert "## Official Module Inventory" in markdown
    assert "Verified Official Modules: 35/35" in markdown
    first = catalog["module_inventory"]["modules"][0]
    assert first["name"] == "CALCENG.8xp"
    size, crc = first["manifest_size_bytes"], first["manifest_crc32"]
    assert f"| 1 | `CALCENG.8xp` | {size} | `{crc}` | `{crc}` | `matched` |" in markdown


def test_artifice_smoke_crc_catalog_matches_the_baselined_suite() -> None:
    baselined = _load_json(ARTIFICE_BASELINED_PATH)
    catalog = _load_json(ARTIFICE_CATALOG_JSON_PATH)

    expected_entries = []
    for order, (hash_name, payload) in enumerate(baselined["hashes"].items(), start=1):
        expected_entries.append(
                {
                    "order": order,
                    "hash_name": hash_name,
                    "section": _expected_catalog_section(hash_name),
                    "description": payload["description"],
                    "expected_crcs": [str(value).upper() for value in payload["expected_CRCs"]],
                    "timeout_ms": int(payload["timeout"]),
                "start": payload["start"],
                "size": payload["size"],
            }
        )

    assert catalog["suite"] == "autotest.artifice-smoke"
    assert _path_matches_repo_suffix(catalog["source_baselined_path"], ARTIFICE_BASELINED_PATH)
    assert catalog["hash_count"] == len(catalog["entries"]) == len(expected_entries) == 19
    assert catalog["entries"] == expected_entries

    launch_entry = next(entry for entry in catalog["entries"] if entry["hash_name"] == "calcengine_launch_screen")
    menu_open_entry = next(entry for entry in catalog["entries"] if entry["hash_name"] == "calcengine_menu_open")
    menu_closed_entry = next(entry for entry in catalog["entries"] if entry["hash_name"] == "calcengine_menu_closed")

    assert launch_entry["expected_crcs"] == ["28B88B56"]
    assert menu_open_entry["expected_crcs"] == ["D12593CE"]
    assert menu_closed_entry["expected_crcs"] == ["828489BA"]


def test_artifice_smoke_crc_catalog_markdown_reports_current_launch_hashes() -> None:
    markdown = ARTIFICE_CATALOG_MARKDOWN_PATH.read_text(encoding="utf-8")

    assert "# CalcEngine autotest.artifice-smoke CRC Catalog" in markdown
    assert "| 4 | `calcengine_launch_screen` | `UNSPECIFIED` | `28B88B56` |" in markdown
    assert "| 16 | `calcengine_menu_open` | `UNSPECIFIED` | `D12593CE` |" in markdown
    assert "| 19 | `calcengine_menu_closed` | `UNSPECIFIED` | `828489BA` |" in markdown
