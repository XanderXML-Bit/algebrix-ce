from __future__ import annotations

import argparse
import json
import re
from datetime import datetime
from pathlib import Path
from typing import Iterable, Sequence

from pipeline_support import (
    DEFAULT_DOWNLOADS_DIR,
    DEFAULT_MANIFEST_PATH,
    REPO_ROOT,
    TransferManifestEntry,
    compute_crc32_hex,
    load_transfer_manifest,
)


SECTION_PATTERN = re.compile(r"^(s\d{2})_", re.IGNORECASE)


def _suite_name_for_path(source_path: Path) -> str:
    suite_name = source_path.stem
    for suffix in (".baselined", ".generated"):
        if suite_name.endswith(suffix):
            suite_name = suite_name[: -len(suffix)]
    return suite_name or "autotest"


def _deterministic_generated_at(source_path: Path) -> str:
    return datetime.fromtimestamp(source_path.stat().st_mtime).astimezone().isoformat()


def _hash_section_name(hash_name: str) -> str:
    match = SECTION_PATTERN.match(hash_name)
    if match is None:
        return "UNSPECIFIED"
    return match.group(1).upper()


def _iter_hash_entries(config: dict[str, object]) -> list[dict[str, object]]:
    hashes = config.get("hashes")
    if not isinstance(hashes, dict):
        raise ValueError("CRC catalog source config must contain a 'hashes' object.")

    entries: list[dict[str, object]] = []
    for order, (hash_name, payload) in enumerate(hashes.items(), start=1):
        if not isinstance(payload, dict):
            raise ValueError(f"Hash definition '{hash_name}' must be a JSON object.")
        entries.append(
            {
                "order": order,
                "hash_name": str(hash_name),
                "section": _hash_section_name(str(hash_name)),
                "description": str(payload["description"]),
                "expected_crcs": [str(value).upper() for value in payload["expected_CRCs"]],
                "timeout_ms": int(payload["timeout"]),
                "start": str(payload["start"]),
                "size": str(payload["size"]),
            }
        )
    return entries


def _candidate_roots(
    *,
    repo_root: Path,
    downloads_dir: Path | None,
) -> tuple[Path, ...]:
    ordered_candidates = [
        repo_root / "bin",
        repo_root / "_research_tmp" / "toolchain" / "src",
        repo_root,
        downloads_dir,
    ]
    roots: list[Path] = []
    seen: set[str] = set()
    for candidate in ordered_candidates:
        if candidate is None:
            continue
        resolved = candidate.expanduser().resolve()
        key = str(resolved).lower()
        if key in seen or not resolved.exists():
            continue
        seen.add(key)
        roots.append(resolved)
    return tuple(roots)


def _iter_named_candidates(root: Path, filename: str) -> Iterable[Path]:
    direct_candidate = root / filename
    if direct_candidate.is_file():
        yield direct_candidate

    if root.is_file() and root.name.lower() == filename.lower():
        yield root
        return

    if not root.is_dir():
        return

    discovered = sorted(
        (
            path
            for path in root.rglob(filename)
            if path.is_file()
        ),
        key=lambda path: (len(path.parts), len(str(path)), str(path).lower()),
    )
    for path in discovered:
        if path != direct_candidate:
            yield path


def _artifact_matches(entry: TransferManifestEntry, candidate: Path) -> bool:
    if not candidate.is_file():
        return False
    if candidate.stat().st_size != entry.size_bytes:
        return False
    return compute_crc32_hex(candidate) == entry.crc32


def _resolve_module_path(
    entry: TransferManifestEntry,
    candidate_roots: Sequence[Path],
) -> Path | None:
    for root in candidate_roots:
        for candidate in _iter_named_candidates(root, entry.name):
            if _artifact_matches(entry, candidate):
                return candidate.resolve()
    return None


def _build_module_inventory(
    *,
    manifest_path: Path,
    repo_root: Path,
    downloads_dir: Path | None,
) -> dict[str, object]:
    module_entries, _runtime_entries, _readme_entry = load_transfer_manifest(manifest_path)
    candidate_roots = _candidate_roots(repo_root=repo_root, downloads_dir=downloads_dir)

    rows: list[dict[str, object]] = []
    missing_modules: list[str] = []
    unexpected_modules: list[str] = []

    for order, entry in enumerate(module_entries, start=1):
        resolved_path = _resolve_module_path(entry, candidate_roots)
        if resolved_path is None:
            missing_modules.append(entry.name)
            rows.append(
                {
                    "order": order,
                    "name": entry.name,
                    "manifest_size_bytes": entry.size_bytes,
                    "manifest_crc32": entry.crc32,
                    "resolved_path": "",
                    "resolved_size_bytes": -1,
                    "resolved_crc32": "",
                    "status": "missing",
                }
            )
            continue

        rows.append(
            {
                "order": order,
                "name": entry.name,
                "manifest_size_bytes": entry.size_bytes,
                "manifest_crc32": entry.crc32,
                "resolved_path": str(resolved_path),
                "resolved_size_bytes": resolved_path.stat().st_size,
                "resolved_crc32": compute_crc32_hex(resolved_path),
                "status": "matched",
            }
        )

    return {
        "manifest_path": str(manifest_path.resolve()),
        "expected_module_count": len(module_entries),
        "verified_module_count": sum(1 for row in rows if row["status"] == "matched"),
        "missing_modules": missing_modules,
        "unexpected_modules": unexpected_modules,
        "candidate_roots": [str(path) for path in candidate_roots],
        "modules": rows,
    }


