from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath
from typing import Any, Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_CEMU_PATH = REPO_ROOT / "tests" / "cemu"
if str(TESTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(TESTS_CEMU_PATH))


from artifice_jailbreak import (
    DEFAULT_ARTIFICE_PROGRAM_NAME,
    JailbreakResult,
    normalize_crc,
    run_artifice_jailbreak,
)
from calcengine_launcher import LaunchCheckpointResult, LaunchResult, run_calcengine_launch_verification
from cemu_runner import CEmuRunner, CEmuRunnerError, create_host_accessible_temp_dir
from cemu_hardening import load_hardening_cases, resolve_configured_path, run_hardening_cases
from pipeline_support import (
    TransferPackageResolutionError,
    build_runner_config,
    resolve_real_cemu_environment,
    stage_transfer_package,
)
from visible_smoke import (
    DEFAULT_VISIBLE_SMOKE_CASE_PATH,
    load_visible_smoke_cases,
    run_visible_smoke_cases,
    summarize_visible_smoke_results,
)


DEFAULT_GATE_CONFIG_PATH = REPO_ROOT / "tests" / "cemu" / "ci-config" / "release-candidate-gate.json"
KEYPAD_FIDELITY_VERIFY_SUMMARY_PATH = REPO_ROOT / "tests" / "emulator" / "outputs" / "keypad-fidelity.verify.summary.json"
LCD_FIDELITY_VERIFY_SUMMARY_PATH = REPO_ROOT / "tests" / "emulator" / "outputs" / "lcd-fidelity.verify.summary.json"
DEFAULT_EXIT_PASS = 0
DEFAULT_EXIT_FAIL = 1
DEFAULT_EXIT_ERROR = 2

_FIRST_TRY_CONFIRMATION_CHECKLIST_DEFINITIONS: tuple[dict[str, object], ...] = (
    {
        "id": "launch",
        "label": "Launch CalcEngine",
        "hash_names": ("calcengine_launch_screen",),
    },
    {
        "id": "addition_2_plus_3",
        "label": "2+3",
        "hash_names": ("calcengine_result_2plus3",),
    },
    {
        "id": "multiplication_6_times_7",
        "label": "6*7",
        "hash_names": ("calcengine_result_6times7",),
    },
    {
        "id": "negative_5_plus_10",
        "label": "-5+10",
        "hash_names": ("calcengine_result_neg5_plus10",),
    },
    {
        "id": "decimal_3p14_times_2",
        "label": "3.14*2",
        "hash_names": ("calcengine_result_decimal_6_28",),
    },
    {
        "id": "parenthesized_sum_times_3",
        "label": "(4+5)*3",
        "hash_names": ("calcengine_result_parens_27",),
    },
    {
        "id": "power_2_pow_8",
        "label": "2^8",
        "hash_names": ("calcengine_result_power_256",),
    },
    {
        "id": "divide_by_zero",
        "label": "divide-by-zero",
        "hash_names": ("calcengine_divide_by_zero_error",),
    },
    {
        "id": "menu_open_close",
        "label": "menu open/close",
        "hash_names": ("calcengine_menu_open", "calcengine_menu_closed"),
    },
    {
        "id": "return_to_prompt_after_result",
        "label": "return-to-prompt after result",
        "hash_names": ("calcengine_prompt_after_2plus3",),
    },
)


@dataclass(frozen=True)
class GateSmokeCase:
    name: str
    category: str
    source_case_name: str
    baseline_path: Path
    description: str
    expected_crcs: tuple[str, ...]
    expected_display: str
    input_expression: str
    input_tokens: tuple[str, ...]
    key_delay_ms: int
    enter_settle_delay_ms: int
    result_delay_ms: int
    auto_enter: bool


def load_gate_config(path: Path = DEFAULT_GATE_CONFIG_PATH) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def _ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def _write_json(path: Path, payload: Mapping[str, object]) -> None:
    _ensure_parent(path)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _find_named_case(path: Path, case_name: str) -> Mapping[str, object]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    for row in payload.get("cases", ()):
        if isinstance(row, Mapping) and str(row.get("name", "")).strip() == case_name:
            return row
    raise KeyError(f"Smoke case '{case_name}' was not found in {path}.")


def load_gate_smoke_case(path: Path, case_name: str) -> GateSmokeCase:
    row = _find_named_case(path, case_name)
    input_tokens = tuple(str(value) for value in row.get("input_tokens", ()))
    return GateSmokeCase(
        name=str(row["name"]),
        category=str(row.get("category", "")),
        source_case_name=case_name,
        baseline_path=path,
        description=str(row["description"]),
        expected_crcs=tuple(normalize_crc(value) for value in row.get("expected_crcs", ())),
        expected_display=str(row.get("expected_display", "")),
        input_expression=str(row.get("input_expression", "")),
        input_tokens=input_tokens,
        key_delay_ms=int(row.get("key_delay_ms", 250)),
        enter_settle_delay_ms=int(row.get("enter_settle_delay_ms", 200)),
        result_delay_ms=int(row.get("result_delay_ms", 2500)),
        auto_enter=bool(row.get("auto_enter", True)),
    )


def load_named_hardening_case(path: Path, case_name: str) -> Any:
    for case in load_hardening_cases(path):
        if case.name == case_name:
            return case
    raise KeyError(f"Smoke case '{case_name}' was not found in {path}.")


def _launch_checkpoint_to_payload(checkpoint: LaunchCheckpointResult) -> dict[str, object]:
    return {
        "step": checkpoint.step,
        "description": checkpoint.description,
        "expected_crcs": list(checkpoint.expected_crcs),
        "observed_crc": checkpoint.observed_crc,
        "passed": checkpoint.passed,
        "snapshot_path": str(checkpoint.snapshot_path) if checkpoint.snapshot_path is not None else "",
        "text_check_passed": checkpoint.text_check_passed,
        "screen_payload": dict(checkpoint.screen_payload),
    }


def _artifice_checkpoint_to_payload(checkpoint: Any) -> dict[str, object]:
    return {
        "step": str(checkpoint.step),
        "description": str(checkpoint.description),
        "expected_crcs": list(checkpoint.expected_crcs),
        "observed_crc": str(checkpoint.observed_crc),
        "passed": bool(checkpoint.passed),
        "snapshot_path": str(checkpoint.snapshot_path) if checkpoint.snapshot_path is not None else "",
        "screen_payload": dict(checkpoint.screen_payload),
    }


def _build_first_try_confirmation_hash_payload(hash_row: Mapping[str, object]) -> dict[str, object]:
    observed_crc = str(hash_row.get("observed_crc", "")).strip()
    status = str(hash_row.get("status", "")).strip().lower()
    inferred = not observed_crc and status == "pass"
    evidence_source = "reported_crc" if observed_crc else "inferred_expected_crc" if inferred else "unavailable"
    return {
        "hash_name": str(hash_row.get("hash_name", "")).strip(),
        "description": str(hash_row.get("description", "")).strip(),
        "expected_crcs": [str(value) for value in hash_row.get("expected_crcs", ())],
        "observed_crc": observed_crc,
        "status": status,
        "passed": status == "pass",
        "observed_crc_source": evidence_source,
        "observed_crc_inferred": inferred,
    }


def _build_first_try_confirmation_checklist_from_summary_path(summary_path: Path | None) -> dict[str, object]:
    unavailable = {
        "available": False,
        "required_item_count": len(_FIRST_TRY_CONFIRMATION_CHECKLIST_DEFINITIONS),
        "covered_item_count": 0,
        "passed_item_count": 0,
        "reported_crc_item_count": 0,
        "inferred_crc_item_count": 0,
        "missing_items": [str(item["label"]) for item in _FIRST_TRY_CONFIRMATION_CHECKLIST_DEFINITIONS],
        "items": [],
    }
    if summary_path is None:
        return {**unavailable, "reason": "No delegated arTIfiCE smoke summary was attached to the launch verification checkpoints."}
    if not summary_path.is_file():
        return {**unavailable, "reason": f"Delegated arTIfiCE smoke summary is missing: {summary_path}"}

    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {**unavailable, "reason": f"Unable to parse delegated arTIfiCE smoke summary '{summary_path}': {exc}"}

    verify_stage = summary.get("delegated_runner", {}).get("summary_json", {}).get("verify_stage", {})
    hash_results = verify_stage.get("hash_results", [])
    if not isinstance(hash_results, list):
        return {**unavailable, "reason": f"Delegated arTIfiCE smoke summary '{summary_path}' does not expose verify_stage.hash_results."}

    hash_map = {
        str(row.get("hash_name", "")).strip(): row
        for row in hash_results
        if isinstance(row, Mapping) and str(row.get("hash_name", "")).strip()
    }
    items: list[dict[str, object]] = []
    reported_crc_item_count = 0
    inferred_crc_item_count = 0

    for definition in _FIRST_TRY_CONFIRMATION_CHECKLIST_DEFINITIONS:
        hash_names = tuple(str(value) for value in definition["hash_names"])
        matched_rows = [hash_map[name] for name in hash_names if name in hash_map]
        missing_hash_names = [name for name in hash_names if name not in hash_map]
        hash_payloads = [_build_first_try_confirmation_hash_payload(row) for row in matched_rows]
        passed = bool(hash_payloads) and not missing_hash_names and all(bool(row.get("passed")) for row in hash_payloads)
        inferred = bool(hash_payloads) and all(bool(row.get("observed_crc_inferred")) for row in hash_payloads)
        reported = bool(hash_payloads) and all(row.get("observed_crc_source") == "reported_crc" for row in hash_payloads)
        if reported:
            reported_crc_item_count += 1
        elif inferred:
            inferred_crc_item_count += 1
        items.append(
            {
                "id": str(definition["id"]),
                "label": str(definition["label"]),
                "hash_names": list(hash_names),
                "missing_hash_names": missing_hash_names,
                "status": "passed" if passed else "missing" if missing_hash_names else "failed",
                "passed": passed,
                "covered": not missing_hash_names,
                "evidence_strength": "reported_crc" if reported else "inferred_expected_crc" if inferred else "mixed_or_unavailable",
                "hashes": hash_payloads,
            }
        )

    missing_items = [str(item["label"]) for item in items if not bool(item.get("covered"))]
    return {
        "available": True,
        "source_summary_path": str(summary_path),
        "source_runner": str(summary.get("runner", "")),
        "required_item_count": len(items),
        "covered_item_count": sum(1 for item in items if bool(item.get("covered"))),
        "passed_item_count": sum(1 for item in items if bool(item.get("passed"))),
        "reported_crc_item_count": reported_crc_item_count,
        "inferred_crc_item_count": inferred_crc_item_count,
        "missing_items": missing_items,
        "items": items,
    }


