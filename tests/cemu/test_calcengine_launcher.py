from __future__ import annotations

import calcengine_launcher as launcher_module
import cemu_runner as cemu_runner_module
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

import pytest

from artifice_jailbreak import (
    JailbreakIndicators,
    JailbreakResult,
    load_artifice_expected_hashes,
    resolve_artifice_path,
    run_artifice_jailbreak,
)
from calcengine_launcher import (
    _build_fallback_checkpoint,
    load_calcengine_launch_expected_hashes,
    run_calcengine_launch_verification,
)
from cemu_runner import CEmuRunner
from pipeline_support import (
    TransferManifestEntry,
    TransferPackage,
    build_runner_config,
    resolve_real_cemu_environment,
)


REAL_CEMU_ENVIRONMENT = resolve_real_cemu_environment()
REAL_ARTIFICE_PATH = resolve_artifice_path()
REAL_LAUNCH_READY = REAL_CEMU_ENVIRONMENT.ready and REAL_ARTIFICE_PATH is not None
REAL_LAUNCH_SKIP_REASON = (
    "Real CEmu environment or arTIfiCE asset is unavailable."
    if not REAL_LAUNCH_READY
    else ""
)


@dataclass(frozen=True)
class DummyCommandResult:
    command: str
    payload: Mapping[str, object]
    raw: Mapping[str, object]


class MockCEmuRunner:
    def __init__(
        self,
        *,
        screen_responses: list[dict[str, object]],
        initial_app_count: int,
        load_app_app_count: int = 39,
    ) -> None:
        self.is_running = False
        self.screen_responses = list(screen_responses)
        self.load_app_app_count = load_app_app_count
        self.load_app_payloads: list[dict[str, object]] = []
        self.sequences: list[list[object]] = []
        self.shutdown_called = False
        self.state = {
            "rom_loaded": False,
            "running": False,
            "halted": False,
            "app_count": initial_app_count,
            "target_name": "",
            "target_is_asm": True,
        }
        self.last_screen: dict[str, object] | None = None

    def start(self) -> None:
        self.is_running = True
        self.state["running"] = True

    def get_state(self, timeout_s: float | None = None) -> DummyCommandResult:
        return DummyCommandResult("get_state", dict(self.state), dict(self.state))

    def load_rom(self, timeout_s: float | None = None) -> DummyCommandResult:
        self.state["rom_loaded"] = True
        return DummyCommandResult("load_rom", {"ok": True}, {"ok": True})

    def _send_command(
        self,
        payload: Mapping[str, Any],
        timeout_s: float | None = None,
    ) -> DummyCommandResult:
        normalized = dict(payload)
        self.load_app_payloads.append(normalized)
        self.state["app_count"] = self.load_app_app_count
        self.state["target_name"] = str(payload.get("target_name", ""))
        self.state["target_is_asm"] = bool(payload.get("target_is_asm", True))
        return DummyCommandResult("load_app", normalized, normalized)

    def send_sequence(
        self,
        events: list[object] | tuple[object, ...],
        timeout_s: float | None = None,
    ) -> list[DummyCommandResult]:
        materialized = list(events)
        self.sequences.append(materialized)
        return [
            DummyCommandResult("inject_key", {"ok": True, "index": index}, {"ok": True, "index": index})
            for index, _event in enumerate(materialized)
        ]

    def capture_screen(self, timeout_s: float | None = None) -> DummyCommandResult:
        if not self.screen_responses:
            raise AssertionError("No mock screens remain.")
        self.last_screen = self.screen_responses.pop(0)
        payload = {
            "ok": True,
            "cmd": "get_screen",
            "crc": self.last_screen["crc"],
            "output_excerpt": self.last_screen.get("output_excerpt", ""),
        }
        return DummyCommandResult("get_screen", payload, payload)

    def snapshot(self, path: Path | str, timeout_s: float | None = None) -> DummyCommandResult:
        if self.last_screen is None:
            raise AssertionError("snapshot called before capture_screen")
        snapshot_path = Path(path)
        snapshot_path.parent.mkdir(parents=True, exist_ok=True)
        snapshot_path.write_text(
            json.dumps({"screen": {"text_rows": self.last_screen.get("text_rows", [])}}),
            encoding="utf-8",
        )
        payload = {"ok": True, "cmd": "snapshot", "path": str(snapshot_path)}
        return DummyCommandResult("snapshot", payload, payload)

    def shutdown(self) -> None:
        self.shutdown_called = True


