from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

import pytest

import artifice_jailbreak as artifice_module
import pipeline_support as pipeline_support_module
from artifice_jailbreak import (
    DEFAULT_ARTIFICE_PROGRAM_NAME,
    JailbreakIndicators,
    JailbreakResult,
    build_artifice_shell_exit_sequence,
    build_artifice_shell_launch_sequence,
    build_tios_program_menu_sequence,
    run_artifice_jailbreak,
)
from calcengine_launcher import run_calcengine_launch_verification
from cemu_runner import CEmuProcessTimeoutError
from fake_backend import build_fake_cemu_runner, fake_crc_for_keys
from pipeline_support import (
    DEFAULT_BOOT_SEQUENCE,
    DEFAULT_PROMPT_CLEAR_SEQUENCE,
    TransferManifestEntry,
    TransferPackage,
    build_paced_expression_sequence,
    build_paced_token_sequence,
    build_program_menu_sequence,
    resolve_real_cemu_environment,
    run_smoke_checklist,
    stage_transfer_package,
    summarize_checkpoints,
)


CURRENT_SMOKE_CHECKPOINTS = (
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
)

PREVIOUS_PASS_OWNER_CHECKPOINTS = (
    "boot_home",
    "addition_result",
    "prompt_after_addition",
    "multiplication_result",
    "negative_result",
    "decimal_result",
    "parenthesized_result",
    "power_result",
    "divide_by_zero_error",
    "program_menu_open",
    "prompt_after_menu_close",
)


@dataclass(frozen=True)
class DummyCommandResult:
    command: str
    payload: Mapping[str, object]
    raw: Mapping[str, object]


class ScriptedRunner:
    def __init__(
        self,
        *,
        screen_responses: list[dict[str, object]],
        initial_app_count: int = 0,
        load_app_app_count: int = 40,
        fail_on_sequence_index: int | None = None,
        sequence_failure: Exception | None = None,
    ) -> None:
        self.is_running = False
        self.screen_responses = list(screen_responses)
        self.load_app_app_count = load_app_app_count
        self.fail_on_sequence_index = fail_on_sequence_index
        self.sequence_failure = sequence_failure
        self.load_app_payloads: list[dict[str, object]] = []
        self.sequences: list[list[object]] = []
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

    def shutdown(self) -> None:
        self.is_running = False
        self.state["running"] = False
        self.state["halted"] = True

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
        sequence_index = len(self.sequences)
        if self.fail_on_sequence_index is not None and sequence_index == self.fail_on_sequence_index:
            raise self.sequence_failure or CEmuProcessTimeoutError("scripted timeout")
        materialized = list(events)
        self.sequences.append(materialized)
        return [
            DummyCommandResult("inject_key", {"ok": True, "index": index}, {"ok": True, "index": index})
            for index, _event in enumerate(materialized)
        ]

    def capture_screen(self, timeout_s: float | None = None) -> DummyCommandResult:
        if not self.screen_responses:
            raise AssertionError("No scripted screens remain.")
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
        module_entries.append(
            TransferManifestEntry(name=name, size_bytes=path.stat().st_size, crc32="", kind="module")
        )
        resolved_paths[name] = path

    for name in runtime_names:
        path = tmp_path / name
        path.write_bytes(name.encode("ascii"))
        runtime_entries.append(
            TransferManifestEntry(name=name, size_bytes=path.stat().st_size, crc32="", kind="runtime")
        )
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
        artifice_path=stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp",
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


def _sequence_keys(actions: Sequence[object]) -> list[str]:
    keys: list[str] = []
    for action in actions:
        if getattr(action, "kind", "") == "key":
            keys.append(str(getattr(action, "key")))
    return keys


def _fake_crc_for_history(keys: Sequence[str]) -> str:
    return fake_crc_for_keys(*(keys or ("idle",)))


