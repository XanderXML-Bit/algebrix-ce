from __future__ import annotations

import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from accuracy_audit import DEFAULT_CASE_RESET_SEQUENCE
from cemu_hardening import (
    DEFAULT_ARITHMETIC_BATTERY_PATH,
    DEFAULT_MODULE_BATTERY_PATH,
    DEFAULT_PROMPT_CRCS,
    HardeningCase,
    _build_case_events,
    _expression_to_tokens,
    _normalize_event_spec,
    load_hardening_cases,
)
from cemu_runner import (
    CEmuCommandError,
    CEmuProcessTimeoutError,
    CEmuRunner,
    CEmuRunnerError,
)
from keypress_sequences import KeyAction
from pipeline_support import DEFAULT_BOOT_SEQUENCE


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_VISIBLE_SMOKE_CASE_PATH = REPO_ROOT / "tests" / "cemu" / "visible-smoke-cases.json"


@dataclass(frozen=True)
class VisibleSmokeCase:
    name: str
    lane: str
    description: str
    source_case_name: str
    primary_case: HardeningCase
    followup_name: str
    followup_description: str
    followup_events: tuple[Mapping[str, Any], ...]
    followup_expected_crcs: tuple[str, ...]
    followup_expected_output_excerpt_contains: tuple[str, ...]
    notes: str

    def to_summary(self) -> dict[str, object]:
        return {
            "name": self.name,
            "lane": self.lane,
            "description": self.description,
            "source_case_name": self.source_case_name,
            "module": self.primary_case.module,
            "input_expression": self.primary_case.input_expression,
            "expected_display": self.primary_case.expected_display,
            "expected_crcs": list(self.primary_case.expected_crcs),
            "followup_name": self.followup_name,
            "followup_description": self.followup_description,
            "followup_expected_crcs": list(self.followup_expected_crcs),
            "notes": self.notes,
        }


def _load_source_cases() -> dict[str, HardeningCase]:
    cases: dict[str, HardeningCase] = {}
    for path in (DEFAULT_ARITHMETIC_BATTERY_PATH, DEFAULT_MODULE_BATTERY_PATH):
        for case in load_hardening_cases(path):
            cases[case.name] = case
    return cases


def _build_inline_case(row: Mapping[str, Any], name: str) -> HardeningCase:
    input_expression = str(row.get("input_expression", ""))
    input_tokens = (
        tuple(str(value) for value in row["input_tokens"])
        if "input_tokens" in row
        else _expression_to_tokens(input_expression)
    )
    return HardeningCase(
        name=name,
        suite="visible_smoke",
        category=str(row.get("category", row.get("lane", "visible_smoke"))),
        backend=str(row.get("backend", "real")),
        module=str(row.get("module", "")),
        description=str(row["description"]),
        expected_crcs=tuple(str(value).upper() for value in row["expected_crcs"]),
        input_expression=input_expression,
        input_tokens=input_tokens,
        events=tuple(_normalize_event_spec(event) for event in row.get("events", [])),
        post_events=tuple(_normalize_event_spec(event) for event in row.get("post_events", [])),
        timeout_ms=int(row.get("timeout_ms", 30000)),
        key_delay_ms=int(row.get("key_delay_ms", 250)),
        enter_settle_delay_ms=int(row.get("enter_settle_delay_ms", 200)),
        result_delay_ms=int(row.get("result_delay_ms", 2500)),
        auto_enter=bool(row.get("auto_enter", True)),
        prompt_reset=bool(row.get("prompt_reset", True)),
        expected_output_excerpt_contains=tuple(
            str(value) for value in row.get("expected_output_excerpt_contains", [])
        ),
        expected_display=str(row.get("expected_display", "")),
        notes=str(row.get("notes", "")),
    )


