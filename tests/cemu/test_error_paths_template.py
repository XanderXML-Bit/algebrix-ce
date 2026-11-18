from __future__ import annotations

import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TEMPLATE_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.error-paths.json"
CRC_PATTERN = re.compile(r"^[0-9A-F]{8}$")
COMPREHENSIVE_TEMPLATE_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.json"
EXTRA_VALID_KEYS = {"on"}


def _load_key_tokens(path: Path) -> set[str]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return {
        entry.split("|", 1)[1]
        for entry in payload.get("sequence", [])
        if isinstance(entry, str) and entry.startswith("key|")
    }


def test_error_paths_template_has_real_baseline_crcs() -> None:
    payload = json.loads(TEMPLATE_PATH.read_text(encoding="utf-8"))
    hashes = payload.get("hashes", {})

    invalid_entries: list[str] = []
    placeholder_entries: list[str] = []
    zero_crc_entries: list[str] = []

    for hash_name, hash_payload in hashes.items():
        expected_crcs = hash_payload.get("expected_CRCs", [])
        if not expected_crcs:
            invalid_entries.append(f"{hash_name}: missing expected_CRCs")
            continue
        for index, crc in enumerate(expected_crcs):
            crc_text = str(crc).strip()
            if crc_text == "__REPLACE_WITH_BASELINE_CRC__":
                placeholder_entries.append(f"{hash_name}[{index}]")
                continue
            if crc_text == "00000000":
                zero_crc_entries.append(f"{hash_name}[{index}]")
                continue
            if not CRC_PATTERN.fullmatch(crc_text):
                invalid_entries.append(f"{hash_name}[{index}]={crc_text!r}")

    assert not placeholder_entries, f"error-path suite still has string placeholders: {placeholder_entries}"
    assert not zero_crc_entries, f"error-path suite still has dummy 00000000 CRCs: {zero_crc_entries}"
    assert not invalid_entries, f"error-path suite has invalid CRC entries: {invalid_entries}"


def test_error_paths_sequence_uses_known_autotester_keys() -> None:
    comprehensive_keys = _load_key_tokens(COMPREHENSIVE_TEMPLATE_PATH) | EXTRA_VALID_KEYS
    error_path_keys = _load_key_tokens(TEMPLATE_PATH)
    unknown_keys = sorted(error_path_keys - comprehensive_keys)

    assert not unknown_keys, f"error-path suite uses unknown autotester keys: {unknown_keys}"