def write_fake_artifice_baseline(path: Path) -> Path:
    shell_keys = _sequence_keys(build_artifice_shell_launch_sequence())
    quit_keys = [*shell_keys, *_sequence_keys(build_artifice_shell_exit_sequence())]
    menu_keys = [*quit_keys, *_sequence_keys(build_tios_program_menu_sequence())]
    payload = {
        "screens": [
            {"step": "artifice_shell_loaded", "hash": _fake_crc_for_history(shell_keys)},
            {"step": "artifice_quit_transition", "hash": _fake_crc_for_history(quit_keys)},
            {"step": "ti_os_program_menu_open", "hash": _fake_crc_for_history(menu_keys)},
        ]
    }
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def build_fake_smoke_expectations() -> dict[str, dict[str, object]]:
    expectations = pipeline_support_module.load_real_smoke_expectations()
    key_history: list[str] = _sequence_keys(DEFAULT_BOOT_SEQUENCE)

    def capture(name: str) -> None:
        expectations[name] = {
            **expectations[name],
            "expected_crcs": (_fake_crc_for_history(key_history),),
        }

    capture("boot_home")

    key_history.extend(_sequence_keys(build_paced_expression_sequence("2+3")))
    capture("addition_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    capture("prompt_after_addition")

    key_history.extend(_sequence_keys(build_paced_expression_sequence("6*7")))
    capture("multiplication_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    capture("prompt_after_multiplication")

    key_history.extend(_sequence_keys(build_paced_expression_sequence("(-)5+10")))
    capture("negative_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("3.14*2")))
    capture("decimal_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("(4+5)*3")))
    capture("parenthesized_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("2^8")))
    capture("power_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("1/0", result_delay_ms=3000)))
    capture("divide_by_zero_error")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    capture("prompt_after_divide_by_zero")

    key_history.extend(_sequence_keys(build_paced_expression_sequence("graph(1,x)", result_delay_ms=3000)))
    capture("graphing_mode_result")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    capture("prompt_after_graphing_mode")

    key_history.extend(_sequence_keys(build_program_menu_sequence()))
    capture("program_menu_open")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    capture("prompt_after_menu_close")

    key_history.extend(_sequence_keys(build_paced_token_sequence(("5", "2nd", "enter", "alpha", "sto"))))
    capture("memory_store_variable")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("x+2")))
    capture("memory_recall_variable")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_token_sequence(("0", "2nd", "enter", "alpha", "sto"))))
    capture("memory_clear_variable")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("x")))
    capture("memory_recall_after_clear")

    key_history.extend(_sequence_keys(DEFAULT_PROMPT_CLEAR_SEQUENCE))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("99999999999999^9", result_delay_ms=3500)))
    capture("overflow_error")

    key_history.extend(_sequence_keys((pipeline_support_module.press("clear"), pipeline_support_module.pause(600))))
    key_history.extend(_sequence_keys(build_paced_expression_sequence("2+", result_delay_ms=2500)))
    capture("syntax_error_screen")

    key_history.extend(_sequence_keys((
        pipeline_support_module.press("clear"),
        pipeline_support_module.pause(600),
        pipeline_support_module.press("clear"),
        pipeline_support_module.pause(800),
    )))
    capture("syntax_error_prompt_restore")

    key_history.extend(_sequence_keys(build_paced_expression_sequence("hex(255)", result_delay_ms=3000)))
    capture("archive_program_execution")

    return expectations


