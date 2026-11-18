from __future__ import annotations

from pathlib import Path

import pytest

from cemu_runner import CEmuRunner
from pipeline_support import (
    DEFAULT_BOOT_SEQUENCE,
    DEFAULT_PROMPT_CLEAR_SEQUENCE,
    build_paced_expression_sequence,
    build_program_menu_sequence,
    get_real_cemu_status,
    load_real_smoke_expectations,
    run_smoke_checklist,
    summarize_checkpoints,
)
from screen_capture import ScreenRect, assert_text_region_contains, load_text_grid


REAL_CEMU_READY, REAL_CEMU_SKIP_REASON = get_real_cemu_status()


def test_smoke_expectations_cover_owner_checklist() -> None:
    expectations = load_real_smoke_expectations()

    assert expectations["boot_home"]["expected_crcs"]
    assert expectations["addition_result"]["expected_crcs"]
    assert expectations["multiplication_result"]["expected_crcs"]
    assert expectations["negative_result"]["expected_crcs"]
    assert expectations["decimal_result"]["expected_crcs"]
    assert expectations["parenthesized_result"]["expected_crcs"]
    assert expectations["power_result"]["expected_crcs"]
    assert expectations["divide_by_zero_error"]["expected_crcs"]
    assert expectations["prompt_after_divide_by_zero"]["expected_crcs"]
    assert expectations["graphing_mode_result"]["expected_crcs"]
    assert expectations["prompt_after_graphing_mode"]["expected_crcs"]
    assert expectations["program_menu_open"]["expected_crcs"]
    assert expectations["prompt_after_menu_close"]["expected_crcs"]
    assert expectations["memory_store_variable"]["expected_crcs"]
    assert expectations["memory_recall_variable"]["expected_crcs"]
    assert expectations["memory_clear_variable"]["expected_crcs"]
    assert expectations["memory_recall_after_clear"]["expected_crcs"]
    assert expectations["overflow_error"]["expected_crcs"]
    assert expectations["syntax_error_screen"]["expected_crcs"]
    assert expectations["syntax_error_prompt_restore"]["expected_crcs"]
    assert expectations["archive_program_execution"]["expected_crcs"]


def test_cemu_smoke_fake_backend(
    fake_cemu_runner: CEmuRunner,
    staged_transfer_package_dir: Path,
    tmp_path: Path,
) -> None:
    fake_cemu_runner.boot(
        program_paths=[staged_transfer_package_dir],
        ready_sequence=DEFAULT_BOOT_SEQUENCE,
    )
    fake_cemu_runner.send_sequence(build_paced_expression_sequence("2+3"))
    addition_crc = fake_cemu_runner.capture_screen().payload["crc"]
    fake_cemu_runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    fake_cemu_runner.send_sequence(build_program_menu_sequence())
    menu_crc = fake_cemu_runner.capture_screen().payload["crc"]

    snapshot_path = tmp_path / "fake-smoke-snapshot.json"
    fake_cemu_runner.snapshot(snapshot_path)
    text_grid = load_text_grid(snapshot_path)

    assert len(addition_crc) == 8
    assert len(menu_crc) == 8
    assert_text_region_contains(
        text_grid,
        ScreenRect(left=0, top=0, width=16, height=1),
        expected_text="CALCENGINE READY",
        case_sensitive=False,
    )
    assert_text_region_contains(
        text_grid,
        ScreenRect(left=0, top=1, width=9, height=1),
        expected_text="RESULT: 5",
        case_sensitive=False,
    )


@pytest.mark.skipif(not REAL_CEMU_READY, reason=REAL_CEMU_SKIP_REASON)
def test_cemu_smoke_real_backend(real_cemu_config, staged_transfer_package_dir: Path) -> None:
    assert real_cemu_config is not None

    with CEmuRunner(real_cemu_config) as runner:
        runner.boot(
            program_paths=[staged_transfer_package_dir],
            ready_sequence=DEFAULT_BOOT_SEQUENCE,
        )
        checkpoints = run_smoke_checklist(runner)

    summary = summarize_checkpoints(checkpoints)
    assert summary["success"] is True
    assert summary["failed_count"] == 0
    assert [checkpoint["name"] for checkpoint in checkpoints] == [
        "boot_home",
        "addition_result",
        "prompt_after_addition",
        "multiplication_result",
        "prompt_after_multiplication",
        "negative_result",
        "decimal_result",
        "parenthesized_result",
        "power_result",
        "divide_by_zero_error",
        "prompt_after_divide_by_zero",
        "graphing_mode_result",
        "prompt_after_graphing_mode",
        "program_menu_open",
        "prompt_after_menu_close",
        "memory_store_variable",
        "memory_recall_variable",
        "memory_clear_variable",
        "memory_recall_after_clear",
        "overflow_error",
        "syntax_error_screen",
        "syntax_error_prompt_restore",
        "archive_program_execution",
    ]
