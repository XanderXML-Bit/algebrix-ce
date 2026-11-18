from __future__ import annotations

import artifice_jailbreak as artifice_module
import threading
from pathlib import Path

import pytest

from artifice_jailbreak import evaluate_artifice_rom_compatibility, run_artifice_jailbreak
from cemu_runner import CEmuCommandError, CEmuProcessTimeoutError
from pipeline_support import DEFAULT_BOOT_SEQUENCE, stage_transfer_package
from rom_validation import validate_rom_file
from stress_harness import (
    build_fake_cemu_stress_runner,
    stage_transfer_subset,
    write_fake_artifice_baseline,
    write_synthetic_rom,
)


def _module_id(name: str) -> str:
    return Path(name).stem.upper()


def _write_artifice_assets(tmp_path: Path) -> tuple[Path, Path]:
    tmp_path.mkdir(parents=True, exist_ok=True)
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    baseline_path = write_fake_artifice_baseline(tmp_path / "artifice-baseline.json")
    return artifice_path, baseline_path


def _load_target_module(runner, stage_dir: Path, module_name: str, *, timeout_s: float = 5.0) -> None:
    runner._send_command(
        {
            "cmd": "load_app",
            "path": str(stage_dir),
            "target_name": module_name,
            "target_is_asm": True,
        },
        timeout_s=timeout_s,
    )


def _subset_entry_names(resolved_transfer_package, module_names: list[str]) -> list[str]:
    return [*module_names, *resolved_transfer_package.runtime_names]


def _subset_archive_bytes(resolved_transfer_package, module_names: list[str]) -> int:
    selected_names = set(module_names)
    return sum(
        entry.size_bytes
        for entry in (*resolved_transfer_package.module_entries, *resolved_transfer_package.runtime_entries)
        if entry.name in selected_names or entry.name in resolved_transfer_package.runtime_names
    )


def _boot_target_name(entry_names: list[str]) -> str:
    return Path(sorted(entry_names, key=str.lower)[0]).stem.upper()