def _build_first_try_confirmation_checklist_from_launch_checkpoints(
    checkpoints: Sequence[LaunchCheckpointResult],
) -> dict[str, object]:
    candidate_paths: list[Path] = []
    seen: set[str] = set()
    for checkpoint in checkpoints:
        if checkpoint.snapshot_path is None:
            continue
        path = Path(checkpoint.snapshot_path)
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        candidate_paths.append(path)

    if not candidate_paths:
        return _build_first_try_confirmation_checklist_from_summary_path(None)

    reasons: list[str] = []
    for path in candidate_paths:
        checklist = _build_first_try_confirmation_checklist_from_summary_path(path)
        if checklist.get("available"):
            return checklist
        reason = str(checklist.get("reason", "")).strip()
        if reason:
            reasons.append(reason)

    return {
        **_build_first_try_confirmation_checklist_from_summary_path(None),
        "reason": reasons[0] if reasons else "No delegated arTIfiCE smoke summary exposed the first-try confirmation checklist.",
        "candidate_paths": [str(path) for path in candidate_paths],
    }


def _build_smoke_payload(
    *,
    name: str,
    description: str,
    category: str,
    source: str,
    checkpoints: Sequence[Mapping[str, object]],
    notes: Sequence[str] = (),
    expected_display: str = "",
    baseline_path: Path | None = None,
    source_case_names: Sequence[str] = (),
) -> dict[str, object]:
    passed = all(bool(checkpoint.get("passed")) for checkpoint in checkpoints)
    return {
        "name": name,
        "description": description,
        "category": category,
        "source": source,
        "status": "passed" if passed else "failed",
        "passed": passed,
        "expected_display": expected_display,
        "baseline_path": str(baseline_path) if baseline_path is not None else "",
        "source_case_names": list(source_case_names),
        "checkpoints": [dict(checkpoint) for checkpoint in checkpoints],
        "notes": list(notes),
    }


def _append_launch_smoke_payload(
    smoke_tests: list[dict[str, object]],
    *,
    launch_by_step: Mapping[str, LaunchCheckpointResult],
    name: str,
    description: str,
    category: str,
    step_names: Sequence[str],
    expected_display: str,
    notes: Sequence[str],
) -> None:
    checkpoints = []
    for step_name in step_names:
        checkpoint = launch_by_step.get(step_name)
        if checkpoint is None:
            return
        checkpoints.append(_launch_checkpoint_to_payload(checkpoint))

    smoke_tests.append(
        _build_smoke_payload(
            name=name,
            description=description,
            category=category,
            source="launch_verification",
            checkpoints=checkpoints,
            notes=notes,
            expected_display=expected_display,
        )
    )


def _hardening_result_to_checkpoint(result: Mapping[str, object], *, evidence_dir: Path) -> dict[str, object]:
    evidence_dir.mkdir(parents=True, exist_ok=True)
    snapshot_path = evidence_dir / f"{result['name']}.state.json"
    snapshot_payload = {
        "name": str(result["name"]),
        "description": str(result["description"]),
        "expected_crcs": list(result.get("expected_crcs", [])),
        "observed_crc": str(result.get("observed_crc", "")),
        "expected_display": str(result.get("expected_display", "")),
        "observed_output_excerpt": str(result.get("observed_output_excerpt", "")),
        "status": str(result.get("status", "")),
        "diagnostics": list(result.get("diagnostics", [])),
    }
    snapshot_path.write_text(json.dumps(snapshot_payload, indent=2), encoding="utf-8")
    return {
        "step": str(result["name"]),
        "description": str(result["description"]),
        "expected_crcs": list(result.get("expected_crcs", [])),
        "observed_crc": str(result.get("observed_crc", "")),
        "passed": bool(result.get("passed")),
        "snapshot_path": str(snapshot_path),
        "expected_display": str(result.get("expected_display", "")),
        "screen_payload": {
            "observed_output_excerpt": str(result.get("observed_output_excerpt", "")),
            "status": str(result.get("status", "")),
            "diagnostics": list(result.get("diagnostics", [])),
        },
    }


def run_direct_gate_smoke_cases(
    *,
    environment: Any,
    runner_factory: Any,
    transfer_package: Any,
    cases: Sequence[Any],
    evidence_dir: Path,
    timeout_s: float,
) -> list[dict[str, object]]:
    stage_dir = create_host_accessible_temp_dir(
        prefix="calcengine-release-gate-direct-",
        executable_path=environment.headless_path,
    )
    try:
        stage_transfer_package(transfer_package, stage_dir)
        results = run_hardening_cases(
            make_runner=runner_factory,
            program_paths=[stage_dir],
            cases=cases,
            default_timeout_s=timeout_s,
        )
        return [_hardening_result_to_checkpoint(result, evidence_dir=evidence_dir) for result in results]
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)


def run_release_gate_visible_smoke_suite(
    *,
    environment: Any,
    runner_factory: Any,
    transfer_package: Any,
    visible_smoke_case_path: Path,
    timeout_s: float,
) -> dict[str, object]:
    stage_dir = create_host_accessible_temp_dir(
        prefix="calcengine-release-gate-visible-",
        executable_path=environment.headless_path,
    )
    try:
        stage_transfer_package(transfer_package, stage_dir)
        cases = load_visible_smoke_cases(visible_smoke_case_path)
        results = run_visible_smoke_cases(
            make_runner=runner_factory,
            program_paths=[stage_dir],
            cases=cases,
            default_timeout_s=timeout_s,
        )
        return {
            **summarize_visible_smoke_results(cases, results),
            "baseline_path": str(visible_smoke_case_path),
        }
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)


def _summarize_smoke_tests(smoke_tests: Sequence[Mapping[str, object]]) -> dict[str, object]:
    passed = [row for row in smoke_tests if bool(row.get("passed"))]
    failed = [row for row in smoke_tests if not bool(row.get("passed"))]
    total = len(smoke_tests)
    passed_count = len(passed)
    return {
        "total": total,
        "passed": passed_count,
        "failed": len(failed),
        "failed_names": [str(row["name"]) for row in failed],
        "confidence_score_percent": round((passed_count / total) * 100.0, 2) if total else 0.0,
    }


def _find_smoke_test(smoke_tests: Sequence[Mapping[str, object]], name: str) -> Mapping[str, object] | None:
    for smoke_test in smoke_tests:
        if str(smoke_test.get("name", "")).strip() == name:
            return smoke_test
    return None


def _collect_checkpoint_snapshot_artifacts(checkpoints: Sequence[Mapping[str, object]]) -> dict[str, object]:
    checkpoint_reference_count = 0
    artifact_paths: list[str] = []
    seen_paths: set[str] = set()
    for checkpoint in checkpoints:
        if not isinstance(checkpoint, Mapping):
            continue
        snapshot_path = str(checkpoint.get("snapshot_path", "")).strip()
        if not snapshot_path:
            continue
        checkpoint_reference_count += 1
        if snapshot_path in seen_paths:
            continue
        seen_paths.add(snapshot_path)
        artifact_paths.append(snapshot_path)
    return {
        "checkpoint_snapshot_reference_count": checkpoint_reference_count,
        "snapshot_evidence_count": len(artifact_paths),
        "snapshot_artifact_paths": artifact_paths,
    }


def _build_fidelity_suite_item(*, item_id: str, label: str, summary_path: Path) -> dict[str, object]:
    unavailable = {
        "id": item_id,
        "label": label,
        "covered": False,
        "passed": False,
        "status": "missing",
        "summary_path": str(summary_path),
        "template_path": "",
        "baselined_path": "",
        "crc_catalog_json_path": "",
        "crc_catalog_markdown_path": "",
        "emulator_backend": "",
        "package_validation_success": False,
        "expected_hash_count": 0,
        "attempted_count": 0,
        "pass_count": 0,
        "fail_count": 0,
        "mismatch_count": 0,
        "missing_count": 0,
    }
    if not summary_path.is_file():
        return unavailable

    payload = json.loads(summary_path.read_text(encoding="utf-8"))
    runner_summary = payload.get("runner_summary", {})
    verify_stage = runner_summary.get("verify_stage", {})
    crc_catalog = runner_summary.get("crc_catalog", {})
    expected_hash_count = int(verify_stage.get("crc_catalog_hash_count", crc_catalog.get("hash_count", 0)) or 0)
    attempted_count = int(verify_stage.get("attempted_count", expected_hash_count) or 0)
    pass_count = int(verify_stage.get("pass_count", 0) or 0)
    fail_count = int(verify_stage.get("fail_count", 0) or 0)
    mismatch_count = int(verify_stage.get("mismatch_count", 0) or 0)
    missing_count = int(verify_stage.get("missing_count", 0) or 0)
    package_validation_success = bool(payload.get("package_validation", {}).get("success"))
    suite_success = bool(payload.get("success"))
    covered = bool(runner_summary) and expected_hash_count > 0
    passed = (
        covered
        and suite_success
        and package_validation_success
        and fail_count == 0
        and mismatch_count == 0
        and missing_count == 0
        and pass_count == expected_hash_count
    )
    return {
        "id": item_id,
        "label": label,
        "covered": covered,
        "passed": passed,
        "status": "passed" if passed else "failed" if covered else "missing",
        "summary_path": str(summary_path),
        "template_path": str(payload.get("template_path", "")),
        "baselined_path": str(payload.get("baselined_path", "")),
        "crc_catalog_json_path": str(payload.get("crc_catalog_json_path", "")),
        "crc_catalog_markdown_path": str(payload.get("crc_catalog_markdown_path", "")),
        "emulator_backend": str(runner_summary.get("emulator_backend", "")),
        "package_validation_success": package_validation_success,
        "expected_hash_count": expected_hash_count,
        "attempted_count": attempted_count,
        "pass_count": pass_count,
        "fail_count": fail_count,
        "mismatch_count": mismatch_count,
        "missing_count": missing_count,
    }


