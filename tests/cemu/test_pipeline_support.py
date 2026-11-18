from __future__ import annotations

import json
from pathlib import Path
import shutil
from types import SimpleNamespace

import pytest

import pipeline_support as pipeline_support_module

from pipeline_support import (
    DEFAULT_CEMU_BOOT_TIMEOUT_S,
    DEFAULT_CEMU_COMMAND_TIMEOUT_S,
    DEFAULT_CEMU_SHUTDOWN_TIMEOUT_S,
    DEFAULT_CEMU_STARTUP_TIMEOUT_S,
    TransferManifestEntry,
    TransferManifestSchemaError,
    TransferPackage,
    TransferPackageResolutionError,
    build_runner_config,
    compute_crc32_hex,
    get_cemu_timeout_multiplier,
    load_transfer_manifest,
    resolve_transfer_package,
    scale_cemu_timeout_seconds,
    stage_transfer_package,
    validate_staged_transfer_directory,
)


def build_dummy_transfer_package(tmp_path: Path) -> TransferPackage:
    tmp_path.mkdir(parents=True, exist_ok=True)
    module_entries: list[TransferManifestEntry] = []
    runtime_entries: list[TransferManifestEntry] = []
    resolved_paths: dict[str, Path] = {}

    for name, payload in (
        ("CALCENG.8xp", b"CALCENGINE-LAUNCHER"),
        ("CEBASE.8xp", b"BASE-MODULE"),
        ("fileioc.8xv", b"FILEIOC"),
    ):
        artifact_path = tmp_path / name
        artifact_path.write_bytes(payload)
        entry = TransferManifestEntry(
            name=name,
            size_bytes=artifact_path.stat().st_size,
            crc32=compute_crc32_hex(artifact_path),
            kind="module" if name.lower().endswith(".8xp") else "runtime",
        )
        if entry.kind == "module":
            module_entries.append(entry)
        else:
            runtime_entries.append(entry)
        resolved_paths[name] = artifact_path

    readme_path = tmp_path / "README.md"
    readme_path.write_text("CalcEngine transfer package", encoding="utf-8")
    readme_entry = TransferManifestEntry(
        name="README.md",
        size_bytes=-1,
        crc32=compute_crc32_hex(readme_path),
        kind="readme",
    )
    resolved_paths["README.md"] = readme_path

    return TransferPackage(
        manifest_path=tmp_path / "manifest.json",
        module_entries=tuple(module_entries),
        runtime_entries=tuple(runtime_entries),
        readme_entry=readme_entry,
        resolved_paths=resolved_paths,
        candidate_roots=(tmp_path,),
        missing_entries=(),
    )


