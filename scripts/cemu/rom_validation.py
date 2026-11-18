from __future__ import annotations

import hashlib
import re
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping


EXPECTED_ROM_SIZES = (4_194_304,)
MODEL_SIGNATURES = (
    "TI-84 Plus CE",
    "TI-84 Plus CE-T",
    "TI-84 Plus CE Python",
    "Python",
)
KNOWN_ROM_FINGERPRINTS: dict[str, dict[str, object]] = {
    "AE5D87F96A01CB124B41ADF5D4F5D88CC3E6FF22149A97EC79024B736D1C5014": {
        "crc32": "AC9BF466",
        "model": "TI-84 Plus CE Python",
        "product_family": "TI-84 Plus CE",
        "os_version": "5.8.0.0022",
        "boot_version": "5.5.2.0044",
        "firmware_family": "5.8.x",
        "description": "Known CalcEngine validation ROM dump for the TI-84 Plus CE Python family.",
        "required_signatures": ("TI-84 Plus CE", "Python", "5.8.0.0022", "5.5.2.0044"),
    }
}
VERSION_PATTERN = re.compile(rb"(?<!\d)(\d+\.\d+\.\d+\.\d{4})(?!\d)")


@dataclass(frozen=True)
class RomValidationResult:
    path: Path
    size_bytes: int
    crc32: str
    sha1: str
    sha256: str
    recognized_size: bool
    recognized_fingerprint: bool
    expected_crc32: str
    expected_sha256: str
    product_family: str
    model: str
    os_version: str
    boot_version: str
    firmware_family: str
    description: str
    signature_offsets: Mapping[str, int]
    version_candidates: tuple[str, ...]
    required_signatures: tuple[str, ...]
    missing_signatures: tuple[str, ...]
    warnings: tuple[str, ...]
    errors: tuple[str, ...]

    @property
    def integrity_ok(self) -> bool:
        return not self.errors

    @property
    def known_good_hash_match(self) -> bool:
        if not self.recognized_fingerprint:
            return False
        if self.expected_sha256 and self.expected_sha256 != self.sha256:
            return False
        if self.expected_crc32 and self.expected_crc32 != self.crc32:
            return False
        return True

    def to_summary(self) -> dict[str, object]:
        return {
            "path": str(self.path),
            "size_bytes": self.size_bytes,
            "crc32": self.crc32,
            "sha1": self.sha1,
            "sha256": self.sha256,
            "recognized_size": self.recognized_size,
            "recognized_fingerprint": self.recognized_fingerprint,
            "expected_crc32": self.expected_crc32,
            "expected_sha256": self.expected_sha256,
            "integrity_ok": self.integrity_ok,
            "known_good_hash_match": self.known_good_hash_match,
            "product_family": self.product_family,
            "model": self.model,
            "os_version": self.os_version,
            "boot_version": self.boot_version,
            "firmware_family": self.firmware_family,
            "description": self.description,
            "signature_offsets": dict(self.signature_offsets),
            "version_candidates": list(self.version_candidates),
            "required_signatures": list(self.required_signatures),
            "missing_signatures": list(self.missing_signatures),
            "warnings": list(self.warnings),
            "errors": list(self.errors),
        }


def _hash_bytes(algorithm: str, payload: bytes) -> str:
    digest = hashlib.new(algorithm)
    digest.update(payload)
    return digest.hexdigest().upper()


def _find_signature_offsets(payload: bytes, signatures: Iterable[str]) -> dict[str, int]:
    return {signature: payload.find(signature.encode("ascii")) for signature in signatures}


def _extract_version_candidates(payload: bytes) -> tuple[str, ...]:
    discovered: list[str] = []
    seen: set[str] = set()
    for match in VERSION_PATTERN.finditer(payload):
        value = match.group(1).decode("ascii")
        if value in seen:
            continue
        seen.add(value)
        discovered.append(value)
    return tuple(discovered)


def _select_version_pair(version_candidates: Iterable[str]) -> tuple[str, str]:
    ordered = sorted(version_candidates, key=lambda candidate: tuple(int(part) for part in candidate.split(".")), reverse=True)
    if not ordered:
        return "", ""
    if len(ordered) == 1:
        return ordered[0], ""
    return ordered[0], ordered[1]


def _firmware_family_from_version(version: str) -> str:
    if not version:
        return ""
    parts = version.split(".")
    if len(parts) < 2:
        return ""
    return f"{parts[0]}.{parts[1]}.x"


