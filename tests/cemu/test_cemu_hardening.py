from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

import pytest

from cemu_runner import CEmuProcessTimeoutError

REPO_ROOT = Path(__file__).resolve().parents[2]

from cemu_hardening import (
    DEFAULT_ARITHMETIC_BATTERY_PATH,
    DEFAULT_EDGE_CASE_BASELINE_PATH,
    DEFAULT_FAKE_CASE_BASELINE_PATH,
    DEFAULT_MODULE_BATTERY_PATH,
    load_hardening_cases,
    run_hardening_cases,
    run_fake_pipeline_sections,
    run_module_loading_verification,
    resolve_pipeline_environment,
    summarize_case_results,
)
from fake_backend import build_fake_cemu_runner
from pipeline_support import get_real_cemu_status
from pipeline_support import (
    TransferManifestEntry,
    TransferPackage,
    stage_transfer_package,
    validate_staged_transfer_directory,
)


REAL_CEMU_READY, REAL_CEMU_SKIP_REASON = get_real_cemu_status()


@dataclass(frozen=True)
class DummyCommandResult:
    command: str
    payload: Mapping[str, object]
    raw: Mapping[str, object]


class BootFailureOnFirstAttemptRunner:
    def __init__(self, delegate) -> None:
        self._delegate = delegate

    @property
    def is_running(self) -> bool:
        return self._delegate.is_running

    @property
    def stdout_lines(self) -> tuple[str, ...]:
        return tuple(getattr(self._delegate, "stdout_lines", ()))

    @property
    def stderr_lines(self) -> tuple[str, ...]:
        return tuple(getattr(self._delegate, "stderr_lines", ()))

    def boot(self, *args, **kwargs):
        raise CEmuProcessTimeoutError("synthetic ROM boot timeout before prompt recovery")

    def shutdown(self) -> None:
        self._delegate.shutdown()

    def __getattr__(self, name: str):
        return getattr(self._delegate, name)


class SnapshotOrderRunner:
    def __init__(self, transfer_files: Sequence[Path], app_count: int) -> None:
        self._transfer_files = [str(path) for path in transfer_files]
        self._app_count = app_count
        self.is_running = False

    def boot(self, *args, **kwargs):
        self.is_running = True
        return None

    def snapshot(self, path: Path, timeout_s: float | None = None):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"transfer_files": self._transfer_files}),
            encoding="utf-8",
        )
        return DummyCommandResult("snapshot", {"ok": True}, {"ok": True})

    def get_state(self, timeout_s: float | None = None):
        payload = {"app_count": self._app_count}
        return DummyCommandResult("get_state", payload, payload)

    def shutdown(self) -> None:
        self.is_running = False


def test_hardening_case_catalogs_cover_required_lanes() -> None:
    real_cases = load_hardening_cases(DEFAULT_EDGE_CASE_BASELINE_PATH)
    arithmetic_cases = load_hardening_cases(DEFAULT_ARITHMETIC_BATTERY_PATH)
    module_cases = load_hardening_cases(DEFAULT_MODULE_BATTERY_PATH)
    fake_cases = load_hardening_cases(DEFAULT_FAKE_CASE_BASELINE_PATH)

    real_names = {case.name for case in real_cases}
    arithmetic_names = {case.name for case in arithmetic_cases}
    module_names = {case.name for case in module_cases}
    fake_names = {case.name for case in fake_cases}

    assert "edge_division_by_zero" in real_names
    assert "edge_scientific_notation_large_literal" in real_names
    assert "edge_negative_exponent_parenthesized" in real_names
    assert "edge_repeated_equals_after_addition" in real_names
    assert "recovery_syntax_error_prompt_restore" in real_names
    assert "recovery_interrupted_operation_prompt_restore" in real_names
    assert len(arithmetic_cases) >= 30
    assert "arith_division_by_zero" in arithmetic_names
    assert "arith_overflow_14_digit_pow_9" in arithmetic_names
    assert "arith_sci_mul_1e_minus9_times_1e9" in arithmetic_names
    assert len(module_cases) >= 20
    assert "module_base_hex_255" in module_names
    assert "module_limit_advanced_cosine" in module_names
    assert "module_matrix_inverse_identity" in module_names
    assert "fake_memory_sto_rcl_roundtrip" in fake_names
    assert "fake_out_of_memory_recovery" in fake_names