def _build_primary_case(
    row: Mapping[str, Any],
    source_cases: Mapping[str, HardeningCase],
) -> tuple[HardeningCase, str]:
    source_case_name = str(row.get("source_case", ""))
    if source_case_name:
        if source_case_name not in source_cases:
            raise KeyError(f"Visible smoke source case '{source_case_name}' was not found.")
        source_case = source_cases[source_case_name]
        return (
            HardeningCase(
                name=str(row["name"]),
                suite="visible_smoke",
                category=str(row.get("category", source_case.category)),
                backend=str(row.get("backend", source_case.backend)),
                module=str(row.get("module", source_case.module)),
                description=str(row.get("description", source_case.description)),
                expected_crcs=tuple(
                    str(value).upper() for value in row.get("expected_crcs", source_case.expected_crcs)
                ),
                input_expression=str(row.get("input_expression", source_case.input_expression)),
                input_tokens=(
                    tuple(str(value) for value in row["input_tokens"])
                    if "input_tokens" in row
                    else source_case.input_tokens
                ),
                events=tuple(
                    _normalize_event_spec(event)
                    for event in row.get("events", source_case.events)
                ),
                post_events=tuple(
                    _normalize_event_spec(event)
                    for event in row.get("post_events", source_case.post_events)
                ),
                timeout_ms=int(row.get("timeout_ms", source_case.timeout_ms)),
                key_delay_ms=int(row.get("key_delay_ms", source_case.key_delay_ms)),
                enter_settle_delay_ms=int(
                    row.get("enter_settle_delay_ms", source_case.enter_settle_delay_ms)
                ),
                result_delay_ms=int(row.get("result_delay_ms", source_case.result_delay_ms)),
                auto_enter=bool(row.get("auto_enter", source_case.auto_enter)),
                prompt_reset=bool(row.get("prompt_reset", source_case.prompt_reset)),
                expected_output_excerpt_contains=tuple(
                    str(value)
                    for value in row.get(
                        "expected_output_excerpt_contains",
                        source_case.expected_output_excerpt_contains,
                    )
                ),
                expected_display=str(row.get("expected_display", source_case.expected_display)),
                notes=str(row.get("notes", source_case.notes)),
            ),
            source_case_name,
        )
    return _build_inline_case(row, str(row["name"])), ""


def load_visible_smoke_cases(
    path: Path = DEFAULT_VISIBLE_SMOKE_CASE_PATH,
    *,
    source_cases: Mapping[str, HardeningCase] | None = None,
) -> tuple[VisibleSmokeCase, ...]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    resolved_sources = dict(source_cases or _load_source_cases())
    cases: list[VisibleSmokeCase] = []
    for row in payload["cases"]:
        primary_case, source_case_name = _build_primary_case(row, resolved_sources)
        cases.append(
            VisibleSmokeCase(
                name=str(row["name"]),
                lane=str(row["lane"]),
                description=str(row.get("description", primary_case.description)),
                source_case_name=source_case_name,
                primary_case=primary_case,
                followup_name=str(row["followup_name"]),
                followup_description=str(row["followup_description"]),
                followup_events=tuple(
                    _normalize_event_spec(event) for event in row.get("followup_events", [])
                ),
                followup_expected_crcs=tuple(
                    str(value).upper() for value in row.get("followup_expected_crcs", ())
                ),
                followup_expected_output_excerpt_contains=tuple(
                    str(value)
                    for value in row.get("followup_expected_output_excerpt_contains", [])
                ),
                notes=str(row.get("notes", primary_case.notes)),
            )
        )
    return tuple(cases)


def _boot_runner(
    runner: CEmuRunner,
    *,
    program_paths: Sequence[Path],
    boot_timeout_s: float,
) -> None:
    runner.boot(program_paths=program_paths, ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=boot_timeout_s)


def _evaluate_assertion(
    *,
    name: str,
    description: str,
    expected_crcs: Sequence[str],
    observed_crc: str,
    expected_output_excerpt_contains: Sequence[str],
    observed_output_excerpt: str,
) -> dict[str, object]:
    crc_passed = observed_crc in expected_crcs
    excerpt_passed = all(
        expected_fragment.lower() in observed_output_excerpt.lower()
        for expected_fragment in expected_output_excerpt_contains
    )
    passed = crc_passed and excerpt_passed
    return {
        "name": name,
        "description": description,
        "expected_crcs": list(expected_crcs),
        "observed_crc": observed_crc,
        "expected_output_excerpt_contains": list(expected_output_excerpt_contains),
        "observed_output_excerpt": observed_output_excerpt,
        "passed": passed,
    }


