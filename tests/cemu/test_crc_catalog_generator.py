from __future__ import annotations

import json
import zlib
from pathlib import Path

from crc_catalog import write_crc_catalog


def _crc32_hex(path: Path) -> str:
    return f"{zlib.crc32(path.read_bytes()) & 0xFFFFFFFF:08X}"


def test_write_crc_catalog_prefers_repo_build_outputs_over_stale_downloads(tmp_path: Path) -> None:
    repo_root = tmp_path / "repo"
    baselined_path = repo_root / "tests" / "emulator" / "autotest.comprehensive.baselined.json"
    manifest_path = repo_root / "tests" / "emulator" / "manifest.json"
    bin_dir = repo_root / "bin" / "algebra"
    launcher_dir = repo_root / "bin" / "launcher"
    downloads_dir = tmp_path / "downloads"

    baselined_path.parent.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    launcher_dir.mkdir(parents=True, exist_ok=True)
    downloads_dir.mkdir(parents=True, exist_ok=True)

    calceng_path = launcher_dir / "CALCENG.8xp"
    calceng_path.write_bytes(b"CALCENGINE-LAUNCHER")
    cealg_repo_path = bin_dir / "CEALG.8xp"
    cealg_repo_path.write_bytes(b"CANONICAL-ALGEBRA-MODULE")
    (downloads_dir / "CEALG.8xp").write_bytes(b"STALE-DOWNLOAD-ALG")

    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "module_count": 2,
                "runtime_dependency_count": 0,
                "modules": [
                    {
                        "name": "CALCENG.8xp",
                        "size_bytes": calceng_path.stat().st_size,
                        "crc32": _crc32_hex(calceng_path),
                    },
                    {
                        "name": "CEALG.8xp",
                        "size_bytes": cealg_repo_path.stat().st_size,
                        "crc32": _crc32_hex(cealg_repo_path),
                    },
                ],
                "runtime_dependencies": [],
                "readme": {
                    "name": "README.md",
                    "crc32": "",
                },
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    baselined_path.write_text(
        json.dumps(
            {
                "hashes": {
                    "s00_boot_home": {
                        "description": "Boot screen",
                        "expected_CRCs": ["11111111"],
                        "timeout": 5000,
                        "start": "vram_start",
                        "size": "vram_16_size",
                    },
                    "s01_after_key_1": {
                        "description": "After pressing 1",
                        "expected_CRCs": ["22222222"],
                        "timeout": 5000,
                        "start": "vram_start",
                        "size": "vram_16_size",
                    },
                }
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    json_path = repo_root / "tests" / "emulator" / "autotest.comprehensive.crc-catalog.json"
    markdown_path = repo_root / "tests" / "emulator" / "autotest.comprehensive.crc-catalog.md"

    catalog = write_crc_catalog(
        config_path=baselined_path,
        source_path=baselined_path,
        json_path=json_path,
        markdown_path=markdown_path,
        manifest_path=manifest_path,
        repo_root=repo_root,
        downloads_dir=downloads_dir,
    )

    inventory = catalog["module_inventory"]
    assert inventory["expected_module_count"] == 2
    assert inventory["verified_module_count"] == 2
    assert inventory["missing_modules"] == []
    assert inventory["unexpected_modules"] == []
    assert Path(inventory["candidate_roots"][0]) == (repo_root / "bin").resolve()

    rows = inventory["modules"]
    assert rows[0]["resolved_path"] == str(calceng_path.resolve())
    assert rows[1]["resolved_path"] == str(cealg_repo_path.resolve())
    assert rows[1]["resolved_crc32"] == _crc32_hex(cealg_repo_path)
    assert rows[1]["resolved_size_bytes"] == cealg_repo_path.stat().st_size
    assert rows[1]["resolved_path"] != str((downloads_dir / "CEALG.8xp").resolve())

    written_catalog = json.loads(json_path.read_text(encoding="utf-8"))
    written_markdown = markdown_path.read_text(encoding="utf-8")

    assert written_catalog["module_inventory"]["modules"][1]["resolved_path"] == str(cealg_repo_path.resolve())
    assert "## Official Module Inventory" in written_markdown
    assert "| 2 | `CEALG.8xp` |" in written_markdown


def test_write_crc_catalog_keeps_generated_timestamp_stable_when_inputs_do_not_change(tmp_path: Path) -> None:
    source_path = tmp_path / "autotest.comprehensive.baselined.json"
    config_path = source_path
    json_path = tmp_path / "autotest.comprehensive.crc-catalog.json"
    markdown_path = tmp_path / "autotest.comprehensive.crc-catalog.md"

    source_path.write_text(
        json.dumps(
            {
                "hashes": {
                    "s00_boot_home": {
                        "description": "Boot screen",
                        "expected_CRCs": ["11111111"],
                        "timeout": 5000,
                        "start": "vram_start",
                        "size": "vram_16_size",
                    }
                }
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    first_catalog = write_crc_catalog(
        config_path=config_path,
        source_path=source_path,
        json_path=json_path,
        markdown_path=markdown_path,
        downloads_dir=None,
    )
    second_catalog = write_crc_catalog(
        config_path=config_path,
        source_path=source_path,
        json_path=json_path,
        markdown_path=markdown_path,
        downloads_dir=None,
    )

    assert first_catalog["generated_at"] == second_catalog["generated_at"]