def test_catalog_input_expression_expands_to_tokens(tmp_path: Path) -> None:
    catalog_path = tmp_path / "expression-catalog.json"
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "suite": "expression_test",
                "cases": [
                    {
                        "name": "expression_case",
                        "suite": "expression_suite",
                        "category": "parser",
                        "backend": "real",
                        "description": "Input expressions should expand into key tokens.",
                        "expected_crcs": ["DEADBEEF"],
                        "input_expression": "1e-9+hex(15)",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )

    case = load_hardening_cases(catalog_path)[0]

    assert case.input_expression == "1e-9+hex(15)"
    assert case.input_tokens == (
        "1",
        "2nd",
        "(-)",
        "-",
        "9",
        "+",
        "alpha",
        "^",
        "alpha",
        "sin",
        "alpha",
        "sto",
        "(",
        "1",
        "5",
        ")",
    )


def test_fake_hardening_suite_passes(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    temp_rom_path: Path,
) -> None:
    cases = load_hardening_cases(DEFAULT_FAKE_CASE_BASELINE_PATH)

    def make_runner():
        return build_fake_cemu_runner(tmp_path, temp_rom_path)

    results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=cases,
        default_timeout_s=5.0,
    )
    summary = summarize_case_results(cases, results)

    assert summary["success"] is True
    assert summary["failed_count"] == 0


def test_timeout_case_is_reported_as_failure_not_hang(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    temp_rom_path: Path,
) -> None:
    base_case = load_hardening_cases(DEFAULT_FAKE_CASE_BASELINE_PATH)[0]
    hang_case = base_case.__class__(
        name="fake_timeout_case",
        suite=base_case.suite,
        category="timeout",
        backend=base_case.backend,
        module="",
        description="A hung capture must be reported as a timeout failure.",
        expected_crcs=("FFFFFFFF",),
        input_expression="",
        input_tokens=(),
        events=(
            {"kind": "key", "key": "__HANG_SCREEN__"},
            {"kind": "pause", "duration_ms": 100},
        ),
        post_events=(),
        timeout_ms=200,
        key_delay_ms=base_case.key_delay_ms,
        enter_settle_delay_ms=base_case.enter_settle_delay_ms,
        result_delay_ms=base_case.result_delay_ms,
        auto_enter=False,
        prompt_reset=base_case.prompt_reset,
        expected_output_excerpt_contains=(),
        expected_display="",
        notes="Synthetic timeout case.",
    )

    def make_runner():
        return build_fake_cemu_runner(
            tmp_path,
            temp_rom_path,
            command_timeout_s=0.2,
            boot_timeout_s=2.0,
        )

    results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=[hang_case],
        default_timeout_s=0.2,
    )

    assert results[0]["passed"] is False
    assert results[0]["status"] == "timeout"
    assert results[0]["failure_stage"] == "capture_screen"
    assert "Case timed out during capture_screen" in " | ".join(results[0]["diagnostics"])


def test_boot_failure_is_retried_and_case_recovers(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    temp_rom_path: Path,
) -> None:
    case = load_hardening_cases(DEFAULT_FAKE_CASE_BASELINE_PATH)[0]
    runner_factories_used: list[str] = []

    def make_runner():
        if not runner_factories_used:
            runner_factories_used.append("boot-failure")
            failed_root = tmp_path / "boot-failure-runner"
            failed_root.mkdir(parents=True, exist_ok=True)
            return BootFailureOnFirstAttemptRunner(build_fake_cemu_runner(failed_root, temp_rom_path))
        runner_factories_used.append("recovery")
        recovery_root = tmp_path / "boot-recovery-runner"
        recovery_root.mkdir(parents=True, exist_ok=True)
        return build_fake_cemu_runner(recovery_root, temp_rom_path)

    results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=[case],
        default_timeout_s=5.0,
    )
    summary = summarize_case_results([case], results)

    assert runner_factories_used == ["boot-failure", "recovery"]
    assert results[0]["passed"] is True
    assert results[0]["boot_attempts"] == 2
    assert results[0]["recovered_from_boot_failure"] is True
    assert "boot attempt 1 failed" in " | ".join(results[0]["diagnostics"])
    assert summary["recovered_from_boot_failure_names"] == [case.name]