def test_artifice_e2e_fake_backend_chains_archive_jailbreak_and_launch(
    tmp_path: Path,
    temp_rom_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    baseline_path = write_fake_artifice_baseline(tmp_path / "artifice-baseline.json")
    runner_root = tmp_path / "fake-runner"
    runner_root.mkdir(parents=True, exist_ok=True)
    runner = build_fake_cemu_runner(runner_root, temp_rom_path)

    try:
        jailbreak_result = run_artifice_jailbreak(
            runner,
            transfer_package=package,
            artifice_path=artifice_path,
            baseline_path=baseline_path,
            evidence_dir=tmp_path / "evidence",
            timeout_s=5.0,
        )
        launch_result = run_calcengine_launch_verification(
            runner,
            transfer_package=package,
            jailbreak_result=jailbreak_result,
            evidence_dir=tmp_path / "evidence",
            timeout_s=5.0,
        )
    finally:
        runner.shutdown()

    assert jailbreak_result.success is True
    assert launch_result.success is True
    assert jailbreak_result.loaded_archive_count == package.transfer_count + 1
    assert launch_result.loaded_archive_count == package.transfer_count + 1
    assert jailbreak_result.stage_dir is not None
    assert (jailbreak_result.stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert launch_result.basic_operation_passed is True
    assert launch_result.menu_open_close_passed is True
    assert [checkpoint.step for checkpoint in launch_result.checkpoints] == [
        "calcengine_launch_menu",
        "calcengine_result_2plus3",
        "calcengine_menu_open",
        "calcengine_menu_closed",
    ]
    assert launch_result.checkpoints[0].text_check_passed is True
    assert launch_result.checkpoints[1].text_check_passed is True
    assert launch_result.checkpoints[2].text_check_passed is True
    assert launch_result.checkpoints[3].text_check_passed is True


def test_fake_smoke_checklist_covers_current_and_previous_owner_checklists(
    tmp_path: Path,
    temp_rom_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    staged_transfer_dir = tmp_path / "staged-transfer"
    stage_transfer_package(package, staged_transfer_dir)
    expectations = build_fake_smoke_expectations()
    runner_root = tmp_path / "smoke-runner"
    runner_root.mkdir(parents=True, exist_ok=True)
    runner = build_fake_cemu_runner(runner_root, temp_rom_path)

    try:
        runner.boot(program_paths=[staged_transfer_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        checkpoints = run_smoke_checklist(runner, expectations=expectations)
    finally:
        runner.shutdown()

    summary = summarize_checkpoints(checkpoints)
    names = [str(checkpoint["name"]) for checkpoint in checkpoints]

    assert summary["success"] is True
    assert names == list(CURRENT_SMOKE_CHECKPOINTS)
    assert len(PREVIOUS_PASS_OWNER_CHECKPOINTS) == 11
    assert set(PREVIOUS_PASS_OWNER_CHECKPOINTS).issubset(names)


def test_fake_smoke_checklist_reports_prompt_after_multiplication_drift(
    tmp_path: Path,
    temp_rom_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    staged_transfer_dir = tmp_path / "staged-transfer"
    stage_transfer_package(package, staged_transfer_dir)
    expectations = build_fake_smoke_expectations()
    expectations["prompt_after_multiplication"] = {
        **expectations["prompt_after_multiplication"],
        "expected_crcs": ("DEADBEEF",),
    }
    runner_root = tmp_path / "smoke-runner"
    runner_root.mkdir(parents=True, exist_ok=True)
    runner = build_fake_cemu_runner(runner_root, temp_rom_path)

    try:
        runner.boot(program_paths=[staged_transfer_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE, timeout_s=5.0)
        checkpoints = run_smoke_checklist(runner, expectations=expectations)
    finally:
        runner.shutdown()

    summary = summarize_checkpoints(checkpoints)

    assert summary["success"] is False
    assert summary["failed_count"] == 1
    assert summary["failed_names"] == ["prompt_after_multiplication"]
    assert checkpoints[4]["name"] == "prompt_after_multiplication"
    assert checkpoints[4]["passed"] is False


def test_resolve_real_cemu_environment_reports_missing_rom(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    missing_rom_path = tmp_path / "missing.rom"
    monkeypatch.setattr(pipeline_support_module, "DEFAULT_ROM_PATH", missing_rom_path)

    environment = resolve_real_cemu_environment()

    assert environment.ready is False
    assert any("ROM file is missing" in blocker for blocker in environment.blockers)


def test_run_artifice_jailbreak_reports_missing_artifice_asset(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    sandbox_root = tmp_path / "fake-repo"
    sandbox_root.mkdir(parents=True, exist_ok=True)
    monkeypatch.setattr(artifice_module, "REPO_ROOT", tmp_path / "fake-repo")
    monkeypatch.setattr(
        artifice_module,
        "DEFAULT_ARTIFICE_PATH",
        tmp_path / "fake-repo" / "tools" / "emulator" / "assets" / "artifice-v2.8xp",
    )
    monkeypatch.delenv("CALCENGINE_ARTIFICE_PATH", raising=False)
    monkeypatch.chdir(sandbox_root)

    result = run_artifice_jailbreak(
        ScriptedRunner(screen_responses=[]),  # type: ignore[arg-type]
        transfer_package=package,
        evidence_dir=tmp_path / "evidence",
        timeout_s=1.0,
    )

    assert result.success is False
    assert "asset could not be resolved" in result.failure_reason
    assert result.stage_dir is None


def test_artifice_timeout_propagates_into_launch_rejection(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    baseline_path = write_fake_artifice_baseline(tmp_path / "artifice-baseline.json")
    runner = ScriptedRunner(
        screen_responses=[{"crc": _fake_crc_for_history(()), "text_rows": ["ARTIFICE", "", ""]}],
        fail_on_sequence_index=1,
        sequence_failure=CEmuProcessTimeoutError("synthetic jailbreak timeout"),
    )

    jailbreak_result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        baseline_path=baseline_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=1.0,
    )
    launch_result = run_calcengine_launch_verification(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        jailbreak_result=jailbreak_result,
        timeout_s=1.0,
    )

    assert jailbreak_result.success is False
    assert jailbreak_result.timed_out is True
    assert "synthetic jailbreak timeout" in jailbreak_result.failure_reason
    assert "Timed out during arTIfiCE step 'shell_exit'" in " | ".join(jailbreak_result.diagnostics)
    assert launch_result.success is False
    assert "requires a successful arTIfiCE jailbreak result" in launch_result.failure_reason


def test_run_calcengine_launch_verification_reports_checkpoint_failure(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir(parents=True, exist_ok=True)
    (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").write_bytes(b"ARTIFICE")
    jailbreak_result = build_successful_jailbreak_result(
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        transfer_count=package.transfer_count,
    )
    runner = ScriptedRunner(
        screen_responses=[
            {"crc": "DEADBEEF", "text_rows": ["WRONG SCREEN", "", ""]},
            {"crc": "DEADBEEF", "text_rows": ["", "WRONG RESULT", ""]},
            {"crc": "DEADBEEF", "text_rows": ["WRONG SCREEN", "", ""]},
            {"crc": "DEADBEEF", "text_rows": ["WRONG SCREEN", "", ""]},
        ],
        initial_app_count=package.transfer_count + 1,
    )

    result = run_calcengine_launch_verification(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        jailbreak_result=jailbreak_result,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert "locked launch/result/menu checkpoints" in result.failure_reason
    assert result.basic_operation_passed is False
    assert result.menu_open_close_passed is False


def test_build_paced_expression_sequence_rejects_invalid_keypress_sequence() -> None:
    with pytest.raises(ValueError, match="Unsupported expression character"):
        build_paced_expression_sequence("2+?")