def _build_capability_depth_summary(
    *,
    smoke_tests: Sequence[Mapping[str, object]],
    first_try_confirmation_checklist: Mapping[str, object],
    archive_lifecycle: Mapping[str, object],
    visible_smoke_summary: Mapping[str, object],
) -> dict[str, object]:
    items: list[dict[str, object]] = []

    checklist_available = bool(first_try_confirmation_checklist.get("available"))
    checklist_required = int(first_try_confirmation_checklist.get("required_item_count", 0)) if checklist_available else 0
    checklist_covered = int(first_try_confirmation_checklist.get("covered_item_count", 0)) if checklist_available else 0
    checklist_passed = int(first_try_confirmation_checklist.get("passed_item_count", 0)) if checklist_available else 0
    checklist_status = "missing"
    if checklist_available:
        checklist_status = "passed" if checklist_required > 0 and checklist_required == checklist_passed else "failed"
    items.append(
        {
            "id": "first_try_confirmation",
            "label": "First-try confirmation checklist",
            "covered": checklist_available,
            "passed": checklist_status == "passed",
            "status": checklist_status,
            "required_item_count": checklist_required,
            "covered_item_count": checklist_covered,
            "passed_item_count": checklist_passed,
            "reported_crc_item_count": int(first_try_confirmation_checklist.get("reported_crc_item_count", 0)) if checklist_available else 0,
            "inferred_crc_item_count": int(first_try_confirmation_checklist.get("inferred_crc_item_count", 0)) if checklist_available else 0,
            "missing_items": list(first_try_confirmation_checklist.get("missing_items", ())) if checklist_available else [],
        }
    )

    visual_smoke_names = ("launch_calcengine", "addition_2_plus_3", "menu_open_close")
    visual_smoke_tests = [
        smoke_test
        for smoke_test in (_find_smoke_test(smoke_tests, name) for name in visual_smoke_names)
        if smoke_test is not None
    ]
    visual_covered = len(visual_smoke_tests) == len(visual_smoke_names)
    visual_passed = visual_covered and all(bool(smoke_test.get("passed")) for smoke_test in visual_smoke_tests)
    visual_status = "missing"
    if visual_smoke_tests:
        visual_status = "passed" if visual_passed else "failed" if visual_covered else "partial"
    visual_checkpoint_artifacts = _collect_checkpoint_snapshot_artifacts(
        [
            checkpoint
            for smoke_test in visual_smoke_tests
            for checkpoint in smoke_test.get("checkpoints", ())
            if isinstance(checkpoint, Mapping)
        ]
    )
    items.append(
        {
            "id": "launch_visual_surfaces",
            "label": "Launch/menu visual surfaces",
            "covered": visual_covered,
            "passed": visual_passed,
            "status": visual_status,
            "required_smoke_tests": list(visual_smoke_names),
            "covered_smoke_tests": [str(smoke_test.get("name", "")) for smoke_test in visual_smoke_tests],
            "expected_display_count": sum(1 for smoke_test in visual_smoke_tests if str(smoke_test.get("expected_display", "")).strip()),
            **visual_checkpoint_artifacts,
        }
    )

    visible_smoke_available = bool(visible_smoke_summary)
    visible_smoke_passed = visible_smoke_available and bool(visible_smoke_summary.get("success"))
    visible_smoke_status = "missing"
    if visible_smoke_available:
        visible_smoke_status = "passed" if visible_smoke_passed else "failed"
    visible_smoke_lanes = [str(lane) for lane in visible_smoke_summary.get("lanes", ())] if visible_smoke_available else []
    items.append(
        {
            "id": "visible_multi_lane_smoke",
            "label": "Visible multi-lane prompt/result smoke",
            "covered": visible_smoke_available,
            "passed": visible_smoke_passed,
            "status": visible_smoke_status,
            "case_count": int(visible_smoke_summary.get("case_count", 0)) if visible_smoke_available else 0,
            "assertion_count": int(visible_smoke_summary.get("assertion_count", 0)) if visible_smoke_available else 0,
            "lane_count": len(visible_smoke_lanes),
            "lanes": visible_smoke_lanes,
            "failed_names": [str(name) for name in visible_smoke_summary.get("failed_names", ())] if visible_smoke_available else [],
            "failed_assertions": [str(name) for name in visible_smoke_summary.get("failed_assertions", ())] if visible_smoke_available else [],
            "baseline_path": str(visible_smoke_summary.get("baseline_path", "")) if visible_smoke_available else "",
        }
    )

    matrix_smoke = _find_smoke_test(smoke_tests, "mode_switching")
    matrix_covered = matrix_smoke is not None
    matrix_passed = matrix_covered and bool(matrix_smoke.get("passed"))
    matrix_status = "missing"
    if matrix_covered:
        matrix_status = "passed" if matrix_passed else "failed"
    matrix_source_case_names = list(matrix_smoke.get("source_case_names", ())) if matrix_covered else []
    matrix_checkpoint_artifacts = _collect_checkpoint_snapshot_artifacts(matrix_smoke.get("checkpoints", ())) if matrix_covered else {
        "checkpoint_snapshot_reference_count": 0,
        "snapshot_evidence_count": 0,
        "snapshot_artifact_paths": [],
    }
    items.append(
        {
            "id": "matrix_literal_dispatch",
            "label": "Matrix-literal direct smoke",
            "covered": matrix_covered,
            "passed": matrix_passed,
            "status": matrix_status,
            "source_smoke_test": "mode_switching",
            "source_case_names": matrix_source_case_names,
            "matrix_case_names": [name for name in matrix_source_case_names if "matrix" in name.lower()],
            "direct_checkpoint_count": len(matrix_smoke.get("checkpoints", ())) if matrix_covered else 0,
            **matrix_checkpoint_artifacts,
        }
    )

    archive_covered = bool(archive_lifecycle)
    archive_stable = archive_covered and bool(archive_lifecycle.get("stable_within_attempt"))
    archive_status = "missing"
    if archive_covered:
        archive_status = "passed" if archive_stable else "failed"
    items.append(
        {
            "id": "archive_lifecycle",
            "label": "Archive lifecycle stability",
            "covered": archive_covered,
            "passed": archive_stable,
            "status": archive_status,
            "inventory_supported": bool(archive_lifecycle.get("inventory_supported")) if archive_covered else False,
            "expected_transfer_count": int(archive_lifecycle.get("expected_transfer_count", 0)) if archive_covered else 0,
            "loaded_archive_count_delta": int(archive_lifecycle.get("loaded_archive_count_delta", 0)) if archive_covered else 0,
            "blocker_count": len(archive_lifecycle.get("blockers", ())) if archive_covered else 0,
        }
    )

    items.append(
        _build_fidelity_suite_item(
            item_id="keypad_control_fidelity",
            label="Keypad control fidelity suite",
            summary_path=KEYPAD_FIDELITY_VERIFY_SUMMARY_PATH,
        )
    )
    items.append(
        _build_fidelity_suite_item(
            item_id="lcd_visual_fidelity",
            label="LCD visual fidelity suite",
            summary_path=LCD_FIDELITY_VERIFY_SUMMARY_PATH,
        )
    )

    covered_item_count = sum(1 for item in items if bool(item.get("covered")))
    passed_item_count = sum(1 for item in items if bool(item.get("passed")))
    failing_items = [str(item["label"]) for item in items if bool(item.get("covered")) and not bool(item.get("passed"))]
    missing_items = [str(item["label"]) for item in items if not bool(item.get("covered"))]

    return {
        "available": True,
        "required_item_count": len(items),
        "covered_item_count": covered_item_count,
        "passed_item_count": passed_item_count,
        "status": "passed" if covered_item_count == len(items) and passed_item_count == len(items) else "partial",
        "failing_items": failing_items,
        "missing_items": missing_items,
        "items": items,
    }


def _normalize_positive_int(value: object, *, field_name: str, minimum: int = 1) -> int:
    try:
        normalized = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field_name} must be an integer greater than or equal to {minimum}.") from exc
    if normalized < minimum:
        raise ValueError(f"{field_name} must be an integer greater than or equal to {minimum}.")
    return normalized


def _normalize_boolean(value: object, *, field_name: str) -> bool:
    if isinstance(value, bool):
        return value
    normalized = str(value).strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{field_name} must be a boolean-compatible value.")


def _build_summary_base(
    *,
    config_path: Path,
    output_path: Path,
    evidence_dir: Path,
    timeout_s: float,
    attempt_number: int | None = None,
    attempt_count: int | None = None,
) -> dict[str, object]:
    summary: dict[str, object] = {
        "schema_version": 2,
        "runner": "scripts/cemu/release_candidate_gate.py",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "success": False,
        "release_candidate_ready": False,
        "status": "error",
        "exit_code": DEFAULT_EXIT_ERROR,
        "failure_stage": "",
        "duration_seconds": 0.0,
        "config_path": str(config_path.resolve()),
        "output_path": str(output_path),
        "evidence_dir": str(evidence_dir),
        "default_timeout_seconds": timeout_s,
        "availability": {},
        "artifice_jailbreak": {},
        "launch_verification": {},
        "archive_lifecycle": {},
        "visible_smoke_summary": {},
        "capability_depth": {},
        "smoke_tests": [],
        "totals": {},
        "blockers": [],
        "diagnostics": [],
        "evidence_cleanup": {},
    }
    if attempt_number is not None and attempt_count is not None:
        summary["attempt_number"] = attempt_number
        summary["attempt_count"] = attempt_count
        summary["attempt_label"] = f"attempt-{attempt_number:02d}"
    return summary


