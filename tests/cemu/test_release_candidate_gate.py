from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from artifice_jailbreak import JailbreakIndicators, JailbreakResult, resolve_artifice_path
from calcengine_launcher import LaunchCheckpointResult, LaunchResult
from cemu_runner import CEmuArchiveCapacityError, CEmuProcessExitedError
from pipeline_support import resolve_real_cemu_environment
import release_candidate_gate
from release_candidate_gate import (
    DEFAULT_GATE_CONFIG_PATH,
    load_gate_config,
    load_gate_smoke_case,
    run_release_candidate_gate,
)


REAL_GATE_ENVIRONMENT = resolve_real_cemu_environment(repo_root=REPO_ROOT)
REAL_ARTIFICE_PATH = resolve_artifice_path()
REAL_GATE_READY = REAL_GATE_ENVIRONMENT.ready and REAL_ARTIFICE_PATH is not None
REAL_GATE_SKIP_REASON = (
    "Real CEmu environment or arTIfiCE asset is unavailable."
    if not REAL_GATE_READY
    else ""
)


class StubCommandResult:
    def __init__(self, crc: str) -> None:
        self.payload = {
            "crc": crc,
            "format": "crc",
            "output_excerpt": "",
        }


class StubRunner:
    def __init__(self, screen_crcs: list[str]) -> None:
        self.screen_crcs = list(screen_crcs)
        self.sequences: list[list[object]] = []
        self.snapshot_paths: list[Path] = []
        self.shutdown_called = False
        self.boot_calls: list[dict[str, object]] = []

    def boot(self, program_paths, ready_sequence=(), timeout_s: float | None = None):
        self.boot_calls.append(
            {
                "program_paths": [str(path) for path in program_paths],
                "ready_sequence_length": len(tuple(ready_sequence)),
                "timeout_s": timeout_s,
            }
        )
        return SimpleNamespace(
            state={"rom_loaded": True, "running": True, "app_count": 39},
            loaded_programs=tuple(),
        )

    def send_sequence(self, events, timeout_s: float | None = None):
        self.sequences.append(list(events))
        return []

    def reset(self, timeout_s: float | None = None):
        return SimpleNamespace(payload={"ok": True})

    def capture_screen(self, timeout_s: float | None = None):
        if not self.screen_crcs:
            raise AssertionError("No stub CRCs remain for capture_screen.")
        return StubCommandResult(self.screen_crcs.pop(0))

    def snapshot(self, path: Path | str, timeout_s: float | None = None):
        snapshot_path = Path(path)
        snapshot_path.parent.mkdir(parents=True, exist_ok=True)
        snapshot_path.write_text(
            json.dumps({"screen": {"text_rows": []}}, indent=2),
            encoding="utf-8",
        )
        self.snapshot_paths.append(snapshot_path)
        return SimpleNamespace(payload={"path": str(snapshot_path)})

    def shutdown(self) -> None:
        self.shutdown_called = True


def build_release_gate_transfer_inventory_names() -> list[str]:
    module_names = ["CALCENG.8xp", *[f"MOD{index:02d}.8xp" for index in range(1, 35)]]
    runtime_names = ["fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv"]
    return sorted(["A.8xp", *module_names, *runtime_names], key=str.lower)


def write_transfer_snapshot(snapshot_path: Path, transfer_names: list[str]) -> None:
    snapshot_path.parent.mkdir(parents=True, exist_ok=True)
    snapshot_path.write_text(
        json.dumps(
            {
                "transfer_files": [str(snapshot_path.parent / name) for name in transfer_names],
                "screen": {"text_rows": []},
            },
            indent=2,
        ),
        encoding="utf-8",
    )


def write_windows_transfer_snapshot(snapshot_path: Path, transfer_names: list[str]) -> None:
    snapshot_path.parent.mkdir(parents=True, exist_ok=True)
    stage_root = Path(r"C:/Users/user/AppData/Local/Temp/calcengine-stage")
    snapshot_path.write_text(
        json.dumps(
            {
                "transfer_files": [
                    str((stage_root / name)).replace("/", r"\\")
                    for name in transfer_names
                ],
                "screen": {"text_rows": []},
            },
            indent=2,
        ),
        encoding="utf-8",
    )


def write_artifice_smoke_summary(summary_path: Path) -> None:
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    hash_rows = [
        ("calcengine_launch_screen", "CalcEngine landing screen after exiting arTIfiCE and launching CALCENG", "28B88B56"),
        ("calcengine_result_2plus3", "CalcEngine result screen for 2+3 through the jailbroken path", "C23818C9"),
        ("calcengine_prompt_after_2plus3", "Prompt restored after dismissing the 2+3 result", "A32840C8"),
        ("calcengine_result_6times7", "CalcEngine result screen for 6*7 through the jailbroken path", "46B2FD53"),
        ("calcengine_result_neg5_plus10", "CalcEngine result screen for -5+10 through the jailbroken path", "E893F9A6"),
        ("calcengine_result_decimal_6_28", "CalcEngine result screen for 3.14*2 through the jailbroken path", "F9FE44E6"),
        ("calcengine_result_parens_27", "CalcEngine result screen for (4+5)*3 through the jailbroken path", "3956B827"),
        ("calcengine_result_power_256", "CalcEngine result screen for 2^8 through the jailbroken path", "9DF8F505"),
        ("calcengine_divide_by_zero_error", "CalcEngine error screen for 1/0 through the jailbroken path", "32BFD188"),
        ("calcengine_menu_open", "CalcEngine menu screen after pressing MODE", "151319C8"),
        ("calcengine_menu_closed", "CalcEngine prompt after closing the menu with CLEAR", "828489BA"),
    ]
    summary_path.write_text(
        json.dumps(
            {
                "runner": "run-artifice-smoke.ps1",
                "delegated_runner": {
                    "summary_json": {
                        "verify_stage": {
                            "hash_results": [
                                {
                                    "hash_name": name,
                                    "description": description,
                                    "expected_crcs": [crc],
                                    "observed_crc": "",
                                    "status": "pass",
                                }
                                for name, description, crc in hash_rows
                            ]
                        }
                    }
                },
            },
            indent=2,
        ),
        encoding="utf-8",
    )


