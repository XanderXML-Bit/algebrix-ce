from __future__ import annotations

import artifice_jailbreak as artifice_module
import cemu_runner as cemu_runner_module
import json
import threading
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Mapping

import pytest

from artifice_jailbreak import (
    DEFAULT_ARTIFICE_PROGRAM_NAME,
    _calculate_usb_retry_delay_seconds,
    _resolve_step_timeout,
    build_alpha_program_launch_sequence,
    run_artifice_jailbreak,
    resolve_artifice_path,
)
from cemu_runner import CEmuProcessTimeoutError, CEmuRunner, CEmuRunnerConfig, CEmuRunnerError
from pipeline_support import (
    TransferManifestEntry,
    TransferPackage,
    build_runner_config,
    resolve_real_cemu_environment,
    validate_staged_transfer_directory,
)
from rom_validation import validate_rom_file


REAL_CEMU_ENVIRONMENT = resolve_real_cemu_environment()
REAL_ARTIFICE_PATH = resolve_artifice_path()
REAL_ARTIFICE_READY = REAL_CEMU_ENVIRONMENT.ready and REAL_ARTIFICE_PATH is not None
REAL_ARTIFICE_SKIP_REASON = (
    "Real CEmu environment or arTIfiCE asset is unavailable."
    if not REAL_ARTIFICE_READY
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
        initial_app_count: int = 0,
        load_app_app_count: int = 40,
        load_app_app_counts: list[int] | None = None,
        load_app_failures_before_success: int = 0,
        get_state_failures_before_success: int = 0,
        get_state_failure_indices: list[int] | None = None,
        fail_on_sequence_index: int | None = None,
        sequence_failure: Exception | None = None,
        fail_on_capture_index: int | None = None,
        capture_failure: Exception | None = None,
        capture_failure_indices: list[int] | None = None,
    ) -> None:
        self.is_running = False
        self.screen_responses = list(screen_responses)
        self.initial_app_count = initial_app_count
        self.load_app_app_count = load_app_app_count
        self.load_app_app_counts = list(load_app_app_counts or [])
        self.load_app_failures_before_success = load_app_failures_before_success
        self.get_state_failures_before_success = get_state_failures_before_success
        self.get_state_failure_indices = set(get_state_failure_indices or [])
        self.fail_on_sequence_index = fail_on_sequence_index
        self.sequence_failure = sequence_failure
        self.fail_on_capture_index = fail_on_capture_index
        self.capture_failure = capture_failure
        self.capture_failure_indices = set(capture_failure_indices or [])
        self.load_app_payloads: list[dict[str, object]] = []
        self.sequences: list[list[object]] = []
        self.reset_calls = 0
        self.get_state_calls = 0
        self.capture_screen_calls = 0
        self.call_timeouts: dict[str, list[float | None]] = {
            "get_state": [],
            "load_rom": [],
            "load_app": [],
            "send_sequence": [],
            "capture_screen": [],
            "snapshot": [],
            "reset": [],
        }
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
        self.call_timeouts["get_state"].append(timeout_s)
        self.get_state_calls += 1
        if self.get_state_calls <= self.get_state_failures_before_success:
            raise CEmuProcessTimeoutError("mock get_state timeout")
        if (self.get_state_calls - 1) in self.get_state_failure_indices:
            raise CEmuProcessTimeoutError(f"mock get_state timeout on call {self.get_state_calls}")
        return DummyCommandResult("get_state", dict(self.state), dict(self.state))

    def load_rom(self, timeout_s: float | None = None) -> DummyCommandResult:
        self.call_timeouts["load_rom"].append(timeout_s)
        self.state["rom_loaded"] = True
        return DummyCommandResult("load_rom", {"ok": True}, {"ok": True})

    def _send_command(
        self,
        payload: Mapping[str, Any],
        timeout_s: float | None = None,
    ) -> DummyCommandResult:
        self.call_timeouts["load_app"].append(timeout_s)
        if payload["cmd"] != "load_app":
            raise AssertionError(f"Unexpected command {payload['cmd']}")
        normalized = dict(payload)
        normalized.setdefault("backend", str(self.state.get("backend", "autotester_wrapper")))
        self.load_app_payloads.append(normalized)
        if len(self.load_app_payloads) <= self.load_app_failures_before_success:
            raise CEmuProcessTimeoutError("mock usb connection timeout")
        if self.load_app_app_counts:
            self.state["app_count"] = self.load_app_app_counts.pop(0)
        else:
            self.state["app_count"] = self.load_app_app_count
        self.state["target_name"] = str(payload.get("target_name", ""))
        self.state["target_is_asm"] = bool(payload.get("target_is_asm", True))
        return DummyCommandResult("load_app", normalized, normalized)

    def send_sequence(
        self,
        events: list[object] | tuple[object, ...],
        timeout_s: float | None = None,
    ) -> list[DummyCommandResult]:
        self.call_timeouts["send_sequence"].append(timeout_s)
        sequence_index = len(self.sequences)
        if (
            self.fail_on_sequence_index is not None
            and sequence_index == self.fail_on_sequence_index
        ):
            raise self.sequence_failure or CEmuProcessTimeoutError("mock timeout")
        materialized = list(events)
        self.sequences.append(materialized)
        return [
            DummyCommandResult("inject_key", {"ok": True, "index": index}, {"ok": True, "index": index})
            for index, _event in enumerate(materialized)
        ]

    def capture_screen(self, timeout_s: float | None = None) -> DummyCommandResult:
        self.call_timeouts["capture_screen"].append(timeout_s)
        capture_index = self.capture_screen_calls
        self.capture_screen_calls += 1
        if (
            self.fail_on_capture_index is not None
            and capture_index == self.fail_on_capture_index
        ) or capture_index in self.capture_failure_indices:
            raise self.capture_failure or CEmuProcessTimeoutError("mock capture_screen timeout")
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
        self.call_timeouts["snapshot"].append(timeout_s)
        if self.last_screen is None:
            raise AssertionError("snapshot called before capture_screen")
        snapshot_path = Path(path)
        snapshot_path.parent.mkdir(parents=True, exist_ok=True)
        snapshot_path.write_text(
            json.dumps(
                {
                    "screen": {
                        "text_rows": self.last_screen.get(
                            "text_rows",
                            ["CALCENGINE READY", "STATUS: PASS", ""],
                        )
                    }
                }
            ),
            encoding="utf-8",
        )
        payload = {"ok": True, "cmd": "snapshot", "path": str(snapshot_path)}
        return DummyCommandResult("snapshot", payload, payload)

    def reset(self, timeout_s: float | None = None) -> DummyCommandResult:
        self.call_timeouts["reset"].append(timeout_s)
        self.reset_calls += 1
        self.state["key_count"] = 0
        return DummyCommandResult("reset", {"ok": True}, {"ok": True})