def test_module_verification_detects_fake_transfer_integrity(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    snapshot_path = tmp_path / "module-verification.json"

    def make_runner():
        return build_fake_cemu_runner(tmp_path, temp_rom_path)

    summary = run_module_loading_verification(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        transfer_package=resolved_transfer_package,
        snapshot_path=snapshot_path,
        default_timeout_s=5.0,
    )

    assert summary["success"] is True
    assert summary["observed_transfer_count"] == resolved_transfer_package.transfer_count
    assert summary["missing_modules"] == []
    assert summary["duplicate_archive_slots"] == []


def test_module_verification_reports_missing_module_failure(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    missing_module_name = resolved_transfer_package.module_names[0]
    missing_module_path = staged_transfer_package_dir / missing_module_name
    missing_module_path.unlink()
    snapshot_path = tmp_path / "missing-module-verification.json"

    def make_runner():
        return build_fake_cemu_runner(tmp_path, temp_rom_path)

    summary = run_module_loading_verification(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        transfer_package=resolved_transfer_package,
        snapshot_path=snapshot_path,
        default_timeout_s=5.0,
    )

    assert summary["success"] is False
    assert summary["observed_transfer_count"] == resolved_transfer_package.transfer_count - 1
    assert summary["missing_modules"] == [missing_module_name]


def test_module_verification_reports_corrupted_staged_module(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    corrupted_module_name = resolved_transfer_package.module_names[0]
    corrupted_module_path = staged_transfer_package_dir / corrupted_module_name
    corrupted_module_path.write_bytes(corrupted_module_path.read_bytes() + b"-CORRUPTED")
    snapshot_path = tmp_path / "corrupted-module-verification.json"

    def make_runner():
        return build_fake_cemu_runner(tmp_path, temp_rom_path)

    summary = run_module_loading_verification(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        transfer_package=resolved_transfer_package,
        snapshot_path=snapshot_path,
        default_timeout_s=5.0,
    )

    manifest_row = next(row for row in summary["manifest_checks"] if row["name"] == corrupted_module_name)

    assert summary["success"] is False
    assert summary["mismatched_manifest_entries"] == [corrupted_module_name]
    assert manifest_row["matches_manifest"] is False
    assert manifest_row["actual_size_bytes"] != manifest_row["expected_size_bytes"]
    assert corrupted_module_name in " | ".join(summary["diagnostics"])


def test_module_verification_reports_out_of_order_manifest_load(
    tmp_path: Path,
    resolved_transfer_package,
) -> None:
    snapshot_path = tmp_path / "out-of-order-module-verification.json"
    ordered_paths = sorted(resolved_transfer_package.transfer_paths, key=lambda path: path.name.lower())
    reordered_paths = [ordered_paths[1], ordered_paths[0], *ordered_paths[2:]]

    def make_runner():
        return SnapshotOrderRunner(
            transfer_files=reordered_paths,
            app_count=resolved_transfer_package.transfer_count,
        )

    summary = run_module_loading_verification(
        make_runner=make_runner,
        program_paths=[tmp_path],
        transfer_package=resolved_transfer_package,
        snapshot_path=snapshot_path,
        default_timeout_s=5.0,
    )

    assert summary["success"] is False
    assert summary["load_order_matches_manifest"] is False
    assert summary["observed_transfer_order"][:2] == [
        ordered_paths[1].name,
        ordered_paths[0].name,
    ]
    assert summary["load_order_mismatches"][:2] == [
        {
            "index": 0,
            "expected_name": ordered_paths[0].name,
            "observed_name": ordered_paths[1].name,
        },
        {
            "index": 1,
            "expected_name": ordered_paths[1].name,
            "observed_name": ordered_paths[0].name,
        },
    ]
    assert "deterministic staged filename order" in " | ".join(summary["diagnostics"])


def test_rapid_fire_multi_module_archive_cycles_restore_clean_stage_without_stale_artifacts(
    tmp_path: Path,
    resolved_transfer_package,
    temp_rom_path: Path,
) -> None:
    rapid_case_names = [
        "module_base_hex_255",
        "module_alg_factor_quadratic",
        "module_solve_linear",
        "module_finance_present_value",
        "module_matrix_determinant",
        "module_complex_real_part",
    ]
    case_lookup = {
        case.name: case
        for case in load_hardening_cases(DEFAULT_MODULE_BATTERY_PATH)
    }
    rapid_cases = [case_lookup[name] for name in rapid_case_names]
    stage_dir = tmp_path / "rapid-fire-stage"
    sampled_module_names = list(resolved_transfer_package.module_names[:6])
    sampled_payloads = {
        module_name: resolved_transfer_package.resolved_paths[module_name].read_bytes()
        for module_name in sampled_module_names
    }
    expected_transfer_names = sorted(
        [
            *resolved_transfer_package.module_names,
            *resolved_transfer_package.runtime_names,
        ],
        key=str.lower,
    )
    observed_archive_counts: list[int] = []
    observed_stage_fingerprints: list[tuple[tuple[str, int], ...]] = []

    for cycle_index, case in enumerate(rapid_cases, start=1):
        stage_transfer_package(resolved_transfer_package, stage_dir)
        validation = validate_staged_transfer_directory(
            resolved_transfer_package,
            stage_dir,
        )

        assert validation["success"] is True
        assert list(stage_dir.glob("STALE*.8xp")) == []
        assert list(stage_dir.glob("*.calcengine-stage")) == []

        observed_stage_fingerprints.append(
            tuple(
                (path.name, path.stat().st_size)
                for path in sorted(
                    (
                        candidate
                        for candidate in stage_dir.iterdir()
                        if candidate.is_file() and candidate.suffix.lower() in {".8xp", ".8xv"}
                    ),
                    key=lambda candidate: candidate.name.lower(),
                )
            )
        )

        runner_root = tmp_path / f"rapid-fire-runner-{cycle_index}"
        runner_root.mkdir(parents=True, exist_ok=True)
        runner = build_fake_cemu_runner(runner_root, temp_rom_path)
        try:
            runner.boot(program_paths=[stage_dir])
            runner.reset(timeout_s=5.0)
            runner.send_sequence(case.events, timeout_s=5.0)
            screen = runner.capture_screen(timeout_s=5.0)
            snapshot_path = tmp_path / f"{case.name}-cycle-{cycle_index}.snapshot.json"
            runner.snapshot(snapshot_path, timeout_s=5.0)
            state = runner.get_state(timeout_s=5.0).payload
        finally:
            runner.shutdown()

        snapshot_payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
        observed_transfer_names = [
            Path(raw_path).name for raw_path in snapshot_payload["transfer_files"]
        ]

        assert len(str(screen.payload["crc"])) == 8
        assert observed_transfer_names == expected_transfer_names
        assert int(state["app_count"]) == resolved_transfer_package.transfer_count
        assert int(snapshot_payload["app_count"]) == resolved_transfer_package.transfer_count
        observed_archive_counts.append(int(state["app_count"]))

        if cycle_index < len(rapid_cases):
            stressed_module_name = sampled_module_names[cycle_index - 1]
            stressed_module_path = stage_dir / stressed_module_name
            stressed_module_path.write_bytes(
                stressed_module_path.read_bytes() + f"|CYCLE-{cycle_index}-CORRUPTION".encode("ascii")
            )
            (stage_dir / f"STALE{cycle_index:02d}.8xp").write_bytes(b"STALE-ARTIFACT")
            (stage_dir / f".STALE{cycle_index:02d}.8xp.calcengine-stage").write_bytes(b"PARTIAL-STAGE")

    final_validation = validate_staged_transfer_directory(
        resolved_transfer_package,
        stage_dir,
    )

    assert len(observed_archive_counts) == len(rapid_cases)
    assert len(observed_archive_counts) >= 5
    assert observed_archive_counts == [
        resolved_transfer_package.transfer_count
    ] * len(rapid_cases)
    assert all(
        fingerprint == observed_stage_fingerprints[0]
        for fingerprint in observed_stage_fingerprints[1:]
    )
    assert final_validation["success"] is True
    assert final_validation["unexpected_entries"] == []
    assert list(stage_dir.glob("STALE*.8xp")) == []
    assert list(stage_dir.glob("*.calcengine-stage")) == []
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload


def test_pipeline_environment_resolves_relative_config_paths(
    tmp_path: Path,
    resolved_transfer_package,
) -> None:
    config_dir = tmp_path / "ci-config"
    downloads_dir = tmp_path / "downloads"
    manifest_dir = tmp_path / "tests" / "emulator"
    config_dir.mkdir(parents=True, exist_ok=True)
    downloads_dir.mkdir(parents=True, exist_ok=True)
    manifest_dir.mkdir(parents=True, exist_ok=True)

    source_manifest_path = REPO_ROOT / "tests" / "emulator" / "manifest.json"
    manifest_path = manifest_dir / "manifest.json"
    manifest_path.write_text(source_manifest_path.read_text(encoding="utf-8"), encoding="utf-8")
    manifest_payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_module_names = {entry["name"] for entry in manifest_payload["modules"]}
    expected_runtime_names = {entry["name"] for entry in manifest_payload["runtime_dependencies"]}

    for artifact_path in resolved_transfer_package.transfer_paths:
        target_path = downloads_dir / artifact_path.name
        target_path.write_bytes(artifact_path.read_bytes())

    config_path = config_dir / "cemu-validate-config.json"
    config_path.write_text(
            json.dumps(
                {
                    "manifest_path": "../tests/emulator/manifest.json",
                    "downloads_dir": "../downloads",
                }
            ),
        encoding="utf-8",
    )

    environment, transfer_package = resolve_pipeline_environment(
        json.loads(config_path.read_text(encoding="utf-8")),
        config_path=config_path,
    )

    assert transfer_package.ready is True
    assert transfer_package.candidate_roots[0] == downloads_dir.resolve()
    assert set(transfer_package.module_names) == expected_module_names
    assert set(transfer_package.runtime_names) == expected_runtime_names
    assert environment.transfer_package is not None
    assert environment.transfer_package.ready is True


def test_fake_only_validation_driver_writes_summary(tmp_path: Path) -> None:
    summary_path = tmp_path / "fake-summary.json"
    snapshot_path = tmp_path / "fake-snapshot.json"
    process = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve().parent / "run_cemu_validation.py"),
            "--mode",
            "fake-only",
            "--output",
            str(summary_path),
            "--snapshot",
            str(snapshot_path),
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    payload = json.loads(summary_path.read_text(encoding="utf-8"))
    assert process.returncode == 0
    assert payload["success"] is True
    assert payload["fake_hardening_summary"]["success"] is True
    assert payload["failed_sections"] == []
    assert payload["failure_details"] == []


def test_fake_only_validation_driver_reports_actionable_failure_details(tmp_path: Path) -> None:
    summary_path = tmp_path / "fake-failure-summary.json"
    snapshot_path = tmp_path / "fake-failure-snapshot.json"
    fake_cases_path = tmp_path / "fake-failure-cases.json"
    payload = json.loads(DEFAULT_FAKE_CASE_BASELINE_PATH.read_text(encoding="utf-8"))
    payload["cases"][0]["expected_crcs"] = ["DEADBEEF"]
    fake_cases_path.write_text(json.dumps(payload), encoding="utf-8")

    process = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve().parent / "run_cemu_validation.py"),
            "--mode",
            "fake-only",
            "--fake-cases",
            str(fake_cases_path),
            "--output",
            str(summary_path),
            "--snapshot",
            str(snapshot_path),
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    summary = json.loads(summary_path.read_text(encoding="utf-8"))

    assert process.returncode == 1
    assert summary["success"] is False
    assert summary["failed_sections"] == ["fake_hardening_summary"]
    assert summary["failure_details"][0]["section"] == "fake_hardening_summary"
    assert summary["failure_details"][0]["failed_names"] == [payload["cases"][0]["name"]]
    assert summary["failure_details"][0]["failure_details"][0]["failure_stage"] == "assertion"


def test_fake_pipeline_sections_uses_placeholder_transfer_package_when_artifacts_are_missing() -> None:
    transfer_package = TransferPackage(
        manifest_path=REPO_ROOT / "tests" / "emulator" / "manifest.json",
        module_entries=(
            TransferManifestEntry(name="CALCENG.8xp", size_bytes=1, crc32="", kind="module"),
        ),
        runtime_entries=(
            TransferManifestEntry(name="fileioc.8xv", size_bytes=1, crc32="", kind="runtime"),
        ),
        readme_entry=TransferManifestEntry(name="README.md", size_bytes=-1, crc32="", kind="readme"),
        resolved_paths={},
        candidate_roots=(REPO_ROOT / "bin",),
        missing_entries=("CALCENG.8xp", "fileioc.8xv", "README.md"),
    )

    sections = run_fake_pipeline_sections(
        fake_case_path=DEFAULT_FAKE_CASE_BASELINE_PATH,
        transfer_package=transfer_package,
        default_timeout_s=5.0,
    )

    assert sections["fake_hardening_summary"]["success"] is True
    assert sections["fake_hardening_summary"]["transfer_package_mode"] == "placeholder"
    assert sections["fake_hardening_summary"]["transfer_package_staged_count"] == 2


@pytest.mark.skipif(not REAL_CEMU_READY, reason=REAL_CEMU_SKIP_REASON)
def test_real_edge_case_suite_passes(real_cemu_config, staged_transfer_package_dir: Path) -> None:
    assert real_cemu_config is not None
    cases = [
        case
        for case in load_hardening_cases(DEFAULT_EDGE_CASE_BASELINE_PATH)
        if case.suite == "edge_cases"
    ]

    def make_runner():
        from cemu_runner import CEmuRunner

        return CEmuRunner(real_cemu_config)

    results = run_hardening_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=cases,
        default_timeout_s=30.0,
    )
    summary = summarize_case_results(cases, results)

    assert summary["success"] is True
    assert summary["failed_count"] == 0
