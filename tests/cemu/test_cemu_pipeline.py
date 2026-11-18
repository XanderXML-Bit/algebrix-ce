from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

from cemu_runner import CEmuRunner, CEmuRunnerConfig, CEmuProcessTimeoutError
from fake_backend import build_fake_cemu_runner
from keypress_sequences import boot_to_calcengine_prompt, dismiss_result_to_prompt, evaluate_expression, flatten
from pipeline_support import get_real_cemu_status


REAL_CEMU_READY, REAL_CEMU_SKIP_REASON = get_real_cemu_status()


def test_runner_boots_and_executes_mock_pipeline(
    fake_cemu_runner: CEmuRunner,
    temp_program_path: Path,
    tmp_path: Path,
) -> None:
    summary = fake_cemu_runner.boot(
        program_paths=[temp_program_path],
        ready_sequence=boot_to_calcengine_prompt(),
    )

    assert summary.state["rom_loaded"] is True
    assert summary.state["running"] is True
    assert summary.state["app_count"] == 1

    fake_cemu_runner.send_sequence(
        flatten(
            evaluate_expression("2+3"),
            dismiss_result_to_prompt(),
        )
    )

    screen = fake_cemu_runner.capture_screen()
    assert screen.payload["format"] == "crc"
    assert len(screen.payload["crc"]) == 8

    snapshot_path = tmp_path / "mock-screen.json"
    fake_cemu_runner.snapshot(snapshot_path)
    assert snapshot_path.exists()


def test_runner_times_out_when_process_never_reports_ready(temp_rom_path: Path) -> None:
    config = CEmuRunnerConfig(
        executable_path=sys.executable,
        arguments=("-c", "import time; time.sleep(30)"),
        rom_path=temp_rom_path,
        startup_timeout_s=0.5,
        command_timeout_s=1.0,
        shutdown_timeout_s=0.5,
    )
    runner = CEmuRunner(config=config)
    with pytest.raises(CEmuProcessTimeoutError):
        runner.start()
    runner.shutdown()


def test_runner_reset_clears_mock_key_history(
    fake_cemu_runner: CEmuRunner,
    temp_program_path: Path,
) -> None:
    fake_cemu_runner.boot(program_paths=[temp_program_path])
    fake_cemu_runner.send_sequence(evaluate_expression("6*7"))

    state_before = fake_cemu_runner.get_state()
    assert state_before.payload["key_count"] > 0

    fake_cemu_runner.reset()
    state_after = fake_cemu_runner.get_state()
    assert state_after.payload["key_count"] == 0


def test_fake_pipeline_load_and_execute_sequence_is_deterministic(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    temp_rom_path: Path,
) -> None:
    def run_pipeline_once(run_name: str) -> dict[str, object]:
        runner_root = tmp_path / run_name
        runner_root.mkdir(parents=True, exist_ok=True)
        runner = build_fake_cemu_runner(runner_root, temp_rom_path)
        snapshot_path = tmp_path / f"{run_name}.snapshot.json"

        try:
            runner.boot(
                program_paths=[staged_transfer_package_dir],
                ready_sequence=boot_to_calcengine_prompt(),
            )
            runner.send_sequence(
                flatten(
                    evaluate_expression("hex(255)"),
                    dismiss_result_to_prompt(),
                    evaluate_expression("2+3"),
                )
            )
            screen = runner.capture_screen().payload
            runner.snapshot(snapshot_path)
            state = runner.get_state().payload
        finally:
            runner.shutdown()

        snapshot_payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
        return {
            "screen": {
                "crc": str(screen["crc"]),
                "format": str(screen["format"]),
                "output_excerpt": str(screen["output_excerpt"]),
            },
            "snapshot": {
                "crc": str(snapshot_payload["crc"]),
                "keys": list(snapshot_payload["keys"]),
                "transfer_files": [Path(raw_path).name for raw_path in snapshot_payload["transfer_files"]],
                "app_count": int(snapshot_payload["app_count"]),
                "text_rows": list(snapshot_payload["screen"]["text_rows"]),
            },
            "state": {
                "rom_loaded": bool(state["rom_loaded"]),
                "running": bool(state["running"]),
                "app_count": int(state["app_count"]),
                "key_count": int(state["key_count"]),
                "target_name": str(state["target_name"]),
                "target_is_asm": bool(state["target_is_asm"]),
                "last_crc": str(state["last_crc"]),
            },
        }

    first_run = run_pipeline_once("deterministic-first")
    second_run = run_pipeline_once("deterministic-second")

    assert first_run == second_run
    assert first_run["snapshot"]["app_count"] > 0
    assert first_run["state"]["last_crc"] == first_run["screen"]["crc"]


@pytest.mark.skipif(not REAL_CEMU_READY, reason=REAL_CEMU_SKIP_REASON)
def test_real_cemu_headless_pipeline(real_cemu_config: CEmuRunnerConfig | None) -> None:
    assert real_cemu_config is not None
    program_path = Path(__file__).resolve().parents[2] / "bin" / "launcher" / "CALCENG.8xp"
    if not program_path.exists():
        pytest.skip("CalcEngine launcher program is unavailable.")

    with CEmuRunner(real_cemu_config) as runner:
        runner.boot(program_paths=[program_path], ready_sequence=boot_to_calcengine_prompt())

        boot_crc = runner.capture_screen().payload["crc"]
        assert boot_crc in {"128C50A4", "B76BC405"}

        runner.send_sequence(evaluate_expression("2+3"))
        result_crc = runner.capture_screen().payload["crc"]
        assert result_crc == "9AE2B32C"