def compute_transfer_inventory_signature(transfer_names: list[str]) -> str:
    digest = hashlib.sha256()
    for name in transfer_names:
        digest.update(name.encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest().upper()


def build_rom_validation() -> SimpleNamespace:
    return SimpleNamespace(
        path=Path("rom/Rom.rom"),
        size_bytes=4_194_304,
        crc32="AC9BF466",
        sha256="AE5D87F96A01CB124B41ADF5D4F5D88CC3E6FF22149A97EC79024B736D1C5014",
        recognized_size=True,
        recognized_fingerprint=True,
        expected_crc32="AC9BF466",
        expected_sha256="AE5D87F96A01CB124B41ADF5D4F5D88CC3E6FF22149A97EC79024B736D1C5014",
        integrity_ok=True,
        known_good_hash_match=True,
        product_family="TI-84 Plus CE",
        model="TI-84 Plus CE Python",
        os_version="5.8.0.0022",
        boot_version="5.5.2.0044",
        firmware_family="5.8.x",
        description="Known CalcEngine validation ROM dump for the TI-84 Plus CE Python family.",
        warnings=(),
        errors=(),
    )


def build_ready_environment() -> SimpleNamespace:
    return SimpleNamespace(
        ready=True,
        blockers=(),
        rom_path=Path("rom/Rom.rom"),
        headless_path=Path("tools/emulator/cemu-headless/build/Release/cemu-headless.exe"),
        autotester_path=Path("C:/Tools/CEmu/autotester.exe"),
        gui_path=Path("C:/Tools/CEmu/CEmu.exe"),
        rom_validation=build_rom_validation(),
        transfer_package=SimpleNamespace(
            transfer_count=39,
            module_names=tuple(["CALCENG.8xp", *[f"MOD{index:02d}.8xp" for index in range(1, 35)]]),
            runtime_names=("fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv"),
        ),
    )


def build_successful_jailbreak_result(
    evidence_dir: Path,
    *,
    transfer_count: int = 39,
    loaded_archive_count: int = 40,
    transfer_names: list[str] | None = None,
    artifice_source_name: str = "A.8xp",
) -> JailbreakResult:
    stage_dir = evidence_dir / "stage"
    stage_dir.mkdir(parents=True, exist_ok=True)
    artifice_path = stage_dir / artifice_source_name
    artifice_path.write_text("ARTIFICE", encoding="utf-8")
    checkpoint_path = evidence_dir / "ti_os_program_menu_open.state.json"
    write_transfer_snapshot(checkpoint_path, transfer_names or build_release_gate_transfer_inventory_names())
    return JailbreakResult(
        success=True,
        failure_reason="",
        timed_out=False,
        artifice_path=artifice_path,
        baseline_path=None,
        stage_dir=stage_dir,
        evidence_dir=evidence_dir,
        module_count=35,
        runtime_dependency_count=4,
        transfer_count=transfer_count,
        staged_file_count=40,
        loaded_archive_count=loaded_archive_count,
        checkpoints=(
            SimpleNamespace(
                step="ti_os_program_menu_open",
                description="ti_os_program_menu_open",
                expected_crcs=("C84EC5D0",),
                observed_crc="C84EC5D0",
                passed=True,
                snapshot_path=checkpoint_path,
                screen_payload={"crc": "C84EC5D0"},
            ),
        ),
        indicators=JailbreakIndicators(True, True, ("artifice_shell_loaded",), ()),
        diagnostics=(),
        sequence_documentation=(),
        keypress_log=(),
        total_duration_ms=1000,
        bypass_active=True,
    )


def build_successful_launch_result(
    evidence_dir: Path,
    *,
    transfer_count: int = 39,
    loaded_archive_count: int = 40,
    transfer_names: list[str] | None = None,
    include_artifice_smoke_summary: bool = False,
) -> LaunchResult:
    checkpoint_dir = evidence_dir / "launch"
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    resolved_transfer_names = transfer_names or build_release_gate_transfer_inventory_names()
    summary_path: Path | None = None
    if include_artifice_smoke_summary:
        summary_path = checkpoint_dir / "artifice-smoke.summary.json"
        write_artifice_smoke_summary(summary_path)
    checkpoints = []
    for step, crc in (
        ("calcengine_launch_menu", "28B88B56"),
        ("calcengine_result_2plus3", "C23818C9"),
        ("calcengine_menu_open", "151319C8"),
        ("calcengine_menu_closed", "828489BA"),
    ):
        snapshot_path = summary_path or (checkpoint_dir / f"{step}.state.json")
        if summary_path is None:
            write_transfer_snapshot(snapshot_path, resolved_transfer_names)
        checkpoints.append(
            LaunchCheckpointResult(
                step=step,
                description=step,
                expected_crcs=(crc,),
                observed_crc=crc,
                passed=True,
                snapshot_path=snapshot_path,
                text_check_passed=True,
                screen_payload={"crc": crc},
            )
        )

    return LaunchResult(
        success=True,
        failure_reason="",
        timed_out=False,
        evidence_dir=evidence_dir,
        stage_dir=evidence_dir / "stage",
        module_count=35,
        runtime_dependency_count=4,
        transfer_count=transfer_count,
        loaded_archive_count=loaded_archive_count,
        checkpoints=tuple(checkpoints),
        diagnostics=(),
        keypress_log=(),
        basic_operation_passed=True,
        menu_open_close_passed=True,
        total_duration_ms=1000,
    )


def build_direct_smoke_results(
    *,
    multiplication_crc: str = "E35569F2",
    base_crc: str = "96FD4303",
    matrix_crc: str = "9FBBCE5B",
    matrix_expected_crcs: list[str] | None = None,
    matrix_passed: bool = True,
) -> list[dict[str, object]]:
    return [
        {
            "name": "arith_mul_6_times_7",
            "description": "Small integer multiplication stays deterministic.",
            "expected_crcs": [multiplication_crc],
            "observed_crc": multiplication_crc,
            "passed": True,
            "expected_display": "42",
            "status": "passed",
            "observed_output_excerpt": "",
            "diagnostics": [],
        },
        {
            "name": "module_base_hex_255",
            "description": "The base-conversion module remains routable through hex conversion.",
            "expected_crcs": [base_crc],
            "observed_crc": base_crc,
            "passed": True,
            "expected_display": "FF",
            "status": "passed",
            "observed_output_excerpt": "",
            "diagnostics": [],
        },
        {
            "name": "module_matrix_determinant",
            "description": "Determinant routing remains stable for a 2x2 matrix literal.",
            "expected_crcs": list(matrix_expected_crcs or [matrix_crc]),
            "observed_crc": matrix_crc,
            "passed": matrix_passed,
            "expected_display": "-2",
            "status": "passed" if matrix_passed else "failed",
            "observed_output_excerpt": "",
            "diagnostics": [],
        },
    ]


def build_visible_smoke_summary(
    base_path: Path,
    *,
    success: bool = True,
    failed_names: list[str] | None = None,
    failed_assertions: list[str] | None = None,
) -> dict[str, object]:
    resolved_failed_names = list(failed_names or [])
    resolved_failed_assertions = list(failed_assertions or [])
    case_count = 14
    failed_count = len(resolved_failed_names)
    return {
        "case_count": case_count,
        "assertion_count": case_count * 2,
        "passed_count": case_count - failed_count,
        "failed_count": failed_count,
        "timeout_count": 0,
        "error_count": 0,
        "failed_names": resolved_failed_names,
        "failed_assertions": resolved_failed_assertions,
        "lanes": [
            "arithmetic",
            "graphing",
            "launcher",
            "list_operations",
            "module_dispatch",
            "sequence",
            "table_generation",
        ],
        "accepted_prompt_crcs": ["349F4775", "A32840C8", "B76BC405"],
        "success": success,
        "baseline_path": str(base_path / "visible-smoke-cases.json"),
    }


def build_fidelity_verify_summary(
    base_path: Path,
    *,
    suite_name: str,
    hash_count: int,
    success: bool = True,
    fail_count: int = 0,
    mismatch_count: int = 0,
    missing_count: int = 0,
    package_validation_success: bool = True,
) -> dict[str, object]:
    verify_stage_pass_count = max(hash_count - fail_count - missing_count, 0)
    summary_path = base_path / f"{suite_name}.verify.summary.json"
    return {
        "success": success,
        "exit_code": 0 if success else 1,
        "template_path": str(base_path / f"{suite_name}.json"),
        "baselined_path": str(base_path / f"{suite_name}.baselined.json"),
        "crc_catalog_json_path": str(base_path / f"{suite_name}.crc-catalog.json"),
        "crc_catalog_markdown_path": str(base_path / f"{suite_name}.crc-catalog.md"),
        "package_validation": {
            "success": package_validation_success,
        },
        "runner_summary": {
            "emulator_backend": "Autotester",
            "crc_catalog": {
                "hash_count": hash_count,
            },
            "verify_stage": {
                "crc_catalog_hash_count": hash_count,
                "attempted_count": hash_count,
                "pass_count": verify_stage_pass_count,
                "fail_count": fail_count,
                "mismatch_count": mismatch_count,
                "missing_count": missing_count,
                "mismatches": [],
                "missing_hashes": [],
            },
        },
        "summary_path": str(summary_path),
    }


@pytest.fixture(autouse=True)
def stub_visible_smoke_suite(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setattr(
        release_candidate_gate,
        "run_release_gate_visible_smoke_suite",
        lambda **kwargs: build_visible_smoke_summary(tmp_path),
    )

    keypad_summary_path = tmp_path / "keypad-fidelity.verify.summary.json"
    keypad_summary_path.write_text(
        json.dumps(
            build_fidelity_verify_summary(
                tmp_path,
                suite_name="keypad-fidelity",
                hash_count=27,
            ),
            indent=2,
        ),
        encoding="utf-8",
    )
    lcd_summary_path = tmp_path / "lcd-fidelity.verify.summary.json"
    lcd_summary_path.write_text(
        json.dumps(
            build_fidelity_verify_summary(
                tmp_path,
                suite_name="lcd-fidelity",
                hash_count=18,
            ),
            indent=2,
        ),
        encoding="utf-8",
    )
    monkeypatch.setattr(
        release_candidate_gate,
        "KEYPAD_FIDELITY_VERIFY_SUMMARY_PATH",
        keypad_summary_path,
    )
    monkeypatch.setattr(
        release_candidate_gate,
        "LCD_FIDELITY_VERIFY_SUMMARY_PATH",
        lcd_summary_path,
    )


def test_release_candidate_gate_config_selects_required_smoke_cases() -> None:
    config = load_gate_config(DEFAULT_GATE_CONFIG_PATH)
    multiplication_case = load_gate_smoke_case(
        REPO_ROOT / "tests" / "cemu" / "edge-cases" / "arithmetic-battery.json",
        str(config["multiplication_case_name"]),
    )
    mode_cases = [
        load_gate_smoke_case(
            REPO_ROOT / "tests" / "cemu" / "edge-cases" / "module-battery.json",
            str(case_name),
        )
        for case_name in config["mode_switch_case_names"]
    ]

    assert multiplication_case.name == "arith_mul_6_times_7"
    assert multiplication_case.input_expression == "6*7"
    assert config["attempt_count"] == 3
    assert config["required_passing_attempts"] == 3
    assert config["stop_on_first_failure"] is True
    assert config["enforce_smoke_crc_stability"] is True
    assert config["visible_smoke_case_path"] == "../visible-smoke-cases.json"
    assert [case.name for case in mode_cases] == [
        "module_base_hex_255",
        "module_matrix_determinant",
    ]


def test_release_candidate_gate_reports_unavailable_environment(tmp_path: Path) -> None:
    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=SimpleNamespace(
            ready=False,
            blockers=("headless binary missing",),
            rom_path=None,
            headless_path=None,
            autotester_path=None,
            gui_path=None,
            rom_validation=None,
            transfer_package=None,
        ),
    )

    assert summary["success"] is False
    assert summary["status"] == "error"
    assert summary["exit_code"] == 2
    assert summary["blockers"] == ["headless binary missing"]
    assert summary["availability"]["rom_validation"] == {}


def test_release_candidate_gate_builds_successful_confidence_report(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    expected_transfer_names = build_release_gate_transfer_inventory_names()
    expected_inventory_signature = compute_transfer_inventory_signature(expected_transfer_names)
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(
            tmp_path / "artifice",
            transfer_names=expected_transfer_names,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(
            tmp_path / "launch",
            transfer_names=expected_transfer_names,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is True
    assert summary["release_candidate_ready"] is True
    assert summary["status"] == "passed"
    assert summary["exit_code"] == 0
    assert summary["attempt_count"] == 3
    assert summary["required_passing_attempts"] == 3
    assert summary["totals"] == {
        "total": 5,
        "passed": 5,
        "failed": 0,
        "failed_names": [],
        "confidence_score_percent": 100.0,
    }
    assert [row["name"] for row in summary["smoke_tests"]] == [
        "launch_calcengine",
        "addition_2_plus_3",
        "menu_open_close",
        "multiplication_6_times_7",
        "mode_switching",
    ]
    assert summary["attempts_summary"] == {
        "configured_attempt_count": 3,
        "required_passing_attempts": 3,
        "executed_attempt_count": 3,
        "passed_attempt_count": 3,
        "failed_attempt_count": 0,
        "passed_attempt_numbers": [1, 2, 3],
        "failed_attempt_numbers": [],
        "halted_early": False,
        "stop_on_first_failure": True,
        "attempt_confidence_score_percent": 100.0,
    }
    assert summary["availability"]["rom_validation"] == {
        "path": "rom/Rom.rom",
        "size_bytes": 4_194_304,
        "crc32": "AC9BF466",
        "sha256": "AE5D87F96A01CB124B41ADF5D4F5D88CC3E6FF22149A97EC79024B736D1C5014",
        "recognized_size": True,
        "recognized_fingerprint": True,
        "expected_crc32": "AC9BF466",
        "expected_sha256": "AE5D87F96A01CB124B41ADF5D4F5D88CC3E6FF22149A97EC79024B736D1C5014",
        "integrity_ok": True,
        "known_good_hash_match": True,
        "product_family": "TI-84 Plus CE",
        "model": "TI-84 Plus CE Python",
        "os_version": "5.8.0.0022",
        "boot_version": "5.5.2.0044",
        "firmware_family": "5.8.x",
        "description": "Known CalcEngine validation ROM dump for the TI-84 Plus CE Python family.",
        "warnings": [],
        "errors": [],
    }
    assert any(
        line
        == "ROM provenance for this gate run: model=TI-84 Plus CE Python, os_version=5.8.0.0022, firmware_family=5.8.x, recognized_fingerprint=True, known_good_hash_match=True, sha256=AE5D87F96A01CB124B41ADF5D4F5D88CC3E6FF22149A97EC79024B736D1C5014."
        for line in summary["diagnostics"]
    )
    assert summary["archive_lifecycle"]["expected_transfer_count"] == 39
    assert summary["archive_lifecycle"]["artifice_transfer_count"] == 39
    assert summary["archive_lifecycle"]["artifice_loaded_archive_count"] == 40
    assert summary["archive_lifecycle"]["launch_transfer_count"] == 39
    assert summary["archive_lifecycle"]["launch_loaded_archive_count"] == 40
    assert summary["archive_lifecycle"]["loaded_archive_count_delta"] == 0
    assert summary["archive_lifecycle"]["transfer_counts_match"] is True
    assert summary["archive_lifecycle"]["minimum_archive_count_satisfied"] is True
    assert summary["archive_lifecycle"]["stage_counts_match"] is True
    assert summary["archive_lifecycle"]["inventory_supported"] is True
    assert summary["archive_lifecycle"]["jailbreak_inventory_count_matches_loaded_archive_count"] is True
    assert summary["archive_lifecycle"]["launch_inventory_count_matches_loaded_archive_count"] is True
    assert summary["archive_lifecycle"]["inventory_stage_signatures_match"] is True
    assert summary["archive_lifecycle"]["stable_within_attempt"] is True
    assert summary["archive_lifecycle"]["blockers"] == []
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["supported"] is True
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["supported"] is True
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["expected_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["expected_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["observed_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["observed_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["matches_manifest_inventory"] is True
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["matches_manifest_inventory"] is True
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["observed_transfer_count"] == 40
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["observed_transfer_count"] == 40
    assert summary["archive_lifecycle"]["diagnostics"] == [
        "Archive lifecycle expected transfer count=39.",
        "arTIfiCE jailbreak reported transfer_count=39 and loaded_archive_count=40.",
        "CalcEngine launch reported transfer_count=39 and loaded_archive_count=40.",
        "arTIfiCE jailbreak archive inventory matched the deterministic manifest-backed transfer order.",
        "CalcEngine launch archive inventory matched the deterministic manifest-backed transfer order.",
        "Archive lifecycle counts remained stable across the arTIfiCE jailbreak handoff and the CalcEngine launch verifier.",
    ]


def test_release_candidate_gate_normalizes_windows_style_transfer_paths_in_archive_inventory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    expected_transfer_names = build_release_gate_transfer_inventory_names()
    expected_inventory_signature = compute_transfer_inventory_signature(expected_transfer_names)
    stub_runner = StubRunner([])
    jailbreak_result = build_successful_jailbreak_result(
        tmp_path / "artifice",
        transfer_names=expected_transfer_names,
    )
    launch_result = build_successful_launch_result(
        tmp_path / "launch",
        transfer_names=expected_transfer_names,
    )
    write_windows_transfer_snapshot(
        jailbreak_result.checkpoints[0].snapshot_path,
        expected_transfer_names,
    )
    for checkpoint in launch_result.checkpoints:
        assert checkpoint.snapshot_path is not None
        write_windows_transfer_snapshot(checkpoint.snapshot_path, expected_transfer_names)

    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: jailbreak_result,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: launch_result,
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is True
    assert summary["archive_lifecycle"]["inventory_stage_signatures_match"] is True
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["observed_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["observed_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["unexpected_entries"] == []
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["unexpected_entries"] == []
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["duplicate_archive_slots"] == []
    assert summary["archive_lifecycle"]["launch_archive_inventory"]["duplicate_archive_slots"] == []

    assert summary["stability"]["enforced"] is True
    assert summary["stability"]["stable"] is True
    assert summary["stability"]["unstable_smoke_tests"] == []
    assert summary["archive_lifecycle_stability"]["expected_attempt_count"] == 3
    assert summary["archive_lifecycle_stability"]["executed_attempt_count"] == 3
    assert summary["archive_lifecycle_stability"]["observed_attempt_count"] == 3
    assert summary["archive_lifecycle_stability"]["present_attempt_numbers"] == [1, 2, 3]
    assert summary["archive_lifecycle_stability"]["missing_attempt_numbers"] == []
    assert summary["archive_lifecycle_stability"]["unstable_attempt_numbers"] == []
    assert summary["archive_lifecycle_stability"]["signature_count"] == 1
    assert summary["archive_lifecycle_stability"]["stable"] is True
    assert summary["archive_lifecycle_stability"]["signatures"] == [
        {
            "attempt_numbers": [1, 2, 3],
            "archive_signature": [
                "expected_transfer_count|39",
                "artifice_transfer_count|39",
                "artifice_loaded_archive_count|40",
                "launch_transfer_count|39",
                "launch_loaded_archive_count|40",
                f"jailbreak_inventory_signature|{expected_inventory_signature}",
                f"launch_inventory_signature|{expected_inventory_signature}",
            ],
        }
    ]
    assert summary["evidence_cleanup"] == {
        "base_dir": str((tmp_path / "evidence").resolve()),
        "attempt_count": 3,
        "removed_paths": [],
        "removed_path_count": 0,
    }
    assert len(summary["attempts"]) == 3
    assert summary["selected_attempt_number"] == 3
    assert (tmp_path / "evidence" / "attempt-01" / "summary.json").is_file()
    assert (tmp_path / "evidence" / "attempt-02" / "summary.json").is_file()
    assert (tmp_path / "evidence" / "attempt-03" / "summary.json").is_file()
    assert stub_runner.shutdown_called is True


def test_release_candidate_gate_surfaces_first_try_confirmation_checklist_from_delegated_artifice_smoke_summary(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(
            tmp_path / "launch",
            include_artifice_smoke_summary=True,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    checklist = summary["first_try_confirmation_checklist"]
    assert checklist["available"] is True
    assert checklist["source_runner"] == "run-artifice-smoke.ps1"
    assert checklist["required_item_count"] == 10
    assert checklist["covered_item_count"] == 10
    assert checklist["passed_item_count"] == 10
    assert checklist["reported_crc_item_count"] == 0
    assert checklist["inferred_crc_item_count"] == 10
    assert checklist["missing_items"] == []
    labels = [item["label"] for item in checklist["items"]]
    assert labels == [
        "Launch CalcEngine",
        "2+3",
        "6*7",
        "-5+10",
        "3.14*2",
        "(4+5)*3",
        "2^8",
        "divide-by-zero",
        "menu open/close",
        "return-to-prompt after result",
    ]
    menu_item = next(item for item in checklist["items"] if item["id"] == "menu_open_close")
    assert menu_item["hash_names"] == ["calcengine_menu_open", "calcengine_menu_closed"]
    assert menu_item["status"] == "passed"
    assert menu_item["evidence_strength"] == "inferred_expected_crc"
    assert [row["hash_name"] for row in menu_item["hashes"]] == [
        "calcengine_menu_open",
        "calcengine_menu_closed",
    ]
    assert any(
        "First-try confirmation checklist covered 10/10 required items with 10 currently passing" in line
        for line in summary["diagnostics"]
    )
    assert summary["visible_smoke_summary"] == build_visible_smoke_summary(tmp_path)
    capability_depth = summary["capability_depth"]
    assert capability_depth["available"] is True
    assert capability_depth["required_item_count"] == 7
    assert capability_depth["covered_item_count"] == 7
    assert capability_depth["passed_item_count"] == 7
    assert capability_depth["status"] == "passed"
    items_by_id = {item["id"]: item for item in capability_depth["items"]}
    assert items_by_id["first_try_confirmation"]["status"] == "passed"
    assert items_by_id["launch_visual_surfaces"] == {
        "id": "launch_visual_surfaces",
        "label": "Launch/menu visual surfaces",
        "covered": True,
        "passed": True,
        "status": "passed",
        "required_smoke_tests": ["launch_calcengine", "addition_2_plus_3", "menu_open_close"],
        "covered_smoke_tests": ["launch_calcengine", "addition_2_plus_3", "menu_open_close"],
        "expected_display_count": 3,
        "checkpoint_snapshot_reference_count": 4,
        "snapshot_evidence_count": 1,
        "snapshot_artifact_paths": [str(tmp_path / "launch" / "launch" / "artifice-smoke.summary.json")],
    }
    assert items_by_id["visible_multi_lane_smoke"] == {
        "id": "visible_multi_lane_smoke",
        "label": "Visible multi-lane prompt/result smoke",
        "covered": True,
        "passed": True,
        "status": "passed",
        "case_count": 14,
        "assertion_count": 28,
        "lane_count": 7,
        "lanes": [
            "arithmetic",
            "graphing",
            "launcher",
            "list_operations",
            "module_dispatch",
            "sequence",
            "table_generation",
        ],
        "failed_names": [],
        "failed_assertions": [],
        "baseline_path": str(tmp_path / "visible-smoke-cases.json"),
    }
    assert items_by_id["matrix_literal_dispatch"] == {
        "id": "matrix_literal_dispatch",
        "label": "Matrix-literal direct smoke",
        "covered": True,
        "passed": True,
        "status": "passed",
        "source_smoke_test": "mode_switching",
        "source_case_names": ["module_base_hex_255", "module_matrix_determinant"],
        "matrix_case_names": ["module_matrix_determinant"],
        "direct_checkpoint_count": 2,
        "checkpoint_snapshot_reference_count": 2,
        "snapshot_evidence_count": 2,
        "snapshot_artifact_paths": [
            str(tmp_path / "evidence" / "attempt-03" / "direct-smoke" / "module_base_hex_255.state.json"),
            str(tmp_path / "evidence" / "attempt-03" / "direct-smoke" / "module_matrix_determinant.state.json"),
        ],
    }
    assert items_by_id["archive_lifecycle"]["status"] == "passed"
    assert items_by_id["keypad_control_fidelity"] == {
        "id": "keypad_control_fidelity",
        "label": "Keypad control fidelity suite",
        "covered": True,
        "passed": True,
        "status": "passed",
        "summary_path": str(tmp_path / "keypad-fidelity.verify.summary.json"),
        "template_path": str(tmp_path / "keypad-fidelity.json"),
        "baselined_path": str(tmp_path / "keypad-fidelity.baselined.json"),
        "crc_catalog_json_path": str(tmp_path / "keypad-fidelity.crc-catalog.json"),
        "crc_catalog_markdown_path": str(tmp_path / "keypad-fidelity.crc-catalog.md"),
        "emulator_backend": "Autotester",
        "package_validation_success": True,
        "expected_hash_count": 27,
        "attempted_count": 27,
        "pass_count": 27,
        "fail_count": 0,
        "mismatch_count": 0,
        "missing_count": 0,
    }
    assert items_by_id["lcd_visual_fidelity"] == {
        "id": "lcd_visual_fidelity",
        "label": "LCD visual fidelity suite",
        "covered": True,
        "passed": True,
        "status": "passed",
        "summary_path": str(tmp_path / "lcd-fidelity.verify.summary.json"),
        "template_path": str(tmp_path / "lcd-fidelity.json"),
        "baselined_path": str(tmp_path / "lcd-fidelity.baselined.json"),
        "crc_catalog_json_path": str(tmp_path / "lcd-fidelity.crc-catalog.json"),
        "crc_catalog_markdown_path": str(tmp_path / "lcd-fidelity.crc-catalog.md"),
        "emulator_backend": "Autotester",
        "package_validation_success": True,
        "expected_hash_count": 18,
        "attempted_count": 18,
        "pass_count": 18,
        "fail_count": 0,
        "mismatch_count": 0,
        "missing_count": 0,
    }


def test_release_candidate_gate_accepts_staged_artifice_alias_in_archive_inventory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    expected_transfer_names = build_release_gate_transfer_inventory_names()
    expected_inventory_signature = compute_transfer_inventory_signature(expected_transfer_names)
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(
            tmp_path / "artifice",
            transfer_names=expected_transfer_names,
            artifice_source_name="artifice-v2.8xp",
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(
            tmp_path / "launch",
            transfer_names=expected_transfer_names,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is True
    assert summary["release_candidate_ready"] is True
    assert summary["archive_lifecycle"]["inventory_supported"] is True
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["expected_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["observed_transfer_order"] == expected_transfer_names
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["matches_manifest_inventory"] is True
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["unexpected_entries"] == []
    assert summary["archive_lifecycle"]["jailbreak_archive_inventory"]["load_order_mismatches"] == []
    assert summary["archive_lifecycle"]["diagnostics"] == [
        "Archive lifecycle expected transfer count=39.",
        "arTIfiCE jailbreak reported transfer_count=39 and loaded_archive_count=40.",
        "CalcEngine launch reported transfer_count=39 and loaded_archive_count=40.",
        "arTIfiCE jailbreak archive inventory matched the deterministic manifest-backed transfer order.",
        "CalcEngine launch archive inventory matched the deterministic manifest-backed transfer order.",
        "Archive lifecycle counts remained stable across the arTIfiCE jailbreak handoff and the CalcEngine launch verifier.",
    ]
    assert summary["archive_lifecycle_stability"]["signatures"] == [
        {
            "attempt_numbers": [1, 2, 3],
            "archive_signature": [
                "expected_transfer_count|39",
                "artifice_transfer_count|39",
                "artifice_loaded_archive_count|40",
                "launch_transfer_count|39",
                "launch_loaded_archive_count|40",
                f"jailbreak_inventory_signature|{expected_inventory_signature}",
                f"launch_inventory_signature|{expected_inventory_signature}",
            ],
        }
    ]


def test_release_candidate_gate_marks_failed_smoke_case_on_crc_drift(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner(["E35569F2", "96FD4303", "DEADBEEF"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(tmp_path / "launch"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(
            matrix_crc="DEADBEEF",
            matrix_expected_crcs=["9FBBCE5B"],
            matrix_passed=False,
        ),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["exit_code"] == 1
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: mode_switching",
    ]
    assert summary["totals"]["failed_names"] == ["mode_switching"]
    capability_depth = summary["capability_depth"]
    assert capability_depth["status"] == "partial"
    assert "Matrix-literal direct smoke" in capability_depth["failing_items"]
    assert capability_depth["missing_items"] == []
    items_by_id = {item["id"]: item for item in capability_depth["items"]}
    assert items_by_id["matrix_literal_dispatch"]["status"] == "failed"
    assert items_by_id["matrix_literal_dispatch"]["passed"] is False
    assert summary["attempts_summary"]["executed_attempt_count"] == 1
    assert summary["attempts_summary"]["failed_attempt_numbers"] == [1]
    assert summary["selected_attempt_number"] == 1


def test_release_candidate_gate_fails_when_visible_smoke_breadth_is_not_green(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(tmp_path / "launch"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_release_gate_visible_smoke_suite",
        lambda **kwargs: build_visible_smoke_summary(
            tmp_path,
            success=False,
            failed_names=["graph_constant_plot_visible_then_dispatch"],
            failed_assertions=["graph_constant_plot_visible_then_dispatch:graph_module_result"],
        ),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["exit_code"] == 1
    assert summary["failure_stage"] == "capability_depth"
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: Capability-depth verification incomplete: Visible multi-lane prompt/result smoke",
    ]
    assert summary["visible_smoke_summary"]["failed_names"] == ["graph_constant_plot_visible_then_dispatch"]
    capability_depth = summary["capability_depth"]
    assert capability_depth["status"] == "partial"
    assert "Visible multi-lane prompt/result smoke" in capability_depth["failing_items"]
    items_by_id = {item["id"]: item for item in capability_depth["items"]}
    assert items_by_id["visible_multi_lane_smoke"]["status"] == "failed"
    assert items_by_id["visible_multi_lane_smoke"]["failed_assertions"] == [
        "graph_constant_plot_visible_then_dispatch:graph_module_result"
    ]
    assert summary["totals"]["failed_names"] == []
    assert summary["attempts_summary"]["executed_attempt_count"] == 1
    assert summary["selected_attempt_number"] == 1


def test_release_candidate_gate_rejects_missing_or_failed_fidelity_verification(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(
            tmp_path / "launch",
            include_artifice_smoke_summary=True,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )
    (tmp_path / "keypad-fidelity.verify.summary.json").write_text(
        json.dumps(
            build_fidelity_verify_summary(
                tmp_path,
                suite_name="keypad-fidelity",
                hash_count=27,
                success=False,
                fail_count=1,
            ),
            indent=2,
        ),
        encoding="utf-8",
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["failure_stage"] == "capability_depth"
    assert any(
        "Capability-depth verification incomplete: Keypad control fidelity suite" in blocker
        for blocker in summary["blockers"]
    )
    capability_depth = summary["capability_depth"]
    assert capability_depth["status"] == "partial"
    assert "Keypad control fidelity suite" in capability_depth["failing_items"]
    items_by_id = {item["id"]: item for item in capability_depth["items"]}
    assert items_by_id["keypad_control_fidelity"]["status"] == "failed"
    assert items_by_id["lcd_visual_fidelity"]["status"] == "passed"


def test_release_candidate_gate_rejects_archive_lifecycle_drift_within_attempt(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    expected_transfer_names = build_release_gate_transfer_inventory_names()
    stub_runner = StubRunner([])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(
            tmp_path / "artifice",
            loaded_archive_count=40,
            transfer_names=expected_transfer_names,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(
            tmp_path / "launch",
            loaded_archive_count=39,
            transfer_names=expected_transfer_names,
        ),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["failure_stage"] == "archive_lifecycle"
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: Archive lifecycle verification failed after the arTIfiCE handoff and CalcEngine launch validation.",
    ]
    assert summary["archive_lifecycle"]["stable_within_attempt"] is False
    assert summary["archive_lifecycle"]["loaded_archive_count_delta"] == -1
    assert summary["archive_lifecycle"]["blockers"] == [
        "Archive lifecycle counts drifted between the arTIfiCE jailbreak handoff and the CalcEngine launch verifier: 40 -> 39.",
        "CalcEngine launch archive inventory count drifted away from the runner-reported loaded archive count: 40 vs 39.",
    ]
    assert summary["attempts_summary"]["failed_attempt_numbers"] == [1]
    assert summary["archive_lifecycle_stability"]["missing_attempt_numbers"] == []
    assert summary["archive_lifecycle_stability"]["unstable_attempt_numbers"] == [1]


def test_release_candidate_gate_falls_back_to_count_only_archive_lifecycle_when_inventory_snapshots_are_unavailable(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def build_count_only_jailbreak_result() -> JailbreakResult:
        result = build_successful_jailbreak_result(tmp_path / "artifice")
        checkpoint_path = Path(str(result.checkpoints[0].snapshot_path))
        checkpoint_path.write_text(json.dumps({"screen": {"text_rows": []}}, indent=2), encoding="utf-8")
        return result

    def build_count_only_launch_result() -> LaunchResult:
        result = build_successful_launch_result(tmp_path / "launch")
        for checkpoint in result.checkpoints:
            assert checkpoint.snapshot_path is not None
            Path(str(checkpoint.snapshot_path)).write_text(
                json.dumps({"screen": {"text_rows": []}}, indent=2),
                encoding="utf-8",
            )
        return result

    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_count_only_jailbreak_result(),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_count_only_launch_result(),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is True
    assert summary["archive_lifecycle"]["inventory_supported"] is False
    assert summary["archive_lifecycle"]["stable_within_attempt"] is True
    assert summary["archive_lifecycle"]["blockers"] == []
    assert any(
        "count-only archive lifecycle telemetry" in message
        for message in summary["archive_lifecycle"]["diagnostics"]
    )
    assert summary["archive_lifecycle_stability"]["stable"] is True


def test_release_candidate_gate_rejects_archive_inventory_order_drift_within_attempt(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    expected_transfer_names = build_release_gate_transfer_inventory_names()
    drifted_transfer_names = list(reversed(expected_transfer_names))
    stub_runner = StubRunner([])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(
            tmp_path / "artifice",
            transfer_names=expected_transfer_names,
        ),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(
            tmp_path / "launch",
            transfer_names=drifted_transfer_names,
        ),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["failure_stage"] == "archive_lifecycle"
    assert summary["archive_lifecycle"]["inventory_supported"] is True
    assert summary["archive_lifecycle"]["inventory_stage_signatures_match"] is False
    assert summary["archive_lifecycle"]["stable_within_attempt"] is False
    assert summary["archive_lifecycle"]["blockers"] == [
        "CalcEngine launch archive inventory no longer matches the deterministic manifest-backed transfer payload.",
        "Archive inventory order drifted between the arTIfiCE jailbreak handoff and the CalcEngine launch verifier.",
    ]
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: Archive lifecycle verification failed after the arTIfiCE handoff and CalcEngine launch validation.",
    ]


def test_release_candidate_gate_detects_crc_drift_across_repeated_passing_attempts(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(tmp_path / "launch"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )

    hardening_results = iter(
        (
            build_direct_smoke_results(),
            build_direct_smoke_results(
                matrix_crc="1A2B3C4D",
                matrix_expected_crcs=["9FBBCE5B", "1A2B3C4D"],
                matrix_passed=True,
            ),
            build_direct_smoke_results(),
        )
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: next(hardening_results),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["release_candidate_ready"] is False
    assert summary["status"] == "failed"
    assert summary["exit_code"] == 1
    assert summary["attempts_summary"]["executed_attempt_count"] == 3
    assert summary["attempts_summary"]["passed_attempt_count"] == 3
    assert summary["stability"]["enforced"] is True
    assert summary["stability"]["stable"] is False
    assert summary["stability"]["unstable_smoke_tests"] == ["mode_switching"]
    assert summary["blockers"] == [
        "Smoke CRC stability drift was detected across repeated attempts for: mode_switching."
    ]
    mode_switch_stability = next(
        row for row in summary["stability"]["smoke_tests"] if row["name"] == "mode_switching"
    )
    assert mode_switch_stability["stable"] is False
    assert mode_switch_stability["signature_count"] == 2
    assert mode_switch_stability["present_attempt_numbers"] == [1, 2, 3]


def test_release_candidate_gate_detects_archive_lifecycle_drift_across_repeated_passing_attempts(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])

    def build_count_only_jailbreak_result(base_path: Path, *, loaded_archive_count: int) -> JailbreakResult:
        result = build_successful_jailbreak_result(base_path, loaded_archive_count=loaded_archive_count)
        checkpoint_path = Path(str(result.checkpoints[0].snapshot_path))
        checkpoint_path.write_text(json.dumps({"screen": {"text_rows": []}}, indent=2), encoding="utf-8")
        return result

    def build_count_only_launch_result(base_path: Path, *, loaded_archive_count: int) -> LaunchResult:
        result = build_successful_launch_result(base_path, loaded_archive_count=loaded_archive_count)
        for checkpoint in result.checkpoints:
            assert checkpoint.snapshot_path is not None
            Path(str(checkpoint.snapshot_path)).write_text(
                json.dumps({"screen": {"text_rows": []}}, indent=2),
                encoding="utf-8",
            )
        return result

    jailbreak_results = iter(
        (
            build_count_only_jailbreak_result(
                tmp_path / "artifice-01",
                loaded_archive_count=40,
            ),
            build_count_only_jailbreak_result(
                tmp_path / "artifice-02",
                loaded_archive_count=49,
            ),
            build_count_only_jailbreak_result(
                tmp_path / "artifice-03",
                loaded_archive_count=40,
            ),
        )
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: next(jailbreak_results),
    )
    launch_results = iter(
        (
            build_count_only_launch_result(
                tmp_path / "launch-01",
                loaded_archive_count=40,
            ),
            build_count_only_launch_result(
                tmp_path / "launch-02",
                loaded_archive_count=49,
            ),
            build_count_only_launch_result(
                tmp_path / "launch-03",
                loaded_archive_count=40,
            ),
        )
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: next(launch_results),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
        stop_on_first_failure=False,
    )

    assert summary["success"] is False
    assert summary["release_candidate_ready"] is False
    assert summary["status"] == "failed"
    assert summary["exit_code"] == 1
    assert summary["attempts_summary"]["executed_attempt_count"] == 3
    assert summary["attempts_summary"]["passed_attempt_count"] == 3
    assert summary["archive_lifecycle_stability"]["stable"] is False
    assert summary["archive_lifecycle_stability"]["signature_count"] == 2
    assert summary["archive_lifecycle_stability"]["present_attempt_numbers"] == [1, 2, 3]
    assert summary["blockers"] == [
        "Archive lifecycle drift was detected across repeated attempts."
    ]


def test_release_candidate_gate_reports_runner_setup_failure(tmp_path: Path) -> None:
    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: (_ for _ in ()).throw(CEmuProcessExitedError(
            "synthetic runner startup crash",
            returncode=5,
        )),
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["failure_stage"] == "runner_setup"
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: Unable to initialize the CEmu runner: synthetic runner startup crash",
    ]


def test_release_candidate_gate_reports_launch_bridge_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner([])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: (_ for _ in ()).throw(CEmuProcessExitedError(
            "synthetic launch-session crash",
            returncode=9,
        )),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["failure_stage"] == "launch_verification"
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: CalcEngine launch verification failed: synthetic launch-session crash",
    ]


def test_release_candidate_gate_reports_direct_smoke_bridge_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stub_runner = StubRunner([])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(tmp_path / "launch"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: (_ for _ in ()).throw(
            CEmuArchiveCapacityError("load_app failed: archive requires 65536 bytes, but capacity is limited to 32768 bytes")
        ),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
    )

    assert summary["success"] is False
    assert summary["status"] == "failed"
    assert summary["failure_stage"] == "direct_smoke"
    assert summary["blockers"] == [
        "Repeated release-candidate validation passed 0/3 attempt(s); 3 passing attempt(s) are required.",
        "Attempt 1: Direct real-ROM smoke verification failed: load_app failed: archive requires 65536 bytes, but capacity is limited to 32768 bytes",
    ]
    assert [row["name"] for row in summary["smoke_tests"]] == [
        "launch_calcengine",
        "addition_2_plus_3",
        "menu_open_close",
    ]


def test_release_candidate_gate_prunes_stale_attempt_artifacts_before_new_run(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    evidence_dir = tmp_path / "evidence"
    stale_attempt_dir = evidence_dir / "attempt-04"
    stale_single_attempt_summary = evidence_dir / "summary.json"
    stale_single_attempt_stage = evidence_dir / "direct-smoke"
    stale_attempt_dir.mkdir(parents=True, exist_ok=True)
    stale_single_attempt_stage.mkdir(parents=True, exist_ok=True)
    (stale_attempt_dir / "stale.txt").write_text("stale", encoding="utf-8")
    stale_single_attempt_summary.write_text("{}", encoding="utf-8")
    (stale_single_attempt_stage / "stale.txt").write_text("stale", encoding="utf-8")

    stub_runner = StubRunner(["E35569F2", "96FD4303", "9FBBCE5B"])
    monkeypatch.setattr(
        "release_candidate_gate.run_artifice_jailbreak",
        lambda runner, **kwargs: build_successful_jailbreak_result(tmp_path / "artifice"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_calcengine_launch_verification",
        lambda runner, **kwargs: build_successful_launch_result(tmp_path / "launch"),
    )
    monkeypatch.setattr(
        "release_candidate_gate.stage_transfer_package",
        lambda package, destination_dir: destination_dir,
    )
    monkeypatch.setattr(
        "release_candidate_gate.run_hardening_cases",
        lambda **kwargs: build_direct_smoke_results(),
    )

    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=evidence_dir,
        environment=build_ready_environment(),
        runner_factory=lambda: stub_runner,
        attempt_count=3,
    )

    assert summary["success"] is True
    assert summary["evidence_cleanup"] == {
        "base_dir": str(evidence_dir.resolve()),
        "attempt_count": 3,
        "removed_paths": ["attempt-04", "direct-smoke", "summary.json"],
        "removed_path_count": 3,
    }
    assert stale_attempt_dir.exists() is False
    assert stale_single_attempt_summary.exists() is False
    assert stale_single_attempt_stage.exists() is False


@pytest.mark.skipif(not REAL_GATE_READY, reason=REAL_GATE_SKIP_REASON)
def test_release_candidate_gate_real_backend(tmp_path: Path) -> None:
    summary = run_release_candidate_gate(
        output_path=tmp_path / "confidence.json",
        evidence_dir=tmp_path / "evidence",
        attempt_count=1,
    )

    assert summary["success"] is True
    assert summary["release_candidate_ready"] is True
    assert summary["status"] == "passed"
    assert summary["totals"]["failed"] == 0
    assert summary["attempts_summary"]["configured_attempt_count"] == 1
    assert [row["name"] for row in summary["smoke_tests"]] == [
        "launch_calcengine",
        "addition_2_plus_3",
        "menu_open_close",
        "multiplication_6_times_7",
        "mode_switching",
    ]