def test_multi_program_rapid_switching_does_not_leak_module_state(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "rapid-switch-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    module_a = _module_id(resolved_transfer_package.module_names[0])
    module_b = _module_id(resolved_transfer_package.module_names[1])

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        written = runner.execute_module(module_a, action="write_scratch", scratch_value="A-ONLY")
        switched = runner.execute_module(module_b, action="read_scratch")
        returned = runner.execute_module(module_a, action="read_scratch")
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert written["scratch"] == "A-ONLY"
    assert switched["scratch"] == ""
    assert returned["scratch"] == "A-ONLY"
    assert state["target_name"] == module_a
    assert state["module_history"][-3:] == [module_a, module_b, module_a]


def test_concurrent_module_load_requests_preserve_archive_integrity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "concurrent-load-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    module_a = _module_id(resolved_transfer_package.module_names[0])
    module_b = _module_id(resolved_transfer_package.module_names[1])
    barrier = threading.Barrier(3)
    results: list[dict[str, object]] = []
    failures: list[BaseException] = []

    def load_target(target_name: str) -> None:
        try:
            barrier.wait(timeout=2.0)
            command_result = runner._send_command(
                {
                    "cmd": "load_app",
                    "path": str(stage_dir),
                    "target_name": target_name,
                    "target_is_asm": True,
                },
                timeout_s=5.0,
            )
            results.append(dict(command_result.payload))
        except BaseException as exc:  # pragma: no cover - surfaced by the assertion below
            failures.append(exc)

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        thread_a = threading.Thread(target=load_target, args=(module_a,), daemon=True)
        thread_b = threading.Thread(target=load_target, args=(module_b,), daemon=True)
        thread_a.start()
        thread_b.start()
        barrier.wait(timeout=2.0)
        thread_a.join(timeout=5.0)
        thread_b.join(timeout=5.0)
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert failures == []
    assert len(results) == 2
    assert set(runner.load_app_history[-2:]) == {module_a, module_b}
    assert state["app_count"] == resolved_transfer_package.transfer_count
    assert state["archive_fragment_count"] == 0
    assert set(state["archive_entry_names"]) == {
        *resolved_transfer_package.module_names,
        *resolved_transfer_package.runtime_names,
    }
    assert state["target_name"] in {module_a, module_b}


def test_rapid_archive_group_swaps_under_memory_pressure_preserve_integrity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    sized_modules = sorted(
        resolved_transfer_package.module_entries,
        key=lambda entry: entry.size_bytes,
        reverse=True,
    )[:15]
    group_a = [entry.name for entry in sized_modules[0:5]]
    group_b = [entry.name for entry in sized_modules[5:10]]
    group_c = [entry.name for entry in sized_modules[10:15]]
    group_a_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "swap-group-a",
        module_names=group_a,
    )
    group_b_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "swap-group-b",
        module_names=group_b,
    )
    group_c_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "swap-group-c",
        module_names=group_c,
    )
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-archive-swap-pressure",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    required_capacity_bytes = max(
        _subset_archive_bytes(resolved_transfer_package, group_a),
        _subset_archive_bytes(resolved_transfer_package, group_b),
        _subset_archive_bytes(resolved_transfer_package, group_c),
    ) + 96
    expected_cycle = [
        ("group_a", group_a_dir, group_a),
        ("group_b", group_b_dir, group_b),
        ("group_c", group_c_dir, group_c),
        ("group_a", group_a_dir, group_a),
        ("group_c", group_c_dir, group_c),
        ("group_b", group_b_dir, group_b),
    ]
    usage_ratios: list[float] = []

    try:
        runner.set_archive_capacity_bytes(required_capacity_bytes)
        runner.boot(program_paths=[group_a_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        initial_boot = runner.validate_boot_state(
            expected_module_names=group_a,
            expected_runtime_names=resolved_transfer_package.runtime_names,
            expected_target_name=_boot_target_name(_subset_entry_names(resolved_transfer_package, group_a)),
            timeout_s=5.0,
        )
        assert initial_boot["success"] is True

        previous_expected_names = _subset_entry_names(resolved_transfer_package, group_a)
        for _group_label, group_dir, group_modules in expected_cycle[1:]:
            before_integrity = runner.verify_archive_integrity(
                expected_entry_names=previous_expected_names,
            )
            assert before_integrity["success"] is True
            runner._send_command(
                {
                    "cmd": "load_app",
                    "path": str(group_dir),
                    "target_name": _module_id(group_modules[0]),
                    "target_is_asm": True,
                },
                timeout_s=5.0,
            )
            after_boot = runner.validate_boot_state(
                expected_module_names=group_modules,
                expected_runtime_names=resolved_transfer_package.runtime_names,
                expected_target_name=_module_id(group_modules[0]),
                timeout_s=5.0,
            )
            assert after_boot["success"] is True
            usage_ratios.append(
                int(after_boot["state"]["archive_used_bytes"]) / int(after_boot["state"]["archive_capacity_bytes"])
            )
            previous_expected_names = _subset_entry_names(resolved_transfer_package, group_modules)

        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert usage_ratios != []
    assert max(usage_ratios) > 0.95
    assert final_state["archive_fragment_count"] == 0
    assert final_state["app_count"] == len(group_b) + len(resolved_transfer_package.runtime_names)
    assert final_state["target_name"] == _module_id(group_b[0])


def test_interrupted_archive_group_swap_restores_previous_group_before_clean_retry(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    group_a = list(resolved_transfer_package.module_names[:6])
    group_b = list(resolved_transfer_package.module_names[6:12])
    group_a_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "interrupt-group-a",
        module_names=group_a,
    )
    group_b_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "interrupt-group-b",
        module_names=group_b,
    )
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-interrupted-group-swap",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.set_archive_capacity_bytes(
            max(
                _subset_archive_bytes(resolved_transfer_package, group_a),
                _subset_archive_bytes(resolved_transfer_package, group_b),
            ) + 64
        )
        runner.boot(program_paths=[group_a_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        baseline_integrity = runner.verify_archive_integrity(
            expected_entry_names=_subset_entry_names(resolved_transfer_package, group_a),
        )
        baseline_digest = baseline_integrity["archive_digest"]
        runner.configure_transfer_interrupts("payload")
        with pytest.raises(CEmuProcessTimeoutError, match="payload"):
            runner._send_command(
                {
                    "cmd": "load_app",
                    "path": str(group_b_dir),
                    "target_name": _module_id(group_b[0]),
                    "target_is_asm": True,
                },
                timeout_s=5.0,
            )
        interrupted_state = runner.get_state(timeout_s=5.0).payload
        interrupted_integrity = runner.verify_archive_integrity(
            expected_entry_names=_subset_entry_names(resolved_transfer_package, group_b),
        )
        runner.reset(timeout_s=5.0)
        recovered_integrity = runner.verify_archive_integrity(
            expected_entry_names=_subset_entry_names(resolved_transfer_package, group_a),
        )
        runner._send_command(
            {
                "cmd": "load_app",
                "path": str(group_b_dir),
                "target_name": _module_id(group_b[0]),
                "target_is_asm": True,
            },
            timeout_s=5.0,
        )
        retried_boot = runner.validate_boot_state(
            expected_module_names=group_b,
            expected_runtime_names=resolved_transfer_package.runtime_names,
            expected_target_name=_module_id(group_b[0]),
            timeout_s=5.0,
        )
        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert baseline_integrity["success"] is True
    assert interrupted_state["last_transfer_stage"] == "payload"
    assert interrupted_integrity["success"] is False
    assert interrupted_integrity["missing_entry_names"] != []
    assert recovered_integrity["success"] is True
    assert recovered_integrity["archive_digest"] == baseline_digest
    assert retried_boot["success"] is True
    assert final_state["transfer_rollbacks"] >= 1
    assert final_state["target_name"] == _module_id(group_b[0])


def test_archive_group_swaps_preserve_appvars_and_integrity_near_full_capacity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    sized_modules = sorted(
        resolved_transfer_package.module_entries,
        key=lambda entry: entry.size_bytes,
        reverse=True,
    )[:8]
    group_a = [entry.name for entry in sized_modules[0:4]]
    group_b = [entry.name for entry in sized_modules[4:8]]
    group_a_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "appvar-group-a",
        module_names=group_a,
    )
    group_b_dir = stage_transfer_subset(
        resolved_transfer_package,
        tmp_path / "appvar-group-b",
        module_names=group_b,
    )
    settings_payload = b"\x57\xCE\x02\x06\x01\x09\x0A\x0B"
    history_payload = b"ans->x:det([[1,2][3,4]])"
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-appvar-archive-swap",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[group_a_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        runner.create_appvar("CESETTNG", settings_payload)
        runner.create_appvar("CEHISTOR", history_payload)
        appvar_overhead_bytes = sum(
            runner.get_appvar_info(name)["archive_bytes"]
            for name in ("CESETTNG", "CEHISTOR")
        )
        required_capacity_bytes = max(
            _subset_archive_bytes(resolved_transfer_package, group_a),
            _subset_archive_bytes(resolved_transfer_package, group_b),
        ) + appvar_overhead_bytes + 96
        runner.set_archive_capacity_bytes(required_capacity_bytes)

        for cycle_index in range(8):
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
            boot_state = runner.validate_boot_state(
                expected_module_names=group_modules,
                expected_runtime_names=resolved_transfer_package.runtime_names,
                expected_target_name=_module_id(group_modules[0]),
                timeout_s=5.0,
            )
            assert boot_state["success"] is True
            assert runner.read_appvar("CESETTNG") == settings_payload
            assert runner.read_appvar("CEHISTOR") == history_payload

        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    usage_ratio = int(final_state["archive_used_bytes"]) / int(final_state["archive_capacity_bytes"])

    assert usage_ratio > 0.90
    assert final_state["appvar_names"] == ["CEHISTOR", "CESETTNG"]
    assert final_state["appvar_count"] == 2
    assert final_state["archive_fragment_count"] == 0
    assert final_state["target_name"] == _module_id(group_b[0])


def test_archive_fragmentation_defrag_and_reload_restore_integrity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "fragmentation-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        fragmented = runner.simulate_archive_fragmentation(fill_ratio=0.96, delete_middle_count=5)
        fragmented_digest = runner.archive_digest()
        defragged = runner.defragment_archive()
        runner._send_command(
            {
                "cmd": "load_app",
                "path": str(stage_dir),
                "target_name": _module_id(resolved_transfer_package.module_names[0]),
                "target_is_asm": True,
            },
            timeout_s=5.0,
        )
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert fragmented["fragment_count"] >= 1
    assert fragmented["deleted_entries"] != []
    assert fragmented["archive_used_bytes"] < int(state["archive_capacity_bytes"])
    assert fragmented_digest != tuple((entry.name, entry.size_bytes) for entry in [])
    assert defragged["success"] is True
    assert defragged["fragment_count"] == 0
    assert state["archive_fragment_count"] == 0
    assert state["app_count"] == resolved_transfer_package.transfer_count
    assert set(state["archive_entry_names"]) == {
        *resolved_transfer_package.module_names,
        *resolved_transfer_package.runtime_names,
    }


def test_archive_defragmentation_stays_clean_across_twelve_fragment_reload_cycles(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "defrag-cycle-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        for cycle in range(12):
            fragmented = runner.simulate_archive_fragmentation(
                fill_ratio=0.96,
                delete_middle_count=2 + (cycle % 3),
            )
            assert fragmented["fragment_count"] >= 1
            defragged = runner.defragment_archive()
            assert defragged["success"] is True
            assert defragged["fragment_count"] == 0
            runner._send_command(
                {
                    "cmd": "load_app",
                    "path": str(stage_dir),
                    "target_name": _module_id(resolved_transfer_package.module_names[0]),
                    "target_is_asm": True,
                },
                timeout_s=5.0,
            )
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert state["defrag_count"] == 12
    assert state["archive_fragment_count"] == 0
    assert state["app_count"] == resolved_transfer_package.transfer_count
    assert set(state["archive_entry_names"]) == {
        *resolved_transfer_package.module_names,
        *resolved_transfer_package.runtime_names,
    }


def test_artifice_attempt_while_module_executes_is_gracefully_rejected(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    artifice_path, baseline_path = _write_artifice_assets(tmp_path)
    stage_dir = tmp_path / "execution-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=1.0,
        boot_timeout_s=2.0,
    )
    started_event = threading.Event()
    release_event = threading.Event()
    execution_thread = threading.Thread(
        target=runner.hold_module_execution,
        kwargs={
            "module_name": _module_id(resolved_transfer_package.module_names[0]),
            "started_event": started_event,
            "release_event": release_event,
        },
        daemon=True,
    )

    try:
        runner.start()
        runner.load_rom(timeout_s=2.0)
        runner._send_command(
            {
                "cmd": "load_app",
                "path": str(stage_dir),
                "target_name": _module_id(resolved_transfer_package.module_names[0]),
                "target_is_asm": True,
            },
            timeout_s=2.0,
        )
        execution_thread.start()
        assert started_event.wait(timeout=1.0) is True
        result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "rejected-evidence",
            timeout_s=1.0,
            usb_retry_count=1,
        )
    finally:
        release_event.set()
        execution_thread.join(timeout=2.0)
        runner.shutdown()

    assert result.success is False
    assert "cannot start artifice while module" in result.failure_reason.lower()
    assert runner.rejected_jailbreaks == 1
    assert any("usb_connect" in line for line in result.diagnostics)


def test_artifice_attempt_can_queue_until_module_execution_finishes(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    artifice_path, baseline_path = _write_artifice_assets(tmp_path)
    stage_dir = tmp_path / "execution-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )
    runner.queue_artifice_requests = True
    started_event = threading.Event()
    release_event = threading.Event()
    execution_thread = threading.Thread(
        target=runner.hold_module_execution,
        kwargs={
            "module_name": _module_id(resolved_transfer_package.module_names[1]),
            "started_event": started_event,
            "release_event": release_event,
        },
        daemon=True,
    )
    result_box: dict[str, object] = {}

    def run_queued_jailbreak() -> None:
        result_box["result"] = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "queued-evidence",
            timeout_s=2.0,
        )

    queued_thread = threading.Thread(target=run_queued_jailbreak, daemon=True)

    try:
        runner.start()
        runner.load_rom(timeout_s=2.0)
        runner._send_command(
            {
                "cmd": "load_app",
                "path": str(stage_dir),
                "target_name": _module_id(resolved_transfer_package.module_names[1]),
                "target_is_asm": True,
            },
            timeout_s=2.0,
        )
        execution_thread.start()
        assert started_event.wait(timeout=1.0) is True
        queued_thread.start()
        assert runner.wait_for_artifice_conflict(timeout_s=1.0) is True
        release_event.set()
        queued_thread.join(timeout=5.0)
    finally:
        execution_thread.join(timeout=2.0)
        runner.shutdown()

    queued_result = result_box["result"]
    assert queued_result.success is True
    assert runner.queued_jailbreaks == 1
    assert queued_result.usb_retry_count == 1


def test_artifice_usb_conflict_can_reject_then_queue_without_losing_archive_state(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    artifice_path, baseline_path = _write_artifice_assets(tmp_path / "reject-then-queue")
    stage_dir = tmp_path / "reject-then-queue-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-reject-then-queue",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )
    result_box: dict[str, object] = {}

    def run_queued_attempt() -> None:
        result_box["result"] = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "queued-after-reject-evidence",
            timeout_s=2.0,
        )

    queued_thread = threading.Thread(target=run_queued_attempt, daemon=True)

    try:
        runner.start()
        runner.load_rom(timeout_s=2.0)
        first_started_event = threading.Event()
        first_release_event = threading.Event()
        first_execution_thread = threading.Thread(
            target=runner.hold_module_execution,
            kwargs={
                "module_name": _module_id(resolved_transfer_package.module_names[2]),
                "started_event": first_started_event,
                "release_event": first_release_event,
            },
            daemon=True,
        )
        _load_target_module(runner, stage_dir, _module_id(resolved_transfer_package.module_names[2]), timeout_s=2.0)
        first_execution_thread.start()
        assert first_started_event.wait(timeout=1.0) is True

        rejected_result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "rejected-before-queue-evidence",
            timeout_s=1.0,
            usb_retry_count=1,
        )

        first_release_event.set()
        first_execution_thread.join(timeout=2.0)

        runner.queue_artifice_requests = True
        second_started_event = threading.Event()
        second_release_event = threading.Event()
        second_execution_thread = threading.Thread(
            target=runner.hold_module_execution,
            kwargs={
                "module_name": _module_id(resolved_transfer_package.module_names[3]),
                "started_event": second_started_event,
                "release_event": second_release_event,
            },
            daemon=True,
        )
        _load_target_module(runner, stage_dir, _module_id(resolved_transfer_package.module_names[3]), timeout_s=2.0)
        second_execution_thread.start()
        assert second_started_event.wait(timeout=1.0) is True
        queued_thread.start()
        queued_thread.join(timeout=0.2)
        assert queued_thread.is_alive() is True
        second_release_event.set()
        queued_thread.join(timeout=5.0)
        second_execution_thread.join(timeout=2.0)
        final_state = runner.get_state(timeout_s=2.0).payload
    finally:
        runner.shutdown()

    queued_result = result_box["result"]

    assert rejected_result.success is False
    assert rejected_result.rollback_performed is True
    assert "cannot start artifice while module" in rejected_result.failure_reason.lower()
    assert queued_result.success is True
    assert queued_result.rollback_performed is False
    assert runner.rejected_jailbreaks == 1
    assert int(final_state["app_count"]) == resolved_transfer_package.transfer_count + 1
    assert final_state["target_name"] == "A"
    assert final_state["archive_fragment_count"] == 0
    assert final_state["executing_module"] == ""
    assert any("cannot start arTIfiCE while module" in line for line in rejected_result.diagnostics)
    assert any("USB simulation attempt 1/3" in line for line in queued_result.diagnostics)