def _snapshot_transfer_inventory_signature(transfer_names: Sequence[str]) -> str:
    digest = hashlib.sha256()
    for name in transfer_names:
        digest.update(str(name).encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest().upper()


def _normalize_snapshot_transfer_name(raw_path: object) -> str:
    text = str(raw_path).strip()
    if not text:
        return ""
    if "\\" in text or (len(text) >= 2 and text[1] == ":"):
        return PureWindowsPath(text).name
    return Path(text).name


def _build_expected_archive_inventory_names(
    *,
    transfer_package: Any,
    extra_entry_names: Sequence[str] = (),
) -> tuple[str, ...]:
    ordered_names = [
        *[str(name) for name in transfer_package.module_names],
        *[str(name) for name in transfer_package.runtime_names],
    ]
    seen = {name.lower() for name in ordered_names}
    for raw_name in extra_entry_names:
        normalized_name = str(raw_name).strip()
        if not normalized_name:
            continue
        key = normalized_name.lower()
        if key in seen:
            continue
        seen.add(key)
        ordered_names.append(normalized_name)
    return tuple(sorted(ordered_names, key=str.lower))


def _resolve_archive_inventory_snapshot_path(checkpoints: Sequence[Any]) -> Path | None:
    for checkpoint in reversed(tuple(checkpoints)):
        snapshot_path = getattr(checkpoint, "snapshot_path", None)
        if snapshot_path is None:
            continue
        resolved_path = Path(snapshot_path)
        if resolved_path.exists():
            return resolved_path
    return None


def _build_archive_inventory_payload(
    *,
    stage_name: str,
    snapshot_path: Path | None,
    transfer_package: Any,
    extra_entry_names: Sequence[str] = (),
) -> dict[str, object]:
    expected_transfer_names = _build_expected_archive_inventory_names(
        transfer_package=transfer_package,
        extra_entry_names=extra_entry_names,
    )
    expected_module_names = {str(name) for name in transfer_package.module_names}
    expected_runtime_names = {str(name) for name in transfer_package.runtime_names}
    expected_entry_name_set = {str(name).lower() for name in expected_transfer_names}
    diagnostics: list[str] = []

    payload: dict[str, object] = {
        "stage_name": stage_name,
        "supported": False,
        "snapshot_path": str(snapshot_path) if snapshot_path is not None else "",
        "expected_transfer_count": len(expected_transfer_names),
        "expected_transfer_order": list(expected_transfer_names),
        "expected_transfer_order_signature": _snapshot_transfer_inventory_signature(expected_transfer_names),
        "observed_transfer_count": 0,
        "observed_transfer_order": [],
        "observed_transfer_order_signature": "",
        "matches_manifest_inventory": False,
        "load_order_matches_manifest": False,
        "missing_modules": [],
        "missing_runtime_dependencies": [],
        "unexpected_entries": [],
        "duplicate_archive_slots": [],
        "diagnostics": diagnostics,
    }

    if snapshot_path is None:
        diagnostics.append(
            f"{stage_name} archive inventory snapshot is unavailable; falling back to count-only archive lifecycle telemetry."
        )
        return payload

    try:
        snapshot_payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        diagnostics.append(
            f"{stage_name} archive inventory snapshot '{snapshot_path}' could not be read cleanly: {exc}"
        )
        return payload

    raw_transfer_files = snapshot_payload.get("transfer_files")
    if not isinstance(raw_transfer_files, Sequence) or isinstance(raw_transfer_files, (str, bytes)):
        diagnostics.append(
            f"{stage_name} archive inventory snapshot '{snapshot_path}' did not expose a transfer_files array; falling back to count-only archive lifecycle telemetry."
        )
        return payload

    observed_transfer_names = [
        _normalize_snapshot_transfer_name(raw_path)
        for raw_path in raw_transfer_files
    ]
    observed_transfer_name_set = {name.lower() for name in observed_transfer_names}
    observed_module_names = {
        name for name in observed_transfer_names if name.lower().endswith(".8xp")
    }
    observed_runtime_names = {
        name for name in observed_transfer_names if name.lower().endswith(".8xv")
    }
    load_order_mismatches: list[dict[str, object]] = []
    for index in range(max(len(expected_transfer_names), len(observed_transfer_names))):
        expected_name = expected_transfer_names[index] if index < len(expected_transfer_names) else ""
        observed_name = observed_transfer_names[index] if index < len(observed_transfer_names) else ""
        if expected_name != observed_name:
            load_order_mismatches.append(
                {
                    "index": index,
                    "expected_name": expected_name,
                    "observed_name": observed_name,
                }
            )

    archive_slot_counts: dict[str, int] = {}
    for name in observed_transfer_names:
        archive_slot_name = Path(name).stem.upper()[:8]
        archive_slot_counts[archive_slot_name] = archive_slot_counts.get(archive_slot_name, 0) + 1
    duplicate_archive_slots = sorted(
        slot_name
        for slot_name, count in archive_slot_counts.items()
        if count > 1
    )
    missing_modules = sorted(
        expected_module_names - observed_module_names
    )
    missing_runtime_dependencies = sorted(
        expected_runtime_names - observed_runtime_names
    )
    unexpected_entries = sorted(
        name for name in observed_transfer_names if name.lower() not in expected_entry_name_set
    )
    load_order_matches_manifest = not load_order_mismatches
    matches_manifest_inventory = (
        load_order_matches_manifest
        and not missing_modules
        and not missing_runtime_dependencies
        and not unexpected_entries
        and not duplicate_archive_slots
        and len(observed_transfer_names) == len(expected_transfer_names)
    )

    payload.update(
        {
            "supported": True,
            "observed_transfer_count": len(observed_transfer_names),
            "observed_transfer_order": observed_transfer_names,
            "observed_transfer_order_signature": _snapshot_transfer_inventory_signature(observed_transfer_names),
            "matches_manifest_inventory": matches_manifest_inventory,
            "load_order_matches_manifest": load_order_matches_manifest,
            "load_order_mismatches": load_order_mismatches,
            "missing_modules": missing_modules,
            "missing_runtime_dependencies": missing_runtime_dependencies,
            "unexpected_entries": unexpected_entries,
            "duplicate_archive_slots": duplicate_archive_slots,
        }
    )

    if matches_manifest_inventory:
        diagnostics.append(
            f"{stage_name} archive inventory matched the deterministic manifest-backed transfer order."
        )
    else:
        if missing_modules:
            diagnostics.append(
                f"{stage_name} archive inventory is missing modules: {', '.join(missing_modules)}."
            )
        if missing_runtime_dependencies:
            diagnostics.append(
                f"{stage_name} archive inventory is missing runtime dependencies: {', '.join(missing_runtime_dependencies)}."
            )
        if unexpected_entries:
            diagnostics.append(
                f"{stage_name} archive inventory contains unexpected entries: {', '.join(unexpected_entries)}."
            )
        if duplicate_archive_slots:
            diagnostics.append(
                f"{stage_name} archive inventory contains duplicate 8-character archive slots: {', '.join(duplicate_archive_slots)}."
            )
        if load_order_mismatches:
            diagnostics.append(
                f"{stage_name} archive inventory order drifted from the deterministic manifest-backed order at {len(load_order_mismatches)} position(s)."
            )

    return payload


def _build_availability_payload(environment: Any) -> dict[str, object]:
    rom_validation = getattr(environment, "rom_validation", None)
    rom_validation_payload = {}
    if rom_validation is not None:
        rom_validation_payload = {
            "path": str(getattr(rom_validation, "path", "") or ""),
            "size_bytes": int(getattr(rom_validation, "size_bytes", 0) or 0),
            "crc32": str(getattr(rom_validation, "crc32", "") or ""),
            "sha256": str(getattr(rom_validation, "sha256", "") or ""),
            "recognized_size": bool(getattr(rom_validation, "recognized_size", False)),
            "recognized_fingerprint": bool(getattr(rom_validation, "recognized_fingerprint", False)),
            "expected_crc32": str(getattr(rom_validation, "expected_crc32", "") or ""),
            "expected_sha256": str(getattr(rom_validation, "expected_sha256", "") or ""),
            "integrity_ok": bool(getattr(rom_validation, "integrity_ok", False)),
            "known_good_hash_match": bool(getattr(rom_validation, "known_good_hash_match", False)),
            "product_family": str(getattr(rom_validation, "product_family", "") or ""),
            "model": str(getattr(rom_validation, "model", "") or ""),
            "os_version": str(getattr(rom_validation, "os_version", "") or ""),
            "boot_version": str(getattr(rom_validation, "boot_version", "") or ""),
            "firmware_family": str(getattr(rom_validation, "firmware_family", "") or ""),
            "description": str(getattr(rom_validation, "description", "") or ""),
            "warnings": [str(value) for value in getattr(rom_validation, "warnings", ())],
            "errors": [str(value) for value in getattr(rom_validation, "errors", ())],
        }
    return {
        "ready": bool(environment.ready),
        "blockers": list(environment.blockers),
        "rom_path": str(environment.rom_path) if environment.rom_path else "",
        "rom_validation": rom_validation_payload,
        "headless_path": str(environment.headless_path) if environment.headless_path else "",
        "autotester_path": str(environment.autotester_path) if environment.autotester_path else "",
        "gui_path": str(environment.gui_path) if environment.gui_path else "",
        "transfer_count": (
            int(environment.transfer_package.transfer_count)
            if environment.transfer_package is not None
            else 0
        ),
    }


def _resolve_attempt_evidence_dir(base_dir: Path, *, attempt_number: int, attempt_count: int) -> Path:
    if attempt_count <= 1:
        return base_dir
    return base_dir / f"attempt-{attempt_number:02d}"


def _build_smoke_test_signature(smoke_test: Mapping[str, object]) -> tuple[str, ...]:
    checkpoints = smoke_test.get("checkpoints", ())
    signature: list[str] = []
    for checkpoint in checkpoints:
        if not isinstance(checkpoint, Mapping):
            signature.append(str(checkpoint))
            continue
        signature.append(
            "|".join(
                (
                    str(checkpoint.get("step", "")),
                    str(checkpoint.get("observed_crc", "")),
                    str(bool(checkpoint.get("passed"))).lower(),
                )
            )
        )
    return tuple(signature)


def _prepare_attempt_evidence_dir(evidence_dir: Path) -> dict[str, object]:
    removed_entries: list[str] = []
    if evidence_dir.exists():
        removed_entries = sorted(child.name for child in evidence_dir.iterdir())
        shutil.rmtree(evidence_dir, ignore_errors=True)
    evidence_dir.mkdir(parents=True, exist_ok=True)
    return {
        "path": str(evidence_dir),
        "cleared_existing_entries": bool(removed_entries),
        "removed_entry_count": len(removed_entries),
        "removed_entries": removed_entries,
    }


def _prune_stale_evidence_artifacts(base_dir: Path, *, attempt_count: int) -> dict[str, object]:
    removed_paths: list[str] = []
    if not base_dir.exists():
        base_dir.mkdir(parents=True, exist_ok=True)
        return {
            "base_dir": str(base_dir),
            "attempt_count": attempt_count,
            "removed_paths": removed_paths,
            "removed_path_count": 0,
        }

    allowed_attempt_dirs = (
        {f"attempt-{index:02d}" for index in range(1, attempt_count + 1)}
        if attempt_count > 1
        else set()
    )
    single_attempt_artifact_names = {
        "summary.json",
        "artifice-jailbreak",
        "launch-verification",
        "direct-smoke",
    }

    for child in sorted(base_dir.iterdir(), key=lambda candidate: candidate.name):
        child_name = child.name
        if child.is_dir() and child_name.startswith("attempt-"):
            if child_name not in allowed_attempt_dirs:
                shutil.rmtree(child, ignore_errors=True)
                removed_paths.append(child_name)
            continue
        if attempt_count > 1 and child_name in single_attempt_artifact_names:
            if child.is_dir():
                shutil.rmtree(child, ignore_errors=True)
            else:
                child.unlink(missing_ok=True)
            removed_paths.append(child_name)

    return {
        "base_dir": str(base_dir),
        "attempt_count": attempt_count,
        "removed_paths": removed_paths,
        "removed_path_count": len(removed_paths),
    }


def _build_attempt_archive_lifecycle_payload(
    *,
    transfer_package: Any,
    expected_transfer_count: int,
    jailbreak_result: JailbreakResult,
    launch_result: LaunchResult,
) -> dict[str, object]:
    expected_inventory_extra_entries = (
        (f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp",)
        if jailbreak_result.artifice_path is not None
        else ()
    )
    artifice_transfer_count = int(jailbreak_result.transfer_count)
    launch_transfer_count = int(launch_result.transfer_count)
    artifice_loaded_archive_count = int(jailbreak_result.loaded_archive_count)
    launch_loaded_archive_count = int(launch_result.loaded_archive_count)
    jailbreak_inventory = _build_archive_inventory_payload(
        stage_name="arTIfiCE jailbreak",
        snapshot_path=_resolve_archive_inventory_snapshot_path(jailbreak_result.checkpoints),
        transfer_package=transfer_package,
        extra_entry_names=expected_inventory_extra_entries,
    )
    launch_inventory = _build_archive_inventory_payload(
        stage_name="CalcEngine launch",
        snapshot_path=_resolve_archive_inventory_snapshot_path(launch_result.checkpoints),
        transfer_package=transfer_package,
        extra_entry_names=expected_inventory_extra_entries,
    )
    diagnostics = [
        f"Archive lifecycle expected transfer count={expected_transfer_count}.",
        "arTIfiCE jailbreak reported "
        f"transfer_count={artifice_transfer_count} and loaded_archive_count={artifice_loaded_archive_count}.",
        "CalcEngine launch reported "
        f"transfer_count={launch_transfer_count} and loaded_archive_count={launch_loaded_archive_count}.",
    ]
    blockers: list[str] = []

    transfer_counts_match = (
        artifice_transfer_count == expected_transfer_count
        and launch_transfer_count == expected_transfer_count
    )
    if not transfer_counts_match:
        blockers.append(
            "Transfer-count mismatch detected across the release-candidate archive lifecycle: "
            f"expected {expected_transfer_count}, arTIfiCE reported {artifice_transfer_count}, "
            f"launch reported {launch_transfer_count}."
        )

    minimum_archive_count_satisfied = (
        artifice_loaded_archive_count >= expected_transfer_count
        and launch_loaded_archive_count >= expected_transfer_count
    )
    if not minimum_archive_count_satisfied:
        blockers.append(
            "Loaded archive count dropped below the staged transfer-package count during the release-candidate lane: "
            f"expected at least {expected_transfer_count}, arTIfiCE reported {artifice_loaded_archive_count}, "
            f"launch reported {launch_loaded_archive_count}."
        )

    stage_counts_match = artifice_loaded_archive_count == launch_loaded_archive_count
    if not stage_counts_match:
        blockers.append(
            "Archive lifecycle counts drifted between the arTIfiCE jailbreak handoff and the CalcEngine launch verifier: "
            f"{artifice_loaded_archive_count} -> {launch_loaded_archive_count}."
        )

    inventory_supported = bool(jailbreak_inventory["supported"] and launch_inventory["supported"])
    if inventory_supported:
        jailbreak_inventory_count_matches = (
            int(jailbreak_inventory["observed_transfer_count"]) == artifice_loaded_archive_count
        )
        launch_inventory_count_matches = (
            int(launch_inventory["observed_transfer_count"]) == launch_loaded_archive_count
        )
        inventory_stage_signatures_match = (
            str(jailbreak_inventory["observed_transfer_order_signature"])
            == str(launch_inventory["observed_transfer_order_signature"])
        )
        if not bool(jailbreak_inventory["matches_manifest_inventory"]):
            blockers.append(
                "arTIfiCE jailbreak archive inventory no longer matches the deterministic manifest-backed transfer payload."
            )
        if not bool(launch_inventory["matches_manifest_inventory"]):
            blockers.append(
                "CalcEngine launch archive inventory no longer matches the deterministic manifest-backed transfer payload."
            )
        if not jailbreak_inventory_count_matches:
            blockers.append(
                "arTIfiCE jailbreak archive inventory count drifted away from the runner-reported loaded archive count: "
                f"{jailbreak_inventory['observed_transfer_count']} vs {artifice_loaded_archive_count}."
            )
        if not launch_inventory_count_matches:
            blockers.append(
                "CalcEngine launch archive inventory count drifted away from the runner-reported loaded archive count: "
                f"{launch_inventory['observed_transfer_count']} vs {launch_loaded_archive_count}."
            )
        if not inventory_stage_signatures_match:
            blockers.append(
                "Archive inventory order drifted between the arTIfiCE jailbreak handoff and the CalcEngine launch verifier."
            )
        diagnostics.extend(str(message) for message in jailbreak_inventory["diagnostics"])
        diagnostics.extend(str(message) for message in launch_inventory["diagnostics"])
    else:
        jailbreak_inventory_count_matches = False
        launch_inventory_count_matches = False
        inventory_stage_signatures_match = False
        diagnostics.append(
            "Archive inventory snapshots were unavailable for one or both stages, so archive lifecycle validation used count-only telemetry."
        )
        diagnostics.extend(str(message) for message in jailbreak_inventory["diagnostics"])
        diagnostics.extend(str(message) for message in launch_inventory["diagnostics"])

    stable_within_attempt = (
        transfer_counts_match
        and minimum_archive_count_satisfied
        and stage_counts_match
        and (
            not inventory_supported
            or (
                bool(jailbreak_inventory["matches_manifest_inventory"])
                and bool(launch_inventory["matches_manifest_inventory"])
                and jailbreak_inventory_count_matches
                and launch_inventory_count_matches
                and inventory_stage_signatures_match
            )
        )
    )
    if stable_within_attempt:
        diagnostics.append(
            "Archive lifecycle counts remained stable across the arTIfiCE jailbreak handoff and the CalcEngine launch verifier."
        )

    return {
        "expected_transfer_count": expected_transfer_count,
        "artifice_transfer_count": artifice_transfer_count,
        "artifice_loaded_archive_count": artifice_loaded_archive_count,
        "launch_transfer_count": launch_transfer_count,
        "launch_loaded_archive_count": launch_loaded_archive_count,
        "loaded_archive_count_delta": launch_loaded_archive_count - artifice_loaded_archive_count,
        "transfer_counts_match": transfer_counts_match,
        "minimum_archive_count_satisfied": minimum_archive_count_satisfied,
        "stage_counts_match": stage_counts_match,
        "inventory_supported": inventory_supported,
        "jailbreak_inventory_count_matches_loaded_archive_count": jailbreak_inventory_count_matches,
        "launch_inventory_count_matches_loaded_archive_count": launch_inventory_count_matches,
        "inventory_stage_signatures_match": inventory_stage_signatures_match,
        "jailbreak_archive_inventory": jailbreak_inventory,
        "launch_archive_inventory": launch_inventory,
        "stable_within_attempt": stable_within_attempt,
        "blockers": blockers,
        "diagnostics": diagnostics,
    }


def _build_archive_lifecycle_signature(payload: Mapping[str, object]) -> tuple[str, ...]:
    signature = [
        f"expected_transfer_count|{int(payload.get('expected_transfer_count', 0))}",
        f"artifice_transfer_count|{int(payload.get('artifice_transfer_count', 0))}",
        f"artifice_loaded_archive_count|{int(payload.get('artifice_loaded_archive_count', 0))}",
        f"launch_transfer_count|{int(payload.get('launch_transfer_count', 0))}",
        f"launch_loaded_archive_count|{int(payload.get('launch_loaded_archive_count', 0))}",
    ]
    if bool(payload.get("inventory_supported")):
        jailbreak_inventory = payload.get("jailbreak_archive_inventory", {})
        launch_inventory = payload.get("launch_archive_inventory", {})
        if isinstance(jailbreak_inventory, Mapping):
            signature.append(
                "jailbreak_inventory_signature|"
                + str(jailbreak_inventory.get("observed_transfer_order_signature", ""))
            )
        if isinstance(launch_inventory, Mapping):
            signature.append(
                "launch_inventory_signature|"
                + str(launch_inventory.get("observed_transfer_order_signature", ""))
            )
    return tuple(signature)


def _summarize_archive_lifecycle_stability(
    attempts: Sequence[Mapping[str, object]],
    *,
    expected_attempt_count: int,
) -> dict[str, object]:
    signature_map: dict[tuple[str, ...], list[int]] = {}
    present_attempt_numbers: list[int] = []
    missing_attempt_numbers: list[int] = []
    unstable_attempt_numbers: list[int] = []

    for attempt in attempts:
        attempt_number = int(attempt.get("attempt_number", 0))
        lifecycle = attempt.get("archive_lifecycle")
        if not isinstance(lifecycle, Mapping) or not lifecycle:
            missing_attempt_numbers.append(attempt_number)
            continue

        present_attempt_numbers.append(attempt_number)
        if not bool(lifecycle.get("stable_within_attempt")):
            unstable_attempt_numbers.append(attempt_number)

        signature = _build_archive_lifecycle_signature(lifecycle)
        signature_map.setdefault(signature, []).append(attempt_number)

    signature_rows = [
        {
            "attempt_numbers": numbers,
            "archive_signature": list(signature),
        }
        for signature, numbers in sorted(signature_map.items(), key=lambda item: item[1][0] if item[1] else 0)
    ]
    stable = (
        len(attempts) == expected_attempt_count
        and not missing_attempt_numbers
        and not unstable_attempt_numbers
        and len(signature_rows) <= 1
    )
    return {
        "expected_attempt_count": expected_attempt_count,
        "executed_attempt_count": len(attempts),
        "observed_attempt_count": len(present_attempt_numbers),
        "present_attempt_numbers": present_attempt_numbers,
        "missing_attempt_numbers": missing_attempt_numbers,
        "unstable_attempt_numbers": unstable_attempt_numbers,
        "signature_count": len(signature_rows),
        "stable": stable,
        "signatures": signature_rows,
    }


def _summarize_attempt_stability(
    attempts: Sequence[Mapping[str, object]],
    *,
    expected_attempt_count: int,
) -> dict[str, object]:
    ordered_smoke_names: list[str] = []
    smoke_name_set: set[str] = set()
    for attempt in attempts:
        for smoke_test in attempt.get("smoke_tests", ()):
            if not isinstance(smoke_test, Mapping):
                continue
            smoke_name = str(smoke_test.get("name", "")).strip()
            if not smoke_name or smoke_name in smoke_name_set:
                continue
            smoke_name_set.add(smoke_name)
            ordered_smoke_names.append(smoke_name)

    smoke_reports: list[dict[str, object]] = []
    stable = True

    for smoke_name in ordered_smoke_names:
        signature_map: dict[tuple[str, ...], list[int]] = {}
        present_attempt_numbers: list[int] = []
        missing_attempt_numbers: list[int] = []
        failed_attempt_numbers: list[int] = []

        for attempt in attempts:
            attempt_number = int(attempt.get("attempt_number", 0))
            smoke_test = next(
                (
                    row
                    for row in attempt.get("smoke_tests", ())
                    if isinstance(row, Mapping) and str(row.get("name", "")).strip() == smoke_name
                ),
                None,
            )
            if smoke_test is None:
                missing_attempt_numbers.append(attempt_number)
                continue

            present_attempt_numbers.append(attempt_number)
            if not bool(smoke_test.get("passed")):
                failed_attempt_numbers.append(attempt_number)

            signature = _build_smoke_test_signature(smoke_test)
            signature_map.setdefault(signature, []).append(attempt_number)

        signature_rows = [
            {
                "attempt_numbers": numbers,
                "checkpoint_signature": list(signature),
            }
            for signature, numbers in sorted(signature_map.items(), key=lambda item: item[1][0] if item[1] else 0)
        ]
        smoke_stable = not missing_attempt_numbers and len(signature_rows) <= 1
        stable = stable and smoke_stable
        smoke_reports.append(
            {
                "name": smoke_name,
                "expected_attempt_count": expected_attempt_count,
                "observed_attempt_count": len(present_attempt_numbers),
                "present_attempt_numbers": present_attempt_numbers,
                "missing_attempt_numbers": missing_attempt_numbers,
                "failed_attempt_numbers": failed_attempt_numbers,
                "signature_count": len(signature_rows),
                "stable": smoke_stable,
                "signatures": signature_rows,
            }
        )

    return {
        "expected_attempt_count": expected_attempt_count,
        "executed_attempt_count": len(attempts),
        "stable": stable and len(attempts) == expected_attempt_count,
        "smoke_tests": smoke_reports,
        "unstable_smoke_tests": [
            report["name"]
            for report in smoke_reports
            if not bool(report.get("stable"))
        ],
    }


def _finalize_failed_summary(
    summary: dict[str, object],
    *,
    stage: str,
    blocker: str,
    diagnostics: Sequence[str] = (),
    smoke_tests: Sequence[Mapping[str, object]] | None = None,
) -> dict[str, object]:
    if smoke_tests is not None:
        summary["smoke_tests"] = [dict(row) for row in smoke_tests]
        summary["totals"] = _summarize_smoke_tests(smoke_tests)
    summary["success"] = False
    summary["release_candidate_ready"] = False
    summary["status"] = "failed"
    summary["exit_code"] = DEFAULT_EXIT_FAIL
    summary["failure_stage"] = stage
    summary["blockers"] = [blocker]
    summary["diagnostics"] = [*summary.get("diagnostics", []), *[str(line) for line in diagnostics]]
    return summary


def _run_release_candidate_gate_attempt(
    *,
    config_path: Path,
    output_path: Path,
    evidence_dir: Path,
    active_environment: Any,
    runner_factory: Any,
    timeout_s: float,
    multiplication_case: GateSmokeCase,
    mode_switch_cases: Sequence[GateSmokeCase],
    arithmetic_baseline_path: Path,
    module_baseline_path: Path,
    visible_smoke_case_path: Path,
    attempt_number: int,
    attempt_count: int,
) -> dict[str, object]:
    started_at = time.time()
    summary = _build_summary_base(
        config_path=config_path,
        output_path=output_path,
        evidence_dir=evidence_dir,
        timeout_s=timeout_s,
        attempt_number=attempt_number,
        attempt_count=attempt_count,
    )
    summary["availability"] = _build_availability_payload(active_environment)

    runner: CEmuRunner | None = None
    smoke_tests: list[dict[str, object]] = []

    try:
        assert active_environment.transfer_package is not None
        summary["evidence_cleanup"] = _prepare_attempt_evidence_dir(evidence_dir)
        try:
            runner = runner_factory()
        except (CEmuRunnerError, OSError, RuntimeError, ValueError) as exc:
            return _finalize_failed_summary(
                summary,
                stage="runner_setup",
                blocker=f"Unable to initialize the CEmu runner: {exc}",
            )

        try:
            jailbreak_result = run_artifice_jailbreak(
                runner,
                transfer_package=active_environment.transfer_package,
                rom_validation=active_environment.rom_validation,
                evidence_dir=evidence_dir / "artifice-jailbreak",
                timeout_s=timeout_s,
            )
        except (CEmuRunnerError, TransferPackageResolutionError, OSError, RuntimeError, ValueError) as exc:
            return _finalize_failed_summary(
                summary,
                stage="artifice_jailbreak",
                blocker=f"arTIfiCE jailbreak verification failed: {exc}",
            )
        summary["artifice_jailbreak"] = {
            "success": jailbreak_result.success,
            "failure_reason": jailbreak_result.failure_reason,
            "timed_out": jailbreak_result.timed_out,
            "bypass_active": jailbreak_result.bypass_active,
            "artifice_path": str(jailbreak_result.artifice_path) if jailbreak_result.artifice_path is not None else "",
            "stage_dir": str(jailbreak_result.stage_dir) if jailbreak_result.stage_dir is not None else "",
            "loaded_archive_count": jailbreak_result.loaded_archive_count,
            "transfer_count": jailbreak_result.transfer_count,
            "usb_retry_count": jailbreak_result.usb_retry_count,
            "checkpoints": [_artifice_checkpoint_to_payload(checkpoint) for checkpoint in jailbreak_result.checkpoints],
            "diagnostics": list(jailbreak_result.diagnostics),
        }
        if not jailbreak_result.success:
            return _finalize_failed_summary(
                summary,
                stage="artifice_jailbreak",
                blocker=jailbreak_result.failure_reason or "arTIfiCE jailbreak verification failed.",
                diagnostics=jailbreak_result.diagnostics,
            )

        try:
            launch_result = run_calcengine_launch_verification(
                runner,
                transfer_package=active_environment.transfer_package,
                jailbreak_result=jailbreak_result,
                evidence_dir=evidence_dir / "launch-verification",
                timeout_s=timeout_s,
            )
        except (CEmuRunnerError, TransferPackageResolutionError, OSError, RuntimeError, ValueError) as exc:
            return _finalize_failed_summary(
                summary,
                stage="launch_verification",
                blocker=f"CalcEngine launch verification failed: {exc}",
                diagnostics=jailbreak_result.diagnostics,
                smoke_tests=smoke_tests,
            )
        summary["launch_verification"] = {
            "success": launch_result.success,
            "failure_reason": launch_result.failure_reason,
            "timed_out": launch_result.timed_out,
            "basic_operation_passed": launch_result.basic_operation_passed,
            "menu_open_close_passed": launch_result.menu_open_close_passed,
            "loaded_archive_count": launch_result.loaded_archive_count,
            "transfer_count": launch_result.transfer_count,
            "checkpoints": [_launch_checkpoint_to_payload(checkpoint) for checkpoint in launch_result.checkpoints],
            "diagnostics": list(launch_result.diagnostics),
        }
        summary["first_try_confirmation_checklist"] = _build_first_try_confirmation_checklist_from_launch_checkpoints(
            launch_result.checkpoints,
        )
        summary["archive_lifecycle"] = _build_attempt_archive_lifecycle_payload(
            transfer_package=active_environment.transfer_package,
            expected_transfer_count=int(active_environment.transfer_package.transfer_count),
            jailbreak_result=jailbreak_result,
            launch_result=launch_result,
        )

        launch_by_step = {checkpoint.step: checkpoint for checkpoint in launch_result.checkpoints}
        _append_launch_smoke_payload(
            smoke_tests,
            launch_by_step=launch_by_step,
            name="launch_calcengine",
            description="CalcEngine launches from the real-ROM arTIfiCE PRGM handoff.",
            category="launch",
            step_names=("calcengine_launch_menu",),
            notes=("Launch smoke is sourced from the arTIfiCE-backed launch verifier.",),
            expected_display="CALCENGINE",
        )
        _append_launch_smoke_payload(
            smoke_tests,
            launch_by_step=launch_by_step,
            name="addition_2_plus_3",
            description="The real-ROM launch path evaluates 2+3 and renders 5.",
            category="arithmetic",
            step_names=("calcengine_result_2plus3",),
            notes=("Addition smoke is sourced from the same live arTIfiCE-backed launch session.",),
            expected_display="5",
        )
        _append_launch_smoke_payload(
            smoke_tests,
            launch_by_step=launch_by_step,
            name="menu_open_close",
            description="The CalcEngine MODE menu opens and closes cleanly after launch.",
            category="menu",
            step_names=("calcengine_menu_open", "calcengine_menu_closed"),
            notes=("Menu smoke is sourced from the same live arTIfiCE-backed launch session.",),
            expected_display="CALCENGINE",
        )

        if not launch_result.success:
            return _finalize_failed_summary(
                summary,
                stage="launch_verification",
                blocker=launch_result.failure_reason or "CalcEngine launch verification failed.",
                diagnostics=launch_result.diagnostics,
                smoke_tests=smoke_tests,
            )
        if not bool(summary["archive_lifecycle"].get("stable_within_attempt")):
            return _finalize_failed_summary(
                summary,
                stage="archive_lifecycle",
                blocker="Archive lifecycle verification failed after the arTIfiCE handoff and CalcEngine launch validation.",
                diagnostics=(
                    *summary["archive_lifecycle"].get("diagnostics", ()),
                    *summary["archive_lifecycle"].get("blockers", ()),
                ),
                smoke_tests=smoke_tests,
            )

        direct_smoke_cases = (
            load_named_hardening_case(arithmetic_baseline_path, multiplication_case.name),
            *(load_named_hardening_case(module_baseline_path, case.name) for case in mode_switch_cases),
        )
        try:
            direct_smoke_checkpoints = run_direct_gate_smoke_cases(
                environment=active_environment,
                runner_factory=runner_factory,
                transfer_package=active_environment.transfer_package,
                cases=direct_smoke_cases,
                evidence_dir=evidence_dir / "direct-smoke",
                timeout_s=timeout_s,
            )
        except (CEmuRunnerError, TransferPackageResolutionError, OSError, RuntimeError, ValueError, KeyError) as exc:
            return _finalize_failed_summary(
                summary,
                stage="direct_smoke",
                blocker=f"Direct real-ROM smoke verification failed: {exc}",
                diagnostics=(
                    *jailbreak_result.diagnostics,
                    *launch_result.diagnostics,
                ),
                smoke_tests=smoke_tests,
            )
        multiplication_checkpoint = direct_smoke_checkpoints[0]
        smoke_tests.append(
            _build_smoke_payload(
                name="multiplication_6_times_7",
                description=multiplication_case.description,
                category="arithmetic",
                source="hardening_case",
                checkpoints=[multiplication_checkpoint],
                notes=("Multiplication smoke runs in a fresh real-ROM direct-launch session after the jailbreak and launch prerequisites pass.",),
                expected_display=multiplication_case.expected_display,
                baseline_path=multiplication_case.baseline_path,
                source_case_names=(multiplication_case.source_case_name,),
            )
        )

        mode_switch_checkpoints = direct_smoke_checkpoints[1:]
        smoke_tests.append(
            _build_smoke_payload(
                name="mode_switching",
                description="CalcEngine routes consecutive base and matrix module requests on the real-ROM headless lane after the jailbreak and launch prerequisites pass.",
                category="module_dispatch",
                source="hardening_case_pair",
                checkpoints=mode_switch_checkpoints,
                notes=(
                    "Mode switching is represented by consecutive CEBASE -> CEMAT dispatch in a fresh direct-launch real-ROM session after the jailbreak and launch prerequisites pass.",
                ),
                baseline_path=module_baseline_path,
                source_case_names=tuple(case.source_case_name for case in mode_switch_cases),
            )
        )

        try:
            summary["visible_smoke_summary"] = run_release_gate_visible_smoke_suite(
                environment=active_environment,
                runner_factory=runner_factory,
                transfer_package=active_environment.transfer_package,
                visible_smoke_case_path=visible_smoke_case_path,
                timeout_s=timeout_s,
            )
        except (CEmuRunnerError, TransferPackageResolutionError, OSError, RuntimeError, ValueError, KeyError) as exc:
            return _finalize_failed_summary(
                summary,
                stage="visible_smoke",
                blocker=f"Visible multi-lane smoke verification failed: {exc}",
                diagnostics=(
                    *jailbreak_result.diagnostics,
                    *launch_result.diagnostics,
                ),
                smoke_tests=smoke_tests,
            )

        totals = _summarize_smoke_tests(smoke_tests)
        summary["smoke_tests"] = smoke_tests
        summary["capability_depth"] = _build_capability_depth_summary(
            smoke_tests=smoke_tests,
            first_try_confirmation_checklist=summary["first_try_confirmation_checklist"],
            archive_lifecycle=summary["archive_lifecycle"],
            visible_smoke_summary=summary["visible_smoke_summary"],
        )
        fidelity_ready = all(
            bool(item.get("passed"))
            for item in summary["capability_depth"].get("items", ())
            if str(item.get("id", "")) in {"keypad_control_fidelity", "lcd_visual_fidelity"}
        )
        summary["totals"] = totals
        summary["success"] = bool(
            jailbreak_result.success
            and launch_result.success
            and totals["failed"] == 0
            and bool(summary["visible_smoke_summary"].get("success"))
            and fidelity_ready
        )
        summary["release_candidate_ready"] = bool(summary["success"])
        summary["status"] = "passed" if summary["success"] else "failed"
        summary["exit_code"] = DEFAULT_EXIT_PASS if summary["success"] else DEFAULT_EXIT_FAIL
        if not summary["success"]:
            if totals["failed"]:
                summary["blockers"] = list(totals["failed_names"])
                summary["failure_stage"] = "smoke_tests"
            elif not bool(summary["visible_smoke_summary"].get("success")):
                summary["blockers"] = [
                    "Capability-depth verification incomplete: Visible multi-lane prompt/result smoke"
                ]
                summary["failure_stage"] = "capability_depth"
            else:
                capability_depth = summary["capability_depth"]
                incomplete_labels = [
                    *[str(label) for label in capability_depth.get("failing_items", ())],
                    *[str(label) for label in capability_depth.get("missing_items", ())],
                ]
                summary["blockers"] = [
                    "Capability-depth verification incomplete"
                    + (f": {', '.join(incomplete_labels)}" if incomplete_labels else ".")
                ]
                summary["failure_stage"] = "capability_depth"
        return summary
    finally:
        summary["duration_seconds"] = round(time.time() - started_at, 2)
        shutdown = getattr(runner, "shutdown", None)
        if callable(shutdown):
            shutdown()


def run_release_candidate_gate(
    *,
    config_path: Path = DEFAULT_GATE_CONFIG_PATH,
    output_path: Path | None = None,
    evidence_dir: Path | None = None,
    environment: Any | None = None,
    runner_factory: Any | None = None,
    default_timeout_s: float | None = None,
    attempt_count: int | None = None,
    required_passing_attempts: int | None = None,
    stop_on_first_failure: bool | None = None,
    enforce_smoke_crc_stability: bool | None = None,
) -> dict[str, object]:
    config = load_gate_config(config_path)
    resolved_output_path = (
        output_path.expanduser().resolve()
        if output_path is not None
        else resolve_configured_path(
            config.get("output_path"),
            config_path=config_path,
            default_path=REPO_ROOT / "tests" / "emulator" / "outputs" / "cemu-release-candidate-confidence.json",
        )
    )
    resolved_evidence_dir = (
        evidence_dir.expanduser().resolve()
        if evidence_dir is not None
        else resolve_configured_path(
            config.get("evidence_dir"),
            config_path=config_path,
            default_path=REPO_ROOT / "tests" / "cemu" / "outputs" / "release-candidate-gate",
        )
    )
    timeout_s = float(config.get("default_timeout_seconds", 60) if default_timeout_s is None else default_timeout_s)
    aggregate_summary = _build_summary_base(
        config_path=config_path,
        output_path=resolved_output_path,
        evidence_dir=resolved_evidence_dir,
        timeout_s=timeout_s,
    )

    try:
        configured_attempt_count = _normalize_positive_int(
            config.get("attempt_count", 1) if attempt_count is None else attempt_count,
            field_name="attempt_count",
        )
        configured_required_default = config.get("required_passing_attempts", configured_attempt_count)
        if required_passing_attempts is None and attempt_count is not None:
            configured_required_default = min(
                _normalize_positive_int(
                    configured_required_default,
                    field_name="required_passing_attempts",
                ),
                configured_attempt_count,
            )
        configured_required_passing_attempts = _normalize_positive_int(
            configured_required_default if required_passing_attempts is None else required_passing_attempts,
            field_name="required_passing_attempts",
        )
        if configured_required_passing_attempts > configured_attempt_count:
            raise ValueError("required_passing_attempts cannot be greater than attempt_count.")
        configured_stop_on_first_failure = _normalize_boolean(
            config.get("stop_on_first_failure", True)
            if stop_on_first_failure is None
            else stop_on_first_failure,
            field_name="stop_on_first_failure",
        )
        configured_enforce_smoke_crc_stability = _normalize_boolean(
            config.get("enforce_smoke_crc_stability", True)
            if enforce_smoke_crc_stability is None
            else enforce_smoke_crc_stability,
            field_name="enforce_smoke_crc_stability",
        )
    except ValueError as exc:
        return _finalize_failed_summary(
            aggregate_summary,
            stage="config",
            blocker=str(exc),
        )

    arithmetic_baseline_path = resolve_configured_path(
        config.get("arithmetic_battery_path"),
        config_path=config_path,
        default_path=REPO_ROOT / "tests" / "cemu" / "edge-cases" / "arithmetic-battery.json",
    )
    module_baseline_path = resolve_configured_path(
        config.get("module_battery_path"),
        config_path=config_path,
        default_path=REPO_ROOT / "tests" / "cemu" / "edge-cases" / "module-battery.json",
    )
    visible_smoke_case_path = resolve_configured_path(
        config.get("visible_smoke_case_path"),
        config_path=config_path,
        default_path=DEFAULT_VISIBLE_SMOKE_CASE_PATH,
    )
    multiplication_case = load_gate_smoke_case(
        arithmetic_baseline_path,
        str(config.get("multiplication_case_name", "arith_mul_6_times_7")),
    )
    mode_switch_case_names = tuple(str(value) for value in config.get("mode_switch_case_names", ()))
    mode_switch_cases = tuple(load_gate_smoke_case(module_baseline_path, case_name) for case_name in mode_switch_case_names)

    started_at = time.time()
    active_environment = environment if environment is not None else resolve_real_cemu_environment(repo_root=REPO_ROOT)
    aggregate_summary["availability"] = _build_availability_payload(active_environment)
    aggregate_summary["attempt_count"] = configured_attempt_count
    aggregate_summary["required_passing_attempts"] = configured_required_passing_attempts
    aggregate_summary["evidence_cleanup"] = _prune_stale_evidence_artifacts(
        resolved_evidence_dir,
        attempt_count=configured_attempt_count,
    )

    if not active_environment.ready:
        aggregate_summary["blockers"] = list(active_environment.blockers)
        aggregate_summary["failure_stage"] = "availability"
        aggregate_summary["duration_seconds"] = round(time.time() - started_at, 2)
        return aggregate_summary

    attempt_runner_factory = runner_factory
    if attempt_runner_factory is None:
        real_config = build_runner_config(active_environment)

        def attempt_runner_factory() -> CEmuRunner:
            return CEmuRunner(real_config)

    attempts: list[dict[str, object]] = []

    for current_attempt in range(1, configured_attempt_count + 1):
        attempt_evidence_dir = _resolve_attempt_evidence_dir(
            resolved_evidence_dir,
            attempt_number=current_attempt,
            attempt_count=configured_attempt_count,
        )
        attempt_output_path = attempt_evidence_dir / "summary.json"
        attempt_summary = _run_release_candidate_gate_attempt(
            config_path=config_path,
            output_path=attempt_output_path,
            evidence_dir=attempt_evidence_dir,
            active_environment=active_environment,
            runner_factory=attempt_runner_factory,
            timeout_s=timeout_s,
            multiplication_case=multiplication_case,
            mode_switch_cases=mode_switch_cases,
            arithmetic_baseline_path=arithmetic_baseline_path,
            module_baseline_path=module_baseline_path,
            visible_smoke_case_path=visible_smoke_case_path,
            attempt_number=current_attempt,
            attempt_count=configured_attempt_count,
        )
        _write_json(attempt_output_path, attempt_summary)
        attempts.append(attempt_summary)
        if configured_stop_on_first_failure and not bool(attempt_summary.get("success")):
            break

    representative_attempt = next((attempt for attempt in attempts if not bool(attempt.get("success"))), None)
    if representative_attempt is None and attempts:
        representative_attempt = attempts[-1]

    if representative_attempt is not None:
        aggregate_summary["artifice_jailbreak"] = deepcopy(representative_attempt.get("artifice_jailbreak", {}))
        aggregate_summary["launch_verification"] = deepcopy(representative_attempt.get("launch_verification", {}))
        aggregate_summary["first_try_confirmation_checklist"] = deepcopy(
            representative_attempt.get("first_try_confirmation_checklist", {})
        )
        aggregate_summary["archive_lifecycle"] = deepcopy(representative_attempt.get("archive_lifecycle", {}))
        aggregate_summary["visible_smoke_summary"] = deepcopy(representative_attempt.get("visible_smoke_summary", {}))
        aggregate_summary["capability_depth"] = deepcopy(representative_attempt.get("capability_depth", {}))
        aggregate_summary["smoke_tests"] = deepcopy(representative_attempt.get("smoke_tests", []))
        aggregate_summary["totals"] = deepcopy(representative_attempt.get("totals", {}))
        aggregate_summary["failure_stage"] = str(representative_attempt.get("failure_stage", ""))
        aggregate_summary["selected_attempt_number"] = int(representative_attempt.get("attempt_number", 0))
        aggregate_summary["selected_attempt_output_path"] = str(representative_attempt.get("output_path", ""))

    passed_attempt_count = sum(1 for attempt in attempts if bool(attempt.get("success")))
    failed_attempts = [attempt for attempt in attempts if not bool(attempt.get("success"))]
    failed_attempt_numbers = [int(attempt.get("attempt_number", 0)) for attempt in failed_attempts]
    halted_early = len(attempts) < configured_attempt_count

    stability = _summarize_attempt_stability(attempts, expected_attempt_count=configured_attempt_count)
    archive_lifecycle_stability = _summarize_archive_lifecycle_stability(
        attempts,
        expected_attempt_count=configured_attempt_count,
    )
    aggregate_summary["attempts"] = attempts
    aggregate_summary["attempts_summary"] = {
        "configured_attempt_count": configured_attempt_count,
        "required_passing_attempts": configured_required_passing_attempts,
        "executed_attempt_count": len(attempts),
        "passed_attempt_count": passed_attempt_count,
        "failed_attempt_count": len(failed_attempts),
        "passed_attempt_numbers": [
            int(attempt.get("attempt_number", 0))
            for attempt in attempts
            if bool(attempt.get("success"))
        ],
        "failed_attempt_numbers": failed_attempt_numbers,
        "halted_early": halted_early,
        "stop_on_first_failure": configured_stop_on_first_failure,
        "attempt_confidence_score_percent": round(
            (passed_attempt_count / configured_attempt_count) * 100.0,
            2,
        ),
    }
    aggregate_summary["stability"] = {
        **stability,
        "enforced": configured_enforce_smoke_crc_stability,
    }
    aggregate_summary["archive_lifecycle_stability"] = archive_lifecycle_stability

    threshold_met = passed_attempt_count >= configured_required_passing_attempts
    all_attempts_passed = (
        len(attempts) == configured_attempt_count and passed_attempt_count == configured_attempt_count
    )
    stability_relevant = configured_enforce_smoke_crc_stability and all_attempts_passed
    stability_met = bool(stability["stable"]) or not stability_relevant
    archive_lifecycle_stability_met = bool(archive_lifecycle_stability["stable"]) or not all_attempts_passed
    aggregate_summary["success"] = bool(threshold_met and stability_met and archive_lifecycle_stability_met)
    aggregate_summary["release_candidate_ready"] = bool(aggregate_summary["success"])
    aggregate_summary["status"] = "passed" if aggregate_summary["success"] else "failed"
    aggregate_summary["exit_code"] = DEFAULT_EXIT_PASS if aggregate_summary["success"] else DEFAULT_EXIT_FAIL

    blockers: list[str] = []
    diagnostics: list[str] = []
    diagnostics.append(
        "Repeated release-candidate validation executed "
        f"{len(attempts)}/{configured_attempt_count} configured attempt(s) "
        f"with {passed_attempt_count} passing attempt(s)."
    )
    if threshold_met:
        diagnostics.append(
            f"Passing-attempt threshold satisfied: {passed_attempt_count}/{configured_attempt_count} "
            f"attempt(s) passed with requirement {configured_required_passing_attempts}."
        )
    else:
        blockers.append(
            f"Repeated release-candidate validation passed {passed_attempt_count}/{configured_attempt_count} attempt(s); "
            f"{configured_required_passing_attempts} passing attempt(s) are required."
        )
    if stability_relevant and not bool(stability["stable"]):
        unstable_names = ", ".join(str(name) for name in stability["unstable_smoke_tests"])
        blockers.append(
            "Smoke CRC stability drift was detected across repeated attempts"
            + (f" for: {unstable_names}." if unstable_names else ".")
        )
    elif stability_relevant:
        diagnostics.append("Smoke CRC stability matched across all repeated attempts.")
    elif configured_enforce_smoke_crc_stability:
        diagnostics.append("Smoke CRC stability was not evaluated because repeated validation did not complete with all attempts passing.")
    else:
        diagnostics.append("Smoke CRC stability enforcement was disabled for this run.")
    if all_attempts_passed and not bool(archive_lifecycle_stability["stable"]):
        blockers.append("Archive lifecycle drift was detected across repeated attempts.")
    elif all_attempts_passed:
        diagnostics.append("Archive lifecycle counts matched across all repeated attempts.")
    else:
        diagnostics.append(
            "Archive lifecycle stability across attempts was not evaluated because repeated validation did not complete with all attempts passing."
        )

    first_try_confirmation_checklist = aggregate_summary.get("first_try_confirmation_checklist", {})
    if bool(first_try_confirmation_checklist.get("available")):
        diagnostics.append(
            "First-try confirmation checklist covered "
            f"{int(first_try_confirmation_checklist.get('covered_item_count', 0))}/"
            f"{int(first_try_confirmation_checklist.get('required_item_count', 0))} required items "
            f"with {int(first_try_confirmation_checklist.get('passed_item_count', 0))} currently passing; "
            f"{int(first_try_confirmation_checklist.get('inferred_crc_item_count', 0))} item(s) still rely on inferred delegated CRC provenance."
        )
    elif first_try_confirmation_checklist:
        diagnostics.append(
            "First-try confirmation checklist is unavailable: "
            f"{str(first_try_confirmation_checklist.get('reason', 'unknown reason')).strip()}"
        )

    capability_depth = aggregate_summary.get("capability_depth", {})
    if capability_depth:
        diagnostics.append(
            "Capability-depth evidence surfaces covered "
            f"{int(capability_depth.get('covered_item_count', 0))}/"
            f"{int(capability_depth.get('required_item_count', 0))} with "
            f"{int(capability_depth.get('passed_item_count', 0))} currently passing."
        )

    availability = aggregate_summary.get("availability", {})
    rom_validation = availability.get("rom_validation", {}) if isinstance(availability, Mapping) else {}
    if isinstance(rom_validation, Mapping) and rom_validation:
        diagnostics.append(
            "ROM provenance for this gate run: "
            f"model={str(rom_validation.get('model', '')).strip() or '<unknown>'}, "
            f"os_version={str(rom_validation.get('os_version', '')).strip() or '<unknown>'}, "
            f"firmware_family={str(rom_validation.get('firmware_family', '')).strip() or '<unknown>'}, "
            f"recognized_fingerprint={bool(rom_validation.get('recognized_fingerprint', False))}, "
            f"known_good_hash_match={bool(rom_validation.get('known_good_hash_match', False))}, "
            f"sha256={str(rom_validation.get('sha256', '')).strip() or '<unknown>'}."
        )

    if failed_attempts:
        first_failed_attempt = failed_attempts[0]
        first_failed_attempt_number = int(first_failed_attempt.get("attempt_number", 0))
        aggregate_summary["failure_stage"] = str(first_failed_attempt.get("failure_stage", ""))
        blockers.extend(
            f"Attempt {first_failed_attempt_number}: {message}"
            for message in first_failed_attempt.get("blockers", ())
        )
        diagnostics.extend(
            f"Attempt {first_failed_attempt_number}: {message}"
            for message in first_failed_attempt.get("diagnostics", ())
        )

    aggregate_summary["blockers"] = blockers
    aggregate_summary["diagnostics"] = diagnostics
    aggregate_summary["duration_seconds"] = round(time.time() - started_at, 2)
    return aggregate_summary


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the real-ROM CalcEngine release-candidate confidence gate.")
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_GATE_CONFIG_PATH,
        help="Path to the release-candidate gate configuration JSON.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Path to write the machine-readable confidence report JSON.",
    )
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        default=None,
        help="Directory for per-checkpoint state snapshots.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=None,
        help="Override the default per-stage timeout in seconds.",
    )
    parser.add_argument(
        "--attempts",
        type=int,
        default=None,
        help="Override the configured number of full release-candidate attempts to execute.",
    )
    parser.add_argument(
        "--required-passing-attempts",
        type=int,
        default=None,
        help="Override the configured minimum number of passing attempts required for release readiness.",
    )
    parser.set_defaults(
        stop_on_first_failure=None,
        enforce_smoke_crc_stability=None,
    )
    failure_group = parser.add_mutually_exclusive_group()
    failure_group.add_argument(
        "--stop-on-first-failure",
        dest="stop_on_first_failure",
        action="store_true",
        help="Stop repeated validation as soon as one attempt fails.",
    )
    failure_group.add_argument(
        "--keep-going-after-failure",
        dest="stop_on_first_failure",
        action="store_false",
        help="Continue running the remaining configured attempts after a failure.",
    )
    stability_group = parser.add_mutually_exclusive_group()
    stability_group.add_argument(
        "--enforce-smoke-crc-stability",
        dest="enforce_smoke_crc_stability",
        action="store_true",
        help="Require every repeated attempt to report identical smoke-test CRC signatures.",
    )
    stability_group.add_argument(
        "--allow-smoke-crc-drift",
        dest="enforce_smoke_crc_stability",
        action="store_false",
        help="Disable the repeated-attempt smoke CRC stability requirement.",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    summary = run_release_candidate_gate(
        config_path=args.config,
        output_path=args.output,
        evidence_dir=args.evidence_dir,
        default_timeout_s=args.timeout,
        attempt_count=args.attempts,
        required_passing_attempts=args.required_passing_attempts,
        stop_on_first_failure=args.stop_on_first_failure,
        enforce_smoke_crc_stability=args.enforce_smoke_crc_stability,
    )
    output_path = Path(str(summary["output_path"]))
    _write_json(output_path, summary)
    return int(summary["exit_code"])


if __name__ == "__main__":
    raise SystemExit(main())