def build_dummy_transfer_package(tmp_path: Path) -> TransferPackage:
    tmp_path.mkdir(parents=True, exist_ok=True)
    module_entries: list[TransferManifestEntry] = []
    runtime_entries: list[TransferManifestEntry] = []
    resolved_paths: dict[str, Path] = {}

    module_names = ["CALCENG.8xp", *[f"MOD{index:02d}.8xp" for index in range(1, 35)]]
    runtime_names = ["fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv"]

    for name in module_names:
        path = tmp_path / name
        path.write_bytes(name.encode("ascii"))
        entry = TransferManifestEntry(name=name, size_bytes=path.stat().st_size, crc32="", kind="module")
        module_entries.append(entry)
        resolved_paths[name] = path

    for name in runtime_names:
        path = tmp_path / name
        path.write_bytes(name.encode("ascii"))
        entry = TransferManifestEntry(name=name, size_bytes=path.stat().st_size, crc32="", kind="runtime")
        runtime_entries.append(entry)
        resolved_paths[name] = path

    readme_path = tmp_path / "README.md"
    readme_path.write_text("CalcEngine transfer package", encoding="utf-8")
    readme_entry = TransferManifestEntry(name="README.md", size_bytes=-1, crc32="", kind="readme")
    resolved_paths["README.md"] = readme_path

    return TransferPackage(
        manifest_path=tmp_path / "manifest.json",
        module_entries=tuple(module_entries),
        runtime_entries=tuple(runtime_entries),
        readme_entry=readme_entry,
        resolved_paths=resolved_paths,
        candidate_roots=(tmp_path,),
        missing_entries=(),
    )


def build_successful_jailbreak_result(stage_dir: Path, evidence_dir: Path, transfer_count: int) -> JailbreakResult:
    return JailbreakResult(
        success=True,
        failure_reason="",
        timed_out=False,
        artifice_path=stage_dir / "A.8xp",
        baseline_path=None,
        stage_dir=stage_dir,
        evidence_dir=evidence_dir,
        module_count=35,
        runtime_dependency_count=4,
        transfer_count=transfer_count,
        staged_file_count=40,
        loaded_archive_count=40,
        checkpoints=(),
        indicators=JailbreakIndicators(
            privilege_escalated=True,
            port_write_access=True,
            verified_markers=("artifice_shell_loaded", "artifice_quit_transition", "ti_os_program_menu_open"),
            diagnostics=(),
        ),
        diagnostics=(),
        sequence_documentation=(),
        keypress_log=(),
        total_duration_ms=0,
    )


def test_load_calcengine_launch_expected_hashes_prefers_current_artifice_smoke_baseline() -> None:
    hashes = load_calcengine_launch_expected_hashes()

    assert hashes["calcengine_launch_menu"] == ("28B88B56",)
    assert hashes["calcengine_result_2plus3"] == ("ED152D3E",)
    assert hashes["calcengine_menu_open"] == ("D12593CE",)
    assert hashes["calcengine_menu_closed"] == ("828489BA",)


def test_artifice_smoke_launch_hashes_stay_aligned_with_current_artifice_e2e_baseline() -> None:
    launch_hashes = load_calcengine_launch_expected_hashes()
    artifice_hashes = load_artifice_expected_hashes()

    assert launch_hashes["calcengine_launch_menu"] == artifice_hashes["calcengine_launch_menu"]
    assert launch_hashes["calcengine_result_2plus3"] == artifice_hashes["calcengine_result_2plus3"]
    assert launch_hashes["calcengine_menu_open"] == artifice_hashes["calcengine_menu_open"]


def _load_artifice_smoke_launch_keys(path: Path) -> list[str]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    sequence = payload["sequence"]
    launch_start = sequence.index("hash|ti_os_program_menu_open") + 1
    launch_end = sequence.index("hash|calcengine_launch_screen")
    return [token for token in sequence[launch_start:launch_end] if token.startswith("key|")]


