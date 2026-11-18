from __future__ import annotations

import hashlib
import zlib
from pathlib import Path

import pytest

import rom_validation as rom_validation_module

from rom_validation import KNOWN_ROM_FINGERPRINTS, validate_rom_file


def _write_synthetic_rom(path: Path) -> None:
    payload = bytearray(b"\x00" * 4_194_304)
    payload[1_024:1_024 + len(b"TI-84 Plus CE")] = b"TI-84 Plus CE"
    payload[4_096:4_096 + len(b"Python")] = b"Python"
    payload[8_192:8_192 + len(b"5.8.0.0022")] = b"5.8.0.0022"
    payload[12_288:12_288 + len(b"5.5.2.0044")] = b"5.5.2.0044"
    path.write_bytes(bytes(payload))


def test_validate_rom_file_rejects_bad_size_and_missing_signature(tmp_path: Path) -> None:
    rom_path = tmp_path / "bad.rom"
    rom_path.write_bytes(b"FAKE-ROM")

    result = validate_rom_file(rom_path)

    assert result.integrity_ok is False
    assert result.recognized_size is False
    assert "TI-84 Plus CE product signature" in " ".join(result.errors)


def test_validate_rom_file_extracts_versions_from_unknown_but_well_formed_rom(tmp_path: Path) -> None:
    rom_path = tmp_path / "synthetic.rom"
    _write_synthetic_rom(rom_path)

    result = validate_rom_file(rom_path)

    assert result.integrity_ok is True
    assert result.recognized_size is True
    assert result.recognized_fingerprint is False
    assert result.expected_crc32 == ""
    assert result.expected_sha256 == ""
    assert result.known_good_hash_match is False
    assert result.product_family == "TI-84 Plus CE"
    assert result.model == "TI-84 Plus CE Python"
    assert result.os_version == "5.8.0.0022"
    assert result.boot_version == "5.5.2.0044"
    assert result.firmware_family == "5.8.x"


def test_validate_rom_file_uses_known_fingerprint_when_repo_rom_matches() -> None:
    repo_rom_path = Path(__file__).resolve().parents[2] / "rom" / "Rom.rom"
    if not repo_rom_path.exists():
        pytest.skip("Real ROM file is not present in this workspace.")

    result = validate_rom_file(repo_rom_path)
    if result.sha256 not in KNOWN_ROM_FINGERPRINTS:
        pytest.skip("Real ROM file does not match the locked known-fingerprint table.")

    assert result.integrity_ok is True
    assert result.recognized_fingerprint is True
    assert result.known_good_hash_match is True
    assert result.expected_crc32 == "AC9BF466"
    assert result.expected_sha256 == result.sha256
    assert result.model == "TI-84 Plus CE Python"
    assert result.os_version == "5.8.0.0022"
    assert result.boot_version == "5.5.2.0044"
    assert result.firmware_family == "5.8.x"


def test_validate_rom_file_rejects_corrupted_header_even_with_expected_size(tmp_path: Path) -> None:
    rom_path = tmp_path / "corrupted-header.rom"
    payload = bytearray(b"\x00" * 4_194_304)
    payload[8_192:8_192 + len(b"5.8.0.0022")] = b"5.8.0.0022"
    payload[12_288:12_288 + len(b"5.5.2.0044")] = b"5.5.2.0044"
    rom_path.write_bytes(bytes(payload))

    result = validate_rom_file(rom_path)

    assert result.integrity_ok is False
    assert result.recognized_size is True
    assert result.os_version == "5.8.0.0022"
    assert result.boot_version == "5.5.2.0044"
    assert "TI-84 Plus CE product signature" in " ".join(result.errors)


def test_validate_rom_file_rejects_truncated_rom_with_embedded_versions(tmp_path: Path) -> None:
    rom_path = tmp_path / "truncated.rom"
    payload = bytearray(b"\x00" * 1_024)
    payload[64:64 + len(b"TI-84 Plus CE")] = b"TI-84 Plus CE"
    payload[256:256 + len(b"5.8.0.0022")] = b"5.8.0.0022"
    payload[384:384 + len(b"5.5.2.0044")] = b"5.5.2.0044"
    rom_path.write_bytes(bytes(payload))

    result = validate_rom_file(rom_path)

    assert result.integrity_ok is False
    assert result.recognized_size is False
    assert result.product_family == "TI-84 Plus CE"
    assert result.os_version == "5.8.0.0022"
    assert result.boot_version == "5.5.2.0044"
    assert "Unexpected ROM size" in " ".join(result.errors)


def test_validate_rom_file_reports_known_good_crc_metadata_for_monkeypatched_fingerprint(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    rom_path = tmp_path / "locked.rom"
    _write_synthetic_rom(rom_path)
    payload = rom_path.read_bytes()
    sha256 = hashlib.sha256(payload).hexdigest().upper()
    crc32 = f"{zlib.crc32(payload) & 0xFFFFFFFF:08X}"
    monkeypatch.setattr(
        rom_validation_module,
        "KNOWN_ROM_FINGERPRINTS",
        {
            sha256: {
                "crc32": crc32,
                "model": "TI-84 Plus CE Python",
                "product_family": "TI-84 Plus CE",
                "os_version": "5.8.0.0022",
                "boot_version": "5.5.2.0044",
                "firmware_family": "5.8.x",
                "description": "Synthetic locked ROM for startup-integrity testing.",
                "required_signatures": ("TI-84 Plus CE", "Python", "5.8.0.0022", "5.5.2.0044"),
            }
        },
    )

    result = validate_rom_file(rom_path)

    assert result.integrity_ok is True
    assert result.recognized_fingerprint is True
    assert result.expected_crc32 == crc32
    assert result.expected_sha256 == sha256
    assert result.known_good_hash_match is True
