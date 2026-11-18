from __future__ import annotations

from pathlib import Path

import pytest

from cemu_runner import CEmuCommandError, CEmuRunner
from pipeline_support import DEFAULT_BOOT_SEQUENCE, get_real_cemu_status, stage_transfer_package
from stress_harness import build_fake_cemu_stress_runner, stage_transfer_subset


REAL_CEMU_READY, REAL_CEMU_SKIP_REASON = get_real_cemu_status()


def test_cemu_boots_fake_backend(
    fake_cemu_runner: CEmuRunner,
    staged_transfer_package_dir: Path,
) -> None:
    summary = fake_cemu_runner.boot(
        program_paths=[staged_transfer_package_dir],
        ready_sequence=DEFAULT_BOOT_SEQUENCE,
    )

    assert summary.state["rom_loaded"] is True
    assert summary.state["running"] is True
    assert summary.state["app_count"] == 39
    assert summary.state["target_name"] == "CALCENG"
    assert summary.boot_duration_ms >= 0
    assert summary.wait_for_boot_duration_ms >= 0
    assert summary.boot_state_poll_count >= 1
    assert summary.boot_duration_ms <= 5_000


@pytest.mark.skipif(not REAL_CEMU_READY, reason=REAL_CEMU_SKIP_REASON)
def test_cemu_boots_real_rom(real_cemu_config, staged_transfer_package_dir: Path) -> None:
    assert real_cemu_config is not None

    with CEmuRunner(real_cemu_config) as runner:
        summary = runner.boot(
            program_paths=[staged_transfer_package_dir],
            ready_sequence=DEFAULT_BOOT_SEQUENCE,
        )
        screen = runner.capture_screen()

    assert summary.state["rom_loaded"] is True
    assert summary.state["running"] is True
    assert summary.state["app_count"] == 39
    assert summary.state["target_name"] == "CALCENG"
    assert summary.boot_duration_ms <= round(real_cemu_config.boot_timeout_s * 1000)
    assert summary.wait_for_boot_duration_ms <= summary.boot_duration_ms
    assert summary.boot_state_poll_count >= 1
    assert screen.payload["crc"] in {"128C50A4", "B76BC405"}


def test_cemu_boot_validates_cross_module_subset_state(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    module_subset = [
        resolved_transfer_package.module_names[0],
        resolved_transfer_package.module_names[7],
        resolved_transfer_package.module_names[19],
        resolved_transfer_package.module_names[34],
    ]
    subset_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "boot-subset",
        module_names=module_subset,
    )
    runner = build_fake_cemu_stress_runner(
        tmp_path / "boot-subset-runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        summary = runner.boot(
            program_paths=[subset_dir],
            ready_sequence=DEFAULT_BOOT_SEQUENCE,
            timeout_s=5.0,
        )
        boot_validation = runner.validate_boot_state(
            expected_module_names=module_subset,
            expected_runtime_names=resolved_transfer_package.runtime_names,
            expected_target_name="CALCENG",
            timeout_s=5.0,
        )
    finally:
        runner.shutdown()

    assert summary.state["rom_loaded"] is True
    assert summary.state["running"] is True
    assert boot_validation["success"] is True
    assert boot_validation["state"]["app_count"] == len(module_subset) + len(resolved_transfer_package.runtime_names)
    assert boot_validation["state"]["target_name"] == "CALCENG"
    assert boot_validation["integrity"]["success"] is True


def test_cemu_boot_validates_maximum_module_count_state(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "boot-max-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "boot-max-runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        summary = runner.boot(
            program_paths=[stage_dir],
            ready_sequence=DEFAULT_BOOT_SEQUENCE,
            timeout_s=5.0,
        )
        boot_validation = runner.validate_boot_state(
            expected_module_names=list(resolved_transfer_package.module_names),
            expected_runtime_names=list(resolved_transfer_package.runtime_names),
            expected_target_name="CALCENG",
            timeout_s=5.0,
        )
    finally:
        runner.shutdown()

    assert summary.state["rom_loaded"] is True
    assert summary.state["running"] is True
    assert boot_validation["success"] is True
    assert boot_validation["state"]["app_count"] == resolved_transfer_package.transfer_count
    assert boot_validation["state"]["archive_fragment_count"] == 0
    assert boot_validation["integrity"]["success"] is True


def test_cemu_boot_rejects_capacity_conflict_for_large_module_combination(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    largest_modules = [
        entry.name
        for entry in sorted(
            resolved_transfer_package.module_entries,
            key=lambda entry: entry.size_bytes,
            reverse=True,
        )[:10]
    ]
    subset_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "boot-capacity-conflict",
        module_names=largest_modules,
    )
    required_bytes = sum(
        entry.size_bytes
        for entry in (*resolved_transfer_package.module_entries, *resolved_transfer_package.runtime_entries)
        if entry.name in largest_modules or entry.name in resolved_transfer_package.runtime_names
    )
    runner = build_fake_cemu_stress_runner(
        tmp_path / "boot-capacity-conflict-runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.set_archive_capacity_bytes(required_bytes - 1)
        with pytest.raises(CEmuCommandError, match="archive requires"):
            runner.boot(
                program_paths=[subset_dir],
                ready_sequence=DEFAULT_BOOT_SEQUENCE,
                timeout_s=5.0,
            )
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert state["app_count"] == 0
    assert state["archive_entry_names"] == []
    assert state["archive_used_bytes"] == 0