def build_crc_catalog(
    *,
    config_path: Path,
    source_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST_PATH,
    repo_root: Path = REPO_ROOT,
    downloads_dir: Path | None = DEFAULT_DOWNLOADS_DIR,
) -> dict[str, object]:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    entries = _iter_hash_entries(config)
    catalog = {
        "schema_version": 1,
        "suite": _suite_name_for_path(source_path),
        "generated_at": _deterministic_generated_at(source_path),
        "source_baselined_path": str(source_path.resolve()),
        "hash_count": len(entries),
        "entries": entries,
    }
    if source_path.name.endswith(".baselined.json"):
        catalog["module_inventory"] = _build_module_inventory(
            manifest_path=manifest_path,
            repo_root=repo_root,
            downloads_dir=downloads_dir,
        )
    return catalog


def render_crc_catalog_markdown(catalog: dict[str, object]) -> str:
    entries = catalog["entries"]
    if not isinstance(entries, list):
        raise ValueError("CRC catalog must contain an 'entries' list.")

    lines = [
        f"# CalcEngine {catalog['suite']} CRC Catalog",
        "",
        f"Generated: {catalog['generated_at']}",
        f"Source: `{catalog['source_baselined_path']}`",
        f"Hashes: {catalog['hash_count']}",
        "",
        "| # | Hash | Section | Expected CRCs | Description |",
        "| --- | --- | --- | --- | --- |",
    ]
    for entry in entries:
        lines.append(
            "| {order} | `{hash_name}` | `{section}` | `{expected_crcs}` | {description} |".format(
                order=entry["order"],
                hash_name=entry["hash_name"],
                section=entry["section"],
                expected_crcs=", ".join(str(value) for value in entry["expected_crcs"]),
                description=str(entry["description"]).replace("|", "\\|"),
            )
        )

    module_inventory = catalog.get("module_inventory")
    if isinstance(module_inventory, dict):
        lines.extend(
            [
                "",
                "## Official Module Inventory",
                "",
                f"Manifest: `{module_inventory['manifest_path']}`",
                "Verified Official Modules: "
                f"{module_inventory['verified_module_count']}/{module_inventory['expected_module_count']}",
                "",
                "| # | Module | Size (Bytes) | Manifest CRC32 | Resolved CRC32 | Status |",
                "| --- | --- | --- | --- | --- | --- |",
            ]
        )
        for row in module_inventory["modules"]:
            lines.append(
                "| {order} | `{name}` | {manifest_size_bytes} | `{manifest_crc32}` | `{resolved_crc32}` | `{status}` |".format(
                    **row
                )
            )

    return "\n".join(lines) + "\n"


def write_crc_catalog(
    *,
    config_path: Path,
    source_path: Path,
    json_path: Path,
    markdown_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST_PATH,
    repo_root: Path = REPO_ROOT,
    downloads_dir: Path | None = DEFAULT_DOWNLOADS_DIR,
) -> dict[str, object]:
    catalog = build_crc_catalog(
        config_path=config_path,
        source_path=source_path,
        manifest_path=manifest_path,
        repo_root=repo_root,
        downloads_dir=downloads_dir,
    )
    json_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    markdown_path.write_text(render_crc_catalog_markdown(catalog), encoding="utf-8")
    return catalog


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a CalcEngine CRC catalog artifact pair.")
    parser.add_argument(
        "--config",
        type=Path,
        default=REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.baselined.json",
        help="Path to the baselined or generated autotest config JSON.",
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.baselined.json",
        help="Source path recorded inside the catalog metadata.",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        default=REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.crc-catalog.json",
        help="Destination path for the machine-readable catalog JSON.",
    )
    parser.add_argument(
        "--markdown-out",
        type=Path,
        default=REPO_ROOT / "tests" / "emulator" / "autotest.comprehensive.crc-catalog.md",
        help="Destination path for the human-readable catalog markdown.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST_PATH,
        help="Path to the transfer manifest used for the official module inventory.",
    )
    parser.add_argument(
        "--downloads-dir",
        type=Path,
        default=DEFAULT_DOWNLOADS_DIR,
        help="Optional downloads directory searched after repo-owned build outputs.",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    write_crc_catalog(
        config_path=args.config,
        source_path=args.source,
        json_path=args.json_out,
        markdown_path=args.markdown_out,
        manifest_path=args.manifest,
        downloads_dir=args.downloads_dir,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