def test_load_transfer_manifest_rejects_declared_count_mismatch(tmp_path: Path) -> None:
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "module_count": 2,
                "runtime_dependency_count": 1,
                "modules": [
                    {
                        "name": "CALCENG.8xp",
                        "size_bytes": 1024,
                        "crc32": "12345678",
                    }
                ],
                "runtime_dependencies": [
                    {
                        "name": "fileioc.8xv",
                        "size_bytes": 512,
                        "crc32": "87654321",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(TransferManifestSchemaError, match="module_count declared 2"):
        load_transfer_manifest(manifest_path)


def test_stage_transfer_package_replaces_stale_transfer_artifacts(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "source")
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir(parents=True, exist_ok=True)
    (stage_dir / "OLDMOD.8xp").write_bytes(b"STALE-MODULE")
    (stage_dir / "README.md").write_text("stale readme", encoding="utf-8")
    (stage_dir / "notes.txt").write_text("keep me", encoding="utf-8")

    stage_transfer_package(package, stage_dir)

    assert not (stage_dir / "OLDMOD.8xp").exists()
    assert (stage_dir / "notes.txt").exists()
    assert (stage_dir / "CALCENG.8xp").read_bytes() == b"CALCENGINE-LAUNCHER"
    assert (stage_dir / "CEBASE.8xp").read_bytes() == b"BASE-MODULE"
    assert (stage_dir / "fileioc.8xv").read_bytes() == b"FILEIOC"
    assert (stage_dir / "README.md").read_text(encoding="utf-8") == "CalcEngine transfer package"
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())

    validation = validate_staged_transfer_directory(package, stage_dir)
    assert validation["success"] is True
    assert validation["missing_entries"] == []
    assert validation["mismatched_entries"] == []
    assert validation["unexpected_entries"] == []


def test_stage_transfer_package_reports_partial_write_and_cleans_staging_temp(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "source")
    stage_dir = tmp_path / "stage"
    original_copy2 = shutil.copy2

    def partial_copy_then_fail(source: Path | str, destination: Path | str, *args, **kwargs):
        destination_path = Path(destination)
        if Path(source).name == "CEBASE.8xp":
            destination_path.write_bytes(b"PARTIAL-STAGE")
            raise OSError("simulated partial write")
        return original_copy2(source, destination, *args, **kwargs)

    monkeypatch.setattr(pipeline_support_module.shutil, "copy2", partial_copy_then_fail)

    with pytest.raises(
        TransferPackageResolutionError,
        match="Staged artifact 'CEBASE.8xp' could not be copied cleanly: simulated partial write",
    ):
        stage_transfer_package(package, stage_dir)

    assert not (stage_dir / ".CEBASE.8xp.calcengine-stage").exists()
    assert not (stage_dir / "CEBASE.8xp").exists()
    validation = validate_staged_transfer_directory(package, stage_dir)
    assert validation["success"] is False
    assert "CEBASE.8xp" in validation["missing_entries"]


def test_validate_staged_transfer_directory_reports_crc_mismatch_without_size_drift(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "source")
    stage_dir = tmp_path / "stage"

    stage_transfer_package(package, stage_dir)

    corrupted_module_path = stage_dir / "CEBASE.8xp"
    original_payload = corrupted_module_path.read_bytes()
    corrupted_module_path.write_bytes(bytes([original_payload[0] ^ 0x5A, *original_payload[1:]]))

    validation = validate_staged_transfer_directory(package, stage_dir)

    assert validation["success"] is False
    assert validation["missing_entries"] == []
    assert validation["mismatched_entries"] == ["CEBASE.8xp"]
    assert validation["unexpected_entries"] == []
    assert corrupted_module_path.stat().st_size == len(original_payload)


def test_validate_staged_transfer_directory_reports_truncated_module_data(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "source")
    stage_dir = tmp_path / "stage"

    stage_transfer_package(package, stage_dir)

    truncated_module_path = stage_dir / "CEBASE.8xp"
    original_payload = truncated_module_path.read_bytes()
    truncated_module_path.write_bytes(original_payload[:-3])

    validation = validate_staged_transfer_directory(package, stage_dir)

    assert validation["success"] is False
    assert validation["missing_entries"] == []
    assert validation["mismatched_entries"] == ["CEBASE.8xp"]
    assert validation["unexpected_entries"] == []
    assert truncated_module_path.stat().st_size == len(original_payload) - 3


def test_stage_transfer_package_preserves_all_35_manifest_module_bytes_and_crcs(
    tmp_path: Path,
    resolved_transfer_package,
) -> None:
    stage_dir = tmp_path / "real-manifest-stage"
    expected_payloads = {
        entry.name: resolved_transfer_package.resolved_paths[entry.name].read_bytes()
        for entry in resolved_transfer_package.module_entries
    }

    stage_transfer_package(resolved_transfer_package, stage_dir)

    validation = validate_staged_transfer_directory(resolved_transfer_package, stage_dir)
    assert validation["success"] is True
    assert len(resolved_transfer_package.module_entries) == 35

    for entry in resolved_transfer_package.module_entries:
        staged_path = stage_dir / entry.name
        assert staged_path.read_bytes() == expected_payloads[entry.name]
        assert staged_path.stat().st_size == entry.size_bytes
        assert compute_crc32_hex(staged_path) == entry.crc32


def test_real_manifest_corruption_injection_is_detected_and_clean_restaging_recovers(
    tmp_path: Path,
    resolved_transfer_package,
) -> None:
    stage_dir = tmp_path / "real-manifest-corruption-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)

    corrupted_entry = resolved_transfer_package.module_entries[17]
    corrupted_path = stage_dir / corrupted_entry.name
    original_payload = corrupted_path.read_bytes()
    flip_index = len(original_payload) // 2
    corrupted_payload = bytearray(original_payload)
    corrupted_payload[flip_index] ^= 0x5A
    corrupted_path.write_bytes(bytes(corrupted_payload))

    corrupted_validation = validate_staged_transfer_directory(resolved_transfer_package, stage_dir)
    assert corrupted_validation["success"] is False
    assert corrupted_validation["missing_entries"] == []
    assert corrupted_validation["mismatched_entries"] == [corrupted_entry.name]
    assert corrupted_validation["unexpected_entries"] == []
    assert compute_crc32_hex(corrupted_path) != corrupted_entry.crc32

    stage_transfer_package(resolved_transfer_package, stage_dir)

    restored_validation = validate_staged_transfer_directory(resolved_transfer_package, stage_dir)
    assert restored_validation["success"] is True
    assert restored_validation["missing_entries"] == []
    assert restored_validation["mismatched_entries"] == []
    assert restored_validation["unexpected_entries"] == []
    assert corrupted_path.read_bytes() == original_payload
    assert compute_crc32_hex(corrupted_path) == corrupted_entry.crc32


def test_get_cemu_timeout_multiplier_ignores_invalid_values(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CEMU_TIMEOUT_MULTIPLIER", "not-a-number")
    assert get_cemu_timeout_multiplier() == 1.0

    monkeypatch.setenv("CEMU_TIMEOUT_MULTIPLIER", "0")
    assert get_cemu_timeout_multiplier() == 1.0


def test_scale_cemu_timeout_seconds_applies_environment_multiplier(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CEMU_TIMEOUT_MULTIPLIER", "1.5")
    assert scale_cemu_timeout_seconds(10.0) == 15.0
    assert scale_cemu_timeout_seconds(2.0, minimum_s=5.0) == 5.0


def test_build_runner_config_scales_default_timeouts(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer")
    rom_path = tmp_path / "Rom.rom"
    headless_path = tmp_path / "cemu-headless.exe"
    autotester_path = tmp_path / "autotester.exe"
    rom_path.write_bytes(b"ROM")
    headless_path.write_bytes(b"HEADLESS")
    autotester_path.write_bytes(b"AUTOTESTER")

    monkeypatch.setenv("CEMU_TIMEOUT_MULTIPLIER", "2")
    config = build_runner_config(
        type(
            "ReadyEnvironment",
            (),
            {
                "ready": True,
                "headless_path": headless_path,
                "rom_path": rom_path,
                "autotester_path": autotester_path,
                "transfer_package": package,
                "blockers": (),
            },
        )()
    )

    assert config.startup_timeout_s == DEFAULT_CEMU_STARTUP_TIMEOUT_S * 2
    assert config.command_timeout_s == DEFAULT_CEMU_COMMAND_TIMEOUT_S * 2
    assert config.shutdown_timeout_s == DEFAULT_CEMU_SHUTDOWN_TIMEOUT_S * 2
    assert config.boot_timeout_s == DEFAULT_CEMU_BOOT_TIMEOUT_S * 2


def test_resolve_real_cemu_environment_blocks_real_lane_on_ci_without_opt_in(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer")
    rom_path = tmp_path / "Rom.rom"
    headless_path = tmp_path / "cemu-headless.exe"
    autotester_path = tmp_path / "autotester.exe"
    gui_path = tmp_path / "CEmu.exe"
    rom_path.write_bytes(b"ROM")
    headless_path.write_bytes(b"HEADLESS")
    autotester_path.write_bytes(b"AUTOTESTER")
    gui_path.write_bytes(b"GUI")

    monkeypatch.setenv("CI", "true")
    monkeypatch.delenv("CALCENGINE_ALLOW_REAL_CEMU_IN_CI", raising=False)
    monkeypatch.setattr(pipeline_support_module, "resolve_rom_path", lambda candidate=None: rom_path)
    monkeypatch.setattr(
        pipeline_support_module,
        "validate_rom_file",
        lambda _path: SimpleNamespace(errors=(), os_version="5.8.0.0022", sha256="locked"),
    )
    monkeypatch.setattr(
        pipeline_support_module,
        "resolve_headless_path",
        lambda repo_root=pipeline_support_module.REPO_ROOT, candidate=None: headless_path,
    )
    monkeypatch.setattr(
        pipeline_support_module,
        "resolve_autotester_path",
        lambda candidate=None: autotester_path,
    )
    monkeypatch.setattr(
        pipeline_support_module,
        "resolve_cemu_gui_path",
        lambda candidate=None: gui_path,
    )
    monkeypatch.setattr(
        pipeline_support_module,
        "resolve_transfer_package",
        lambda **kwargs: package,
    )

    environment = pipeline_support_module.resolve_real_cemu_environment(repo_root=tmp_path)

    assert environment.ready is False
    assert environment.rom_path == rom_path
    assert environment.headless_path == headless_path
    assert environment.autotester_path == autotester_path
    assert environment.transfer_package == package
    assert any("disabled on hosted CI" in blocker for blocker in environment.blockers)


def test_resolve_cemu_tool_paths_supports_wsl_windows_host_fallback(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    fake_home = tmp_path / "home" / "user"
    fake_home.mkdir(parents=True, exist_ok=True)
    users_root = tmp_path / "mnt" / "c" / "Users"
    tool_root = users_root / "user" / "Tools" / "CEmu" / "v2.0"
    autotester_path = tool_root / "autotester.exe"
    gui_path = tool_root / "CEmu.exe"
    autotester_path.parent.mkdir(parents=True, exist_ok=True)
    autotester_path.write_bytes(b"AUTOTESTER")
    gui_path.write_bytes(b"GUI")

    monkeypatch.setattr(pipeline_support_module.Path, "home", classmethod(lambda cls: fake_home))
    monkeypatch.setattr(pipeline_support_module.shutil, "which", lambda name: None)
    monkeypatch.setenv("WSL_WINDOWS_USERS_ROOT", str(users_root))
    monkeypatch.delenv("CEMU_AUTOTESTER_PATH", raising=False)
    monkeypatch.delenv("CEMU_GUI_PATH", raising=False)
    monkeypatch.delenv("CEMU_PATH", raising=False)

    assert pipeline_support_module.resolve_autotester_path() == autotester_path
    assert pipeline_support_module.resolve_cemu_gui_path() == gui_path
