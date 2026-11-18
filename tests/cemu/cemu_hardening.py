from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from accuracy_audit import (
    DEFAULT_ACCURACY_BASELINE_PATH,
    DEFAULT_CASE_RESET_SEQUENCE,
    load_accuracy_cases,
    run_accuracy_audit,
    summarize_accuracy_audit,
)
from cemu_runner import (
    CEmuCommandError,
    CEmuProcessTimeoutError,
    CEmuRunner,
    CEmuRunnerConfig,
    CEmuRunnerError,
)
from keypress_sequences import KeyAction
from pipeline_support import (
    DEFAULT_BOOT_SEQUENCE,
    DEFAULT_DOWNLOADS_DIR,
    DEFAULT_MANIFEST_PATH,
    build_runner_config,
    compute_crc32_hex,
    resolve_real_cemu_environment,
    resolve_transfer_package,
    run_smoke_checklist,
    stage_transfer_package,
    summarize_checkpoints,
)
DEFAULT_EDGE_CASE_BASELINE_PATH = REPO_ROOT / "tests" / "cemu" / "edge-cases" / "real-edge-cases.json"
DEFAULT_FAKE_CASE_BASELINE_PATH = REPO_ROOT / "tests" / "cemu" / "edge-cases" / "fake-hardening-cases.json"
DEFAULT_ARITHMETIC_BATTERY_PATH = REPO_ROOT / "tests" / "cemu" / "edge-cases" / "arithmetic-battery.json"
DEFAULT_MODULE_BATTERY_PATH = REPO_ROOT / "tests" / "cemu" / "edge-cases" / "module-battery.json"
DEFAULT_PIPELINE_CONFIG_PATH = REPO_ROOT / "tests" / "cemu" / "ci-config" / "cemu-validate-config.json"
DEFAULT_PIPELINE_SUMMARY_PATH = REPO_ROOT / "tests" / "cemu" / "outputs" / "cemu-pipeline-summary.json"
DEFAULT_PIPELINE_SNAPSHOT_PATH = REPO_ROOT / "tests" / "cemu" / "outputs" / "cemu-pipeline-snapshot.json"
DEFAULT_PROMPT_CRCS = ("349F4775", "A32840C8", "7C622B05", "7F0F6A91", "DAE8FE30", "B76BC405")
# TI-84 Plus CE ALPHA layer exactly as printed on the keycaps (row order:
# math..÷ = A..M, log..2 = N..Z). Must match src/core/launcher/main.c.
ALPHA_KEY_MAP: Mapping[str, str] = {
    "a": "math",
    "b": "apps",
    "c": "prgm",
    "d": "-1",
    "e": "sin",
    "f": "cos",
    "g": "tan",
    "h": "^",
    "i": "^2",
    "j": ",",
    "k": "(",
    "l": ")",
    "m": "/",
    "n": "log",
    "o": "7",
    "p": "8",
    "q": "9",
    "r": "*",
    "s": "ln",
    "t": "4",
    "u": "5",
    "v": "6",
    "w": "-",
    "x": "sto",
    "y": "1",
    "z": "2",
}
EXPRESSION_KEY_MAP: Mapping[str, str] = {
    "+": "+",
    "-": "-",
    "*": "*",
    "/": "/",
    "^": "^",
    ".": ".",
    "(": "(",
    ")": ")",
    ",": ",",
    ";": "apps",
}