def _build_payload_bytes(label: str, size_bytes: int | None) -> bytes:
    if size_bytes is None:
        return label.encode("ascii")

    encoded = f"{label}|".encode("ascii")
    repetitions = (size_bytes // len(encoded)) + 1
    return (encoded * repetitions)[:size_bytes]


def _sample_module_payloads(
    package: TransferPackage,
    *,
    count: int,
) -> dict[str, bytes]:
    return {
        entry.name: package.resolved_paths[entry.name].read_bytes()
        for entry in package.module_entries[:count]
    }


def build_dummy_transfer_package(
    tmp_path: Path,
    *,
    module_size_bytes: int | None = None,
    runtime_size_bytes: int | None = None,
) -> TransferPackage:
    tmp_path.mkdir(parents=True, exist_ok=True)
    module_entries: list[TransferManifestEntry] = []
    runtime_entries: list[TransferManifestEntry] = []
    resolved_paths: dict[str, Path] = {}

    module_names = ["CALCENG.8xp", *[f"MOD{index:02d}.8xp" for index in range(1, 35)]]
    runtime_names = ["fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv"]

    for name in module_names:
        path = tmp_path / name
        path.write_bytes(_build_payload_bytes(name, module_size_bytes))
        entry = TransferManifestEntry(name=name, size_bytes=path.stat().st_size, crc32="", kind="module")
        module_entries.append(entry)
        resolved_paths[name] = path

    for name in runtime_names:
        path = tmp_path / name
        path.write_bytes(_build_payload_bytes(name, runtime_size_bytes))
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


def _write_versioned_rom(path: Path, *, os_version: str, boot_version: str) -> Path:
    payload = bytearray(b"\x00" * 4_194_304)
    payload[1_024:1_024 + len(b"TI-84 Plus CE")] = b"TI-84 Plus CE"
    payload[4_096:4_096 + len(b"Python")] = b"Python"
    payload[8_192:8_192 + len(os_version.encode("ascii"))] = os_version.encode("ascii")
    payload[12_288:12_288 + len(boot_version.encode("ascii"))] = boot_version.encode("ascii")
    path.write_bytes(bytes(payload))
    return path


def _successful_artifice_screen_responses(*, repeat: int = 1) -> list[dict[str, object]]:
    responses: list[dict[str, object]] = []
    for _ in range(max(1, repeat)):
        responses.extend(
            [
                {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
                {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
                {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
            ]
        )
    return responses


def _native_core_artifice_screen_responses(*, repeat: int = 1) -> list[dict[str, object]]:
    responses: list[dict[str, object]] = []
    for _ in range(max(1, repeat)):
        responses.extend(
            [
                {"crc": "7450762E", "text_rows": ["arTIfiCE SHELL 2.1", "CALCENG", ""]},
                {"crc": "AA943AC0", "text_rows": ["Done", "", ""]},
                {"crc": "FD9D992C", "text_rows": ["TI-BASIC", "EXEC", "*R"]},
            ]
        )
    return responses


def _assert_clean_stage_dir_restored(
    package: TransferPackage,
    stage_dir: Path,
    sampled_payloads: Mapping[str, bytes],
) -> None:
    assert stage_dir.exists()
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())
    assert all((stage_dir / entry.name).exists() for entry in package.module_entries)
    assert all((stage_dir / entry.name).exists() for entry in package.runtime_entries)
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    validation = validate_staged_transfer_directory(package, stage_dir)
    assert validation["success"] is True
    assert validation["missing_entries"] == []
    assert validation["mismatched_entries"] == []
    assert validation["unexpected_entries"] == []


def test_build_alpha_program_launch_sequence_matches_calceng_mapping() -> None:
    sequence = build_alpha_program_launch_sequence("CALCENG", key_delay_ms=250)

    assert len(sequence) == 28
    assert sequence[0].key == "alpha"
    assert sequence[2].key == "prgm"
    assert sequence[12].key == "alpha"
    assert sequence[14].key == "prgm"
    assert sequence[16].key == "alpha"
    assert sequence[18].key == "sin"
    assert sequence[24].key == "alpha"
    assert sequence[26].key == "tan"


def test_build_alpha_program_launch_sequence_rejects_unsupported_character() -> None:
    with pytest.raises(ValueError, match="unsupported character '1'"):
        build_alpha_program_launch_sequence("A1")


def test_run_artifice_jailbreak_success(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(screen_responses=_successful_artifice_screen_responses(), load_app_app_count=40)

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is True
    assert result.failure_reason == ""
    assert result.loaded_archive_count == 40
    assert result.stage_dir is not None
    assert (result.stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert result.indicators.privilege_escalated is True
    assert result.indicators.port_write_access is True
    assert result.bypass_active is True
    assert result.rollback_performed is False
    assert result.usb_retry_count == 1
    assert [checkpoint.step for checkpoint in result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]
    assert runner.load_app_payloads[0]["target_name"] == "A"
    assert runner.load_app_payloads[0]["target_is_asm"] is False
    assert any("key clear" in entry for entry in result.keypress_log)
    assert any("key mode" in entry for entry in result.keypress_log)
    assert any("key prgm" in entry for entry in result.keypress_log)


def test_run_artifice_jailbreak_native_core_launches_artifice_from_home_before_shell_capture(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src-native")
    artifice_path = tmp_path / "artifice-native.v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(screen_responses=_native_core_artifice_screen_responses(), load_app_app_count=40)
    runner.state["native_mode"] = True
    runner.state["backend"] = "native_core"

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence-native",
        timeout_s=5.0,
    )

    assert result.success is True
    assert len(runner.sequences) == 3
    native_launch_keys = [
        event.key
        for event in runner.sequences[0]
        if getattr(event, "kind", "") == "key"
    ]
    assert native_launch_keys == ["prgm", "enter", "enter"]
    assert any(
        "native-core" in entry and "prgmA" in entry
        for entry in result.sequence_documentation
    )


def test_load_program_with_target_converts_wsl_mount_paths_for_windows_runner(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(cemu_runner_module, "_running_under_wsl", lambda: True)
    fake_exe = tmp_path / "fake-cemu.exe"
    stage_dir = Path("/mnt/c/Users/user/AppData/Local/Temp/artifice-target-interop/stage")

    class DirectTargetRunner:
        executable_path = fake_exe

        def load_program_with_target(
            self,
            program_path: Path,
            *,
            target_name: str,
            target_is_asm: bool,
            timeout_s: float | None = None,
        ) -> DummyCommandResult:
            return DummyCommandResult(
                command="load_app",
                payload={
                    "path": cemu_runner_module._path_for_host_process(program_path, self.executable_path),
                    "target_name": target_name,
                    "target_is_asm": target_is_asm,
                },
                raw={},
            )

    result = artifice_module._load_program_with_target(
        DirectTargetRunner(),
        stage_dir,
        target_name=DEFAULT_ARTIFICE_PROGRAM_NAME,
        target_is_asm=False,
        timeout_s=1.0,
    )

    assert result.payload["path"].startswith("C:\\")
    assert "\\mnt\\" not in result.payload["path"]
    assert result.payload["target_name"] == DEFAULT_ARTIFICE_PROGRAM_NAME
    assert result.payload["target_is_asm"] is False


def test_load_program_with_target_prefers_proxy_send_command_over_delegated_loader() -> None:
    call_order: list[str] = []

    class Delegate:
        def load_program_with_target(
            self,
            program_path: Path,
            *,
            target_name: str,
            target_is_asm: bool,
            timeout_s: float | None = None,
        ) -> DummyCommandResult:
            call_order.append("delegate")
            return DummyCommandResult(
                command="load_app",
                payload={"path": str(program_path), "target_name": target_name, "target_is_asm": target_is_asm},
                raw={},
            )

    class ProxyRunner:
        def __init__(self) -> None:
            self._delegate = Delegate()

        def _send_command(self, payload: Mapping[str, object], timeout_s: float | None = None) -> DummyCommandResult:
            call_order.append("proxy")
            return DummyCommandResult(command="load_app", payload=dict(payload), raw=dict(payload))

        def __getattr__(self, name: str) -> object:
            return getattr(self._delegate, name)

    result = artifice_module._load_program_with_target(
        ProxyRunner(),
        Path("/tmp/staged-transfer"),
        target_name=DEFAULT_ARTIFICE_PROGRAM_NAME,
        target_is_asm=False,
        timeout_s=1.0,
    )

    assert call_order == ["proxy"]
    assert result.payload["target_name"] == DEFAULT_ARTIFICE_PROGRAM_NAME
    assert result.payload["target_is_asm"] is False


@pytest.mark.parametrize(
    ("interrupted_stage", "runner_kwargs", "expected_checkpoints", "run_kwargs"),
    [
        pytest.param("runner_ready", {"get_state_failures_before_success": 1}, (), {}, id="runner_ready"),
        pytest.param(
            "usb_connect",
            {"load_app_failures_before_success": 1},
            (),
            {
                "usb_retry_count": 1,
                "usb_retry_delay_ms": 1,
                "usb_retry_max_delay_ms": 1,
                "usb_retry_jitter_ratio": 0.0,
            },
            id="usb_connect",
        ),
        pytest.param(
            "shell_launch",
            {
                "fail_on_sequence_index": 0,
                "sequence_failure": CEmuProcessTimeoutError("mock shell-launch interruption"),
            },
            (),
            {},
            id="shell_launch",
        ),
        pytest.param(
            "shell_capture",
            {
                "fail_on_capture_index": 0,
                "capture_failure": CEmuProcessTimeoutError("mock shell-capture interruption"),
            },
            (),
            {},
            id="shell_capture",
        ),
        pytest.param(
            "shell_exit",
            {
                "fail_on_sequence_index": 1,
                "sequence_failure": CEmuProcessTimeoutError("mock shell-exit interruption"),
            },
            ("artifice_shell_loaded",),
            {},
            id="shell_exit",
        ),
        pytest.param(
            "quit_capture",
            {
                "capture_failure_indices": [1],
                "capture_failure": CEmuProcessTimeoutError("mock quit-capture interruption"),
            },
            ("artifice_shell_loaded",),
            {},
            id="quit_capture",
        ),
        pytest.param(
            "program_menu",
            {
                "fail_on_sequence_index": 2,
                "sequence_failure": CEmuProcessTimeoutError("mock program-menu interruption"),
            },
            ("artifice_shell_loaded", "artifice_quit_transition"),
            {},
            id="program_menu",
        ),
        pytest.param(
            "program_menu_capture",
            {
                "capture_failure_indices": [2],
                "capture_failure": CEmuProcessTimeoutError("mock program-menu-capture interruption"),
            },
            ("artifice_shell_loaded", "artifice_quit_transition"),
            {},
            id="program_menu_capture",
        ),
        pytest.param(
            "final_state",
            {"get_state_failure_indices": [2]},
            ("artifice_shell_loaded", "artifice_quit_transition", "ti_os_program_menu_open"),
            {},
            id="final_state",
        ),
    ],
)
def test_run_artifice_jailbreak_rolls_back_cleanly_from_every_internal_stage(
    tmp_path: Path,
    interrupted_stage: str,
    runner_kwargs: Mapping[str, object],
    expected_checkpoints: tuple[str, ...],
    run_kwargs: Mapping[str, object],
) -> None:
    package = build_dummy_transfer_package(tmp_path / f"transfer-src-{interrupted_stage}")
    artifice_path = tmp_path / f"artifice-{interrupted_stage}.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / f"rollback-stage-{interrupted_stage}"
    sampled_payloads = _sample_module_payloads(package, count=6)
    runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
        **dict(runner_kwargs),
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / f"rollback-evidence-{interrupted_stage}",
        timeout_s=5.0,
        **dict(run_kwargs),
    )

    assert result.success is False
    assert result.rollback_performed is True
    assert result.stage_dir == stage_dir
    assert result.cleanup_actions == (
        "runner.reset",
        "stage_transfer_package",
        "validate_staged_transfer_directory",
    )
    assert tuple(checkpoint.step for checkpoint in result.checkpoints) == expected_checkpoints
    assert any(interrupted_stage in message for message in result.diagnostics)
    _assert_clean_stage_dir_restored(package, stage_dir, sampled_payloads)


def test_run_artifice_jailbreak_reports_checkpoint_mismatch(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "DEADBEEF", "text_rows": ["WRONG EXIT", "", ""]},
            {"crc": "FEEDFACE", "text_rows": ["WRONG MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert "refusing to continue into the PRGM handoff" in result.failure_reason
    assert result.indicators.privilege_escalated is False
    assert result.indicators.port_write_access is False
    assert result.bypass_active is False
    assert result.rollback_performed is True
    assert result.cleanup_actions == (
        "runner.reset",
        "stage_transfer_package",
        "validate_staged_transfer_directory",
    )
    assert result.stage_dir is not None
    assert not (result.stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert all((result.stage_dir / entry.name).exists() for entry in package.module_entries)
    assert all((result.stage_dir / entry.name).exists() for entry in package.runtime_entries)
    assert [checkpoint.passed for checkpoint in result.checkpoints] == [True, False]
    assert result.indicators.verified_markers == ("artifice_shell_loaded",)
    assert any("staged transfer directory is clean" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_reports_partial_artifice_stage_copy_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    original_copy2 = artifice_module.shutil.copy2

    def partial_artifice_copy(source: Path | str, destination: Path | str, *args, **kwargs):
        destination_path = Path(destination)
        if destination_path.name == ".A.8xp.calcengine-stage":
            destination_path.write_bytes(b"PARTIAL-A")
            raise OSError("synthetic artifice partial write")
        return original_copy2(source, destination, *args, **kwargs)

    monkeypatch.setattr(artifice_module.shutil, "copy2", partial_artifice_copy)

    result = run_artifice_jailbreak(
        MockCEmuRunner(screen_responses=[]),  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=tmp_path / "stage",
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert result.failure_reason == "Staged artifact 'A.8xp' could not be copied cleanly: synthetic artifice partial write"
    assert result.stage_dir == tmp_path / "stage"
    assert not ((tmp_path / "stage") / ".A.8xp.calcengine-stage").exists()
    assert not ((tmp_path / "stage") / "A.8xp").exists()
    assert any(
        "arTIfiCE staging failed: Staged artifact 'A.8xp' could not be copied cleanly: synthetic artifice partial write"
        in message
        for message in result.diagnostics
    )


def test_run_artifice_jailbreak_timeout_returns_diagnostics(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
        ],
        fail_on_sequence_index=1,
        sequence_failure=CEmuProcessTimeoutError("mock shell-exit timeout"),
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert result.timed_out is True
    assert "mock shell-exit timeout" in result.failure_reason
    assert result.rollback_performed is True
    assert len(result.checkpoints) == 1
    assert result.checkpoints[0].step == "artifice_shell_loaded"
    assert result.cleanup_actions == (
        "runner.reset",
        "stage_transfer_package",
        "validate_staged_transfer_directory",
    )
    assert any("Timed out during arTIfiCE step 'shell_exit'" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_handles_back_to_back_attempts_without_cooldown(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "rapid-repeat-stage"
    runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(repeat=5),
        load_app_app_count=40,
    )

    results = [
        run_artifice_jailbreak(
            runner,  # type: ignore[arg-type]
            transfer_package=package,
            artifice_path=artifice_path,
            stage_dir=stage_dir,
            evidence_dir=tmp_path / f"repeat-evidence-{attempt}",
            timeout_s=5.0,
        )
        for attempt in range(5)
    ]

    validation = validate_staged_transfer_directory(
        package,
        stage_dir,
        allowed_extra_names=(f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp",),
    )

    assert len(results) == 5
    assert all(result.success for result in results)
    assert all(result.rollback_performed is False for result in results)
    assert all(result.stage_dir == stage_dir for result in results)
    assert validation["success"] is True
    assert runner.load_app_payloads != []
    assert [payload["target_name"] for payload in runner.load_app_payloads] == ["A"] * 5
    assert all(payload["target_is_asm"] is False for payload in runner.load_app_payloads)
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()


@pytest.mark.parametrize("corruption_mode", ["missing_module", "corrupt_module"])
def test_run_artifice_jailbreak_rejects_missing_or_corrupt_staged_module_before_usb_handoff(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    corruption_mode: str,
) -> None:
    package = build_dummy_transfer_package(tmp_path / f"transfer-src-{corruption_mode}")
    artifice_path = tmp_path / f"artifice-{corruption_mode}.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / f"staging-{corruption_mode}"
    target_entry = package.module_entries[0].name
    original_stage_artifice_transfer_package = artifice_module.stage_artifice_transfer_package

    def stage_with_broken_module(
        *,
        package: TransferPackage,
        artifice_path: Path,
        destination_dir: Path,
    ) -> Path:
        staged_dir = original_stage_artifice_transfer_package(
            package=package,
            artifice_path=artifice_path,
            destination_dir=destination_dir,
        )
        target_path = staged_dir / target_entry
        if corruption_mode == "missing_module":
            target_path.unlink()
        else:
            target_path.write_bytes(b"CORRUPTED-STAGED-MODULE")
        return staged_dir

    monkeypatch.setattr(artifice_module, "stage_artifice_transfer_package", stage_with_broken_module)
    runner = MockCEmuRunner(screen_responses=_successful_artifice_screen_responses(), load_app_app_count=40)

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / f"staging-evidence-{corruption_mode}",
        timeout_s=5.0,
    )

    assert result.success is False
    assert result.failure_reason == "Staged transfer payload failed validation before the arTIfiCE handoff."
    assert result.rollback_performed is False
    assert result.stage_dir == stage_dir
    assert result.checkpoints == ()
    assert runner.load_app_payloads == []
    if corruption_mode == "missing_module":
        assert any(
            message == f"Missing staged entries: {target_entry}"
            for message in result.diagnostics
        )
    else:
        assert any(
            message == f"Mismatched staged entries: {target_entry}"
            for message in result.diagnostics
        )


def test_run_artifice_jailbreak_rejects_incompatible_rom_validation(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    rom_path = _write_versioned_rom(
        tmp_path / "os57.rom",
        os_version="5.7.0.0017",
        boot_version="5.5.2.0044",
    )
    rom_validation = validate_rom_file(rom_path)

    result = run_artifice_jailbreak(
        MockCEmuRunner(screen_responses=[]),  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        rom_validation=rom_validation,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert "requires a TI-84 Plus CE 5.8.x ROM" in result.failure_reason
    assert result.stage_dir is None
    assert any("os_version=5.7.0.0017" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_accepts_normalized_os58_metadata(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    rom_path = _write_versioned_rom(
        tmp_path / "os58.rom",
        os_version="5.8.0.0022",
        boot_version="5.5.2.0044",
    )
    rom_validation = replace(
        validate_rom_file(rom_path),
        os_version="TI-84 Plus CE OS 5.8.0.0022 build 22",
        firmware_family="",
    )
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        rom_validation=rom_validation,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is True
    assert any("Normalized ROM os_version metadata to '5.8.0.0022'" in message for message in result.diagnostics)
    assert any("Normalized ROM firmware_family metadata to '5.8.x'" in message for message in result.diagnostics)


def test_calculate_usb_retry_delay_seconds_uses_exponential_backoff_jitter_and_cap() -> None:
    assert _calculate_usb_retry_delay_seconds(
        1,
        base_delay_ms=2_000,
        max_delay_ms=30_000,
        jitter_ratio=0.2,
        random_source=lambda: 0.5,
    ) == pytest.approx(2.0)
    assert _calculate_usb_retry_delay_seconds(
        2,
        base_delay_ms=2_000,
        max_delay_ms=30_000,
        jitter_ratio=0.2,
        random_source=lambda: 0.5,
    ) == pytest.approx(4.0)
    assert _calculate_usb_retry_delay_seconds(
        5,
        base_delay_ms=2_000,
        max_delay_ms=30_000,
        jitter_ratio=0.2,
        random_source=lambda: 0.0,
    ) == pytest.approx(24.0)
    assert _calculate_usb_retry_delay_seconds(
        5,
        base_delay_ms=2_000,
        max_delay_ms=30_000,
        jitter_ratio=0.2,
        random_source=lambda: 1.0,
    ) == pytest.approx(30.0)


def test_resolve_step_timeout_applies_global_timeout_multiplier(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("CEMU_TIMEOUT_MULTIPLIER", "2")

    assert _resolve_step_timeout(
        "runner_ready",
        default_timeout_s=None,
        step_timeouts=None,
    ) == pytest.approx(20.0)
    assert _resolve_step_timeout(
        "usb_connect",
        default_timeout_s=5.0,
        step_timeouts=None,
    ) == pytest.approx(10.0)
    assert _resolve_step_timeout(
        "shell_capture",
        default_timeout_s=None,
        step_timeouts={"shell_capture": 6.5},
    ) == pytest.approx(13.0)


def test_run_artifice_jailbreak_retries_usb_connection_before_succeeding(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_failures_before_success=2,
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=3,
        usb_retry_delay_ms=0,
    )

    assert result.success is True
    assert result.usb_retry_count == 3
    assert len(runner.load_app_payloads) == 3
    assert any("USB simulation attempt 1/3 failed" in message for message in result.diagnostics)
    assert any("USB simulation attempt 3/3: app_count=40" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_succeeds_on_retry_with_backoff_after_usb_timeout(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_failures_before_success=1,
        load_app_app_count=40,
    )
    sleep_durations: list[float] = []

    monkeypatch.setattr("artifice_jailbreak._sleep_before_usb_retry", lambda seconds: sleep_durations.append(seconds))

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=3,
        usb_retry_delay_ms=2_000,
        usb_retry_max_delay_ms=30_000,
        usb_retry_jitter_ratio=0.0,
    )

    assert result.success is True
    assert result.usb_retry_count == 2
    assert len(runner.load_app_payloads) == 2
    assert sleep_durations == [2.0]
    assert any("USB simulation attempt 1/3 failed: mock usb connection timeout" in message for message in result.diagnostics)
    assert any("USB simulation retry backoff before the next attempt: 2.000s" in message for message in result.diagnostics)
    assert any("USB simulation recovered on attempt 2/3 after 1 earlier failure" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_uses_exponential_usb_retry_backoff_and_reports_retry_limit(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[],
        load_app_failures_before_success=5,
    )
    sleep_durations: list[float] = []

    monkeypatch.setattr("artifice_jailbreak._sleep_before_usb_retry", lambda seconds: sleep_durations.append(seconds))

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=3,
        usb_retry_delay_ms=2_000,
        usb_retry_max_delay_ms=30_000,
        usb_retry_jitter_ratio=0.0,
    )

    assert result.success is False
    assert result.failure_reason == "mock usb connection timeout"
    assert result.timed_out is True
    assert result.usb_retry_count == 3
    assert len(runner.load_app_payloads) == 3
    assert sleep_durations == [2.0, 4.0]
    assert any("USB simulation retry backoff before the next attempt: 2.000s" in message for message in result.diagnostics)
    assert any("USB simulation retry backoff before the next attempt: 4.000s" in message for message in result.diagnostics)
    assert any("after 3 USB attempts" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_recovers_from_partial_usb_transfer_before_retry_success(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_counts=[12, 40],
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=2,
        usb_retry_delay_ms=0,
    )

    assert result.success is True
    assert result.usb_retry_count == 2
    assert runner.reset_calls == 1
    assert len(runner.load_app_payloads) == 2
    assert any("partial archive load (12/40 staged files visible)" in message for message in result.diagnostics)
    assert any("Reset the runner before retrying the USB transfer" in message for message in result.diagnostics)
    assert any("Re-validated runner readiness before retrying the USB transfer." in message for message in result.diagnostics)


def test_run_artifice_jailbreak_succeeds_under_near_full_archive_pressure(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(
        tmp_path / "transfer-src",
        module_size_bytes=32_768,
        runtime_size_bytes=12_288,
    )
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    staged_payload_bytes = sum(entry.size_bytes for entry in (*package.module_entries, *package.runtime_entries))
    assert staged_payload_bytes > 1_000_000

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is True
    assert result.rollback_performed is False
    assert result.loaded_archive_count == package.transfer_count + 1
    assert result.stage_dir is not None
    assert result.staged_file_count == package.transfer_count + 2
    assert (result.stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()


def test_run_artifice_jailbreak_preserves_five_plus_staged_modules_across_successful_jailbreak(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "multi-module-stage"
    sampled_payloads = _sample_module_payloads(package, count=6)
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is True
    assert result.rollback_performed is False
    assert result.stage_dir == stage_dir
    assert result.loaded_archive_count == package.transfer_count + 1
    assert result.staged_file_count == package.transfer_count + 2
    assert len(sampled_payloads) >= 5
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert any("Archive load count 40 meets or exceeds the expected staged payload count 40." in message for message in result.diagnostics)


def test_run_artifice_jailbreak_recovers_cleanly_after_mid_transfer_module_swap_interrupt(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "swap-recovery-stage"
    sampled_payloads = _sample_module_payloads(package, count=5)
    swapped_module_name = next(iter(sampled_payloads))
    interrupted_runner = MockCEmuRunner(
        screen_responses=[],
        load_app_app_counts=[17],
    )
    original_send_command = interrupted_runner._send_command

    def mid_transfer_swap_send_command(
        payload: Mapping[str, Any],
        timeout_s: float | None = None,
    ) -> DummyCommandResult:
        staged_module_path = stage_dir / swapped_module_name
        if staged_module_path.exists():
            staged_module_path.write_bytes(staged_module_path.read_bytes() + b"|MID-TRANSFER-SWAP")
        return original_send_command(payload, timeout_s=timeout_s)

    interrupted_runner._send_command = mid_transfer_swap_send_command  # type: ignore[method-assign]

    interrupted_result = run_artifice_jailbreak(
        interrupted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "swap-recovery-evidence",
        timeout_s=5.0,
        usb_retry_count=1,
        usb_retry_delay_ms=0,
    )

    assert interrupted_result.success is False
    assert interrupted_result.rollback_performed is True
    assert "partial archive load (17/40 staged files visible)" in interrupted_result.failure_reason
    assert stage_dir == interrupted_result.stage_dir
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert any("Rollback restored the staged transfer directory" in message for message in interrupted_result.diagnostics)
    assert any("Rollback validated the restored staged transfer directory" in message for message in interrupted_result.diagnostics)

    restarted_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    restarted_result = run_artifice_jailbreak(
        restarted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "swap-restart-evidence",
        timeout_s=5.0,
    )

    assert restarted_result.success is True
    assert restarted_result.rollback_performed is False
    assert restarted_result.stage_dir == stage_dir
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()


def test_run_artifice_jailbreak_supports_back_to_back_attempts_with_intervening_module_loads(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "back-to-back-stage"
    sampled_payloads = _sample_module_payloads(package, count=5)
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "first-attempt-evidence",
        timeout_s=5.0,
    )

    interleaved_module_load = runner._send_command(  # type: ignore[attr-defined]
        {
            "cmd": "load_app",
            "path": str(stage_dir),
            "target_name": "CALCENG",
            "target_is_asm": True,
        }
    )

    second_result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "second-attempt-evidence",
        timeout_s=5.0,
    )

    assert first_result.success is True
    assert second_result.success is True
    assert interleaved_module_load.payload["target_name"] == "CALCENG"
    assert [payload["target_name"] for payload in runner.load_app_payloads] == [
        "A",
        "CALCENG",
        "A",
    ]
    assert runner.load_app_payloads[0]["target_is_asm"] is False
    assert runner.load_app_payloads[1]["target_is_asm"] is True
    assert runner.load_app_payloads[2]["target_is_asm"] is False
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert first_result.stage_dir == stage_dir
    assert second_result.stage_dir == stage_dir
    assert second_result.usb_retry_count == 1


def test_run_artifice_jailbreak_stops_before_usb_handoff_when_runner_ready_times_out(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[],
        get_state_failures_before_success=1,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert result.timed_out is True
    assert "mock get_state timeout" in result.failure_reason
    assert runner.load_app_payloads == []
    assert runner.sequences == []
    assert result.checkpoints == ()
    assert any("Timed out during arTIfiCE step 'runner_ready'" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_rolls_back_after_partial_usb_failure(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[],
        load_app_app_counts=[11],
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=1,
        usb_retry_delay_ms=0,
    )

    assert result.success is False
    assert "partial archive load (11/40 staged files visible)" in result.failure_reason
    assert result.usb_retry_count == 1
    assert result.rollback_performed is True
    assert result.cleanup_actions == (
        "runner.reset",
        "stage_transfer_package",
        "validate_staged_transfer_directory",
    )
    assert result.stage_dir is not None
    assert not (result.stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert all((result.stage_dir / entry.name).exists() for entry in package.module_entries)
    assert all((result.stage_dir / entry.name).exists() for entry in package.runtime_entries)
    assert any("staged transfer directory is clean" in message for message in result.diagnostics)
    assert any("Rollback validated the restored staged transfer directory" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_restores_clean_stage_after_mid_jailbreak_archive_corruption(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "stage"
    corrupted_entry = package.module_entries[0]
    original_payload = package.resolved_paths[corrupted_entry.name].read_bytes()
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "DEADBEEF", "text_rows": ["CORRUPTED MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    original_verify = artifice_module._verify_os58_bypass_active

    def corrupt_stage_then_verify(checkpoints: object) -> tuple[bool, tuple[str, ...]]:
        staged_module_path = stage_dir / corrupted_entry.name
        staged_module_path.write_bytes(original_payload + b"|CORRUPTED-MID-JAILBREAK")
        return original_verify(checkpoints)

    monkeypatch.setattr(
        artifice_module,
        "_verify_os58_bypass_active",
        corrupt_stage_then_verify,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert result.rollback_performed is True
    assert result.stage_dir == stage_dir
    assert (stage_dir / corrupted_entry.name).read_bytes() == original_payload
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert any("Rollback restored the staged transfer directory" in message for message in result.diagnostics)
    assert any("Rollback validated the restored staged transfer directory" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_timeout_cascade_skips_later_steps_and_uses_rollback_timeout(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        fail_on_sequence_index=0,
        sequence_failure=CEmuProcessTimeoutError("mock shell-launch timeout"),
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_launch": 1.5,
            "shell_exit": 2.5,
            "program_menu": 3.5,
            "rollback": 4.5,
        },
    )

    assert result.success is False
    assert result.timed_out is True
    assert result.failure_reason == "mock shell-launch timeout"
    assert result.rollback_performed is True
    assert result.checkpoints == ()
    assert runner.sequences == []
    assert runner.call_timeouts["send_sequence"] == [1.5]
    assert runner.call_timeouts["capture_screen"] == []
    assert runner.call_timeouts["reset"] == [4.5]
    assert any("Timed out during arTIfiCE step 'shell_launch'" in message for message in result.diagnostics)
    assert not any("shell_exit" in message and "Timed out during arTIfiCE step" in message for message in result.diagnostics)
    assert not any("program_menu" in message and "Timed out during arTIfiCE step" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_rolls_back_when_usb_init_times_out(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "usb-timeout-stage"
    sampled_payloads = _sample_module_payloads(package, count=5)
    runner = MockCEmuRunner(
        screen_responses=[],
        load_app_failures_before_success=1,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=1,
        usb_retry_delay_ms=0,
        step_timeouts={
            "usb_connect": 1.25,
            "rollback": 4.25,
        },
    )

    assert result.success is False
    assert result.timed_out is True
    assert result.failure_reason == "mock usb connection timeout"
    assert result.rollback_performed is True
    assert result.cleanup_actions == (
        "runner.reset",
        "stage_transfer_package",
        "validate_staged_transfer_directory",
    )
    assert result.checkpoints == ()
    assert runner.call_timeouts["load_app"] == [1.25]
    assert runner.call_timeouts["send_sequence"] == []
    assert runner.call_timeouts["capture_screen"] == []
    assert runner.call_timeouts["reset"] == [4.25]
    assert result.stage_dir == stage_dir
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert any("Timed out during arTIfiCE step 'usb_connect' after 1 USB attempts" in message for message in result.diagnostics)
    assert any("Rollback reset the runner session" in message for message in result.diagnostics)
    assert any("Rollback validated the restored staged transfer directory" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_rolls_back_when_shell_capture_times_out(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "shell-capture-timeout-stage"
    sampled_payloads = _sample_module_payloads(package, count=5)
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock shell-capture timeout"),
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_launch": 1.5,
            "shell_capture": 2.5,
            "rollback": 4.5,
        },
    )

    assert result.success is False
    assert result.timed_out is True
    assert result.failure_reason == "mock shell-capture timeout"
    assert result.rollback_performed is True
    assert result.checkpoints == ()
    assert runner.call_timeouts["send_sequence"] == [1.5]
    assert runner.call_timeouts["capture_screen"] == [2.5]
    assert runner.call_timeouts["reset"] == [4.5]
    assert result.stage_dir == stage_dir
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert any("Timed out during arTIfiCE step 'shell_capture'" in message for message in result.diagnostics)
    assert not any("Timed out during arTIfiCE step 'shell_exit'" in message for message in result.diagnostics)
    assert any("Rollback restored the staged transfer directory" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_rolls_back_when_program_menu_capture_times_out(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "program-menu-capture-timeout-stage"
    sampled_payloads = _sample_module_payloads(package, count=6)
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock program-menu capture timeout"),
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 1.75,
            "quit_capture": 2.75,
            "program_menu": 3.75,
            "program_menu_capture": 4.75,
            "rollback": 5.75,
        },
    )

    assert result.success is False
    assert result.timed_out is True
    assert result.failure_reason == "mock program-menu capture timeout"
    assert result.rollback_performed is True
    assert [checkpoint.step for checkpoint in result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert [checkpoint.passed for checkpoint in result.checkpoints] == [True, True]
    assert runner.call_timeouts["capture_screen"] == [1.75, 2.75, 4.75]
    assert runner.call_timeouts["send_sequence"][-1] == 3.75
    assert runner.call_timeouts["reset"] == [5.75]
    assert result.stage_dir == stage_dir
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert any("Timed out during arTIfiCE step 'program_menu_capture'" in message for message in result.diagnostics)
    assert not any("Timed out during arTIfiCE step 'final_state'" in message for message in result.diagnostics)
    assert any("Rollback validated the restored staged transfer directory" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_rolls_back_when_final_state_times_out(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "final-state-timeout-stage"
    sampled_payloads = _sample_module_payloads(package, count=5)
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        get_state_failure_indices=[2],
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        step_timeouts={
            "final_state": 2.25,
            "rollback": 4.0,
        },
    )

    assert result.success is False
    assert result.timed_out is True
    assert result.failure_reason == "mock get_state timeout on call 3"
    assert result.rollback_performed is True
    assert [checkpoint.step for checkpoint in result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]
    assert runner.call_timeouts["get_state"] == [5.0, 5.0, 2.25]
    assert runner.call_timeouts["reset"] == [4.0]
    assert result.stage_dir == stage_dir
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert any("Timed out during arTIfiCE step 'final_state'" in message for message in result.diagnostics)
    assert any("Rollback restored the staged transfer directory" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_refuses_prgm_handoff_when_bypass_verification_fails(tmp_path: Path) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "DEADBEEF", "text_rows": ["WRONG SHELL", "", ""]},
            {"crc": "FEEDFACE", "text_rows": ["WRONG QUIT", "", ""]},
        ],
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
    )

    assert result.success is False
    assert result.bypass_active is False
    assert result.rollback_performed is True
    assert [checkpoint.step for checkpoint in result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert result.cleanup_actions == (
        "runner.reset",
        "stage_transfer_package",
        "validate_staged_transfer_directory",
    )
    assert len(runner.sequences) == 2
    assert "refusing to continue into the PRGM handoff" in result.failure_reason
    assert any("OS 5.8 bypass verification failed at the shell-loaded checkpoint." in message for message in result.diagnostics)
    assert any("Rollback restored the staged transfer directory" in message for message in result.diagnostics)
    assert not (result.stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()


def test_run_artifice_jailbreak_retries_when_an_overlapping_usb_transfer_is_rejected(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )
    attempted_payloads: list[dict[str, object]] = []
    original_send = runner._send_command

    def overlapping_send_command(
        payload: Mapping[str, Any],
        timeout_s: float | None = None,
    ) -> DummyCommandResult:
        attempted_payloads.append(dict(payload))
        if len(attempted_payloads) == 1:
            raise CEmuRunnerError("mock usb transfer already in progress")
        return original_send(payload, timeout_s=timeout_s)

    runner._send_command = overlapping_send_command  # type: ignore[method-assign]

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "evidence",
        timeout_s=5.0,
        usb_retry_count=2,
        usb_retry_delay_ms=0,
    )

    assert result.success is True
    assert result.usb_retry_count == 2
    assert len(attempted_payloads) == 2
    assert len(runner.load_app_payloads) == 1
    assert runner.load_app_payloads[0]["target_name"] == "A"
    assert any("mock usb transfer already in progress" in message for message in result.diagnostics)
    assert any("USB simulation recovered on attempt 2/2 after 1 earlier failure" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_recovers_after_power_cycle_restart_from_clean_stage(
    tmp_path: Path,
) -> None:
    class PowerCycleRunner(MockCEmuRunner):
        def send_sequence(
            self,
            events: list[object] | tuple[object, ...],
            timeout_s: float | None = None,
        ) -> list[DummyCommandResult]:
            if len(self.sequences) == 1:
                self.is_running = False
                self.state["running"] = False
                self.state["halted"] = True
                raise CEmuRunnerError("mock power cycle during shell_exit")
            return super().send_sequence(events, timeout_s=timeout_s)

    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "power-cycle-stage"
    failed_runner = PowerCycleRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
        ],
        load_app_app_count=40,
    )

    interrupted_result = run_artifice_jailbreak(
        failed_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "power-cycle-evidence",
        timeout_s=5.0,
    )

    assert interrupted_result.success is False
    assert interrupted_result.failure_reason == "mock power cycle during shell_exit"
    assert interrupted_result.rollback_performed is True
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert any("mock power cycle during shell_exit" in message for message in interrupted_result.diagnostics)

    restarted_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    restarted_result = run_artifice_jailbreak(
        restarted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "power-cycle-restart-evidence",
        timeout_s=5.0,
    )

    assert restarted_result.success is True
    assert restarted_result.rollback_performed is False
    assert restarted_result.stage_dir == stage_dir
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert restarted_runner.load_app_payloads[0]["target_name"] == "A"
    assert restarted_result.usb_retry_count == 1


def test_run_artifice_jailbreak_persists_checkpoint_and_resumes_after_power_cycle(
    tmp_path: Path,
) -> None:
    class PowerCycleRunner(MockCEmuRunner):
        def send_sequence(
            self,
            events: list[object] | tuple[object, ...],
            timeout_s: float | None = None,
        ) -> list[DummyCommandResult]:
            if len(self.sequences) == 1:
                self.is_running = False
                self.state["running"] = False
                self.state["halted"] = True
                raise CEmuRunnerError("mock power cycle during shell_exit")
            return super().send_sequence(events, timeout_s=timeout_s)

    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "checkpoint-stage"
    checkpoint_path = tmp_path / "checkpoint-evidence" / "artifice.checkpoint.json"
    first_runner = PowerCycleRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
        ],
        load_app_app_count=40,
    )

    failed_result = run_artifice_jailbreak(
        first_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    failed_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert failed_result.success is False
    assert failed_result.failure_reason == "mock power cycle during shell_exit"
    assert failed_result.rollback_performed is False
    assert failed_result.stage_dir == stage_dir
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert failed_checkpoint["version"] == artifice_module.DEFAULT_ARTIFICE_CHECKPOINT_VERSION
    assert failed_checkpoint["status"] == "failed"
    assert failed_checkpoint["last_completed_step"] == "artifice_shell_loaded"
    assert failed_checkpoint["checkpoints"][0]["step"] == "artifice_shell_loaded"
    assert any("Preserved the staged arTIfiCE transfer directory" in message for message in failed_result.diagnostics)

    resumed_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    resumed_result = run_artifice_jailbreak(
        resumed_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    resumed_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert resumed_result.success is True
    assert resumed_result.rollback_performed is False
    assert resumed_result.stage_dir == stage_dir
    assert [checkpoint.step for checkpoint in resumed_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]
    assert resumed_checkpoint["status"] == "completed"
    assert resumed_checkpoint["last_completed_step"] == "ti_os_program_menu_open"
    assert any("Resuming from the persisted arTIfiCE checkpoint" in message for message in resumed_result.diagnostics)


def test_run_artifice_jailbreak_recovers_across_stage1_then_stage3_failures_before_completion(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "multi-stage-recovery-stage"
    checkpoint_path = tmp_path / "multi-stage-recovery-evidence" / "artifice.checkpoint.json"
    first_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock stage-1 timeout"),
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        first_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    first_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert first_result.success is False
    assert first_result.timed_out is True
    assert first_result.rollback_performed is False
    assert first_result.checkpoints == ()
    assert first_checkpoint["status"] == "failed"
    assert first_checkpoint["last_completed_step"] == ""
    assert first_checkpoint["checkpoints"] == []
    assert int(first_checkpoint["created_at_epoch_ms"]) > 0
    assert int(first_checkpoint["updated_at_epoch_ms"]) >= int(first_checkpoint["created_at_epoch_ms"])

    second_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock stage-3 timeout"),
        load_app_app_count=40,
    )

    second_result = run_artifice_jailbreak(
        second_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    second_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert second_result.success is False
    assert second_result.timed_out is True
    assert second_result.rollback_performed is False
    assert [checkpoint.step for checkpoint in second_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert second_checkpoint["status"] == "failed"
    assert second_checkpoint["last_completed_step"] == "artifice_quit_transition"
    assert [checkpoint["step"] for checkpoint in second_checkpoint["checkpoints"]] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert int(second_checkpoint["created_at_epoch_ms"]) == int(first_checkpoint["created_at_epoch_ms"])
    assert int(second_checkpoint["updated_at_epoch_ms"]) >= int(first_checkpoint["updated_at_epoch_ms"])
    assert any("Resuming from the persisted arTIfiCE checkpoint" in message for message in second_result.diagnostics)

    recovery_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
    )

    recovery_result = run_artifice_jailbreak(
        recovery_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    final_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert recovery_result.success is True
    assert recovery_result.rollback_performed is False
    assert [checkpoint.step for checkpoint in recovery_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]
    assert final_checkpoint["status"] == "completed"
    assert final_checkpoint["last_completed_step"] == "ti_os_program_menu_open"
    assert int(final_checkpoint["created_at_epoch_ms"]) == int(first_checkpoint["created_at_epoch_ms"])
    assert int(final_checkpoint["updated_at_epoch_ms"]) >= int(second_checkpoint["updated_at_epoch_ms"])
    assert any("Resuming from the persisted arTIfiCE checkpoint" in message for message in recovery_result.diagnostics)


def test_run_artifice_jailbreak_detects_checkpoint_corruption_mid_recovery_and_restarts_cleanly(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "mid-recovery-corruption-stage"
    checkpoint_path = tmp_path / "mid-recovery-corruption-evidence" / "artifice.checkpoint.json"
    interrupted_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock initial recovery seed failure"),
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        interrupted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    assert first_result.success is False
    assert checkpoint_path.exists() is True

    original_write_checkpoint_state = artifice_module._write_checkpoint_state
    corruption_state = {"injected": False}

    def corrupt_failed_resume_checkpoint(*args, **kwargs):
        checkpoint_state = original_write_checkpoint_state(*args, **kwargs)
        if kwargs.get("status") == "failed" and corruption_state["injected"] is False:
            Path(args[0]).write_text(
                json.dumps(
                    {
                        "version": artifice_module.DEFAULT_ARTIFICE_CHECKPOINT_VERSION,
                        "status": "failed",
                        "integrity": {
                            "algorithm": "sha256",
                            "digest": "BROKEN",
                        },
                    },
                    indent=2,
                ),
                encoding="utf-8",
            )
            corruption_state["injected"] = True
        return checkpoint_state

    monkeypatch.setattr(artifice_module, "_write_checkpoint_state", corrupt_failed_resume_checkpoint)
    resumed_failing_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock resumed recovery failure"),
        load_app_app_count=40,
    )

    second_result = run_artifice_jailbreak(
        resumed_failing_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    monkeypatch.setattr(artifice_module, "_write_checkpoint_state", original_write_checkpoint_state)
    recovery_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
    )

    recovery_result = run_artifice_jailbreak(
        recovery_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    rewritten_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert second_result.success is False
    assert corruption_state["injected"] is True
    assert any("Resuming from the persisted arTIfiCE checkpoint" in message for message in second_result.diagnostics)
    assert recovery_result.success is True
    assert rewritten_checkpoint["status"] == "completed"
    assert any("failed integrity validation" in message for message in recovery_result.diagnostics)
    assert any("forcing a clean restart" in message for message in recovery_result.diagnostics)


def test_run_artifice_jailbreak_expires_stale_checkpoint_data_before_clean_restart(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "expired-checkpoint-stage"
    checkpoint_path = tmp_path / "expired-checkpoint-evidence" / "artifice.checkpoint.json"
    interrupted_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock stale checkpoint seed failure"),
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        interrupted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    stale_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))
    stale_checkpoint["created_at_epoch_ms"] = 1
    stale_checkpoint["updated_at_epoch_ms"] = 1
    stale_core_payload = {
        key: value
        for key, value in stale_checkpoint.items()
        if key != "integrity"
    }
    stale_checkpoint["integrity"]["digest"] = artifice_module._checkpoint_digest(stale_core_payload)
    checkpoint_path.write_text(json.dumps(stale_checkpoint, indent=2), encoding="utf-8")

    recovery_runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
    )

    recovery_result = run_artifice_jailbreak(
        recovery_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        checkpoint_max_age_s=1.0,
        timeout_s=5.0,
    )

    rewritten_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert first_result.success is False
    assert recovery_result.success is True
    assert rewritten_checkpoint["status"] == "completed"
    assert int(rewritten_checkpoint["created_at_epoch_ms"]) > 1
    assert int(rewritten_checkpoint["updated_at_epoch_ms"]) >= int(rewritten_checkpoint["created_at_epoch_ms"])
    assert any("checkpoint age" in message for message in recovery_result.diagnostics)
    assert any("forcing a clean restart" in message for message in recovery_result.diagnostics)
    assert not any("Resuming from the persisted arTIfiCE checkpoint" in message for message in recovery_result.diagnostics)


def test_run_artifice_jailbreak_detects_tampered_checkpoint_and_performs_clean_restart(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "tampered-stage"
    checkpoint_path = tmp_path / "tampered-evidence" / "artifice.checkpoint.json"
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    checkpoint_path.write_text(
        json.dumps(
            {
                "version": artifice_module.DEFAULT_ARTIFICE_CHECKPOINT_VERSION,
                "status": "failed",
                "stage_dir": str(stage_dir),
                "evidence_dir": str(checkpoint_path.parent),
                "artifice_path": str(artifice_path),
                "transfer_count": package.transfer_count,
                "module_count": package.module_count,
                "runtime_dependency_count": package.runtime_dependency_count,
                "last_completed_step": "artifice_shell_loaded",
                "checkpoints": [],
                "integrity": {
                    "algorithm": "sha256",
                    "digest": "DEADBEEF",
                },
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    rewritten_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert result.success is True
    assert rewritten_checkpoint["status"] == "completed"
    assert any("failed integrity validation" in message for message in result.diagnostics)
    assert any("forcing a clean restart" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_rejects_concurrent_checkpoint_access(
    tmp_path: Path,
) -> None:
    class BlockingShellLaunchRunner(MockCEmuRunner):
        def __init__(self, *args, block_started: threading.Event, release_block: threading.Event, **kwargs) -> None:
            super().__init__(*args, **kwargs)
            self._block_started = block_started
            self._release_block = release_block

        def send_sequence(
            self,
            events: list[object] | tuple[object, ...],
            timeout_s: float | None = None,
        ) -> list[DummyCommandResult]:
            if len(self.sequences) == 0:
                self._block_started.set()
                assert self._release_block.wait(timeout=5.0) is True
            return super().send_sequence(events, timeout_s=timeout_s)

    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    checkpoint_path = tmp_path / "shared-evidence" / "artifice.checkpoint.json"
    block_started = threading.Event()
    release_block = threading.Event()
    primary_runner = BlockingShellLaunchRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
        block_started=block_started,
        release_block=release_block,
    )
    primary_result_holder: dict[str, object] = {}

    def run_primary_attempt() -> None:
        primary_result_holder["result"] = run_artifice_jailbreak(
            primary_runner,  # type: ignore[arg-type]
            transfer_package=package,
            artifice_path=artifice_path,
            stage_dir=tmp_path / "shared-stage",
            evidence_dir=checkpoint_path.parent,
            checkpoint_path=checkpoint_path,
            timeout_s=5.0,
        )

    primary_thread = threading.Thread(target=run_primary_attempt)
    primary_thread.start()
    assert block_started.wait(timeout=5.0) is True

    rejected_result = run_artifice_jailbreak(
        MockCEmuRunner(screen_responses=_successful_artifice_screen_responses(), load_app_app_count=40),  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=tmp_path / "shared-stage",
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        checkpoint_lock_timeout_s=0.1,
        timeout_s=5.0,
    )

    release_block.set()
    primary_thread.join(timeout=5.0)
    assert primary_thread.is_alive() is False

    primary_result = primary_result_holder["result"]

    assert rejected_result.success is False
    assert "already locked by another process" in rejected_result.failure_reason
    assert any("already locked by another process" in message for message in rejected_result.diagnostics)
    assert primary_result.success is True


def test_run_artifice_jailbreak_migrates_version1_checkpoint_and_rewrites_version2(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "migrated-stage"
    artifice_module.stage_artifice_transfer_package(
        package=package,
        artifice_path=artifice_path,
        destination_dir=stage_dir,
    )
    checkpoint_path = tmp_path / "migrated-evidence" / "artifice.checkpoint.json"
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    checkpoint_path.write_text(
        json.dumps(
            {
                "version": 1,
                "status": "failed",
                "stage_dir": str(stage_dir),
                "evidence_dir": str(checkpoint_path.parent),
                "artifice_path": str(artifice_path),
                "transfer_count": package.transfer_count,
                "module_count": package.module_count,
                "runtime_dependency_count": package.runtime_dependency_count,
                "last_completed_step": "artifice_shell_loaded",
                "checkpoints": [],
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    migrated_checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))

    assert result.success is True
    assert migrated_checkpoint["version"] == artifice_module.DEFAULT_ARTIFICE_CHECKPOINT_VERSION
    assert migrated_checkpoint["status"] == "completed"
    assert migrated_checkpoint["integrity"]["algorithm"] == "sha256"


def test_run_artifice_jailbreak_rejects_unsupported_checkpoint_version_with_clean_restart(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    checkpoint_path = tmp_path / "unsupported-evidence" / "artifice.checkpoint.json"
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    checkpoint_path.write_text(
        json.dumps(
            {
                "version": 99,
                "status": "failed",
                "stage_dir": str(tmp_path / "unsupported-stage"),
                "evidence_dir": str(checkpoint_path.parent),
                "artifice_path": str(artifice_path),
                "transfer_count": package.transfer_count,
                "module_count": package.module_count,
                "runtime_dependency_count": package.runtime_dependency_count,
                "last_completed_step": "",
                "checkpoints": [],
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    runner = MockCEmuRunner(
        screen_responses=_successful_artifice_screen_responses(),
        load_app_app_count=40,
    )

    result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=tmp_path / "unsupported-stage",
        evidence_dir=checkpoint_path.parent,
        checkpoint_path=checkpoint_path,
        timeout_s=5.0,
    )

    assert result.success is True
    assert any("Unsupported arTIfiCE checkpoint version 99" in message for message in result.diagnostics)
    assert any("forcing a clean restart" in message for message in result.diagnostics)


def test_run_artifice_jailbreak_recovers_after_program_menu_power_cycle_then_clean_retry_succeeds(
    tmp_path: Path,
) -> None:
    class ProgramMenuPowerCycleRunner(MockCEmuRunner):
        def send_sequence(
            self,
            events: list[object] | tuple[object, ...],
            timeout_s: float | None = None,
        ) -> list[DummyCommandResult]:
            if len(self.sequences) == 2:
                self.is_running = False
                self.state["running"] = False
                self.state["halted"] = True
                raise CEmuRunnerError("mock power cycle during program_menu")
            return super().send_sequence(events, timeout_s=timeout_s)

    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "program-menu-power-cycle-stage"
    sampled_payloads = _sample_module_payloads(package, count=6)
    interrupted_runner = ProgramMenuPowerCycleRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
        ],
        load_app_app_count=40,
    )

    interrupted_result = run_artifice_jailbreak(
        interrupted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "program-menu-power-cycle-evidence",
        timeout_s=5.0,
    )

    post_interrupt_validation = validate_staged_transfer_directory(package, stage_dir)

    assert interrupted_result.success is False
    assert interrupted_result.failure_reason == "mock power cycle during program_menu"
    assert interrupted_result.rollback_performed is True
    assert [checkpoint.step for checkpoint in interrupted_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert post_interrupt_validation["success"] is True
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload
    assert any("mock power cycle during program_menu" in message for message in interrupted_result.diagnostics)

    restarted_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    restarted_result = run_artifice_jailbreak(
        restarted_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "program-menu-power-cycle-retry-evidence",
        timeout_s=5.0,
    )

    assert restarted_result.success is True
    assert restarted_result.rollback_performed is False
    assert restarted_result.stage_dir == stage_dir
    assert [checkpoint.step for checkpoint in restarted_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert restarted_runner.load_app_payloads[0]["target_name"] == "A"
    assert restarted_result.usb_retry_count == 1


def test_run_artifice_jailbreak_detects_verification_stage_corruption_then_clean_retry_succeeds(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "verification-corruption-stage"
    corrupted_entry = package.module_entries[0]
    original_payload = package.resolved_paths[corrupted_entry.name].read_bytes()
    original_verify = artifice_module._verify_os58_bypass_active
    corrupting_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
        ],
        load_app_app_count=40,
    )

    def verification_detects_stage_corruption(
        checkpoints: object,
    ) -> tuple[bool, tuple[str, ...]]:
        staged_module_path = stage_dir / corrupted_entry.name
        staged_module_path.write_bytes(original_payload + b"|VERIFY-CORRUPTION")
        return False, (
            "Detected staged archive corruption while verifying the locked OS 5.8 bypass checkpoint integrity.",
        )

    monkeypatch.setattr(
        artifice_module,
        "_verify_os58_bypass_active",
        verification_detects_stage_corruption,
    )

    failed_result = run_artifice_jailbreak(
        corrupting_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "verification-corruption-evidence",
        timeout_s=5.0,
    )

    assert failed_result.success is False
    assert failed_result.rollback_performed is True
    assert failed_result.stage_dir == stage_dir
    assert [checkpoint.step for checkpoint in failed_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert (stage_dir / corrupted_entry.name).read_bytes() == original_payload
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert any(
        "Detected staged archive corruption while verifying the locked OS 5.8 bypass checkpoint integrity."
        in message
        for message in failed_result.diagnostics
    )
    assert any("Rollback validated the restored staged transfer directory" in message for message in failed_result.diagnostics)

    failed_validation = validate_staged_transfer_directory(package, stage_dir)
    assert failed_validation["success"] is True

    monkeypatch.setattr(artifice_module, "_verify_os58_bypass_active", original_verify)
    recovery_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    recovery_result = run_artifice_jailbreak(
        recovery_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "verification-corruption-retry-evidence",
        timeout_s=5.0,
    )

    assert recovery_result.success is True
    assert recovery_result.rollback_performed is False
    assert recovery_result.stage_dir == stage_dir
    assert [checkpoint.step for checkpoint in recovery_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()


def test_run_artifice_jailbreak_recovers_after_shell_stage_failure_then_immediate_retry_success(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "shell-stage-retry"
    sampled_payloads = _sample_module_payloads(package, count=5)
    failed_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock shell-stage timeout"),
        load_app_app_count=40,
    )

    failed_result = run_artifice_jailbreak(
        failed_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "shell-stage-failure-evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_launch": 1.25,
            "shell_capture": 2.25,
            "rollback": 4.25,
        },
    )

    assert failed_result.success is False
    assert failed_result.failure_reason == "mock shell-stage timeout"
    assert failed_result.rollback_performed is True
    assert failed_result.stage_dir == stage_dir
    assert failed_result.checkpoints == ()
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload

    retry_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    retry_result = run_artifice_jailbreak(
        retry_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "shell-stage-retry-evidence",
        timeout_s=5.0,
    )

    assert retry_result.success is True
    assert retry_result.rollback_performed is False
    assert retry_result.stage_dir == stage_dir
    assert retry_result.loaded_archive_count == 40
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert [checkpoint.step for checkpoint in retry_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]


def test_run_artifice_jailbreak_recovers_after_program_menu_failure_then_retry_with_increased_timeout_succeeds(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "program-menu-retry"
    sampled_payloads = _sample_module_payloads(package, count=6)
    failed_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock program-menu timeout"),
        load_app_app_count=40,
    )

    failed_result = run_artifice_jailbreak(
        failed_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "program-menu-failure-evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 1.25,
            "quit_capture": 1.5,
            "program_menu": 1.75,
            "program_menu_capture": 2.0,
            "rollback": 4.0,
        },
    )

    assert failed_result.success is False
    assert failed_result.failure_reason == "mock program-menu timeout"
    assert failed_result.rollback_performed is True
    assert failed_result.stage_dir == stage_dir
    assert [checkpoint.step for checkpoint in failed_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert failed_runner.call_timeouts["capture_screen"] == [1.25, 1.5, 2.0]
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload

    retry_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    retry_result = run_artifice_jailbreak(
        retry_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "program-menu-retry-evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 3.25,
            "quit_capture": 4.25,
            "program_menu": 5.25,
            "program_menu_capture": 6.25,
        },
    )

    assert retry_result.success is True
    assert retry_result.rollback_performed is False
    assert retry_result.stage_dir == stage_dir
    assert retry_runner.call_timeouts["capture_screen"] == [3.25, 4.25, 6.25]
    assert retry_runner.call_timeouts["send_sequence"][-1] == 5.25
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert [checkpoint.step for checkpoint in retry_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]


def test_run_artifice_jailbreak_leaves_clean_pre_jailbreak_state_after_two_consecutive_failures(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "double-failure-stage"
    sampled_payloads = _sample_module_payloads(package, count=8)
    first_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock shell-stage timeout"),
        load_app_app_count=40,
    )
    second_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock program-menu timeout"),
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        first_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "double-failure-first-evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 1.75,
            "rollback": 4.25,
        },
    )
    second_result = run_artifice_jailbreak(
        second_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "double-failure-second-evidence",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 1.25,
            "quit_capture": 1.5,
            "program_menu": 1.75,
            "program_menu_capture": 2.0,
            "rollback": 4.0,
        },
    )

    assert first_result.success is False
    assert first_result.rollback_performed is True
    assert second_result.success is False
    assert second_result.rollback_performed is True
    assert second_result.stage_dir == stage_dir
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload

    validation = validate_staged_transfer_directory(package, stage_dir)
    assert validation["success"] is True
    assert validation["missing_entries"] == []
    assert validation["mismatched_entries"] == []
    assert validation["unexpected_entries"] == []


def test_run_artifice_jailbreak_cascades_across_two_failures_then_succeeds_from_a_clean_retry(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "cascade-retry-stage"
    sampled_payloads = _sample_module_payloads(package, count=8)

    first_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock stage-1 timeout"),
        load_app_app_count=40,
    )
    second_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock stage-2 timeout"),
        load_app_app_count=40,
    )
    recovery_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        first_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "cascade-failure-1",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 1.5,
            "rollback": 4.5,
        },
    )

    first_validation = validate_staged_transfer_directory(package, stage_dir)
    assert first_result.success is False
    assert first_result.failure_reason == "mock stage-1 timeout"
    assert first_result.rollback_performed is True
    assert first_result.checkpoints == ()
    assert first_validation["success"] is True
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload

    second_result = run_artifice_jailbreak(
        second_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "cascade-failure-2",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 2.0,
            "quit_capture": 2.5,
            "program_menu": 3.0,
            "program_menu_capture": 4.0,
            "rollback": 5.0,
        },
    )

    second_validation = validate_staged_transfer_directory(package, stage_dir)
    assert second_result.success is False
    assert second_result.failure_reason == "mock stage-2 timeout"
    assert second_result.rollback_performed is True
    assert [checkpoint.step for checkpoint in second_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
    ]
    assert second_validation["success"] is True
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload

    recovery_result = run_artifice_jailbreak(
        recovery_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "cascade-recovery",
        timeout_s=5.0,
    )

    assert recovery_result.success is True
    assert recovery_result.rollback_performed is False
    assert recovery_result.stage_dir == stage_dir
    assert (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert [checkpoint.step for checkpoint in recovery_result.checkpoints] == [
        "artifice_shell_loaded",
        "artifice_quit_transition",
        "ti_os_program_menu_open",
    ]


def test_run_artifice_jailbreak_timeout_escalation_gives_up_after_tripled_budget_and_restores_clean_state(
    tmp_path: Path,
) -> None:
    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    stage_dir = tmp_path / "timeout-escalation-stage"
    sampled_payloads = _sample_module_payloads(package, count=6)

    first_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
        ],
        fail_on_capture_index=0,
        capture_failure=CEmuProcessTimeoutError("mock initial timeout"),
        load_app_app_count=40,
    )
    second_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        capture_failure_indices=[2],
        capture_failure=CEmuProcessTimeoutError("mock doubled-timeout failure"),
        load_app_app_count=40,
    )
    third_runner = MockCEmuRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        get_state_failure_indices=[2],
        load_app_app_count=40,
    )

    first_result = run_artifice_jailbreak(
        first_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "timeout-escalation-initial",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 1.5,
            "rollback": 4.5,
        },
    )
    second_result = run_artifice_jailbreak(
        second_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "timeout-escalation-doubled",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 3.0,
            "quit_capture": 3.5,
            "program_menu": 4.0,
            "program_menu_capture": 6.0,
            "rollback": 5.5,
        },
    )
    third_result = run_artifice_jailbreak(
        third_runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        stage_dir=stage_dir,
        evidence_dir=tmp_path / "timeout-escalation-tripled",
        timeout_s=5.0,
        step_timeouts={
            "shell_capture": 4.5,
            "quit_capture": 5.0,
            "program_menu": 5.5,
            "program_menu_capture": 6.0,
            "final_state": 6.5,
            "rollback": 7.0,
        },
    )

    validation = validate_staged_transfer_directory(package, stage_dir)

    assert first_result.success is False
    assert first_result.timed_out is True
    assert first_result.failure_reason == "mock initial timeout"
    assert first_runner.call_timeouts["capture_screen"] == [1.5]

    assert second_result.success is False
    assert second_result.timed_out is True
    assert second_result.failure_reason == "mock doubled-timeout failure"
    assert second_runner.call_timeouts["capture_screen"] == [3.0, 3.5, 6.0]

    assert third_result.success is False
    assert third_result.timed_out is True
    assert third_result.failure_reason == "mock get_state timeout on call 3"
    assert third_runner.call_timeouts["get_state"] == [5.0, 5.0, 6.5]

    assert first_result.rollback_performed is True
    assert second_result.rollback_performed is True
    assert third_result.rollback_performed is True
    assert validation["success"] is True
    assert not (stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp").exists()
    assert not any(path.name.endswith(".calcengine-stage") for path in stage_dir.iterdir())
    for module_name, expected_payload in sampled_payloads.items():
        assert (stage_dir / module_name).read_bytes() == expected_payload


def test_run_artifice_jailbreak_rejects_a_concurrent_attempt_for_the_same_runner(
    tmp_path: Path,
) -> None:
    class BlockingShellLaunchRunner(MockCEmuRunner):
        def __init__(self, *args, block_started: threading.Event, release_block: threading.Event, **kwargs) -> None:
            super().__init__(*args, **kwargs)
            self._block_started = block_started
            self._release_block = release_block

        def send_sequence(
            self,
            events: list[object] | tuple[object, ...],
            timeout_s: float | None = None,
        ) -> list[DummyCommandResult]:
            if len(self.sequences) == 0:
                self._block_started.set()
                released = self._release_block.wait(timeout=5.0)
                assert released is True
            return super().send_sequence(events, timeout_s=timeout_s)

    package = build_dummy_transfer_package(tmp_path / "transfer-src")
    artifice_path = tmp_path / "artifice-v2.8xp"
    artifice_path.write_bytes(b"ARTIFICE")
    block_started = threading.Event()
    release_block = threading.Event()
    runner = BlockingShellLaunchRunner(
        screen_responses=[
            {"crc": "EAC3FDAC", "text_rows": ["ARTIFICE SHELL", "", ""]},
            {"crc": "28B88B56", "text_rows": ["QUIT TRANSITION", "", ""]},
            {"crc": "C84EC5D0", "text_rows": ["PRGM MENU", "", ""]},
        ],
        load_app_app_count=40,
        block_started=block_started,
        release_block=release_block,
    )
    first_result_holder: dict[str, JailbreakResult] = {}

    def run_primary_attempt() -> None:
        first_result_holder["result"] = run_artifice_jailbreak(
            runner,  # type: ignore[arg-type]
            transfer_package=package,
            artifice_path=artifice_path,
            evidence_dir=tmp_path / "concurrent-primary",
            timeout_s=5.0,
        )

    primary_thread = threading.Thread(target=run_primary_attempt)
    primary_thread.start()
    assert block_started.wait(timeout=5.0) is True

    rejected_result = run_artifice_jailbreak(
        runner,  # type: ignore[arg-type]
        transfer_package=package,
        artifice_path=artifice_path,
        evidence_dir=tmp_path / "concurrent-secondary",
        timeout_s=5.0,
    )

    release_block.set()
    primary_thread.join(timeout=5.0)
    assert primary_thread.is_alive() is False

    primary_result = first_result_holder["result"]

    assert rejected_result.success is False
    assert rejected_result.failure_reason == "Another arTIfiCE jailbreak attempt is already in progress for this runner."
    assert rejected_result.stage_dir is None
    assert rejected_result.checkpoints == ()
    assert any("already active for this runner" in message for message in rejected_result.diagnostics)

    assert primary_result.success is True
    assert primary_result.rollback_performed is False


@pytest.mark.skipif(not REAL_ARTIFICE_READY, reason=REAL_ARTIFICE_SKIP_REASON)
def test_run_artifice_jailbreak_real_backend(
    tmp_path: Path,
    real_cemu_config,
) -> None:
    assert real_cemu_config is not None
    assert REAL_CEMU_ENVIRONMENT.transfer_package is not None
    assert REAL_ARTIFICE_PATH is not None

    with CEmuRunner(build_runner_config(REAL_CEMU_ENVIRONMENT)) as runner:
        result = run_artifice_jailbreak(
            runner,
            transfer_package=REAL_CEMU_ENVIRONMENT.transfer_package,
            artifice_path=REAL_ARTIFICE_PATH,
            rom_validation=REAL_CEMU_ENVIRONMENT.rom_validation,
            evidence_dir=tmp_path / "artifice-real",
            timeout_s=60.0,
        )

    assert result.success is True
    assert result.checkpoints[0].observed_crc == "7450762E"
    assert result.checkpoints[1].observed_crc == "AA943AC0"
    assert result.checkpoints[2].observed_crc == "FD9D992C"
