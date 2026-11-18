from __future__ import annotations

import threading
import time
from pathlib import Path

import pytest

from artifice_jailbreak import (
    ArtificeCheckpointError,
    ArtificeCheckpointResult,
    _acquire_checkpoint_lock,
    _load_checkpoint_state,
    _release_checkpoint_lock,
    _write_checkpoint_state,
)
from pipeline_support import DEFAULT_BOOT_SEQUENCE, stage_transfer_package
from stress_harness import build_fake_cemu_stress_runner, stage_transfer_subset


def _module_id(name: str) -> str:
    return Path(name).stem.upper()


def _subset_archive_bytes(resolved_transfer_package, module_names: list[str]) -> int:
    selected_names = set(module_names)
    return sum(
        entry.size_bytes
        for entry in (*resolved_transfer_package.module_entries, *resolved_transfer_package.runtime_entries)
        if entry.name in selected_names or entry.name in resolved_transfer_package.runtime_names
    )


def _checkpoint_result(step: str) -> ArtificeCheckpointResult:
    return ArtificeCheckpointResult(
        step=step,
        description=step,
        expected_crcs=("A32840C8",),
        observed_crc="A32840C8",
        passed=True,
        snapshot_path=None,
        screen_payload={"crc": "A32840C8"},
    )


@pytest.mark.parametrize(
    ("group_size", "cycles", "capacity_slack"),
    [
        (4, 6, 64),
        (5, 8, 96),
        (6, 10, 128),
        (7, 12, 160),
    ],
    ids=[
        "rapid-swap-4x6",
        "rapid-swap-5x8",
        "rapid-swap-6x10",
        "rapid-swap-7x12",
    ],
)
def test_parametric_archive_subset_swaps_preserve_integrity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
    group_size: int,
    cycles: int,
    capacity_slack: int,
) -> None:
    sized_modules = sorted(
        resolved_transfer_package.module_entries,
        key=lambda entry: entry.size_bytes,
        reverse=True,
    )
    group_a = [entry.name for entry in sized_modules[:group_size]]
    group_b = [entry.name for entry in sized_modules[group_size : group_size * 2]]
    group_a_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / f"group-a-{group_size}",
        module_names=group_a,
    )
    group_b_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / f"group-b-{group_size}",
        module_names=group_b,
    )
    runner = build_fake_cemu_stress_runner(
        tmp_path / f"runner-swap-{group_size}-{cycles}",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    runner.set_archive_capacity_bytes(
        max(
            _subset_archive_bytes(resolved_transfer_package, group_a),
            _subset_archive_bytes(resolved_transfer_package, group_b),
        )
        + capacity_slack
    )

    try:
        runner.boot(program_paths=[group_a_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        for cycle_index in range(cycles):
            group_modules = group_a if cycle_index % 2 == 0 else group_b
            group_dir = group_a_dir if cycle_index % 2 == 0 else group_b_dir
            runner._send_command(
                {
                    "cmd": "load_app",
                    "path": str(group_dir),
                    "target_name": _module_id(group_modules[0]),
                    "target_is_asm": True,
                },
                timeout_s=5.0,
            )
            validated = runner.validate_boot_state(
                expected_module_names=group_modules,
                expected_runtime_names=resolved_transfer_package.runtime_names,
                expected_target_name=_module_id(group_modules[0]),
                timeout_s=5.0,
            )
            assert validated["success"] is True
            assert validated["integrity"]["duplicate_entry_names"] == []
            assert validated["integrity"]["mismatched_size_names"] == []

        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert final_state["archive_fragment_count"] == 0
    assert final_state["app_count"] == len(group_b) + len(resolved_transfer_package.runtime_names)
    assert final_state["target_name"] == _module_id(group_b[0] if cycles % 2 == 0 else group_a[0])


@pytest.mark.parametrize(
    ("release_delay_s", "timeout_s", "expect_success", "status", "step_name"),
    [
        (0.02, 0.25, True, "in_progress", "shell_launch"),
        (0.05, 0.30, True, "completed", "program_menu"),
        (0.20, 0.05, False, "in_progress", "usb_connect"),
    ],
    ids=[
        "checkpoint-lock-short-wait",
        "checkpoint-lock-completed-resume",
        "checkpoint-lock-timeout",
    ],
)
def test_parametric_checkpoint_lock_contention_is_actionable(
    tmp_path: Path,
    release_delay_s: float,
    timeout_s: float,
    expect_success: bool,
    status: str,
    step_name: str,
) -> None:
    checkpoint_path = tmp_path / f"{step_name}.checkpoint.json"
    stage_dir = tmp_path / "stage"
    evidence_dir = tmp_path / "evidence"
    artifice_path = tmp_path / "artifice.8xp"
    stage_dir.mkdir(parents=True, exist_ok=True)
    evidence_dir.mkdir(parents=True, exist_ok=True)
    artifice_path.write_bytes(b"ARTIFICE")
    initial_lock_fd = _acquire_checkpoint_lock(checkpoint_path, timeout_s=0.1)

    def release_lock_later() -> None:
        time.sleep(release_delay_s)
        _release_checkpoint_lock(checkpoint_path, initial_lock_fd)

    release_thread = threading.Thread(target=release_lock_later, daemon=True)
    release_thread.start()

    try:
        if expect_success:
            lock_fd = _acquire_checkpoint_lock(checkpoint_path, timeout_s=timeout_s, poll_interval_s=0.01)
            try:
                written = _write_checkpoint_state(
                    checkpoint_path,
                    status=status,
                    stage_dir=stage_dir,
                    evidence_dir=evidence_dir,
                    artifice_path=artifice_path,
                    transfer_count=39,
                    module_count=35,
                    runtime_dependency_count=4,
                    last_completed_step=step_name,
                    checkpoints=(_checkpoint_result(step_name),),
                )
            finally:
                _release_checkpoint_lock(checkpoint_path, lock_fd)
            loaded = _load_checkpoint_state(checkpoint_path)
            assert loaded is not None
            assert loaded.status == status
            assert loaded.last_completed_step == step_name
            assert loaded.integrity_digest == written.integrity_digest
        else:
            with pytest.raises(ArtificeCheckpointError, match="already locked"):
                _acquire_checkpoint_lock(checkpoint_path, timeout_s=timeout_s, poll_interval_s=0.01)
    finally:
        release_thread.join(timeout=1.0)
        _release_checkpoint_lock(checkpoint_path, None)


@pytest.mark.parametrize(
    ("fill_ratio", "fragment_every", "heap_multiplier"),
    [
        (0.94, 3, 1.5),
        (0.96, 2, 2.0),
        (0.98, 2, 2.5),
    ],
    ids=[
        "pressure-94",
        "pressure-96",
        "pressure-98",
    ],
)
def test_parametric_memory_pressure_churn_keeps_archive_recoverable(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
    fill_ratio: float,
    fragment_every: int,
    heap_multiplier: float,
) -> None:
    stage_dir = tmp_path / f"pressure-stage-{int(fill_ratio * 100)}"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / f"runner-pressure-{int(fill_ratio * 100)}",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    switched_modules = [_module_id(name) for name in resolved_transfer_package.module_names[:9]]

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        initial_state = runner.get_state(timeout_s=5.0).payload
        runner.set_archive_capacity_bytes(
            int(initial_state["archive_used_bytes"]) + max(128, int(initial_state["archive_used_bytes"] * 0.05))
        )

        for index, module_name in enumerate(switched_modules, start=1):
            runner.execute_module(module_name, action="allocate_heap", heap_multiplier=heap_multiplier)
            if index % fragment_every == 0:
                fragmented = runner.simulate_archive_fragmentation(
                    fill_ratio=fill_ratio,
                    delete_middle_count=2 + (index % 3),
                )
                assert fragmented["fragment_count"] >= 1
                recovered = runner.defragment_archive()
                assert recovered["success"] is True
                runner._send_command(
                    {
                        "cmd": "load_app",
                        "path": str(stage_dir),
                        "target_name": module_name,
                        "target_is_asm": True,
                    },
                    timeout_s=5.0,
                )

        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert int(final_state["archive_used_bytes"]) < int(final_state["archive_capacity_bytes"])
    assert int(final_state["peak_heap_bytes"]) > 0
    assert final_state["current_heap_bytes"] == 0
    assert final_state["oom_count"] == 0
    assert final_state["heap_corruption_detected"] is False
    assert final_state["archive_fragment_count"] == 0
    assert int(final_state["app_count"]) == resolved_transfer_package.transfer_count