def validate_rom_file(path: Path) -> RomValidationResult:
    resolved_path = path.resolve()
    payload = resolved_path.read_bytes()
    size_bytes = len(payload)
    crc32 = f"{zlib.crc32(payload) & 0xFFFFFFFF:08X}"
    sha1 = _hash_bytes("sha1", payload)
    sha256 = _hash_bytes("sha256", payload)
    recognized_size = size_bytes in EXPECTED_ROM_SIZES

    known_fingerprint = KNOWN_ROM_FINGERPRINTS.get(sha256, {})
    recognized_fingerprint = bool(known_fingerprint)
    expected_crc32 = str(known_fingerprint.get("crc32", "")).upper()
    expected_sha256 = str(sha256 if recognized_fingerprint else known_fingerprint.get("sha256", "")).upper()

    signature_offsets = _find_signature_offsets(payload, MODEL_SIGNATURES)
    version_candidates = _extract_version_candidates(payload)
    detected_os_version, detected_boot_version = _select_version_pair(version_candidates)

    product_family = "TI-84 Plus CE" if signature_offsets.get("TI-84 Plus CE", -1) >= 0 else ""
    model = product_family
    if signature_offsets.get("Python", -1) >= 0:
        model = "TI-84 Plus CE Python"

    required_signatures = tuple(
        str(signature) for signature in known_fingerprint.get("required_signatures", ())
    )
    required_signature_offsets = _find_signature_offsets(payload, required_signatures)
    missing_signatures = tuple(
        signature
        for signature, offset in required_signature_offsets.items()
        if offset < 0
    )

    if recognized_fingerprint:
        product_family = str(known_fingerprint.get("product_family", product_family))
        model = str(known_fingerprint.get("model", model))
        detected_os_version = str(known_fingerprint.get("os_version", detected_os_version))
        detected_boot_version = str(known_fingerprint.get("boot_version", detected_boot_version))

    firmware_family = (
        str(known_fingerprint.get("firmware_family"))
        if recognized_fingerprint
        else _firmware_family_from_version(detected_os_version)
    )
    description = str(known_fingerprint.get("description", "Unrecognized TI-84 Plus CE-family ROM fingerprint."))

    warnings: list[str] = []
    errors: list[str] = []

    if not recognized_size:
        errors.append(
            f"Unexpected ROM size {size_bytes} bytes; expected one of {', '.join(str(size) for size in EXPECTED_ROM_SIZES)}."
        )
    if not product_family:
        errors.append("ROM does not contain a TI-84 Plus CE product signature.")
    if recognized_fingerprint and missing_signatures:
        errors.append(
            "ROM fingerprint matched a known build but required signatures were missing: "
            + ", ".join(missing_signatures)
        )
    if recognized_fingerprint and expected_crc32 and expected_crc32 != crc32:
        errors.append(
            f"ROM fingerprint metadata expected CRC32 {expected_crc32}, but observed {crc32}."
        )
    if recognized_fingerprint and expected_sha256 and expected_sha256 != sha256:
        errors.append(
            f"ROM fingerprint metadata expected SHA256 {expected_sha256}, but observed {sha256}."
        )
    if not recognized_fingerprint:
        warnings.append(
            "ROM checksum is not in the known-fingerprint table; firmware mapping is based on embedded strings."
        )
    if not detected_os_version:
        warnings.append("Unable to extract an OS version string from the ROM image.")
    if not detected_boot_version:
        warnings.append("Unable to extract a boot version string from the ROM image.")
    if not firmware_family:
        warnings.append("Unable to map the ROM to a TI-84 Plus CE firmware family.")

    merged_offsets = dict(signature_offsets)
    merged_offsets.update(required_signature_offsets)

    return RomValidationResult(
        path=resolved_path,
        size_bytes=size_bytes,
        crc32=crc32,
        sha1=sha1,
        sha256=sha256,
        recognized_size=recognized_size,
        recognized_fingerprint=recognized_fingerprint,
        expected_crc32=expected_crc32,
        expected_sha256=expected_sha256,
        product_family=product_family,
        model=model,
        os_version=detected_os_version,
        boot_version=detected_boot_version,
        firmware_family=firmware_family,
        description=description,
        signature_offsets=merged_offsets,
        version_candidates=version_candidates,
        required_signatures=required_signatures,
        missing_signatures=missing_signatures,
        warnings=tuple(warnings),
        errors=tuple(errors),
    )