def test_artifice_jailbreak_succeeds_with_archive_above_ninety_five_percent_capacity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    artifice_path, baseline_path = _write_artifice_assets(tmp_path / "nearly-full-artifice")
    stage_dir = tmp_path / "nearly-full-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-nearly-full",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )

    try:
        required_archive_bytes = (
            sum(entry.size_bytes for entry in resolved_transfer_package.module_entries)
            + sum(entry.size_bytes for entry in resolved_transfer_package.runtime_entries)
            + artifice_path.stat().st_size
        )
        runner.set_archive_capacity_bytes(required_archive_bytes + max(1, required_archive_bytes // 40))
        result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "nearly-full-evidence",
            timeout_s=2.0,
        )
        final_state = runner.get_state(timeout_s=2.0).payload
    finally:
        runner.shutdown()

    usage_ratio = int(final_state["archive_used_bytes"]) / int(final_state["archive_capacity_bytes"])

    assert result.success is True
    assert result.rollback_performed is False
    assert usage_ratio > 0.95
    assert int(final_state["archive_used_bytes"]) <= int(final_state["archive_capacity_bytes"])
    assert int(final_state["app_count"]) == resolved_transfer_package.transfer_count + 1
    assert final_state["target_name"] == "A"


@pytest.mark.parametrize("interrupted_stage", ["handshake", "payload", "commit"])
def test_power_cycle_mid_transfer_recovery_retries_and_restores_archive(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
    interrupted_stage: str,
) -> None:
    artifice_path, baseline_path = _write_artifice_assets(tmp_path / interrupted_stage)
    runner = build_fake_cemu_stress_runner(
        tmp_path / f"runner-{interrupted_stage}",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )
    runner.configure_transfer_interrupts(interrupted_stage)

    try:
        result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / f"evidence-{interrupted_stage}",
            timeout_s=2.0,
            usb_retry_count=2,
            usb_retry_delay_ms=1,
            usb_retry_max_delay_ms=1,
            usb_retry_jitter_ratio=0.0,
        )
        state = runner.get_state(timeout_s=2.0).payload
    finally:
        runner.shutdown()

    assert result.success is True
    assert result.usb_retry_count == 2
    assert runner.transfer_rollbacks == 1
    assert state["last_transfer_stage"] == ""
    assert state["app_count"] == resolved_transfer_package.transfer_count + 1
    assert any(interrupted_stage in line for line in result.diagnostics)
    assert any("recovered on attempt 2/2" in line for line in result.diagnostics)


