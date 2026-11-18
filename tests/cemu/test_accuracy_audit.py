from __future__ import annotations

import json
from pathlib import Path

from accuracy_audit import load_accuracy_cases, run_accuracy_audit, summarize_accuracy_audit
from cemu_runner import CEmuRunner
from pipeline_support import DEFAULT_BOOT_SEQUENCE


def _fake_crc_for_keys(*keys: str) -> str:
    return f"{(sum(ord(ch) for ch in '-'.join(keys)) & 0xFFFFFFFF):08X}"


def test_locked_accuracy_baseline_covers_required_categories() -> None:
    cases = load_accuracy_cases()

    assert len(cases) >= 16
    assert {case.category for case in cases} == {
        "arithmetic",
        "calculus",
        "logarithms",
        "statistics",
        "trigonometry",
    }


def test_accuracy_audit_runs_against_fake_backend(
    fake_cemu_runner: CEmuRunner,
    temp_program_path: Path,
    tmp_path: Path,
) -> None:
    fake_cemu_runner.boot(
        program_paths=[temp_program_path],
        ready_sequence=DEFAULT_BOOT_SEQUENCE,
    )

    baseline_path = tmp_path / "audit-baseline.json"
    baseline_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "suite": "test",
                "cases": [
                    {
                        "name": "fake_add",
                        "category": "arithmetic",
                        "description": "Fake 2+3 probe",
                        "expected_display": "5",
                        "display_kind": "numeric",
                        "expected_crcs": [_fake_crc_for_keys("clear", "clear", "2", "+", "3", "enter")],
                        "input_tokens": ["2", "+", "3"],
                        "result_delay_ms": 100,
                        "timeout_ms": 1000,
                        "reference_value": "5",
                        "exact_value": "5",
                        "source_hash_name": "fake_add",
                        "notes": "Unique fake baseline."
                    },
                    {
                        "name": "fake_sin_zero",
                        "category": "trigonometry",
                        "description": "Fake sin(0) probe",
                        "expected_display": "0",
                        "display_kind": "numeric",
                        "expected_crcs": [_fake_crc_for_keys("clear", "clear", "sin", "0", ")", "enter")],
                        "input_tokens": ["sin", "0", ")"],
                        "result_delay_ms": 100,
                        "timeout_ms": 1000,
                        "reference_value": "0",
                        "exact_value": "0",
                        "source_hash_name": "fake_sin_zero",
                        "notes": "Unique fake baseline."
                    }
                ]
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    cases = load_accuracy_cases(baseline_path)
    results = run_accuracy_audit(fake_cemu_runner, cases)
    summary = summarize_accuracy_audit(cases, results)

    assert [result["name"] for result in results] == ["fake_add", "fake_sin_zero"]
    assert all(result["passed"] is True for result in results)
    assert summary["success"] is True
    assert summary["failed_count"] == 0


def test_accuracy_audit_summary_reports_mismatches() -> None:
    cases = load_accuracy_cases()
    results = [
        {
            "name": cases[0].name,
            "passed": True,
            "crc_specificity": "unique",
        },
        {
            "name": cases[1].name,
            "passed": False,
            "crc_specificity": "shared",
        },
    ]

    summary = summarize_accuracy_audit(cases[:2], results)

    assert summary["success"] is False
    assert summary["failed_count"] == 1
    assert summary["failed_names"] == [cases[1].name]
    assert summary["shared_crc_case_count"] == 1