@dataclass(frozen=True)
class HardeningCase:
    name: str
    suite: str
    category: str
    backend: str
    module: str
    description: str
    expected_crcs: tuple[str, ...]
    input_expression: str
    input_tokens: tuple[str, ...]
    events: tuple[Mapping[str, Any], ...]
    post_events: tuple[Mapping[str, Any], ...]
    timeout_ms: int
    key_delay_ms: int
    enter_settle_delay_ms: int
    result_delay_ms: int
    auto_enter: bool
    prompt_reset: bool
    expected_output_excerpt_contains: tuple[str, ...]
    expected_display: str
    notes: str

    def to_summary(self) -> dict[str, object]:
        return {
            "name": self.name,
            "suite": self.suite,
            "category": self.category,
            "backend": self.backend,
            "module": self.module,
            "description": self.description,
            "expected_crcs": list(self.expected_crcs),
            "input_expression": self.input_expression,
            "input_tokens": list(self.input_tokens),
            "events": [dict(event) for event in self.events],
            "post_events": [dict(event) for event in self.post_events],
            "timeout_ms": self.timeout_ms,
            "key_delay_ms": self.key_delay_ms,
            "enter_settle_delay_ms": self.enter_settle_delay_ms,
            "result_delay_ms": self.result_delay_ms,
            "auto_enter": self.auto_enter,
            "prompt_reset": self.prompt_reset,
            "expected_output_excerpt_contains": list(self.expected_output_excerpt_contains),
            "expected_display": self.expected_display,
            "notes": self.notes,
        }


def _normalize_event_spec(row: Mapping[str, Any]) -> dict[str, Any]:
    kind = str(row.get("kind", "key"))
    if kind == "key":
        return {"kind": "key", "key": str(row["key"])}
    if kind == "pause":
        return {"kind": "pause", "duration_ms": int(row["duration_ms"])}
    raise ValueError(f"Unsupported event kind: {kind}")


def _key_event(key: str) -> dict[str, Any]:
    return {"kind": "key", "key": key}


def _pause_event(duration_ms: int) -> dict[str, Any]:
    return {"kind": "pause", "duration_ms": duration_ms}


def _looks_like_scientific_notation(expression_text: str, index: int) -> bool:
    if expression_text[index].lower() != "e":
        return False
    previous = expression_text[index - 1] if index > 0 else ""
    following = expression_text[index + 1] if index + 1 < len(expression_text) else ""
    return (previous.isdigit() or previous == ".") and (following.isdigit() or following in "+-")


def _expression_to_tokens(expression_text: str) -> tuple[str, ...]:
    tokens: list[str] = []
    for index, character in enumerate(expression_text):
        if character.isspace():
            continue
        if character.isdigit():
            tokens.append(character)
            continue
        mapped = EXPRESSION_KEY_MAP.get(character)
        if mapped is not None:
            tokens.append(mapped)
            continue
        if character.isalpha():
            if _looks_like_scientific_notation(expression_text, index):
                tokens.extend(("2nd", "(-)"))
                continue
            alpha_key = ALPHA_KEY_MAP.get(character.lower())
            if alpha_key is None:
                raise ValueError(f"Unsupported expression character: {character}")
            tokens.extend(("alpha", alpha_key))
            continue
        raise ValueError(f"Unsupported expression character: {character}")
    return tuple(tokens)


def _build_case_events(case: HardeningCase) -> list[Mapping[str, Any]]:
    if case.events:
        return [dict(event) for event in case.events]

    events: list[Mapping[str, Any]] = []
    for token in case.input_tokens:
        events.append(_key_event(token))
        events.append(_pause_event(case.key_delay_ms))
    if case.auto_enter:
        events.append(_pause_event(case.enter_settle_delay_ms))
        events.append(_key_event("enter"))
        events.append(_pause_event(case.result_delay_ms))
    events.extend(dict(event) for event in case.post_events)
    return events