def test_power_cycle_retry_uses_exponential_backoff_and_rolls_back_each_partial_transfer(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    artifice_path, baseline_path = _write_artifice_assets(tmp_path / "retry-backoff")
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-retry-backoff",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )
    runner.configure_transfer_interrupts("payload", "commit")
    sleep_calls: list[float] = []

    monkeypatch.setattr(
        artifice_module,
        "_sleep_before_usb_retry",
        lambda delay_s: sleep_calls.append(round(float(delay_s), 3)),
    )

    try:
        result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "retry-backoff-evidence",
            timeout_s=2.0,
            usb_retry_count=3,
            usb_retry_delay_ms=10,
            usb_retry_max_delay_ms=40,
            usb_retry_jitter_ratio=0.0,
        )
        state = runner.get_state(timeout_s=2.0).payload
    finally:
        runner.shutdown()

    assert result.success is True
    assert result.usb_retry_count == 3
    assert runner.transfer_rollbacks == 2
    assert sleep_calls == [0.01, 0.02]
    assert state["last_transfer_stage"] == ""
    assert int(state["app_count"]) == resolved_transfer_package.transfer_count + 1
    assert any("USB simulation retry backoff before the next attempt: 0.010s" in line for line in result.diagnostics)
    assert any("USB simulation retry backoff before the next attempt: 0.020s" in line for line in result.diagnostics)
    assert any("USB simulation recovered on attempt 3/3 after 2 earlier failure(s)." in line for line in result.diagnostics)