def test_artifice_smoke_template_and_artifacts_share_the_verified_launch_key_sequence() -> None:
    emulator_dir = Path(__file__).resolve().parents[2] / "tests" / "emulator"
    template_launch_keys = _load_artifice_smoke_launch_keys(emulator_dir / "autotest.artifice-smoke.json")

    assert _load_artifice_smoke_launch_keys(emulator_dir / "autotest.artifice-smoke.baselined.json") == template_launch_keys
    assert _load_artifice_smoke_launch_keys(emulator_dir / "autotest.artifice-smoke.generated.json") == template_launch_keys


def test_build_fallback_checkpoint_marks_inferred_crc_source_when_pass_row_omits_crc(tmp_path: Path) -> None:
    checkpoint = _build_fallback_checkpoint(
        {
            "hash_name": "calcengine_launch_screen",
            "status": "pass",
            "status_reason": "reported_pass",
            "expected_crcs": ["28B88B56", "AAAAAAAA"],
            "observed_crc": "",
        },
        step="calcengine_launch_menu",
        description="CalcEngine landing screen after fallback verification",
        snapshot_path=tmp_path / "fallback.summary.json",
    )

    assert checkpoint.passed is True
    assert checkpoint.observed_crc == "28B88B56"
    assert checkpoint.screen_payload["observed_crc_source"] == "inferred_expected_crc"
    assert checkpoint.screen_payload["observed_crc_inferred"] is True
    assert checkpoint.screen_payload["status_reason"] == "reported_pass"


def test_build_fallback_checkpoint_preserves_reported_crc_when_present(tmp_path: Path) -> None:
    checkpoint = _build_fallback_checkpoint(
        {
            "hash_name": "calcengine_result_2plus3",
            "status": "fail",
            "status_reason": "reported_fail",
            "expected_crcs": ["C23818C9"],
            "observed_crc": "DEADBEEF",
        },
        step="calcengine_result_2plus3",
        description="CalcEngine result screen after fallback verification",
        snapshot_path=tmp_path / "fallback.summary.json",
    )

    assert checkpoint.passed is False
    assert checkpoint.observed_crc == "DEADBEEF"
    assert checkpoint.screen_payload["observed_crc_source"] == "reported"
    assert checkpoint.screen_payload["observed_crc_inferred"] is False
    assert checkpoint.screen_payload["status_reason"] == "reported_fail"


def test_resolve_windows_powershell_command_falls_back_to_shell_on_non_wsl_hosts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(launcher_module, "_running_under_wsl", lambda: False)
    monkeypatch.setattr(
        launcher_module,
        "DEFAULT_WINDOWS_POWERSHELL_PATH",
        Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"),
    )

    assert launcher_module._resolve_windows_powershell_command() == "powershell.exe"