def run_visible_smoke_cases(
    *,
    make_runner: Callable[[], CEmuRunner],
    program_paths: Sequence[Path],
    cases: Sequence[VisibleSmokeCase],
    default_timeout_s: float = 30.0,
    prompt_reset_sequence: Sequence[KeyAction] = DEFAULT_CASE_RESET_SEQUENCE,
) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    runner: CEmuRunner | None = None

    def ensure_runner(case_timeout_s: float) -> CEmuRunner:
        nonlocal runner
        if runner is not None and runner.is_running:
            return runner
        runner = make_runner()
        _boot_runner(runner, program_paths=program_paths, boot_timeout_s=case_timeout_s)
        return runner

    for case in cases:
        started_at = time.time()
        case_timeout_s = max(default_timeout_s, case.primary_case.timeout_ms / 1000.0)
        status = "passed"
        passed = False
        error_message = ""
        assertions: list[dict[str, object]] = []

        try:
            active_runner = ensure_runner(case_timeout_s)
            active_runner.reset(timeout_s=case_timeout_s)
            if case.primary_case.prompt_reset:
                active_runner.send_sequence(prompt_reset_sequence, timeout_s=case_timeout_s)

            primary_events = _build_case_events(case.primary_case)
            if primary_events:
                active_runner.send_sequence(primary_events, timeout_s=case_timeout_s)

            primary_response = active_runner.capture_screen(timeout_s=case_timeout_s)
            primary_observed_crc = str(primary_response.payload.get("crc", "")).upper()
            primary_output_excerpt = str(primary_response.payload.get("output_excerpt", ""))
            assertions.append(
                _evaluate_assertion(
                    name="primary",
                    description=case.description,
                    expected_crcs=case.primary_case.expected_crcs,
                    observed_crc=primary_observed_crc,
                    expected_output_excerpt_contains=case.primary_case.expected_output_excerpt_contains,
                    observed_output_excerpt=primary_output_excerpt,
                )
            )

            if case.followup_events:
                active_runner.send_sequence(case.followup_events, timeout_s=case_timeout_s)
            followup_response = active_runner.capture_screen(timeout_s=case_timeout_s)
            followup_observed_crc = str(followup_response.payload.get("crc", "")).upper()
            followup_output_excerpt = str(followup_response.payload.get("output_excerpt", ""))
            assertions.append(
                _evaluate_assertion(
                    name=case.followup_name,
                    description=case.followup_description,
                    expected_crcs=case.followup_expected_crcs,
                    observed_crc=followup_observed_crc,
                    expected_output_excerpt_contains=case.followup_expected_output_excerpt_contains,
                    observed_output_excerpt=followup_output_excerpt,
                )
            )

            passed = all(bool(assertion["passed"]) for assertion in assertions)
            status = "passed" if passed else "failed"
        except CEmuProcessTimeoutError as exc:
            status = "timeout"
            error_message = str(exc)
            if runner is not None:
                runner.shutdown()
            runner = None
        except (CEmuCommandError, CEmuRunnerError, ValueError, KeyError) as exc:
            status = "error"
            error_message = str(exc)
            if runner is not None:
                runner.shutdown()
            runner = None

        results.append(
            {
                "name": case.name,
                "lane": case.lane,
                "description": case.description,
                "source_case_name": case.source_case_name,
                "module": case.primary_case.module,
                "input_expression": case.primary_case.input_expression,
                "expected_display": case.primary_case.expected_display,
                "assertions": assertions,
                "passed": passed,
                "status": status,
                "timed_out": status == "timeout",
                "error_message": error_message,
                "timeout_ms": case.primary_case.timeout_ms,
                "notes": case.notes,
                "duration_seconds": round(time.time() - started_at, 2),
            }
        )

    if runner is not None:
        runner.shutdown()

    return results


def summarize_visible_smoke_results(
    cases: Sequence[VisibleSmokeCase],
    results: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    passed = [result for result in results if bool(result.get("passed"))]
    failed = [result for result in results if not bool(result.get("passed"))]
    timed_out = [result for result in results if str(result.get("status")) == "timeout"]
    error_count = sum(1 for result in results if str(result.get("status")) == "error")
    failed_assertions = [
        f"{result['name']}:{assertion['name']}"
        for result in results
        for assertion in result.get("assertions", [])
        if not bool(assertion.get("passed"))
    ]
    assertion_count = sum(len(result.get("assertions", [])) for result in results)
    lanes = sorted({case.lane for case in cases})
    lane_summary: dict[str, dict[str, object]] = {}
    for lane in lanes:
        lane_results = [result for result in results if str(result.get("lane", "")) == lane]
        lane_failed_results = [result for result in lane_results if not bool(result.get("passed"))]
        lane_failed_assertions = [
            f"{result['name']}:{assertion['name']}"
            for result in lane_results
            for assertion in result.get("assertions", [])
            if not bool(assertion.get("passed"))
        ]
        lane_summary[lane] = {
            "case_count": len(lane_results),
            "assertion_count": sum(len(result.get("assertions", [])) for result in lane_results),
            "passed_count": sum(1 for result in lane_results if bool(result.get("passed"))),
            "failed_count": len(lane_failed_results),
            "failed_names": [str(result["name"]) for result in lane_failed_results],
            "failed_assertions": lane_failed_assertions,
            "status": "failed" if lane_failed_results else "passed",
        }
    return {
        "case_count": len(cases),
        "assertion_count": assertion_count,
        "passed_count": len(passed),
        "failed_count": len(failed),
        "timeout_count": len(timed_out),
        "error_count": error_count,
        "failed_names": [str(result["name"]) for result in failed],
        "failed_assertions": failed_assertions,
        "lanes": lanes,
        "lane_summary": lane_summary,
        "accepted_prompt_crcs": list(DEFAULT_PROMPT_CRCS),
        "success": not failed,
    }