def test_module_dependency_chain_hands_off_shared_list_output(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "dependency-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    producer_module = _module_id(resolved_transfer_package.module_names[2])
    consumer_module = _module_id(resolved_transfer_package.module_names[3])

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        produced = runner.execute_module(
            producer_module,
            action="produce_list",
            list_name="L6",
            list_values=(2, 3, 5, 8, 13),
        )
        consumed = runner.execute_module(
            consumer_module,
            action="consume_list",
            list_name="L6",
        )
    finally:
        runner.shutdown()

    assert produced["shared_list"] == [2, 3, 5, 8, 13]
    assert consumed["shared_list"] == [2, 3, 5, 8, 13]
    assert consumed["source_module"] == producer_module
    assert consumed["checksum"] == 31
    assert consumed["screen_crc"] != ""


def test_three_new_module_pairs_preserve_crc_determinism_across_reloads(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "determinism-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-determinism",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    pair_specs = [
        (
            _module_id(resolved_transfer_package.module_names[4]),
            _module_id(resolved_transfer_package.module_names[5]),
            "L3",
            (1, 4, 9, 16),
        ),
        (
            _module_id(resolved_transfer_package.module_names[6]),
            _module_id(resolved_transfer_package.module_names[7]),
            "L4",
            (3, 1, 4, 1, 5),
        ),
        (
            _module_id(resolved_transfer_package.module_names[8]),
            _module_id(resolved_transfer_package.module_names[9]),
            "L5",
            (2, 7, 1, 8, 2, 8),
        ),
    ]
    observed_signatures: dict[tuple[str, str], list[tuple[str, str]]] = {
        (producer, consumer): []
        for producer, consumer, _list_name, _values in pair_specs
    }

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        for cycle in range(4):
            for producer, consumer, list_name, values in pair_specs:
                _load_target_module(runner, stage_dir, producer)
                produced = runner.execute_module(
                    producer,
                    action="produce_list",
                    list_name=list_name,
                    list_values=values,
                )
                consumed = runner.execute_module(
                    consumer,
                    action="consume_list",
                    list_name=list_name,
                )
                observed_signatures[(producer, consumer)].append(
                    (produced["screen_crc"], consumed["screen_crc"])
                )
            if cycle < 3:
                fragmented = runner.simulate_archive_fragmentation(fill_ratio=0.965, delete_middle_count=3)
                assert fragmented["fragment_count"] >= 1
                defragged = runner.defragment_archive()
                assert defragged["success"] is True
                _load_target_module(runner, stage_dir, pair_specs[0][0])
        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    for pair, signatures in observed_signatures.items():
        assert len(signatures) == 4
        assert len(set(signatures)) == 1, f"Pair {pair[0]}->{pair[1]} drifted CRCs across repeated runs: {signatures}"

    assert final_state["archive_fragment_count"] == 0
    assert int(final_state["app_count"]) == resolved_transfer_package.transfer_count


def test_full_35_module_sequential_load_keeps_archive_clean(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "sequential-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        results = [
            runner.execute_module(_module_id(name), action="self_test")
            for name in resolved_transfer_package.module_names
        ]
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert len(results) == 35
    assert len({result["module_name"] for result in results}) == 35
    assert all(result["screen_crc"] for result in results)
    assert state["app_count"] == resolved_transfer_package.transfer_count
    assert state["archive_fragment_count"] == 0
    assert state["module_history"][-35:] == [
        _module_id(name) for name in resolved_transfer_package.module_names
    ]


def test_memory_pressure_with_largest_modules_releases_heap_cleanly(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "pressure-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    largest_modules = sorted(
        resolved_transfer_package.module_entries,
        key=lambda entry: entry.size_bytes,
        reverse=True,
    )[:2]

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        first = runner.execute_module(_module_id(largest_modules[0].name), action="allocate_heap", heap_multiplier=4.0)
        second = runner.execute_module(_module_id(largest_modules[1].name), action="allocate_heap", heap_multiplier=4.0)
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert first["allocation_bytes"] >= largest_modules[0].size_bytes
    assert second["allocation_bytes"] >= largest_modules[1].size_bytes
    assert state["current_heap_bytes"] == 0
    assert state["peak_heap_bytes"] >= max(first["allocation_bytes"], second["allocation_bytes"])
    assert state["oom_count"] == 0
    assert state["heap_corruption_detected"] is False


def test_archive_pressure_loads_modules_until_capacity_rejects_cleanly(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "archive-reject-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-archive-reject",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    requested_modules = [_module_id(name) for name in resolved_transfer_package.module_names[:6]]
    module_sizes = [
        entry.size_bytes
        for entry in resolved_transfer_package.module_entries
        if _module_id(entry.name) in requested_modules
    ]

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        initial_state = runner.get_state(timeout_s=5.0).payload
        runner.set_archive_capacity_bytes(
            int(initial_state["archive_used_bytes"]) + sum(module_sizes[:2]) + 128
        )
        pressure = runner.load_modules_until_archive_rejects(requested_modules, repeat=10)
        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert pressure["loaded_count"] >= 1
    assert pressure["loaded_entry_names"] != []
    assert pressure["rejected_module_name"] in requested_modules
    assert int(pressure["rejected_size_bytes"]) > int(pressure["archive_free_bytes"])
    assert int(pressure["archive_used_bytes"]) <= int(pressure["archive_capacity_bytes"])
    assert int(final_state["app_count"]) == resolved_transfer_package.transfer_count + int(
        pressure["loaded_count"]
    )
    assert int(final_state["archive_used_bytes"]) == int(pressure["archive_used_bytes"])


def test_near_full_archive_module_churn_preserves_heap_and_defrag_integrity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "near-full-churn-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner-near-full-churn",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    switched_modules = [
        _module_id(name)
        for name in resolved_transfer_package.module_names[:12]
    ]
    allocations: list[int] = []

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        initial_state = runner.get_state(timeout_s=5.0).payload
        initial_archive_bytes = int(initial_state["archive_used_bytes"])
        runner.set_archive_capacity_bytes(initial_archive_bytes + max(1, initial_archive_bytes // 30))

        for index, module_name in enumerate(switched_modules):
            _load_target_module(runner, stage_dir, module_name)
            result = runner.execute_module(
                module_name,
                action="allocate_heap",
                heap_multiplier=1.0 + ((index % 4) * 0.25),
            )
            allocations.append(int(result["allocation_bytes"]))
            if index in {3, 7, 11}:
                fragmented = runner.simulate_archive_fragmentation(
                    fill_ratio=0.97,
                    delete_middle_count=2 + (index % 3),
                )
                assert fragmented["fragment_count"] >= 1
                defragged = runner.defragment_archive()
                assert defragged["success"] is True
                _load_target_module(runner, stage_dir, module_name)

        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    usage_ratio = int(final_state["archive_used_bytes"]) / int(final_state["archive_capacity_bytes"])

    assert usage_ratio > 0.95
    assert len(allocations) == 12
    assert final_state["module_history"][-12:] == switched_modules
    assert final_state["current_heap_bytes"] == 0
    assert int(final_state["peak_heap_bytes"]) >= max(allocations)
    assert final_state["oom_count"] == 0
    assert final_state["heap_corruption_detected"] is False
    assert final_state["archive_fragment_count"] == 0
    assert final_state["defrag_count"] == 3
    assert int(final_state["app_count"]) == resolved_transfer_package.transfer_count


def test_rom_version_detection_and_os58_gate_drive_successful_artifice_sequence(
    tmp_path: Path,
    resolved_transfer_package,
) -> None:
    rom_path = write_synthetic_rom(
        tmp_path / "os58.rom",
        os_version="5.8.0.0022",
        boot_version="5.5.2.0044",
    )
    validation = validate_rom_file(rom_path)
    compatibility = evaluate_artifice_rom_compatibility(validation)
    artifice_path, baseline_path = _write_artifice_assets(tmp_path)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        rom_path,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )
    runner.swap_rom(rom_path, validation)

    try:
        result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            rom_validation=validation,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "evidence",
            timeout_s=2.0,
        )
    finally:
        runner.shutdown()

    assert validation.integrity_ok is True
    assert validation.os_version == "5.8.0.0022"
    assert compatibility.supported is True
    assert result.success is True
    assert result.bypass_active is True
    assert any("Verified the locked arTIfiCE shell CRC" in line for line in result.diagnostics)
    assert any("key clear" in line for line in result.keypress_log)
    assert any("key mode" in line for line in result.keypress_log)
    assert any("key prgm" in line for line in result.keypress_log)


def test_rom_swap_simulation_updates_pipeline_gate_for_supported_and_unsupported_images(
    tmp_path: Path,
    resolved_transfer_package,
) -> None:
    stage_dir = tmp_path / "rom-swap-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    rom58 = write_synthetic_rom(
        tmp_path / "os58.rom",
        os_version="5.8.0.0022",
        boot_version="5.5.2.0044",
    )
    rom57 = write_synthetic_rom(
        tmp_path / "os57.rom",
        os_version="5.7.0.0014",
        boot_version="5.5.2.0044",
    )
    validation58 = validate_rom_file(rom58)
    validation57 = validate_rom_file(rom57)
    artifice_path, baseline_path = _write_artifice_assets(tmp_path)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        rom58,
        transfer_package=resolved_transfer_package,
        command_timeout_s=2.0,
        boot_timeout_s=2.0,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        runner.swap_rom(rom58, validation58)
        supported_state = runner.get_state(timeout_s=5.0).payload
        runner.swap_rom(rom57, validation57)
        unsupported_state = runner.get_state(timeout_s=5.0).payload
        unsupported_result = run_artifice_jailbreak(
            runner,
            transfer_package=resolved_transfer_package,
            artifice_path=artifice_path,
            rom_validation=validation57,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "unsupported-evidence",
            timeout_s=2.0,
        )
    finally:
        runner.shutdown()

    assert supported_state["os_version"] == "5.8.0.0022"
    assert supported_state["firmware_family"] == "5.8.x"
    assert unsupported_state["os_version"] == "5.7.0.0014"
    assert unsupported_state["firmware_family"] == "5.7.x"
    assert unsupported_result.success is False
    assert "requires a ti-84 plus ce 5.8.x rom" in unsupported_result.failure_reason.lower()


def test_battery_low_during_archive_operations_aborts_without_corrupting_archive(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "battery-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        runner.simulate_archive_fragmentation(fill_ratio=0.95, delete_middle_count=4)
        fragmented_digest = runner.archive_digest()
        runner.set_battery_low(True)
        aborted = runner.defragment_archive()
        runner.set_battery_low(False)
        recovered = runner.defragment_archive()
        runner._send_command(
            {
                "cmd": "load_app",
                "path": str(stage_dir),
                "target_name": _module_id(resolved_transfer_package.module_names[0]),
                "target_is_asm": True,
            },
            timeout_s=5.0,
        )
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert aborted["success"] is False
    assert aborted["aborted"] is True
    assert aborted["reason"] == "battery_low"
    assert aborted["archive_digest"] == fragmented_digest
    assert recovered["success"] is True
    assert state["battery_low"] is False
    assert state["archive_fragment_count"] == 0
    assert state["app_count"] == resolved_transfer_package.transfer_count


def test_appvar_lifecycle_round_trip_preserves_payloads_and_vat_identity(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "appvar-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    initial_payload = b"\x57\xCE\x01\x00\x08\x02"
    updated_payload = b"\x57\xCE\x09\x09\x09\x09\x04"

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        created = runner.create_appvar("CESETTNG", initial_payload)
        initial_read = runner.read_appvar("CESETTNG")
        written = runner.write_appvar("CESETTNG", updated_payload)
        updated_read = runner.read_appvar("CESETTNG")
        deleted = runner.delete_appvar("CESETTNG")
        recreated = runner.create_appvar("CESETTNG", b"\x57\xCE\xAA")
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert created["name"] == "CESETTNG"
    assert created["vat_id"] >= 1
    assert created["generation"] >= 1
    assert created["write_count"] == 1
    assert initial_read == initial_payload
    assert written["vat_id"] == created["vat_id"]
    assert written["generation"] == created["generation"]
    assert written["write_count"] == 2
    assert updated_read == updated_payload
    assert deleted["deleted"] is True
    assert deleted["vat_id"] == created["vat_id"]
    assert recreated["vat_id"] != created["vat_id"]
    assert recreated["generation"] > created["generation"]
    assert state["appvar_names"] == ["CESETTNG"]
    assert state["vat_entry_count"] == 1
    assert state["vat_tombstone_count"] == 1
    assert state["appvars"]["CESETTNG"]["payload_hex"] == "57CEAA"


def test_ipc_appvars_persist_across_module_swaps_and_catalog_queries(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "ipc-appvar-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )
    module_a = _module_id(resolved_transfer_package.module_names[4])
    module_b = _module_id(resolved_transfer_package.module_names[5])

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        settings_info = runner.create_appvar("CESETTNG", b"\x57\xCE\x02\x06\x01")
        history_info = runner.create_appvar("CEHISTOR", b"solve(x+2,x)=x=-2")
        first = runner.execute_module(module_a, action="write_scratch", scratch_value="SETTINGS-SWAP")
        second = runner.execute_module(module_b, action="read_scratch")
        restored_settings = runner.read_appvar("CESETTNG")
        restored_history = runner.read_appvar("CEHISTOR")
        listed = runner.list_appvars()
        state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert settings_info["name"] == "CESETTNG"
    assert history_info["name"] == "CEHISTOR"
    assert first["scratch"] == "SETTINGS-SWAP"
    assert second["scratch"] == ""
    assert restored_settings == b"\x57\xCE\x02\x06\x01"
    assert restored_history == b"solve(x+2,x)=x=-2"
    assert [entry["name"] for entry in listed] == ["CEHISTOR", "CESETTNG"]
    assert state["appvar_names"] == ["CEHISTOR", "CESETTNG"]
    assert state["module_history"][-2:] == [module_a, module_b]
    assert state["appvar_count"] == 2


def test_appvar_write_rejects_near_full_archive_growth_without_corrupting_payload(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "appvar-pressure-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        runner.simulate_archive_fragmentation(fill_ratio=0.985, delete_middle_count=2)
        fragmented_state = runner.get_state(timeout_s=5.0).payload
        created = runner.create_appvar("CE_RESP", b"OK")
        after_create_state = runner.get_state(timeout_s=5.0).payload
        oversized_payload = b"R" * (int(after_create_state["archive_free_bytes"]) + 32)
        with pytest.raises(MemoryError):
            runner.write_appvar("CE_RESP", oversized_payload)
        final_payload = runner.read_appvar("CE_RESP")
        final_state = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert int(fragmented_state["archive_free_bytes"]) > created["archive_bytes"]
    assert final_payload == b"OK"
    assert final_state["appvars"]["CE_RESP"]["payload_hex"] == "4F4B"
    assert final_state["appvars"]["CE_RESP"]["write_count"] == 1
    assert int(final_state["archive_free_bytes"]) == int(after_create_state["archive_free_bytes"])


def test_appvar_delete_and_recreate_updates_vat_catalog_without_stale_entries(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    stage_dir = tmp_path / "vat-stage"
    stage_transfer_package(resolved_transfer_package, stage_dir)
    runner = build_fake_cemu_stress_runner(
        tmp_path / "runner",
        temp_rom_path,
        transfer_package=resolved_transfer_package,
    )

    try:
        runner.boot(program_paths=[stage_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        first_state = runner.create_appvar("CE_STATE", b"\x01\x02\x03")
        runner.create_appvar("CE_REQ", b"det")
        deleted = runner.delete_appvar("CE_STATE")
        after_delete = runner.get_state(timeout_s=5.0).payload
        recreated = runner.create_appvar("CE_STATE", b"\x09\x08")
        after_recreate = runner.get_state(timeout_s=5.0).payload
    finally:
        runner.shutdown()

    assert first_state["vat_id"] != recreated["vat_id"]
    assert recreated["generation"] > first_state["generation"]
    assert deleted["freed_archive_bytes"] >= deleted["size_bytes"]
    assert after_delete["appvar_names"] == ["CE_REQ"]
    assert after_delete["vat_entry_count"] == 1
    assert after_delete["vat_tombstone_count"] == 1
    assert after_recreate["appvar_names"] == ["CE_REQ", "CE_STATE"]
    assert after_recreate["vat_entry_count"] == 2
    assert after_recreate["vat_tombstone_count"] == 1
    assert after_recreate["appvars"]["CE_STATE"]["payload_hex"] == "0908"