def load_hardening_cases(path: Path) -> tuple[HardeningCase, ...]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return tuple(
        HardeningCase(
            name=str(row["name"]),
            suite=str(row["suite"]),
            category=str(row["category"]),
            backend=str(row.get("backend", "real")),
            module=str(row.get("module", "")),
            description=str(row["description"]),
            expected_crcs=tuple(str(value).upper() for value in row["expected_crcs"]),
            input_expression=str(row.get("input_expression", "")),
            input_tokens=(
                tuple(str(value) for value in row["input_tokens"])
                if "input_tokens" in row
                else _expression_to_tokens(str(row.get("input_expression", "")))
            ),
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
        for row in payload["cases"]
    )


def summarize_case_results(
    cases: Sequence[HardeningCase],
    results: Sequence[Mapping[str, Any]],
) -> dict[str, object]:
    passed = [result for result in results if bool(result.get("passed"))]
    failed = [result for result in results if not bool(result.get("passed"))]
    timed_out = [result for result in results if str(result.get("status")) == "timeout"]
    error_count = sum(1 for result in results if str(result.get("status")) == "error")
    return {
        "case_count": len(cases),
        "passed_count": len(passed),
        "failed_count": len(failed),
        "timeout_count": len(timed_out),
        "error_count": error_count,
        "failed_names": [str(result["name"]) for result in failed],
        "timed_out_names": [str(result["name"]) for result in timed_out],
        "recovered_from_boot_failure_names": [
            str(result["name"])
            for result in results
            if bool(result.get("recovered_from_boot_failure"))
        ],
        "failure_details": [
            {
                "name": str(result["name"]),
                "status": str(result.get("status", "")),
                "failure_stage": str(result.get("failure_stage", "")),
                "module": str(result.get("module", "")),
                "observed_crc": str(result.get("observed_crc", "")),
                "expected_crcs": list(result.get("expected_crcs", [])),
                "error_message": str(result.get("error_message", "")),
                "diagnostics": list(result.get("diagnostics", [])),
            }
            for result in failed
        ],
        "modules": sorted({case.module for case in cases if case.module}),
        "suites": sorted({case.suite for case in cases}),
        "categories": sorted({case.category for case in cases}),
        "backends": sorted({case.backend for case in cases}),
        "success": not failed,
    }


def _runner_debug_payload(runner: Any, tail_lines: int = 8) -> dict[str, list[str]]:
    debug_payload: dict[str, list[str]] = {
        "stdout_tail": [],
        "stderr_tail": [],
    }
    for field_name in ("stdout_lines", "stderr_lines"):
        lines = getattr(runner, field_name, ())
        normalized = [str(line) for line in lines if str(line).strip()]
        debug_payload[f"{field_name[:-6]}_tail"] = normalized[-tail_lines:]
    return debug_payload


def _append_runner_debug(diagnostics: list[str], runner_debug: Mapping[str, Sequence[str]]) -> None:
    stdout_tail = [str(line) for line in runner_debug.get("stdout_tail", ()) if str(line).strip()]
    stderr_tail = [str(line) for line in runner_debug.get("stderr_tail", ()) if str(line).strip()]
    if stdout_tail:
        diagnostics.append(f"runner stdout tail: {' | '.join(stdout_tail)}")
    if stderr_tail:
        diagnostics.append(f"runner stderr tail: {' | '.join(stderr_tail)}")


def _boot_runner(
    runner: CEmuRunner,
    *,
    program_paths: Iterable[Path],
    boot_timeout_s: float,
) -> None:
    runner.boot(program_paths=program_paths, ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=boot_timeout_s)


def run_hardening_cases(
    *,
    make_runner: Callable[[], CEmuRunner],
    program_paths: Sequence[Path],
    cases: Sequence[HardeningCase],
    default_timeout_s: float = 30.0,
    prompt_reset_sequence: Sequence[KeyAction] = DEFAULT_CASE_RESET_SEQUENCE,
    boot_retry_count: int = 1,
) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    runner: CEmuRunner | None = None

    def ensure_runner(
        case_timeout_s: float,
        diagnostics: list[str],
    ) -> tuple[CEmuRunner, int, bool]:
        nonlocal runner
        if runner is not None and runner.is_running:
            return runner, 0, False

        last_error: Exception | None = None
        for attempt in range(1, boot_retry_count + 2):
            candidate = make_runner()
            try:
                _boot_runner(candidate, program_paths=program_paths, boot_timeout_s=case_timeout_s)
                runner = candidate
                if attempt > 1:
                    diagnostics.append(
                        f"Recovered after {attempt - 1} boot failure(s); boot attempt {attempt} succeeded."
                    )
                return candidate, attempt, attempt > 1
            except (CEmuProcessTimeoutError, CEmuCommandError, CEmuRunnerError, OSError, ValueError, KeyError) as exc:
                last_error = exc
                diagnostics.append(f"boot attempt {attempt} failed: {exc}")
                _append_runner_debug(diagnostics, _runner_debug_payload(candidate))
                candidate.shutdown()
                if attempt > boot_retry_count:
                    raise

        assert last_error is not None
        raise last_error

    for case in cases:
        started_at = time.time()
        case_timeout_s = max(default_timeout_s, case.timeout_ms / 1000.0)
        observed_crc = ""
        output_excerpt = ""
        status = "passed"
        passed = False
        error_message = ""
        diagnostics: list[str] = []
        runner_debug = {
            "stdout_tail": [],
            "stderr_tail": [],
        }
        failure_stage = ""
        boot_attempts = 0
        recovered_from_boot_failure = False

        try:
            failure_stage = "boot"
            active_runner, boot_attempts, recovered_from_boot_failure = ensure_runner(case_timeout_s, diagnostics)
            failure_stage = "reset"
            active_runner.reset(timeout_s=case_timeout_s)
            if case.prompt_reset:
                failure_stage = "prompt_reset"
                active_runner.send_sequence(prompt_reset_sequence, timeout_s=case_timeout_s)
            failure_stage = "input_sequence"
            active_runner.send_sequence(_build_case_events(case), timeout_s=case_timeout_s)
            failure_stage = "capture_screen"
            response = active_runner.capture_screen(timeout_s=case_timeout_s)
            observed_crc = str(response.payload.get("crc", "")).upper()
            output_excerpt = str(response.payload.get("output_excerpt", ""))
            failure_stage = "assertion"
            passed = observed_crc in case.expected_crcs
            if not passed:
                diagnostics.append(
                    f"Observed CRC {observed_crc or '<empty>'} did not match expected CRCs {', '.join(case.expected_crcs)}."
                )
            if case.expected_output_excerpt_contains:
                excerpt_ok = all(
                    expected_fragment.lower() in output_excerpt.lower()
                    for expected_fragment in case.expected_output_excerpt_contains
                )
                if not excerpt_ok:
                    diagnostics.append(
                        "Observed output excerpt did not contain all required fragments: "
                        + ", ".join(case.expected_output_excerpt_contains)
                    )
                passed = passed and excerpt_ok
            status = "passed" if passed else "failed"
        except CEmuProcessTimeoutError as exc:
            status = "timeout"
            error_message = str(exc)
            diagnostics.append(
                f"Case timed out during {failure_stage or 'unknown'} after {case_timeout_s:.1f}s: {exc}"
            )
            if runner is not None:
                runner_debug = _runner_debug_payload(runner)
                _append_runner_debug(diagnostics, runner_debug)
                runner.shutdown()
            runner = None
        except (CEmuCommandError, CEmuRunnerError, OSError, ValueError, KeyError) as exc:
            status = "error"
            error_message = str(exc)
            diagnostics.append(
                f"Case failed during {failure_stage or 'unknown'}: {exc}"
            )
            if runner is not None:
                runner_debug = _runner_debug_payload(runner)
                _append_runner_debug(diagnostics, runner_debug)
                runner.shutdown()
            runner = None

        results.append(
            {
                "name": case.name,
                "suite": case.suite,
                "category": case.category,
                "backend": case.backend,
                "module": case.module,
                "description": case.description,
                "expected_crcs": list(case.expected_crcs),
                "input_expression": case.input_expression,
                "observed_crc": observed_crc,
                "expected_output_excerpt_contains": list(case.expected_output_excerpt_contains),
                "observed_output_excerpt": output_excerpt,
                "expected_display": case.expected_display,
                "passed": passed,
                "status": status,
                "timed_out": status == "timeout",
                "error_message": error_message,
                "failure_stage": "" if passed else failure_stage,
                "timeout_ms": case.timeout_ms,
                "notes": case.notes,
                "boot_attempts": boot_attempts,
                "recovered_from_boot_failure": recovered_from_boot_failure,
                "diagnostics": diagnostics,
                "runner_debug": runner_debug,
                "duration_seconds": round(time.time() - started_at, 2),
            }
        )

    if runner is not None:
        runner.shutdown()

    return results


def _artifact_manifest_rows(
    package,
    *,
    observed_paths: Mapping[str, Path] | None = None,
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for entry in (*package.module_entries, *package.runtime_entries):
        resolved_path = (
            observed_paths.get(entry.name)
            if observed_paths is not None
            else package.resolved_paths.get(entry.name)
        )
        actual_crc = ""
        actual_size = -1
        exists = resolved_path is not None and resolved_path.exists()
        if exists and resolved_path is not None:
            actual_crc = compute_crc32_hex(resolved_path)
            actual_size = resolved_path.stat().st_size
        rows.append(
            {
                "name": entry.name,
                "kind": entry.kind,
                "expected_crc32": entry.crc32,
                "actual_crc32": actual_crc,
                "expected_size_bytes": entry.size_bytes,
                "actual_size_bytes": actual_size,
                "exists": exists,
                "resolved_path": str(resolved_path) if resolved_path is not None else "",
                "matches_manifest": exists
                and actual_crc == entry.crc32
                and actual_size == entry.size_bytes,
            }
        )
    return rows


def run_module_loading_verification(
    *,
    make_runner: Callable[[], CEmuRunner],
    program_paths: Sequence[Path],
    transfer_package,
    snapshot_path: Path,
    default_timeout_s: float = 30.0,
) -> dict[str, object]:
    runner = make_runner()
    try:
        _boot_runner(runner, program_paths=program_paths, boot_timeout_s=default_timeout_s)
        runner.snapshot(snapshot_path, timeout_s=default_timeout_s)
        state = runner.get_state(timeout_s=default_timeout_s).payload
    finally:
        runner.shutdown()

    payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
    transfer_names = [Path(path).name for path in payload["transfer_files"]]
    transfer_name_set = set(transfer_names)
    expected_transfer_order = sorted(
        [*transfer_package.module_names, *transfer_package.runtime_names],
        key=str.lower,
    )
    expected_module_names = set(transfer_package.module_names)
    expected_runtime_names = set(transfer_package.runtime_names)
    observed_transfer_paths = {
        Path(raw_path).name: Path(raw_path)
        for raw_path in payload["transfer_files"]
    }
    artifact_rows = _artifact_manifest_rows(
        transfer_package,
        observed_paths=observed_transfer_paths,
    )
    manifest_ok = all(bool(row["matches_manifest"]) for row in artifact_rows)

    archive_slot_counts: dict[str, int] = {}
    for name in transfer_names:
        slot_name = Path(name).stem.upper()[:8]
        archive_slot_counts[slot_name] = archive_slot_counts.get(slot_name, 0) + 1
    duplicate_archive_slots = sorted(
        slot_name
        for slot_name, count in archive_slot_counts.items()
        if count > 1
    )
    load_order_mismatches: list[dict[str, object]] = []
    for index in range(max(len(expected_transfer_order), len(transfer_names))):
        expected_name = expected_transfer_order[index] if index < len(expected_transfer_order) else ""
        observed_name = transfer_names[index] if index < len(transfer_names) else ""
        if expected_name != observed_name:
            load_order_mismatches.append(
                {
                    "index": index,
                    "expected_name": expected_name,
                    "observed_name": observed_name,
                }
            )
    load_order_matches_manifest = not load_order_mismatches

    missing_modules = sorted(expected_module_names - transfer_name_set)
    missing_runtime = sorted(expected_runtime_names - transfer_name_set)
    unexpected_entries = sorted(
        transfer_name_set - expected_module_names - expected_runtime_names
    )
    observed_module_names = sorted(
        name for name in transfer_name_set if name.lower().endswith(".8xp")
    )
    observed_runtime_names = sorted(
        name for name in transfer_name_set if name.lower().endswith(".8xv")
    )
    corrupted_entries = sorted(
        str(row["name"])
        for row in artifact_rows
        if bool(row["exists"]) and not bool(row["matches_manifest"])
    )
    diagnostics: list[str] = []
    if missing_modules:
        diagnostics.append(
            "Missing staged modules in transferred archive payload: " + ", ".join(missing_modules)
        )
    if missing_runtime:
        diagnostics.append(
            "Missing staged runtime dependencies in transferred archive payload: "
            + ", ".join(missing_runtime)
        )
    if unexpected_entries:
        diagnostics.append(
            "Unexpected entries were transferred to the emulator archive: "
            + ", ".join(unexpected_entries)
        )
    if duplicate_archive_slots:
        diagnostics.append(
            "Duplicate 8-character archive slots detected: " + ", ".join(duplicate_archive_slots)
        )
    if corrupted_entries:
        diagnostics.append(
            "Transferred files failed manifest size/CRC verification: " + ", ".join(corrupted_entries)
        )
    if not load_order_matches_manifest:
        diagnostics.append(
            "Transferred archive payload order deviated from the deterministic staged filename order: "
            + "; ".join(
                f"index {row['index']}: expected {row['expected_name'] or '<none>'}, observed {row['observed_name'] or '<none>'}"
                for row in load_order_mismatches
            )
        )

    success = (
        manifest_ok
        and int(state.get("app_count", 0)) == transfer_package.transfer_count
        and load_order_matches_manifest
        and not duplicate_archive_slots
        and not missing_modules
        and not missing_runtime
        and not unexpected_entries
        and set(observed_module_names) == expected_module_names
        and set(observed_runtime_names) == expected_runtime_names
    )

    return {
        "success": success,
        "snapshot_path": str(snapshot_path),
        "manifest_checks": artifact_rows,
        "expected_module_count": transfer_package.module_count,
        "expected_runtime_dependency_count": transfer_package.runtime_dependency_count,
        "expected_transfer_count": transfer_package.transfer_count,
        "expected_transfer_order": expected_transfer_order,
        "observed_app_count": int(state.get("app_count", 0)),
        "observed_transfer_count": len(transfer_names),
        "observed_transfer_order": transfer_names,
        "load_order_matches_manifest": load_order_matches_manifest,
        "load_order_mismatches": load_order_mismatches,
        "missing_modules": missing_modules,
        "missing_runtime_dependencies": missing_runtime,
        "unexpected_entries": unexpected_entries,
        "duplicate_archive_slots": duplicate_archive_slots,
        "mismatched_manifest_entries": corrupted_entries,
        "observed_module_names": observed_module_names,
        "observed_runtime_dependency_names": observed_runtime_names,
        "diagnostics": diagnostics,
    }


def run_real_pipeline_sections(
    *,
    environment,
    staged_transfer_dir: Path,
    edge_case_path: Path,
    visible_smoke_case_path: Path,
    arithmetic_case_path: Path,
    module_case_path: Path,
    snapshot_path: Path,
    default_timeout_s: float,
) -> dict[str, object]:
    from visible_smoke import (
        load_visible_smoke_cases,
        run_visible_smoke_cases,
        summarize_visible_smoke_results,
    )

    real_config = build_runner_config(environment)

    def make_runner() -> CEmuRunner:
        return CEmuRunner(real_config)

    module_snapshot_path = snapshot_path.with_name("cemu-module-verification-snapshot.json")
    module_verification = run_module_loading_verification(
        make_runner=make_runner,
        program_paths=[staged_transfer_dir],
        transfer_package=environment.transfer_package,
        snapshot_path=module_snapshot_path,
        default_timeout_s=default_timeout_s,
    )

    with CEmuRunner(real_config) as smoke_runner:
        smoke_runner.boot(program_paths=[staged_transfer_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE)
        checkpoints = run_smoke_checklist(smoke_runner)
    checkpoint_summary = summarize_checkpoints(checkpoints)

    visible_smoke_cases = load_visible_smoke_cases(visible_smoke_case_path)
    visible_smoke_results = run_visible_smoke_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_dir],
        cases=visible_smoke_cases,
        default_timeout_s=default_timeout_s,
    )
    visible_smoke_summary = summarize_visible_smoke_results(
        visible_smoke_cases,
        visible_smoke_results,
    )

    accuracy_cases = load_accuracy_cases(DEFAULT_ACCURACY_BASELINE_PATH)
    with CEmuRunner(real_config) as accuracy_runner:
        accuracy_runner.boot(program_paths=[staged_transfer_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE)
        accuracy_results = run_accuracy_audit(accuracy_runner, accuracy_cases)
        accuracy_summary = summarize_accuracy_audit(accuracy_cases, accuracy_results)

    arithmetic_cases = load_hardening_cases(arithmetic_case_path)
    arithmetic_catalog = [case for case in arithmetic_cases if case.backend == "real"]
    arithmetic_results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_dir],
        cases=arithmetic_catalog,
        default_timeout_s=default_timeout_s,
    )
    arithmetic_summary = summarize_case_results(arithmetic_catalog, arithmetic_results)

    module_cases = load_hardening_cases(module_case_path)
    module_catalog = [case for case in module_cases if case.backend == "real"]
    module_results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_dir],
        cases=module_catalog,
        default_timeout_s=default_timeout_s,
    )
    module_summary = summarize_case_results(module_catalog, module_results)

    edge_cases = load_hardening_cases(edge_case_path)
    edge_case_catalog = [case for case in edge_cases if case.suite == "edge_cases" and case.backend == "real"]
    recovery_catalog = [case for case in edge_cases if case.suite == "recovery" and case.backend == "real"]

    edge_case_results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_dir],
        cases=edge_case_catalog,
        default_timeout_s=default_timeout_s,
    )
    recovery_results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_dir],
        cases=recovery_catalog,
        default_timeout_s=default_timeout_s,
    )
    edge_case_summary = summarize_case_results(edge_case_catalog, edge_case_results)
    recovery_summary = summarize_case_results(recovery_catalog, recovery_results)

    with CEmuRunner(real_config) as snapshot_runner:
        snapshot_runner.boot(program_paths=[staged_transfer_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE)
        snapshot_runner.snapshot(snapshot_path, timeout_s=default_timeout_s)

    return {
        "module_verification": module_verification,
        "checkpoints": checkpoints,
        "checkpoint_summary": checkpoint_summary,
        "visible_smoke": visible_smoke_results,
        "visible_smoke_summary": {
            **visible_smoke_summary,
            "baseline_path": str(visible_smoke_case_path),
        },
        "accuracy_audit": accuracy_results,
        "accuracy_summary": {
            **accuracy_summary,
            "baseline_path": str(DEFAULT_ACCURACY_BASELINE_PATH),
        },
        "arithmetic_battery": arithmetic_results,
        "arithmetic_battery_summary": {
            **arithmetic_summary,
            "baseline_path": str(arithmetic_case_path),
        },
        "module_battery": module_results,
        "module_battery_summary": {
            **module_summary,
            "baseline_path": str(module_case_path),
            "comparison_mode": "screen_crc",
            "limitations": [
                "The current autotester-wrapper backend exposes CRC-only screen captures.",
                "Module routing is asserted by locked launcher-dispatch expressions rather than live child-program telemetry.",
            ],
        },
        "edge_cases": edge_case_results,
        "edge_case_summary": {
            **edge_case_summary,
            "baseline_path": str(edge_case_path),
        },
        "recovery_cases": recovery_results,
        "recovery_summary": {
            **recovery_summary,
            "baseline_path": str(edge_case_path),
            "accepted_prompt_crcs": list(DEFAULT_PROMPT_CRCS),
        },
        "snapshot_written": str(snapshot_path),
    }


def _write_temp_rom(path: Path) -> Path:
    path.write_bytes(b"FAKE-ROM")
    return path


def _stage_placeholder_transfer_package(transfer_package, destination_dir: Path) -> dict[str, object]:
    destination_dir.mkdir(parents=True, exist_ok=True)

    staged_names: list[str] = []
    for entry in (*transfer_package.module_entries, *transfer_package.runtime_entries):
        placeholder_path = destination_dir / entry.name
        placeholder_path.write_bytes(f"PLACEHOLDER:{entry.kind}:{entry.name}".encode("ascii"))
        staged_names.append(entry.name)

    if transfer_package.readme_entry is not None:
        (destination_dir / transfer_package.readme_entry.name).write_text(
            "CalcEngine placeholder transfer package for fake-only validation.\n",
            encoding="utf-8",
        )

    return {
        "mode": "placeholder",
        "staged_count": len(staged_names),
        "missing_entries": list(transfer_package.missing_entries),
    }


def run_fake_pipeline_sections(
    *,
    fake_case_path: Path,
    transfer_package,
    default_timeout_s: float,
) -> dict[str, object]:
    from fake_backend import build_fake_cemu_runner

    working_root = Path(tempfile.mkdtemp(prefix="calcengine-cemu-fake-"))
    temp_rom = _write_temp_rom(working_root / "Rom.rom")
    staged_transfer_dir = working_root / "transfer"
    transfer_package_summary: dict[str, object]
    if transfer_package.ready:
        stage_transfer_package(transfer_package, staged_transfer_dir)
        transfer_package_summary = {
            "mode": "resolved",
            "staged_count": transfer_package.transfer_count,
            "missing_entries": [],
        }
    else:
        transfer_package_summary = _stage_placeholder_transfer_package(transfer_package, staged_transfer_dir)
    fake_cases = load_hardening_cases(fake_case_path)

    def make_runner() -> CEmuRunner:
        return build_fake_cemu_runner(
            working_root,
            temp_rom,
            command_timeout_s=default_timeout_s,
            boot_timeout_s=default_timeout_s,
        )

    try:
        fake_catalog = [case for case in fake_cases if case.backend == "fake"]
        fake_results = run_hardening_cases(
            make_runner=make_runner,
            program_paths=[staged_transfer_dir],
            cases=fake_catalog,
            default_timeout_s=default_timeout_s,
        )
        fake_summary = summarize_case_results(fake_catalog, fake_results)
        return {
            "fake_hardening": fake_results,
            "fake_hardening_summary": {
                **fake_summary,
                "baseline_path": str(fake_case_path),
                "transfer_package_mode": str(transfer_package_summary["mode"]),
                "transfer_package_staged_count": int(transfer_package_summary["staged_count"]),
                "transfer_package_missing_entries": list(transfer_package_summary["missing_entries"]),
            },
        }
    finally:
        shutil.rmtree(working_root, ignore_errors=True)


def load_pipeline_config(path: Path = DEFAULT_PIPELINE_CONFIG_PATH) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def resolve_configured_path(
    raw_value: object | None,
    *,
    config_path: Path | None = None,
    default_path: Path | None = None,
) -> Path:
    if raw_value is None or not str(raw_value).strip():
        if default_path is None:
            raise ValueError("A configured path value was required but not provided.")
        return default_path.expanduser().resolve()

    expanded_text = os.path.expandvars(str(raw_value).strip())
    candidate = Path(expanded_text).expanduser()
    if candidate.is_absolute():
        return candidate.resolve()

    base_dir = config_path.resolve().parent if config_path is not None else REPO_ROOT
    return (base_dir / candidate).resolve()


def resolve_pipeline_environment(
    config: Mapping[str, Any],
    *,
    config_path: Path | None = None,
) -> tuple[Any, Any]:
    manifest_path = resolve_configured_path(
        config.get("manifest_path"),
        config_path=config_path,
        default_path=DEFAULT_MANIFEST_PATH,
    )
    downloads_dir = resolve_configured_path(
        config.get("downloads_dir"),
        config_path=config_path,
        default_path=DEFAULT_DOWNLOADS_DIR,
    )
    environment = resolve_real_cemu_environment(
        repo_root=REPO_ROOT,
        manifest_path=manifest_path,
        downloads_dir=downloads_dir,
    )
    transfer_package = resolve_transfer_package(
        repo_root=REPO_ROOT,
        manifest_path=manifest_path,
        downloads_dir=downloads_dir,
    )
    return environment, transfer_package