def test_run_artifice_smoke_fallback_uses_host_accessible_output_path_under_wsl(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(cemu_runner_module, "_running_under_wsl", lambda: True)
    monkeypatch.setattr(launcher_module, "_running_under_wsl", lambda: True)
    fake_windows_powershell = tmp_path / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    fake_windows_powershell.parent.mkdir(parents=True, exist_ok=True)
    fake_windows_powershell.write_text("", encoding="utf-8")
    monkeypatch.setattr(launcher_module, "DEFAULT_WINDOWS_POWERSHELL_PATH", fake_windows_powershell)
    runner = MockCEmuRunner(screen_responses=[], initial_app_count=39)
    windows_sandbox = tmp_path / "windows-host"
    host_root = windows_sandbox / "c" / "Users" / "user" / "AppData" / "Local" / "Temp" / "launcher-fallback"
    runner.rom_path = host_root / "Rom.rom"
    runner.autotester_path = host_root / "autotester.exe"
    runner.executable_path = host_root / "cemu-headless.exe"
    runner.create_host_accessible_temp_dir = lambda prefix: host_root / "host-temp"

    def fake_windows_path(path: Path | str) -> str:
        candidate = Path(path)
        try:
            relative = candidate.relative_to(windows_sandbox)
        except ValueError:
            return str(candidate)
        drive, *remainder = relative.parts
        suffix = "\\".join(remainder)
        return f"{drive.upper()}:\\{suffix}" if suffix else f"{drive.upper()}:\\"

    monkeypatch.setattr(launcher_module, "_path_for_windows_powershell", fake_windows_path)

    stage_dir = host_root / "stage"
    artifice_path = host_root / "artifice-v2.8xp"
    jailbreak_result = JailbreakResult(
        success=True,
        failure_reason="",
        timed_out=False,
        artifice_path=artifice_path,
        baseline_path=None,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "artifice-evidence",
        module_count=35,
        runtime_dependency_count=4,
        transfer_count=40,
        staged_file_count=40,
        loaded_archive_count=40,
        checkpoints=(),
        indicators=JailbreakIndicators(True, True, (), ()),
        diagnostics=(),
        sequence_documentation=(),
        keypress_log=(),
        total_duration_ms=0,
    )

    captured: dict[str, object] = {}

    def _materialize_windows_output(path_text: str) -> Path:
        if not path_text.startswith("C:\\"):
            return Path(path_text)
        drive = path_text[0].lower()
        remainder = path_text[3:].replace("\\", "/")
        return tmp_path / "windows-host" / drive / remainder

    def fake_run(command: list[str], **kwargs: object):
        captured["command"] = list(command)
        captured["timeout"] = kwargs.get("timeout")
        summary_path_text = command[command.index("-OutputPath") + 1]
        summary_path = _materialize_windows_output(summary_path_text)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(
            json.dumps(
                {
                    "delegated_runner": {
                        "summary_json": {
                            "verify_stage": {
                                "hash_results": [
                                    {
                                        "hash_name": "calcengine_launch_screen",
                                        "status": "pass",
                                        "expected_crcs": ["28B88B56"],
                                        "observed_crc": "28B88B56",
                                    },
                                    {
                                        "hash_name": "calcengine_result_2plus3",
                                        "status": "pass",
                                        "expected_crcs": ["C23818C9"],
                                        "observed_crc": "C23818C9",
                                    },
                                    {
                                        "hash_name": "calcengine_menu_open",
                                        "status": "pass",
                                        "expected_crcs": ["151319C8"],
                                        "observed_crc": "151319C8",
                                    },
                                    {
                                        "hash_name": "calcengine_menu_closed",
                                        "status": "pass",
                                        "expected_crcs": ["828489BA"],
                                        "observed_crc": "828489BA",
                                    },
                                ]
                            }
                        }
                    }
                }
            ),
            encoding="utf-8",
        )
        return type("CompletedProcess", (), {"returncode": 0, "stdout": "", "stderr": ""})()

    monkeypatch.setattr(launcher_module.subprocess, "run", fake_run)

    result = launcher_module._run_artifice_smoke_fallback(
        runner,  # type: ignore[arg-type]
        jailbreak_result=jailbreak_result,
        evidence_dir=tmp_path / "fallback-evidence",
        timeout_s=5.0,
    )

    assert result is not None
    command = captured["command"]
    assert command[0] == str(fake_windows_powershell)
    file_value = command[command.index("-File") + 1]
    assert file_value.endswith("run-artifice-smoke.ps1")
    for flag in ("-RomPath", "-AutotesterPath", "-DownloadsDir", "-ArtificeProgramPath", "-CEmuHeadlessPath"):
        value = command[command.index(flag) + 1]
        assert value.startswith("C:\\")
        assert "\\mnt\\" not in value
    output_value = command[command.index("-OutputPath") + 1]
    assert output_value == "C:\\Users\\user\\AppData\\Local\\Temp\\launcher-fallback\\host-temp\\artifice-smoke.summary.json"
    copied_summary = tmp_path / "fallback-evidence" / "artifice-smoke.summary.json"
    assert copied_summary.exists()
    assert runner.shutdown_called is True
    assert captured["timeout"] == 300.0


def test_run_calcengine_launch_verification_success_from_jailbreak_handoff(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir(parents=True, exist_ok=True)
    (stage_dir / "A.8xp").write_bytes(b"ARTIFICE")
    jailbreak_result = build_successful_jailbreak_result(
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        transfer_count=package.transfer_count,
    )
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "28B88B56", "text_rows": ["CALCENGINE READY", "", ""]},
            {"crc": "C23818C9", "text_rows": ["", "RESULT: 5", ""]},
            {"crc": "151319C8", "text_rows": ["CALCENGINE MENU", "", ""]},
            {"crc": "828489BA", "text_rows": ["CALCENGINE READY", "", ""]},
        ],
        initial_app_count=40,
    )

    result = run_calcengine_launch_verification(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        jailbreak_result=jailbreak_result,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is True
    assert result.failure_reason == ""
    assert result.basic_operation_passed is True
    assert result.menu_open_close_passed is True
    assert result.loaded_archive_count == 40
    assert runner.load_app_payloads == []
    assert [checkpoint.step for checkpoint in result.checkpoints] == [
        "calcengine_launch_menu",
        "calcengine_result_2plus3",
        "calcengine_menu_open",
        "calcengine_menu_closed",
    ]


def test_run_calcengine_launch_verification_loads_transfer_package_when_needed(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "28B88B56", "text_rows": ["CALCENGINE READY", "", ""]},
            {"crc": "C23818C9", "text_rows": ["", "RESULT: 5", ""]},
            {"crc": "151319C8", "text_rows": ["CALCENGINE MENU", "", ""]},
            {"crc": "828489BA", "text_rows": ["CALCENGINE READY", "", ""]},
        ],
        initial_app_count=0,
        load_app_app_count=39,
    )

    result = run_calcengine_launch_verification(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is True
    assert runner.load_app_payloads[0]["target_name"] == "CALCENG"
    assert runner.load_app_payloads[0]["target_is_asm"] is True
    assert result.loaded_archive_count == 39


def test_run_calcengine_launch_verification_rejects_failed_jailbreak(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    failed_jailbreak = JailbreakResult(
        success=False,
        failure_reason="jailbreak failed",
        timed_out=False,
        artifice_path=None,
        baseline_path=None,
        stage_dir=None,
        evidence_dir=None,
        module_count=35,
        runtime_dependency_count=4,
        transfer_count=package.transfer_count,
        staged_file_count=0,
        loaded_archive_count=0,
        checkpoints=(),
        indicators=JailbreakIndicators(False, False, (), ()),
        diagnostics=(),
        sequence_documentation=(),
        keypress_log=(),
        total_duration_ms=0,
    )
    runner = MockCEmuRunner(screen_responses=[], initial_app_count=0)

    result = run_calcengine_launch_verification(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        jailbreak_result=failed_jailbreak,
        timeout_s=5.0,
    )

    assert result.success is False
    assert "requires a successful arTIfiCE jailbreak result" in result.failure_reason


@pytest.mark.skipif(not REAL_LAUNCH_READY, reason=REAL_LAUNCH_SKIP_REASON)
def test_run_calcengine_launch_verification_real_backend(tmp_path: Path) -> None:
    assert REAL_CEMU_ENVIRONMENT.transfer_package is not None
    assert REAL_ARTIFICE_PATH is not None

    with CEmuRunner(build_runner_config(REAL_CEMU_ENVIRONMENT)) as runner:
        jailbreak_result = run_artifice_jailbreak(
            runner,
            transfer_package=REAL_CEMU_ENVIRONMENT.transfer_package,
            artifice_path=REAL_ARTIFICE_PATH,
            rom_validation=REAL_CEMU_ENVIRONMENT.rom_validation,
            evidence_dir=tmp_path / "artifice-real",
            timeout_s=60.0,
        )
        launch_result = run_calcengine_launch_verification(
            runner,
            transfer_package=REAL_CEMU_ENVIRONMENT.transfer_package,
            jailbreak_result=jailbreak_result,
            evidence_dir=tmp_path / "launcher-real",
            timeout_s=60.0,
        )

    assert jailbreak_result.success is True
    assert launch_result.success is True
    assert launch_result.basic_operation_passed is True
    assert launch_result.menu_open_close_passed is True
    assert [checkpoint.step for checkpoint in launch_result.checkpoints] == [
        "calcengine_launch_menu",
        "calcengine_result_2plus3",
        "calcengine_menu_open",
        "calcengine_menu_closed",
    ]
