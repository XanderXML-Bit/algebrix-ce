from __future__ import annotations

import sys
from dataclasses import replace
from pathlib import Path

import pytest

from cemu_hardening import DEFAULT_FAKE_CASE_BASELINE_PATH, load_hardening_cases, run_hardening_cases
from cemu_runner import CEmuProcessExitedError, CEmuRunner, CEmuRunnerConfig, CEmuRunnerError
from fake_backend import build_fake_cemu_runner


class CrashOnCaptureRunner:
    def __init__(self, delegate) -> None:
        self._delegate = delegate

    @property
    def is_running(self) -> bool:
        return self._delegate.is_running

    def boot(self, *args, **kwargs):
        return self._delegate.boot(*args, **kwargs)

    def reset(self, *args, **kwargs):
        return self._delegate.reset(*args, **kwargs)

    def send_sequence(self, *args, **kwargs):
        return self._delegate.send_sequence(*args, **kwargs)

    def capture_screen(self, *args, **kwargs):
        raise CEmuRunnerError("synthetic CEmu crash during screen capture")

    def shutdown(self) -> None:
        self._delegate.shutdown()


def test_runner_reports_process_exit_while_waiting_for_initial_state(temp_rom_path: Path) -> None:
    config = CEmuRunnerConfig(
        executable_path=sys.executable,
        arguments=(
            "-c",
            "import sys; print('fake-cemu ready', file=sys.stderr, flush=True); sys.stdin.readline(); sys.exit(0)",
        ),
        rom_path=temp_rom_path,
        startup_timeout_s=1.0,
        command_timeout_s=1.0,
        shutdown_timeout_s=0.5,
    )
    runner = CEmuRunner(config=config)

    with pytest.raises(CEmuProcessExitedError, match="exited while waiting for 'get_state'"):
        runner.start()

    runner.shutdown()


def test_hardening_suite_recovers_after_runner_crash_between_cases(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    temp_rom_path: Path,
) -> None:
    base_case = load_hardening_cases(DEFAULT_FAKE_CASE_BASELINE_PATH)[0]
    followup_case = replace(base_case, name="fake_recovered_after_crash")
    runner_factories_used: list[str] = []

    def make_runner():
        if not runner_factories_used:
            runner_factories_used.append("crash")
            crash_root = tmp_path / "crash-runner"
            crash_root.mkdir(parents=True, exist_ok=True)
            return CrashOnCaptureRunner(build_fake_cemu_runner(crash_root, temp_rom_path))
        runner_factories_used.append("recovery")
        recovery_root = tmp_path / "recovery-runner"
        recovery_root.mkdir(parents=True, exist_ok=True)
        return build_fake_cemu_runner(recovery_root, temp_rom_path)

    results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=[base_case, followup_case],
        default_timeout_s=5.0,
    )

    assert runner_factories_used == ["crash", "recovery"]
    assert results[0]["passed"] is False
    assert results[0]["status"] == "error"
    assert "synthetic CEmu crash during screen capture" in str(results[0]["error_message"])
    assert results[1]["passed"] is True
    assert results[1]["status"] == "passed"
