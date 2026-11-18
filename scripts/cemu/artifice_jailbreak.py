from __future__ import annotations

import hashlib
import inspect
import json
import os
import random
import re
import shutil
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from cemu_runner import CEmuProcessTimeoutError, CEmuRunner, CEmuRunnerError
from keypress_sequences import KeyAction, pause, press
from pipeline_support import (
    REPO_ROOT,
    TransferManifestEntry,
    TransferPackage,
    TransferPackageResolutionError,
    resolve_transfer_package,
    scale_cemu_timeout_seconds,
    stage_transfer_package,
    validate_staged_transfer_directory,
)
from rom_validation import RomValidationResult


DEFAULT_ARTIFICE_PATH = REPO_ROOT / "tools" / "emulator" / "assets" / "artifice-v2.8xp"
DEFAULT_ARTIFICE_BASELINE_PATH = REPO_ROOT / "tools" / "emulator" / "baselines" / "artifice-e2e-baseline.json"
DEFAULT_ARTIFICE_STAGE_PREFIX = "calcengine-artifice-py-"
DEFAULT_ARTIFICE_PROGRAM_NAME = "A"
DEFAULT_ARTIFICE_SHELL_SETTLE_DELAY_MS = 4_000
DEFAULT_ARTIFICE_EXIT_DELAY_MS = 1_200
DEFAULT_TIOS_PRGM_DELAY_MS = 1_200
DEFAULT_ARTIFICE_USB_RETRY_COUNT = 3
DEFAULT_ARTIFICE_USB_RETRY_DELAY_MS = 2_000
DEFAULT_ARTIFICE_USB_RETRY_MAX_DELAY_MS = 30_000
DEFAULT_ARTIFICE_USB_RETRY_JITTER_RATIO = 0.2
DEFAULT_ARTIFICE_CHECKPOINT_VERSION = 2
DEFAULT_ARTIFICE_CHECKPOINT_LOCK_TIMEOUT_S = 5.0
DEFAULT_ARTIFICE_CHECKPOINT_LOCK_POLL_INTERVAL_S = 0.05
DEFAULT_ARTIFICE_STEP_TIMEOUTS: dict[str, float] = {
    "runner_ready": 10.0,
    "usb_connect": 30.0,
    "shell_launch": 16.0,
    "shell_capture": 12.0,
    "shell_exit": 14.0,
    "quit_capture": 12.0,
    "program_menu": 14.0,
    "program_menu_capture": 12.0,
    "final_state": 8.0,
    "rollback": 8.0,
}
SUPPORTED_ARTIFICE_FIRMWARE_FAMILIES: tuple[str, ...] = ("5.8.x",)
SUPPORTED_ARTIFICE_OS_PREFIXES: tuple[str, ...] = ("5.8.",)
SUPPORTED_ARTIFICE_OS_VERSION_PATTERN = re.compile(r"(?<!\d)(5\.8(?:\.\d+){0,2})(?!\d)")
SUPPORTED_ARTIFICE_FIRMWARE_PATTERN = re.compile(r"(?<!\d)(5\.8(?:\.x|\.\d+)?)(?!\d)", re.IGNORECASE)

DEFAULT_ARTIFICE_HASHES: dict[str, tuple[str, ...]] = {
    "artifice_shell_loaded": ("EAC3FDAC",),
    "artifice_quit_transition": ("28B88B56",),
    "ti_os_program_menu_open": ("C84EC5D0", "967993A8"),
    "calcengine_launch_menu": ("28B88B56",),
    "calcengine_result_2plus3": ("ED152D3E",),
    "calcengine_menu_open": ("D12593CE",),
    "calcengine_menu_closed": ("828489BA",),
}
NATIVE_CORE_ARTIFICE_HASH_OVERRIDES: dict[str, tuple[str, ...]] = {
    "artifice_shell_loaded": ("7450762E",),
    "artifice_quit_transition": ("AA943AC0",),
    "ti_os_program_menu_open": ("FD9D992C",),
}

_ACTIVE_JAILBREAK_LOCK = threading.Lock()
_ACTIVE_JAILBREAK_RUNNERS: set[int] = set()

ARTIFICE_SEQUENCE_DOCUMENTATION: tuple[str, ...] = (
    "Wrapper launch target: load the staged transfer directory with target_name='A' and target_is_asm=False. "
    "This is the automation equivalent of selecting prgmA from the TI-OS PRGM menu.",
    "Wait 4000ms for arTIfiCE v2 to render its shell screen.",
    "Press CLEAR, then wait 1200ms to begin the verified shell-exit choreography.",
    "Press MODE, then wait 1200ms to leave the shell and land in the measured quit-transition state.",
    "Press PRGM, then wait 1200ms to reopen the TI-OS PRGM menu before handing control to the CalcEngine launcher.",
)
NATIVE_ARTIFICE_HOME_LAUNCH_DOCUMENTATION: tuple[str, ...] = (
    "native-core launch prelude: from the TI-OS home screen, press PRGM, press ENTER to insert prgmA from the TI-OS PRGM menu, then press ENTER again to execute it before waiting for the arTIfiCE shell.",
)

# TI-84 Plus CE ALPHA layer exactly as printed on the keycaps (row order:
# math..÷ = A..M, log..2 = N..Z). Must match src/core/launcher/main.c.
ALPHA_KEY_MAP: dict[str, str] = {
    "A": "math",
    "B": "apps",
    "C": "prgm",
    "D": "-1",
    "E": "sin",
    "F": "cos",
    "G": "tan",
    "H": "^",
    "I": "^2",
    "J": ",",
    "K": "(",
    "L": ")",
    "M": "/",
    "N": "log",
    "O": "7",
    "P": "8",
    "Q": "9",
    "R": "*",
    "S": "ln",
    "T": "4",
    "U": "5",
    "V": "6",
    "W": "-",
    "X": "sto",
    "Y": "1",
    "Z": "2",
}


@dataclass(frozen=True)
class ArtificeCheckpointDefinition:
    step: str
    description: str
    expected_crcs: tuple[str, ...]


@dataclass(frozen=True)
class ArtificeCheckpointResult:
    step: str
    description: str
    expected_crcs: tuple[str, ...]
    observed_crc: str
    passed: bool
    snapshot_path: Path | None
    screen_payload: Mapping[str, object]


@dataclass(frozen=True)
class JailbreakIndicators:
    privilege_escalated: bool
    port_write_access: bool
    verified_markers: tuple[str, ...]
    diagnostics: tuple[str, ...]


@dataclass(frozen=True)
class JailbreakResult:
    success: bool
    failure_reason: str
    timed_out: bool
    artifice_path: Path | None
    baseline_path: Path | None
    stage_dir: Path | None
    evidence_dir: Path | None
    module_count: int
    runtime_dependency_count: int
    transfer_count: int
    staged_file_count: int
    loaded_archive_count: int
    checkpoints: tuple[ArtificeCheckpointResult, ...]
    indicators: JailbreakIndicators
    diagnostics: tuple[str, ...]
    sequence_documentation: tuple[str, ...]
    keypress_log: tuple[str, ...]
    total_duration_ms: int
    bypass_active: bool = False
    rollback_performed: bool = False
    usb_retry_count: int = 0
    cleanup_actions: tuple[str, ...] = ()


@dataclass(frozen=True)
class ArtificeRomCompatibility:
    supported: bool
    blockers: tuple[str, ...]
    diagnostics: tuple[str, ...]


class ArtificeUsbRetryExhaustedError(RuntimeError):
    def __init__(self, message: str, *, attempts: int) -> None:
        super().__init__(message)
        self.attempts = attempts


class ArtificeCheckpointError(RuntimeError):
    """Raised when a persisted arTIfiCE checkpoint cannot be read or written cleanly."""


@dataclass(frozen=True)
class ArtificeCheckpointState:
    version: int
    status: str
    stage_dir: Path
    evidence_dir: Path
    artifice_path: Path
    transfer_count: int
    module_count: int
    runtime_dependency_count: int
    last_completed_step: str
    checkpoints: tuple[ArtificeCheckpointResult, ...]
    created_at_epoch_ms: int
    updated_at_epoch_ms: int
    integrity_digest: str

    @property
    def completed(self) -> bool:
        return self.status == "completed"


def _begin_jailbreak_session(runner: CEmuRunner) -> bool:
    runner_key = id(runner)
    with _ACTIVE_JAILBREAK_LOCK:
        if runner_key in _ACTIVE_JAILBREAK_RUNNERS:
            return False
        _ACTIVE_JAILBREAK_RUNNERS.add(runner_key)
    return True


def _finish_jailbreak_session(runner: CEmuRunner) -> None:
    with _ACTIVE_JAILBREAK_LOCK:
        _ACTIVE_JAILBREAK_RUNNERS.discard(id(runner))


def normalize_crc(value: object) -> str:
    return str(value).strip().upper()


def _checkpoint_lock_path(checkpoint_path: Path) -> Path:
    return checkpoint_path.with_name(f"{checkpoint_path.name}.lock")


def _checkpoint_result_to_payload(checkpoint: ArtificeCheckpointResult) -> dict[str, object]:
    return {
        "step": checkpoint.step,
        "description": checkpoint.description,
        "expected_crcs": list(checkpoint.expected_crcs),
        "observed_crc": checkpoint.observed_crc,
        "passed": checkpoint.passed,
        "snapshot_path": str(checkpoint.snapshot_path) if checkpoint.snapshot_path is not None else "",
        "screen_payload": dict(checkpoint.screen_payload),
    }


def _checkpoint_result_from_payload(payload: Mapping[str, object]) -> ArtificeCheckpointResult:
    snapshot_text = str(payload.get("snapshot_path", "")).strip()
    raw_screen_payload = payload.get("screen_payload", {})
    return ArtificeCheckpointResult(
        step=str(payload.get("step", "")).strip(),
        description=str(payload.get("description", "")).strip(),
        expected_crcs=tuple(normalize_crc(value) for value in payload.get("expected_crcs", ())),
        observed_crc=normalize_crc(payload.get("observed_crc", "")),
        passed=bool(payload.get("passed", False)),
        snapshot_path=Path(snapshot_text) if snapshot_text else None,
        screen_payload=dict(raw_screen_payload) if isinstance(raw_screen_payload, Mapping) else {},
    )


def _checkpoint_digest(payload: Mapping[str, object]) -> str:
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest().upper()


def _checkpoint_timestamp_epoch_ms() -> int:
    return int(round(time.time() * 1000))


def _checkpoint_core_payload(
    *,
    version: int,
    status: str,
    stage_dir: Path,
    evidence_dir: Path,
    artifice_path: Path,
    transfer_count: int,
    module_count: int,
    runtime_dependency_count: int,
    last_completed_step: str,
    checkpoints: Sequence[ArtificeCheckpointResult],
    created_at_epoch_ms: int,
    updated_at_epoch_ms: int,
) -> dict[str, object]:
    return {
        "version": version,
        "status": status,
        "stage_dir": str(stage_dir),
        "evidence_dir": str(evidence_dir),
        "artifice_path": str(artifice_path),
        "transfer_count": transfer_count,
        "module_count": module_count,
        "runtime_dependency_count": runtime_dependency_count,
        "last_completed_step": last_completed_step,
        "checkpoints": [_checkpoint_result_to_payload(checkpoint) for checkpoint in checkpoints],
        "created_at_epoch_ms": int(created_at_epoch_ms),
        "updated_at_epoch_ms": int(updated_at_epoch_ms),
    }


def _build_checkpoint_state(payload: Mapping[str, object], *, integrity_digest: str) -> ArtificeCheckpointState:
    checkpoints_payload = payload.get("checkpoints", ())
    if not isinstance(checkpoints_payload, Sequence):
        raise ArtificeCheckpointError("Checkpoint payload 'checkpoints' must be a sequence.")

    return ArtificeCheckpointState(
        version=int(payload.get("version", 0)),
        status=str(payload.get("status", "")).strip(),
        stage_dir=Path(str(payload.get("stage_dir", ""))),
        evidence_dir=Path(str(payload.get("evidence_dir", ""))),
        artifice_path=Path(str(payload.get("artifice_path", ""))),
        transfer_count=int(payload.get("transfer_count", 0)),
        module_count=int(payload.get("module_count", 0)),
        runtime_dependency_count=int(payload.get("runtime_dependency_count", 0)),
        last_completed_step=str(payload.get("last_completed_step", "")).strip(),
        checkpoints=tuple(
            _checkpoint_result_from_payload(row)
            for row in checkpoints_payload
            if isinstance(row, Mapping)
        ),
        created_at_epoch_ms=int(payload.get("created_at_epoch_ms", 0)),
        updated_at_epoch_ms=int(payload.get("updated_at_epoch_ms", 0)),
        integrity_digest=integrity_digest,
    )


def _migrate_checkpoint_payload(raw_payload: Mapping[str, object]) -> dict[str, object]:
    version = int(raw_payload.get("version", 0))
    if version == DEFAULT_ARTIFICE_CHECKPOINT_VERSION:
        return {
            key: value
            for key, value in raw_payload.items()
            if key != "integrity"
        }
    if version == 1:
        migrated = {
            "version": DEFAULT_ARTIFICE_CHECKPOINT_VERSION,
            "status": str(raw_payload.get("status", "in_progress")).strip() or "in_progress",
            "stage_dir": str(raw_payload.get("stage_dir", "")),
            "evidence_dir": str(raw_payload.get("evidence_dir", "")),
            "artifice_path": str(raw_payload.get("artifice_path", "")),
            "transfer_count": int(raw_payload.get("transfer_count", 0)),
            "module_count": int(raw_payload.get("module_count", 0)),
            "runtime_dependency_count": int(raw_payload.get("runtime_dependency_count", 0)),
            "last_completed_step": str(raw_payload.get("last_completed_step", "")).strip(),
            "checkpoints": list(raw_payload.get("checkpoints", ())),
        }
        return migrated
    raise ArtificeCheckpointError(
        f"Unsupported arTIfiCE checkpoint version {version}; expected 1 or {DEFAULT_ARTIFICE_CHECKPOINT_VERSION}."
    )


def _load_checkpoint_state(checkpoint_path: Path) -> ArtificeCheckpointState | None:
    if not checkpoint_path.exists():
        return None

    try:
        raw_payload = json.loads(checkpoint_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ArtificeCheckpointError(f"Checkpoint file '{checkpoint_path}' is unreadable or invalid JSON: {exc}") from exc
    if not isinstance(raw_payload, Mapping):
        raise ArtificeCheckpointError(f"Checkpoint file '{checkpoint_path}' must contain a JSON object.")

    integrity_payload = raw_payload.get("integrity")
    if isinstance(integrity_payload, Mapping):
        provided_digest = str(integrity_payload.get("digest", "")).strip().upper()
    else:
        provided_digest = ""

    migrated_payload = _migrate_checkpoint_payload(raw_payload)
    expected_digest = _checkpoint_digest(migrated_payload)
    if int(raw_payload.get("version", 0)) == DEFAULT_ARTIFICE_CHECKPOINT_VERSION and provided_digest != expected_digest:
        raise ArtificeCheckpointError(
            f"Checkpoint file '{checkpoint_path}' failed integrity validation; expected digest {expected_digest}, observed {provided_digest or '<empty>'}."
        )
    return _build_checkpoint_state(migrated_payload, integrity_digest=expected_digest)


def _write_checkpoint_state(
    checkpoint_path: Path,
    *,
    status: str,
    stage_dir: Path,
    evidence_dir: Path,
    artifice_path: Path,
    transfer_count: int,
    module_count: int,
    runtime_dependency_count: int,
    last_completed_step: str,
    checkpoints: Sequence[ArtificeCheckpointResult],
    created_at_epoch_ms: int | None = None,
) -> ArtificeCheckpointState:
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    timestamp_epoch_ms = _checkpoint_timestamp_epoch_ms()
    resolved_created_at_epoch_ms = (
        timestamp_epoch_ms
        if created_at_epoch_ms is None or int(created_at_epoch_ms) <= 0
        else int(created_at_epoch_ms)
    )
    core_payload = _checkpoint_core_payload(
        version=DEFAULT_ARTIFICE_CHECKPOINT_VERSION,
        status=status,
        stage_dir=stage_dir,
        evidence_dir=evidence_dir,
        artifice_path=artifice_path,
        transfer_count=transfer_count,
        module_count=module_count,
        runtime_dependency_count=runtime_dependency_count,
        last_completed_step=last_completed_step,
        checkpoints=checkpoints,
        created_at_epoch_ms=resolved_created_at_epoch_ms,
        updated_at_epoch_ms=timestamp_epoch_ms,
    )
    payload = {
        **core_payload,
        "integrity": {
            "algorithm": "sha256",
            "digest": _checkpoint_digest(core_payload),
        },
    }
    temp_path = checkpoint_path.with_name(f"{checkpoint_path.name}.tmp")
    try:
        temp_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        temp_path.replace(checkpoint_path)
    except OSError as exc:
        if temp_path.exists():
            temp_path.unlink()
        raise ArtificeCheckpointError(f"Checkpoint file '{checkpoint_path}' could not be written cleanly: {exc}") from exc
    return _build_checkpoint_state(core_payload, integrity_digest=str(payload["integrity"]["digest"]))


def _delete_checkpoint_file(checkpoint_path: Path | None) -> None:
    if checkpoint_path is None or not checkpoint_path.exists():
        return
    checkpoint_path.unlink()


def _acquire_checkpoint_lock(
    checkpoint_path: Path,
    *,
    timeout_s: float,
    poll_interval_s: float = DEFAULT_ARTIFICE_CHECKPOINT_LOCK_POLL_INTERVAL_S,
) -> int:
    lock_path = _checkpoint_lock_path(checkpoint_path)
    deadline = time.perf_counter() + max(0.0, timeout_s)
    while True:
        try:
            lock_fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(lock_fd, str(os.getpid()).encode("ascii", errors="ignore"))
            return lock_fd
        except FileExistsError as exc:
            if time.perf_counter() >= deadline:
                raise ArtificeCheckpointError(
                    f"Checkpoint file '{checkpoint_path}' is already locked by another process."
                ) from exc
            time.sleep(max(0.0, poll_interval_s))


def _release_checkpoint_lock(checkpoint_path: Path, lock_fd: int | None) -> None:
    if lock_fd is not None:
        os.close(lock_fd)
    lock_path = _checkpoint_lock_path(checkpoint_path)
    if lock_path.exists():
        lock_path.unlink()


def _normalize_artifice_os_version(value: str) -> str:
    normalized = value.strip()
    if not normalized:
        return ""
    match = SUPPORTED_ARTIFICE_OS_VERSION_PATTERN.search(normalized)
    return match.group(1) if match is not None else normalized


def _normalize_artifice_firmware_family(value: str, *, fallback_version: str = "") -> str:
    normalized = value.strip()
    if not normalized and fallback_version:
        parts = fallback_version.split(".")
        if len(parts) >= 2:
            return f"{parts[0]}.{parts[1]}.x"
    if not normalized:
        return ""

    match = SUPPORTED_ARTIFICE_FIRMWARE_PATTERN.search(normalized)
    if match is None:
        return normalized

    matched = match.group(1)
    if matched.lower().endswith(".x"):
        parts = matched.split(".")
        if len(parts) >= 2:
            return f"{parts[0]}.{parts[1]}.x"
        return matched

    parts = matched.split(".")
    if len(parts) >= 2:
        return f"{parts[0]}.{parts[1]}.x"
    return matched


def evaluate_artifice_rom_compatibility(
    rom_validation: RomValidationResult | None,
) -> ArtificeRomCompatibility:
    if rom_validation is None:
        return ArtificeRomCompatibility(
            supported=True,
            blockers=(),
            diagnostics=(
                "ROM compatibility was not explicitly validated for the arTIfiCE lane.",
            ),
        )

    diagnostics = [
        "arTIfiCE lane ROM validation: "
        f"model={rom_validation.model or '<unknown>'}, "
        f"os_version={rom_validation.os_version or '<unknown>'}, "
        f"firmware_family={rom_validation.firmware_family or '<unknown>'}.",
    ]
    blockers: list[str] = []
    normalized_os_version = _normalize_artifice_os_version(rom_validation.os_version or "")
    normalized_firmware_family = _normalize_artifice_firmware_family(
        rom_validation.firmware_family or "",
        fallback_version=normalized_os_version,
    )

    if normalized_os_version and normalized_os_version != (rom_validation.os_version or "").strip():
        diagnostics.append(
            f"Normalized ROM os_version metadata to '{normalized_os_version}' for arTIfiCE compatibility checks."
        )
    if normalized_firmware_family and normalized_firmware_family != (rom_validation.firmware_family or "").strip():
        diagnostics.append(
            "Normalized ROM firmware_family metadata to "
            f"'{normalized_firmware_family}' for arTIfiCE compatibility checks."
        )

    if not rom_validation.integrity_ok:
        blockers.append(
            "ROM integrity validation failed: " + "; ".join(rom_validation.errors)
        )

    compatible_os = any(
        normalized_os_version.startswith(prefix)
        for prefix in SUPPORTED_ARTIFICE_OS_PREFIXES
        if normalized_os_version
    )
    compatible_family = normalized_firmware_family in SUPPORTED_ARTIFICE_FIRMWARE_FAMILIES
    if normalized_os_version or normalized_firmware_family:
        if not compatible_os and not compatible_family:
            blockers.append(
                "arTIfiCE lane requires a TI-84 Plus CE 5.8.x ROM, "
                f"but the validated ROM reports os_version='{rom_validation.os_version or '<unknown>'}' "
                f"and firmware_family='{rom_validation.firmware_family or '<unknown>'}' "
                f"(normalized to os_version='{normalized_os_version or '<unknown>'}', "
                f"firmware_family='{normalized_firmware_family or '<unknown>'}')."
            )
    else:
        blockers.append(
            "arTIfiCE lane requires ROM version metadata so it can reject unsupported OS revisions."
        )

    if blockers:
        diagnostics.append(
            "The locked arTIfiCE CRC baselines are only supported on TI-84 Plus CE OS 5.8.x."
        )
    elif compatible_os or compatible_family:
        diagnostics.append(
            "Validated ROM is compatible with the locked TI-84 Plus CE OS 5.8.x arTIfiCE baselines."
        )

    return ArtificeRomCompatibility(
        supported=not blockers,
        blockers=tuple(blockers),
        diagnostics=tuple(diagnostics),
    )


def resolve_artifice_path(
    candidate: Path | None = None,
    *,
    repo_root: Path = REPO_ROOT,
    search_roots: Sequence[Path] = (),
) -> Path | None:
    ordered: list[Path] = []
    seen: set[str] = set()

    def add(path: Path | None) -> None:
        if path is None:
            return
        expanded = path.expanduser()
        key = str(expanded).lower()
        if key in seen:
            return
        seen.add(key)
        ordered.append(expanded)

    env_candidate = Path.cwd()
    explicit_env = Path(env_value).expanduser() if (env_value := os.environ.get("CALCENGINE_ARTIFICE_PATH")) else None

    add(candidate)
    add(explicit_env)
    add(DEFAULT_ARTIFICE_PATH)
    for root in search_roots:
        add(root)
    add(repo_root / "tools" / "emulator" / "assets")
    add(env_candidate / "tools" / "emulator" / "assets")

    for root in ordered:
        if root.is_file() and root.suffix.lower() == ".8xp":
            return root.resolve()
        if not root.is_dir():
            continue
        for file_path in sorted(root.glob("*.8xp"), key=lambda path: path.name.lower()):
            if "artifice" in file_path.name.lower():
                return file_path.resolve()
    return None


def load_artifice_expected_hashes(
    baseline_path: Path = DEFAULT_ARTIFICE_BASELINE_PATH,
) -> dict[str, tuple[str, ...]]:
    expected = dict(DEFAULT_ARTIFICE_HASHES)
    if not baseline_path.exists():
        return expected

    payload = json.loads(baseline_path.read_text(encoding="utf-8"))
    screens = payload.get("screens")
    if not isinstance(screens, list):
        return expected

    for screen in screens:
        if not isinstance(screen, Mapping):
            continue
        step = str(screen.get("step", "")).strip()
        screen_hash = normalize_crc(screen.get("hash", ""))
        if step and screen_hash:
            expected[step] = (screen_hash,)
    expected["ti_os_program_menu_open"] = tuple(
        dict.fromkeys((*expected.get("ti_os_program_menu_open", ()), "967993A8"))
    )
    return expected


def get_artifice_checkpoint_definitions(
    expected_hashes: Mapping[str, Sequence[str]] | None = None,
) -> tuple[ArtificeCheckpointDefinition, ...]:
    hashes = load_artifice_expected_hashes()
    if expected_hashes is not None:
        for step, values in expected_hashes.items():
            hashes[step] = tuple(normalize_crc(value) for value in values)

    return (
        ArtificeCheckpointDefinition(
            step="artifice_shell_loaded",
            description="arTIfiCE v2 shell screen immediately after launching prgmA",
            expected_crcs=tuple(hashes["artifice_shell_loaded"]),
        ),
        ArtificeCheckpointDefinition(
            step="artifice_quit_transition",
            description="Screen state after the verified CLEAR -> MODE shell-exit choreography, before reopening PRGM",
            expected_crcs=tuple(hashes["artifice_quit_transition"]),
        ),
        ArtificeCheckpointDefinition(
            step="ti_os_program_menu_open",
            description="TI-OS program menu after the verified CLEAR -> MODE -> PRGM arTIfiCE exit sequence",
            expected_crcs=tuple(hashes["ti_os_program_menu_open"]),
        ),
    )


def build_artifice_shell_launch_sequence(
    settle_delay_ms: int = DEFAULT_ARTIFICE_SHELL_SETTLE_DELAY_MS,
) -> tuple[KeyAction, ...]:
    return (
        pause(
            settle_delay_ms,
            note="wait for the wrapper-targeted prgmA launch to render the arTIfiCE shell",
        ),
    )


def build_native_artifice_home_launch_sequence(
    *,
    menu_delay_ms: int = DEFAULT_TIOS_PRGM_DELAY_MS,
    command_insert_delay_ms: int = 500,
    settle_delay_ms: int = DEFAULT_ARTIFICE_SHELL_SETTLE_DELAY_MS,
) -> tuple[KeyAction, ...]:
    return (
        press("prgm", note="open the TI-OS PRGM menu from the home screen in native-core mode"),
        pause(menu_delay_ms, note="wait for the TI-OS PRGM menu to render the native-core target list"),
        press("enter", note="insert prgmA from the TI-OS PRGM menu into the home screen command line"),
        pause(command_insert_delay_ms, note="let the TI-OS home screen finish inserting prgmA"),
        press("enter", note="execute prgmA so arTIfiCE opens before shell capture"),
        pause(settle_delay_ms, note="wait for the native-core prgmA execution to render the arTIfiCE shell"),
    )


def build_artifice_shell_exit_sequence(
    settle_delay_ms: int = DEFAULT_ARTIFICE_EXIT_DELAY_MS,
) -> tuple[KeyAction, ...]:
    return (
        press("clear", note="start the verified shell-exit choreography"),
        pause(settle_delay_ms, note="allow the CLEAR exit leg to settle"),
        press("mode", note="complete the shell exit and return toward TI-OS"),
        pause(settle_delay_ms, note="allow the MODE exit leg to settle"),
    )


def build_tios_program_menu_sequence(
    settle_delay_ms: int = DEFAULT_TIOS_PRGM_DELAY_MS,
) -> tuple[KeyAction, ...]:
    return (
        press("prgm", note="reopen the TI-OS PRGM menu after leaving arTIfiCE"),
        pause(settle_delay_ms, note="wait for the TI-OS PRGM menu to render"),
    )


def build_alpha_program_launch_sequence(
    program_name: str,
    *,
    key_delay_ms: int = 250,
) -> list[KeyAction]:
    sequence: list[KeyAction] = []
    for letter in program_name.upper():
        mapped_key = ALPHA_KEY_MAP.get(letter)
        if mapped_key is None:
            raise ValueError(
                f"Program name '{program_name}' contains unsupported character '{letter}'."
            )
        sequence.append(press("alpha", note=f"alpha prefix for '{letter}'"))
        sequence.append(pause(key_delay_ms, note="settle after alpha"))
        sequence.append(press(mapped_key, note=f"alpha-mapped key for '{letter}'"))
        sequence.append(pause(key_delay_ms, note="settle after mapped key"))
    return sequence


def stage_artifice_transfer_package(
    package: TransferPackage,
    artifice_path: Path,
    destination_dir: Path,
) -> Path:
    package.require_complete()
    destination_name = f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp"
    if destination_name.lower() in {entry.name.lower() for entry in package.module_entries}:
        raise TransferPackageResolutionError(
            f"Manifest-backed payload already contains '{destination_name}', so the arTIfiCE wrapper target would collide."
        )
    if artifice_path.suffix.lower() != ".8xp" or not artifice_path.is_file():
        raise TransferPackageResolutionError(
            f"arTIfiCE asset '{artifice_path}' is not a readable .8xp program."
        )
    if artifice_path.stat().st_size <= 0:
        raise TransferPackageResolutionError(
            f"arTIfiCE asset '{artifice_path}' is empty."
        )
    staged_dir = stage_transfer_package(package, destination_dir)
    destination_path = staged_dir / destination_name
    staging_path = staged_dir / f".{destination_name}.calcengine-stage"
    if staging_path.exists():
        staging_path.unlink()
    try:
        shutil.copy2(artifice_path, staging_path)
    except OSError as exc:
        if staging_path.exists():
            staging_path.unlink()
        raise TransferPackageResolutionError(
            f"Staged artifact '{destination_name}' could not be copied cleanly: {exc}"
        ) from exc
    if not staging_path.is_file() or staging_path.stat().st_size != artifice_path.stat().st_size:
        if staging_path.exists():
            staging_path.unlink()
        raise TransferPackageResolutionError(
            f"Staged artifact '{destination_name}' failed size verification immediately after copy."
        )
    try:
        staging_path.replace(destination_path)
    except OSError as exc:
        if staging_path.exists():
            staging_path.unlink()
        raise TransferPackageResolutionError(
            f"Staged artifact '{destination_name}' could not be finalized cleanly: {exc}"
        ) from exc
    return staged_dir


def _result_payload(command: str, payload: Mapping[str, Any]) -> Any:
    return type(
        "CommandResult",
        (),
        {"command": command, "payload": dict(payload), "raw": dict(payload)},
    )()


def _load_program_with_target(
    runner: CEmuRunner,
    program_path: Path,
    *,
    target_name: str,
    target_is_asm: bool,
    timeout_s: float | None = None,
) -> Any:
    targeted_loader = inspect.getattr_static(runner, "load_program_with_target", None)
    if callable(targeted_loader):
        return getattr(runner, "load_program_with_target")(
            program_path,
            target_name=target_name,
            target_is_asm=target_is_asm,
            timeout_s=timeout_s,
        )

    sender = getattr(runner, "_send_command", None)
    if callable(sender):
        return sender(
            {
                "cmd": "load_app",
                "path": str(program_path),
                "target_name": target_name,
                "target_is_asm": target_is_asm,
            },
            timeout_s=timeout_s,
        )

    if target_is_asm:
        return runner.load_program(program_path, timeout_s=timeout_s)

    payload = {
        "ok": True,
        "cmd": "load_app",
        "path": str(program_path),
        "target_name": target_name,
        "target_is_asm": target_is_asm,
    }
    return _result_payload("load_app", payload)


def _ensure_runner_ready(runner: CEmuRunner, timeout_s: float | None = None) -> None:
    if not runner.is_running:
        runner.start()

    state = runner.get_state(timeout_s=timeout_s)
    if not bool(state.payload.get("rom_loaded")):
        runner.load_rom(timeout_s=timeout_s)


def _runner_uses_native_core(state_payload: Mapping[str, object]) -> bool:
    backend = str(state_payload.get("backend", "")).strip().lower()
    if backend == "native_core":
        return True
    return bool(state_payload.get("native_mode", False))


def _capture_checkpoint(
    runner: CEmuRunner,
    definition: ArtificeCheckpointDefinition,
    *,
    evidence_dir: Path | None,
    timeout_s: float | None = None,
) -> ArtificeCheckpointResult:
    screen = runner.capture_screen(timeout_s=timeout_s)
    observed_crc = normalize_crc(screen.payload.get("crc", ""))
    snapshot_path: Path | None = None

    if evidence_dir is not None:
        evidence_dir.mkdir(parents=True, exist_ok=True)
        snapshot_path = evidence_dir / f"{definition.step}.state.json"
        runner.snapshot(snapshot_path, timeout_s=timeout_s)

    return ArtificeCheckpointResult(
        step=definition.step,
        description=definition.description,
        expected_crcs=tuple(normalize_crc(value) for value in definition.expected_crcs),
        observed_crc=observed_crc,
        passed=observed_crc in {normalize_crc(value) for value in definition.expected_crcs},
        snapshot_path=snapshot_path,
        screen_payload=dict(screen.payload),
    )


def _format_sequence(sequence: Sequence[KeyAction]) -> tuple[str, ...]:
    formatted: list[str] = []
    for action in sequence:
        if action.kind == "pause":
            formatted.append(f"pause {action.duration_ms}ms | {action.note}".strip())
            continue
        formatted.append(f"key {action.key} | {action.note}".strip())
    return tuple(formatted)


def _count_staged_files(stage_dir: Path) -> int:
    if not stage_dir.exists():
        return 0
    return sum(1 for path in stage_dir.iterdir() if path.is_file())


def _staged_artifice_payload_is_valid(stage_dir: Path, artifice_path: Path) -> bool:
    staged_wrapper = stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp"
    return (
        staged_wrapper.is_file()
        and staged_wrapper.stat().st_size == artifice_path.stat().st_size
    )


def _checkpoint_can_resume(
    checkpoint: ArtificeCheckpointState,
    *,
    stage_dir: Path,
    evidence_dir: Path,
    artifice_path: Path,
    package: TransferPackage,
    checkpoint_max_age_s: float | None = None,
) -> tuple[bool, str]:
    if checkpoint.stage_dir.resolve() != stage_dir.resolve():
        return False, "checkpoint stage_dir does not match the requested stage_dir"
    if checkpoint.evidence_dir.resolve() != evidence_dir.resolve():
        return False, "checkpoint evidence_dir does not match the requested evidence_dir"
    if checkpoint.artifice_path.resolve() != artifice_path.resolve():
        return False, "checkpoint artifice_path does not match the requested arTIfiCE asset"
    if checkpoint.transfer_count != package.transfer_count:
        return False, "checkpoint transfer_count does not match the resolved transfer package"
    if checkpoint.module_count != package.module_count:
        return False, "checkpoint module_count does not match the resolved transfer package"
    if checkpoint.runtime_dependency_count != package.runtime_dependency_count:
        return False, "checkpoint runtime_dependency_count does not match the resolved transfer package"
    if checkpoint_max_age_s is not None:
        if checkpoint.updated_at_epoch_ms <= 0:
            return False, "checkpoint freshness metadata is missing for expiry validation"
        max_age_epoch_ms = int(max(0.0, checkpoint_max_age_s) * 1000)
        checkpoint_age_epoch_ms = max(
            0,
            _checkpoint_timestamp_epoch_ms() - checkpoint.updated_at_epoch_ms,
        )
        if checkpoint_age_epoch_ms > max_age_epoch_ms:
            return False, (
                f"checkpoint age {checkpoint_age_epoch_ms}ms exceeds the permitted max age of "
                f"{max_age_epoch_ms}ms"
            )
    if not _staged_artifice_payload_is_valid(stage_dir, artifice_path):
        return False, "staged arTIfiCE wrapper payload is missing or size-mismatched"
    validation = validate_staged_transfer_directory(
        package,
        stage_dir,
        allowed_extra_names=(f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp",),
    )
    if not bool(validation["success"]):
        return False, (
            "staged transfer directory no longer validates for resume: "
            f"missing={validation['missing_entries']}, "
            f"mismatched={validation['mismatched_entries']}, "
            f"unexpected={validation['unexpected_entries']}"
        )
    return True, ""


def _resolve_step_timeout(
    step_name: str,
    *,
    default_timeout_s: float | None,
    step_timeouts: Mapping[str, float] | None,
) -> float | None:
    resolved_timeout: float | None
    if step_timeouts is not None and step_name in step_timeouts:
        resolved_timeout = step_timeouts[step_name]
    elif default_timeout_s is not None:
        resolved_timeout = default_timeout_s
    else:
        resolved_timeout = DEFAULT_ARTIFICE_STEP_TIMEOUTS.get(step_name)
    return scale_cemu_timeout_seconds(resolved_timeout)


def _calculate_usb_retry_delay_seconds(
    retry_index: int,
    *,
    base_delay_ms: int,
    max_delay_ms: int,
    jitter_ratio: float,
    random_source: Callable[[], float] = random.random,
) -> float:
    if retry_index <= 0 or base_delay_ms <= 0 or max_delay_ms <= 0:
        return 0.0

    normalized_jitter_ratio = max(0.0, float(jitter_ratio))
    bounded_base_delay_ms = max(0, int(base_delay_ms))
    bounded_max_delay_ms = max(0, int(max_delay_ms))
    exponential_delay_ms = min(
        bounded_max_delay_ms,
        bounded_base_delay_ms * (2 ** max(0, retry_index - 1)),
    )
    if normalized_jitter_ratio <= 0 or exponential_delay_ms <= 0:
        return exponential_delay_ms / 1000.0

    jitter_offset = ((float(random_source()) * 2.0) - 1.0) * normalized_jitter_ratio
    jittered_delay_ms = exponential_delay_ms * (1.0 + jitter_offset)
    bounded_delay_ms = min(
        float(bounded_max_delay_ms),
        max(0.0, jittered_delay_ms),
    )
    return round(bounded_delay_ms / 1000.0, 3)


def _sleep_before_usb_retry(delay_s: float) -> None:
    time.sleep(delay_s)


def _checkpoint_passed(
    checkpoints: Sequence[ArtificeCheckpointResult],
    step_name: str,
) -> bool:
    return any(checkpoint.step == step_name and checkpoint.passed for checkpoint in checkpoints)


def _verify_os58_bypass_active(
    checkpoints: Sequence[ArtificeCheckpointResult],
) -> tuple[bool, tuple[str, ...]]:
    shell_ok = _checkpoint_passed(checkpoints, "artifice_shell_loaded")
    exit_ok = _checkpoint_passed(checkpoints, "artifice_quit_transition")
    diagnostics: list[str] = []
    if shell_ok:
        diagnostics.append("Verified the locked arTIfiCE shell CRC on OS 5.8.x.")
    else:
        diagnostics.append("OS 5.8 bypass verification failed at the shell-loaded checkpoint.")
    if exit_ok:
        diagnostics.append("Verified the locked CLEAR -> MODE quit-transition CRC on OS 5.8.x.")
    else:
        diagnostics.append("OS 5.8 bypass verification failed at the quit-transition checkpoint.")
    return shell_ok and exit_ok, tuple(diagnostics)


def _rollback_partial_jailbreak(
    runner: CEmuRunner,
    *,
    package: TransferPackage,
    active_stage_dir: Path | None,
    diagnostics: list[str],
    timeout_s: float | None,
) -> tuple[bool, tuple[str, ...]]:
    cleanup_actions: list[str] = []
    rollback_performed = False

    reset_method = getattr(runner, "reset", None)
    if callable(reset_method):
        try:
            reset_method(timeout_s=timeout_s)
            cleanup_actions.append("runner.reset")
            rollback_performed = True
            diagnostics.append("Rollback reset the runner session after the partial arTIfiCE failure.")
        except (CEmuProcessTimeoutError, CEmuRunnerError, OSError, RuntimeError, ValueError) as exc:
            diagnostics.append(f"Rollback could not reset the runner session cleanly: {exc}")

    if active_stage_dir is not None:
        try:
            stage_transfer_package(package, active_stage_dir)
            cleanup_actions.append("stage_transfer_package")
            rollback_performed = True
            diagnostics.append("Rollback restored the staged transfer directory and removed the partial arTIfiCE wrapper payload.")
            wrapper_payload_path = active_stage_dir / f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp"
            if wrapper_payload_path.exists():
                diagnostics.append(
                    "Rollback restored the staged transfer directory, but the arTIfiCE wrapper payload is still present and requires manual inspection."
                )
            else:
                diagnostics.append(
                    "Rollback confirmed the staged transfer directory is clean and the arTIfiCE wrapper payload was removed."
                )
            validation = validate_staged_transfer_directory(package, active_stage_dir)
            if bool(validation["success"]):
                cleanup_actions.append("validate_staged_transfer_directory")
                diagnostics.append(
                    "Rollback validated the restored staged transfer directory after removing the partial arTIfiCE payload."
                )
            else:
                diagnostics.append(
                    "Rollback restored the staged transfer directory, but validation still reported "
                    f"missing={validation['missing_entries']}, mismatched={validation['mismatched_entries']}, "
                    f"unexpected={validation['unexpected_entries']}."
                )
        except (TransferPackageResolutionError, OSError) as exc:
            diagnostics.append(f"Rollback could not restore the staged transfer directory: {exc}")

    return rollback_performed, tuple(cleanup_actions)


def _recover_runner_for_usb_retry(
    runner: CEmuRunner,
    *,
    diagnostics: list[str],
    timeout_s: float | None,
) -> None:
    reset_method = getattr(runner, "reset", None)
    if callable(reset_method):
        try:
            reset_method(timeout_s=timeout_s)
            diagnostics.append(
                "Reset the runner before retrying the USB transfer after a partial or timed-out attempt."
            )
        except (CEmuProcessTimeoutError, CEmuRunnerError, OSError, RuntimeError, ValueError) as exc:
            diagnostics.append(f"Runner reset before the USB retry failed: {exc}")

    try:
        _ensure_runner_ready(runner, timeout_s=timeout_s)
        diagnostics.append("Re-validated runner readiness before retrying the USB transfer.")
    except (CEmuProcessTimeoutError, CEmuRunnerError, OSError, RuntimeError, ValueError) as exc:
        diagnostics.append(f"Runner readiness re-check before the USB retry failed: {exc}")


def _load_program_with_usb_retry(
    runner: CEmuRunner,
    program_path: Path,
    *,
    target_name: str,
    target_is_asm: bool,
    timeout_s: float | None,
    expected_archive_count: int,
    retry_count: int,
    retry_delay_ms: int,
    retry_max_delay_ms: int,
    retry_jitter_ratio: float,
    diagnostics: list[str],
) -> tuple[Any, int]:
    last_error: Exception | None = None
    attempts = max(1, retry_count)

    for attempt in range(1, attempts + 1):
        try:
            load_result = _load_program_with_target(
                runner,
                program_path,
                target_name=target_name,
                target_is_asm=target_is_asm,
                timeout_s=timeout_s,
            )
            state = runner.get_state(timeout_s=timeout_s)
            loaded_count = int(state.payload.get("app_count", 0))
            observed_target = str(state.payload.get("target_name", ""))
            observed_target_is_asm = bool(state.payload.get("target_is_asm", target_is_asm))
            diagnostics.append(
                f"USB simulation attempt {attempt}/{attempts}: app_count={loaded_count}, target_name={observed_target!r}, target_is_asm={observed_target_is_asm}."
            )
            if loaded_count < expected_archive_count:
                raise RuntimeError(
                    "USB connection simulation reported a partial archive load "
                    f"({loaded_count}/{expected_archive_count} staged files visible)."
                )
            if observed_target.upper() != target_name.upper():
                diagnostics.append(
                    "USB simulation returned non-authoritative target metadata "
                    f"('{observed_target}' instead of '{target_name}'); continuing because the staged archive count is intact."
                )
            if observed_target_is_asm != target_is_asm:
                diagnostics.append(
                    "USB simulation returned non-authoritative target_is_asm metadata "
                    f"({observed_target_is_asm!r} instead of {target_is_asm!r}); continuing because the staged archive count is intact."
                )
            if attempt > 1:
                diagnostics.append(
                    f"USB simulation recovered on attempt {attempt}/{attempts} after {attempt - 1} earlier failure(s)."
                )
            return load_result, attempt
        except (CEmuProcessTimeoutError, CEmuRunnerError, OSError, RuntimeError, ValueError) as exc:
            last_error = exc
            diagnostics.append(f"USB simulation attempt {attempt}/{attempts} failed: {exc}")
            if attempt >= attempts:
                break
            _recover_runner_for_usb_retry(
                runner,
                diagnostics=diagnostics,
                timeout_s=timeout_s,
            )
            retry_delay_s = _calculate_usb_retry_delay_seconds(
                attempt,
                base_delay_ms=retry_delay_ms,
                max_delay_ms=retry_max_delay_ms,
                jitter_ratio=retry_jitter_ratio,
            )
            diagnostics.append(
                "USB simulation retry backoff before the next attempt: "
                f"{retry_delay_s:.3f}s (base={retry_delay_ms}ms, max={retry_max_delay_ms}ms, jitter={retry_jitter_ratio:.2f})."
            )
            _sleep_before_usb_retry(retry_delay_s)

    assert last_error is not None
    raise ArtificeUsbRetryExhaustedError(str(last_error), attempts=attempts) from last_error


def evaluate_jailbreak_indicators(
    checkpoints: Sequence[ArtificeCheckpointResult],
    *,
    loaded_archive_count: int,
    expected_archive_count: int,
) -> JailbreakIndicators:
    step_map = {checkpoint.step: checkpoint for checkpoint in checkpoints}
    shell_ok = step_map.get("artifice_shell_loaded", ArtificeCheckpointResult("", "", (), "", False, None, {})).passed
    exit_ok = step_map.get("artifice_quit_transition", ArtificeCheckpointResult("", "", (), "", False, None, {})).passed
    prgm_ok = step_map.get("ti_os_program_menu_open", ArtificeCheckpointResult("", "", (), "", False, None, {})).passed

    verified_markers = tuple(
        checkpoint.step for checkpoint in checkpoints if checkpoint.passed
    )

    diagnostics = [
        "Privilege escalation is simulated from locked shell and quit-transition CRCs because the current headless wrapper does not expose raw privilege registers.",
        "Port write access is simulated from a successful TI-OS PRGM-menu return with the staged archive payload still present, because the wrapper does not expose direct port probes.",
    ]
    if shell_ok and exit_ok:
        diagnostics.append("The shell-loaded and quit-transition checkpoints matched the locked OS 5.8 arTIfiCE baseline.")
    if prgm_ok:
        diagnostics.append("The verified CLEAR -> MODE -> PRGM choreography returned to the TI-OS PRGM menu.")
    if loaded_archive_count >= expected_archive_count:
        diagnostics.append(
            f"Archive load count {loaded_archive_count} meets or exceeds the expected staged payload count {expected_archive_count}."
        )
    else:
        diagnostics.append(
            f"Archive load count {loaded_archive_count} is below the expected staged payload count {expected_archive_count}."
        )

    return JailbreakIndicators(
        privilege_escalated=shell_ok and exit_ok,
        port_write_access=prgm_ok and loaded_archive_count >= expected_archive_count,
        verified_markers=verified_markers,
        diagnostics=tuple(diagnostics),
    )


def run_artifice_jailbreak(
    runner: CEmuRunner,
    *,
    transfer_package: TransferPackage | None = None,
    artifice_path: Path | None = None,
    rom_validation: RomValidationResult | None = None,
    baseline_path: Path = DEFAULT_ARTIFICE_BASELINE_PATH,
    stage_dir: Path | None = None,
    evidence_dir: Path | None = None,
    timeout_s: float | None = None,
    step_timeouts: Mapping[str, float] | None = None,
    checkpoint_path: Path | None = None,
    checkpoint_lock_timeout_s: float = DEFAULT_ARTIFICE_CHECKPOINT_LOCK_TIMEOUT_S,
    checkpoint_max_age_s: float | None = None,
    usb_retry_count: int = DEFAULT_ARTIFICE_USB_RETRY_COUNT,
    usb_retry_delay_ms: int = DEFAULT_ARTIFICE_USB_RETRY_DELAY_MS,
    usb_retry_max_delay_ms: int = DEFAULT_ARTIFICE_USB_RETRY_MAX_DELAY_MS,
    usb_retry_jitter_ratio: float = DEFAULT_ARTIFICE_USB_RETRY_JITTER_RATIO,
) -> JailbreakResult:
    started_at = time.perf_counter()
    diagnostics: list[str] = []
    checkpoints: list[ArtificeCheckpointResult] = []
    timed_out = False
    failure_reason = ""
    active_stage_dir: Path | None = None
    active_evidence_dir: Path | None = None
    loaded_archive_count = 0
    bypass_active = False
    rollback_performed = False
    cleanup_actions: tuple[str, ...] = ()
    usb_attempts = 0
    resolved_checkpoint_path: Path | None = None
    checkpoint_lock_fd: int | None = None
    checkpoint_created_at_epoch_ms = 0
    sequence_documentation = ARTIFICE_SEQUENCE_DOCUMENTATION

    package = transfer_package or resolve_transfer_package(repo_root=REPO_ROOT)
    if not package.ready:
        return JailbreakResult(
            success=False,
            failure_reason="CalcEngine transfer package is incomplete: "
            + ", ".join(package.missing_entries),
            timed_out=False,
            artifice_path=None,
            baseline_path=baseline_path if baseline_path.exists() else None,
            stage_dir=None,
            evidence_dir=None,
            module_count=package.module_count,
            runtime_dependency_count=package.runtime_dependency_count,
            transfer_count=package.transfer_count,
            staged_file_count=0,
            loaded_archive_count=0,
            checkpoints=(),
            indicators=JailbreakIndicators(False, False, (), ()),
            diagnostics=(
                "The arTIfiCE lane cannot start until the manifest-backed transfer package is complete.",
            ),
            sequence_documentation=sequence_documentation,
            keypress_log=(),
            total_duration_ms=round((time.perf_counter() - started_at) * 1000),
        )

    rom_compatibility = evaluate_artifice_rom_compatibility(rom_validation)
    diagnostics.extend(rom_compatibility.diagnostics)
    if not rom_compatibility.supported:
        return JailbreakResult(
            success=False,
            failure_reason=rom_compatibility.blockers[0],
            timed_out=False,
            artifice_path=None,
            baseline_path=baseline_path if baseline_path.exists() else None,
            stage_dir=None,
            evidence_dir=None,
            module_count=package.module_count,
            runtime_dependency_count=package.runtime_dependency_count,
            transfer_count=package.transfer_count,
            staged_file_count=0,
            loaded_archive_count=0,
            checkpoints=(),
            indicators=JailbreakIndicators(False, False, (), ()),
            diagnostics=tuple(diagnostics),
            sequence_documentation=sequence_documentation,
            keypress_log=(),
            total_duration_ms=round((time.perf_counter() - started_at) * 1000),
        )

    resolved_artifice_path = resolve_artifice_path(
        candidate=artifice_path,
        repo_root=REPO_ROOT,
    )
    if resolved_artifice_path is None:
        return JailbreakResult(
            success=False,
            failure_reason="arTIfiCE .8xp asset could not be resolved.",
            timed_out=False,
            artifice_path=None,
            baseline_path=baseline_path if baseline_path.exists() else None,
            stage_dir=None,
            evidence_dir=None,
            module_count=package.module_count,
            runtime_dependency_count=package.runtime_dependency_count,
            transfer_count=package.transfer_count,
            staged_file_count=0,
            loaded_archive_count=0,
            checkpoints=(),
            indicators=JailbreakIndicators(False, False, (), ()),
            diagnostics=(
                "Expected the upstream asset at tools/emulator/assets/artifice-v2.8xp or CALCENGINE_ARTIFICE_PATH.",
            ),
            sequence_documentation=sequence_documentation,
            keypress_log=(),
            total_duration_ms=round((time.perf_counter() - started_at) * 1000),
        )

    if not _begin_jailbreak_session(runner):
        diagnostics.append(
            "Rejected arTIfiCE jailbreak request because another jailbreak attempt is already active for this runner."
        )
        return JailbreakResult(
            success=False,
            failure_reason="Another arTIfiCE jailbreak attempt is already in progress for this runner.",
            timed_out=False,
            artifice_path=resolved_artifice_path,
            baseline_path=baseline_path if baseline_path.exists() else None,
            stage_dir=None,
            evidence_dir=None,
            module_count=package.module_count,
            runtime_dependency_count=package.runtime_dependency_count,
            transfer_count=package.transfer_count,
            staged_file_count=0,
            loaded_archive_count=0,
            checkpoints=(),
            indicators=JailbreakIndicators(False, False, (), ()),
            diagnostics=tuple(diagnostics),
            sequence_documentation=sequence_documentation,
            keypress_log=(),
            total_duration_ms=round((time.perf_counter() - started_at) * 1000),
        )

    try:
        if stage_dir is None:
            create_stage_dir = getattr(runner, "create_host_accessible_temp_dir", None)
            if callable(create_stage_dir):
                active_stage_dir = create_stage_dir(DEFAULT_ARTIFICE_STAGE_PREFIX)
            else:
                active_stage_dir = Path(tempfile.mkdtemp(prefix=DEFAULT_ARTIFICE_STAGE_PREFIX))
        else:
            active_stage_dir = stage_dir
            active_stage_dir.mkdir(parents=True, exist_ok=True)

        if evidence_dir is None:
            active_evidence_dir = active_stage_dir / "evidence"
        else:
            active_evidence_dir = evidence_dir
        active_evidence_dir.mkdir(parents=True, exist_ok=True)

        resume_checkpoint: ArtificeCheckpointState | None = None
        if checkpoint_path is not None:
            resolved_checkpoint_path = (
                checkpoint_path
                if checkpoint_path.is_absolute()
                else active_evidence_dir / checkpoint_path
            )
            try:
                checkpoint_lock_fd = _acquire_checkpoint_lock(
                    resolved_checkpoint_path,
                    timeout_s=checkpoint_lock_timeout_s,
                )
            except ArtificeCheckpointError as exc:
                diagnostics.append(str(exc))
                return JailbreakResult(
                    success=False,
                    failure_reason=str(exc),
                    timed_out=False,
                    artifice_path=resolved_artifice_path,
                    baseline_path=baseline_path if baseline_path.exists() else None,
                    stage_dir=active_stage_dir,
                    evidence_dir=active_evidence_dir,
                    module_count=package.module_count,
                    runtime_dependency_count=package.runtime_dependency_count,
                    transfer_count=package.transfer_count,
                    staged_file_count=_count_staged_files(active_stage_dir),
                    loaded_archive_count=0,
                    checkpoints=(),
                    indicators=JailbreakIndicators(False, False, (), ()),
                    diagnostics=tuple(diagnostics),
                    sequence_documentation=sequence_documentation,
                    keypress_log=(),
                    total_duration_ms=round((time.perf_counter() - started_at) * 1000),
                )
            try:
                loaded_checkpoint = _load_checkpoint_state(resolved_checkpoint_path)
            except ArtificeCheckpointError as exc:
                diagnostics.append(str(exc))
                if resolved_checkpoint_path.exists():
                    _delete_checkpoint_file(resolved_checkpoint_path)
                    diagnostics.append(
                        "Persisted arTIfiCE checkpoint was corrupt or unsupported; deleted it and forcing a clean restart."
                    )
            else:
                if loaded_checkpoint is not None and not loaded_checkpoint.completed:
                    can_resume, resume_reason = _checkpoint_can_resume(
                        loaded_checkpoint,
                        stage_dir=active_stage_dir,
                        evidence_dir=active_evidence_dir,
                        artifice_path=resolved_artifice_path,
                        package=package,
                        checkpoint_max_age_s=checkpoint_max_age_s,
                    )
                    if can_resume:
                        resume_checkpoint = loaded_checkpoint
                        checkpoint_created_at_epoch_ms = loaded_checkpoint.created_at_epoch_ms
                        diagnostics.append(
                            "Resuming from the persisted arTIfiCE checkpoint after validating the staged transfer directory and wrapper payload."
                        )
                    else:
                        diagnostics.append(
                            "Persisted arTIfiCE checkpoint could not be resumed; forcing a clean restart because "
                            f"{resume_reason}."
                        )
                        _delete_checkpoint_file(resolved_checkpoint_path)
                elif loaded_checkpoint is not None and loaded_checkpoint.completed:
                    diagnostics.append(
                        "Found a completed arTIfiCE checkpoint; ignoring it and starting a fresh run."
                    )

        if resume_checkpoint is None:
            try:
                stage_artifice_transfer_package(
                    package=package,
                    artifice_path=resolved_artifice_path,
                    destination_dir=active_stage_dir,
                )
            except (TransferPackageResolutionError, OSError) as exc:
                diagnostics.append(f"arTIfiCE staging failed: {exc}")
                return JailbreakResult(
                    success=False,
                    failure_reason=str(exc),
                    timed_out=False,
                    artifice_path=resolved_artifice_path,
                    baseline_path=baseline_path if baseline_path.exists() else None,
                    stage_dir=active_stage_dir,
                    evidence_dir=active_evidence_dir,
                    module_count=package.module_count,
                    runtime_dependency_count=package.runtime_dependency_count,
                    transfer_count=package.transfer_count,
                    staged_file_count=_count_staged_files(active_stage_dir),
                    loaded_archive_count=0,
                    checkpoints=(),
                    indicators=JailbreakIndicators(False, False, (), ()),
                    diagnostics=tuple(diagnostics),
                    sequence_documentation=sequence_documentation,
                    keypress_log=(),
                    total_duration_ms=round((time.perf_counter() - started_at) * 1000),
                )

        sequence_documentation = ARTIFICE_SEQUENCE_DOCUMENTATION
        shell_sequence = build_artifice_shell_launch_sequence()
        exit_sequence = build_artifice_shell_exit_sequence()
        prgm_sequence = build_tios_program_menu_sequence()
        keypress_log = (
            *_format_sequence(shell_sequence),
            *_format_sequence(exit_sequence),
            *_format_sequence(prgm_sequence),
        )

        diagnostics.append(f"Resolved arTIfiCE asset: {resolved_artifice_path}")
        diagnostics.append(f"Staged transfer directory: {active_stage_dir}")
        diagnostics.append(
            f"Staged archive payload contains {package.module_count} modules, "
            f"{package.runtime_dependency_count} runtime dependencies, and prgmA."
        )
        staged_validation = validate_staged_transfer_directory(
            package,
            active_stage_dir,
            allowed_extra_names=(f"{DEFAULT_ARTIFICE_PROGRAM_NAME}.8xp",),
        )
        if not bool(staged_validation["success"]):
            diagnostics.append(
                "Staged archive payload failed post-copy validation before launching arTIfiCE."
            )
            return JailbreakResult(
                success=False,
                failure_reason="Staged transfer payload failed validation before the arTIfiCE handoff.",
                timed_out=False,
                artifice_path=resolved_artifice_path,
                baseline_path=baseline_path if baseline_path.exists() else None,
                stage_dir=active_stage_dir,
                evidence_dir=active_evidence_dir,
                module_count=package.module_count,
                runtime_dependency_count=package.runtime_dependency_count,
                transfer_count=package.transfer_count,
                staged_file_count=_count_staged_files(active_stage_dir),
                loaded_archive_count=0,
                checkpoints=(),
                indicators=JailbreakIndicators(False, False, (), ()),
                diagnostics=tuple(
                    [
                        *diagnostics,
                        f"Missing staged entries: {', '.join(staged_validation['missing_entries']) or 'none'}",
                        f"Mismatched staged entries: {', '.join(staged_validation['mismatched_entries']) or 'none'}",
                        f"Unexpected staged entries: {', '.join(staged_validation['unexpected_entries']) or 'none'}",
                    ]
                ),
                sequence_documentation=sequence_documentation,
                keypress_log=(),
                total_duration_ms=round((time.perf_counter() - started_at) * 1000),
            )
        diagnostics.append(
            "Post-copy validation confirmed the staged manifest payload before launching arTIfiCE."
        )
        if resume_checkpoint is not None:
            diagnostics.append(
                f"Resumed checkpoint last_completed_step='{resume_checkpoint.last_completed_step or '<none>'}' before replaying the runtime-dependent emulator steps."
            )
        if resolved_checkpoint_path is not None:
            checkpoint_state = _write_checkpoint_state(
                resolved_checkpoint_path,
                status="in_progress",
                stage_dir=active_stage_dir,
                evidence_dir=active_evidence_dir,
                artifice_path=resolved_artifice_path,
                transfer_count=package.transfer_count,
                module_count=package.module_count,
                runtime_dependency_count=package.runtime_dependency_count,
                last_completed_step=resume_checkpoint.last_completed_step if resume_checkpoint is not None else "",
                checkpoints=resume_checkpoint.checkpoints if resume_checkpoint is not None else (),
                created_at_epoch_ms=checkpoint_created_at_epoch_ms,
            )
            checkpoint_created_at_epoch_ms = checkpoint_state.created_at_epoch_ms
            diagnostics.append(
                f"Persisted arTIfiCE checkpoint state to '{resolved_checkpoint_path}' with digest {checkpoint_state.integrity_digest}."
            )

        checkpoint_definitions = get_artifice_checkpoint_definitions(
            expected_hashes=load_artifice_expected_hashes(baseline_path=baseline_path),
        )

        current_step = "runner_ready"
        try:
            _ensure_runner_ready(
                runner,
                timeout_s=_resolve_step_timeout(
                    "runner_ready",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
            )

            current_step = "usb_connect"
            load_result, usb_attempts = _load_program_with_usb_retry(
                runner,
                active_stage_dir,
                target_name=DEFAULT_ARTIFICE_PROGRAM_NAME,
                target_is_asm=False,
                timeout_s=_resolve_step_timeout(
                    "usb_connect",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
                expected_archive_count=package.transfer_count + 1,
                retry_count=usb_retry_count,
                retry_delay_ms=usb_retry_delay_ms,
                retry_max_delay_ms=usb_retry_max_delay_ms,
                retry_jitter_ratio=usb_retry_jitter_ratio,
                diagnostics=diagnostics,
            )
            diagnostics.append(
                "Sent load_app with target_name='A' and target_is_asm=False to emulate launching prgmA."
            )
            diagnostics.append(f"load_app response keys: {sorted(dict(load_result.payload).keys())}")
            if _runner_uses_native_core(load_result.payload):
                native_expected_hashes = load_artifice_expected_hashes(baseline_path=baseline_path)
                native_expected_hashes.update(NATIVE_CORE_ARTIFICE_HASH_OVERRIDES)
                checkpoint_definitions = get_artifice_checkpoint_definitions(
                    expected_hashes=native_expected_hashes,
                )
                shell_sequence = build_native_artifice_home_launch_sequence()
                sequence_documentation = (
                    *ARTIFICE_SEQUENCE_DOCUMENTATION,
                    *NATIVE_ARTIFICE_HOME_LAUNCH_DOCUMENTATION,
                )
                keypress_log = (
                    *_format_sequence(shell_sequence),
                    *_format_sequence(exit_sequence),
                    *_format_sequence(prgm_sequence),
                )
                diagnostics.append(
                    "Detected native-core CEmu mode from the load_app response; switching the shell-launch step to the explicit TI-OS PRGM → ENTER → ENTER prgmA execution path before shell capture."
                )
                diagnostics.append(
                    "Using native-core-specific OS 5.8 arTIfiCE checkpoint CRCs for the shell, quit-transition, and TI-OS PRGM menu states."
                )

            current_step = "shell_launch"
            runner.send_sequence(
                shell_sequence,
                timeout_s=_resolve_step_timeout(
                    "shell_launch",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
            )
            current_step = "shell_capture"
            checkpoints.append(
                _capture_checkpoint(
                    runner,
                    checkpoint_definitions[0],
                    evidence_dir=active_evidence_dir,
                    timeout_s=_resolve_step_timeout(
                        "shell_capture",
                        default_timeout_s=timeout_s,
                        step_timeouts=step_timeouts,
                    ),
                )
            )
            if resolved_checkpoint_path is not None:
                _write_checkpoint_state(
                    resolved_checkpoint_path,
                    status="in_progress",
                    stage_dir=active_stage_dir,
                    evidence_dir=active_evidence_dir,
                    artifice_path=resolved_artifice_path,
                    transfer_count=package.transfer_count,
                    module_count=package.module_count,
                    runtime_dependency_count=package.runtime_dependency_count,
                    last_completed_step=checkpoints[-1].step,
                    checkpoints=checkpoints,
                    created_at_epoch_ms=checkpoint_created_at_epoch_ms,
                )

            current_step = "shell_exit"
            runner.send_sequence(
                exit_sequence,
                timeout_s=_resolve_step_timeout(
                    "shell_exit",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
            )
            current_step = "quit_capture"
            checkpoints.append(
                _capture_checkpoint(
                    runner,
                    checkpoint_definitions[1],
                    evidence_dir=active_evidence_dir,
                    timeout_s=_resolve_step_timeout(
                        "quit_capture",
                        default_timeout_s=timeout_s,
                        step_timeouts=step_timeouts,
                    ),
                )
            )
            if resolved_checkpoint_path is not None:
                _write_checkpoint_state(
                    resolved_checkpoint_path,
                    status="in_progress",
                    stage_dir=active_stage_dir,
                    evidence_dir=active_evidence_dir,
                    artifice_path=resolved_artifice_path,
                    transfer_count=package.transfer_count,
                    module_count=package.module_count,
                    runtime_dependency_count=package.runtime_dependency_count,
                    last_completed_step=checkpoints[-1].step,
                    checkpoints=checkpoints,
                    created_at_epoch_ms=checkpoint_created_at_epoch_ms,
                )

            bypass_active, bypass_diagnostics = _verify_os58_bypass_active(checkpoints)
            diagnostics.extend(bypass_diagnostics)
            if not bypass_active:
                raise RuntimeError(
                    "OS 5.8 bypass verification did not pass the locked shell/quit checkpoints; refusing to continue into the PRGM handoff."
                )

            current_step = "program_menu"
            runner.send_sequence(
                prgm_sequence,
                timeout_s=_resolve_step_timeout(
                    "program_menu",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
            )
            current_step = "program_menu_capture"
            checkpoints.append(
                _capture_checkpoint(
                    runner,
                    checkpoint_definitions[2],
                    evidence_dir=active_evidence_dir,
                    timeout_s=_resolve_step_timeout(
                        "program_menu_capture",
                        default_timeout_s=timeout_s,
                        step_timeouts=step_timeouts,
                    ),
                )
            )
            if resolved_checkpoint_path is not None:
                _write_checkpoint_state(
                    resolved_checkpoint_path,
                    status="in_progress",
                    stage_dir=active_stage_dir,
                    evidence_dir=active_evidence_dir,
                    artifice_path=resolved_artifice_path,
                    transfer_count=package.transfer_count,
                    module_count=package.module_count,
                    runtime_dependency_count=package.runtime_dependency_count,
                    last_completed_step=checkpoints[-1].step,
                    checkpoints=checkpoints,
                    created_at_epoch_ms=checkpoint_created_at_epoch_ms,
                )

            current_step = "final_state"
            state = runner.get_state(
                timeout_s=_resolve_step_timeout(
                    "final_state",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
            )
            loaded_archive_count = int(state.payload.get("app_count", 0))
            diagnostics.append(
                f"Runner state after jailbreak reports app_count={loaded_archive_count} and target_name={state.payload.get('target_name', '')!r}."
            )
        except ArtificeUsbRetryExhaustedError as exc:
            usb_attempts = exc.attempts
            failure_reason = str(exc)
            timed_out = isinstance(exc.__cause__, CEmuProcessTimeoutError)
            if timed_out:
                diagnostics.append(
                    f"Timed out during arTIfiCE step '{current_step}' after {usb_attempts} USB attempts: {exc}"
                )
            else:
                diagnostics.append(f"arTIfiCE step '{current_step}' failed after {usb_attempts} USB attempts: {exc}")
        except CEmuProcessTimeoutError as exc:
            timed_out = True
            failure_reason = str(exc)
            diagnostics.append(f"Timed out during arTIfiCE step '{current_step}': {exc}")
        except (ArtificeCheckpointError, CEmuRunnerError, OSError, RuntimeError, ValueError) as exc:
            failure_reason = str(exc)
            diagnostics.append(f"arTIfiCE step '{current_step}' failed: {exc}")

        indicators = evaluate_jailbreak_indicators(
            checkpoints,
            loaded_archive_count=loaded_archive_count,
            expected_archive_count=package.transfer_count + 1,
        )
        diagnostics.extend(indicators.diagnostics)

        success = (
            not failure_reason
            and len(checkpoints) == len(checkpoint_definitions)
            and all(checkpoint.passed for checkpoint in checkpoints)
            and bypass_active
            and indicators.privilege_escalated
            and indicators.port_write_access
        )
        if not success and not failure_reason:
            failure_reason = "One or more arTIfiCE checkpoints did not match the locked OS 5.8 baseline."

        if success and resolved_checkpoint_path is not None:
            completed_checkpoint = _write_checkpoint_state(
                resolved_checkpoint_path,
                status="completed",
                stage_dir=active_stage_dir,
                evidence_dir=active_evidence_dir,
                artifice_path=resolved_artifice_path,
                transfer_count=package.transfer_count,
                module_count=package.module_count,
                runtime_dependency_count=package.runtime_dependency_count,
                last_completed_step=checkpoints[-1].step if checkpoints else "",
                checkpoints=checkpoints,
                created_at_epoch_ms=checkpoint_created_at_epoch_ms,
            )
            diagnostics.append(
                f"Marked the persisted arTIfiCE checkpoint as completed with digest {completed_checkpoint.integrity_digest}."
            )

        if not success and resolved_checkpoint_path is not None:
            failed_checkpoint = _write_checkpoint_state(
                resolved_checkpoint_path,
                status="failed",
                stage_dir=active_stage_dir,
                evidence_dir=active_evidence_dir,
                artifice_path=resolved_artifice_path,
                transfer_count=package.transfer_count,
                module_count=package.module_count,
                runtime_dependency_count=package.runtime_dependency_count,
                last_completed_step=checkpoints[-1].step if checkpoints else "",
                checkpoints=checkpoints,
                created_at_epoch_ms=checkpoint_created_at_epoch_ms,
            )
            diagnostics.append(
                "Preserved the staged arTIfiCE transfer directory and checkpoint file for a resumable retry after failure. "
                f"Checkpoint digest: {failed_checkpoint.integrity_digest}."
            )
        elif not success:
            rollback_performed, cleanup_actions = _rollback_partial_jailbreak(
                runner,
                package=package,
                active_stage_dir=active_stage_dir,
                diagnostics=diagnostics,
                timeout_s=_resolve_step_timeout(
                    "rollback",
                    default_timeout_s=timeout_s,
                    step_timeouts=step_timeouts,
                ),
            )

        return JailbreakResult(
            success=success,
            failure_reason=failure_reason,
            timed_out=timed_out,
            artifice_path=resolved_artifice_path,
            baseline_path=baseline_path if baseline_path.exists() else None,
            stage_dir=active_stage_dir,
            evidence_dir=active_evidence_dir,
            module_count=package.module_count,
            runtime_dependency_count=package.runtime_dependency_count,
            transfer_count=package.transfer_count,
            staged_file_count=_count_staged_files(active_stage_dir),
            loaded_archive_count=loaded_archive_count,
            checkpoints=tuple(checkpoints),
            indicators=indicators,
            diagnostics=tuple(diagnostics),
            sequence_documentation=sequence_documentation,
            keypress_log=keypress_log,
            total_duration_ms=round((time.perf_counter() - started_at) * 1000),
            bypass_active=bypass_active,
            rollback_performed=rollback_performed,
            usb_retry_count=usb_attempts,
            cleanup_actions=cleanup_actions,
        )
    finally:
        if resolved_checkpoint_path is not None and checkpoint_lock_fd is not None:
            _release_checkpoint_lock(resolved_checkpoint_path, checkpoint_lock_fd)
        _finish_jailbreak_session(runner)


__all__ = [
    "ALPHA_KEY_MAP",
    "ARTIFICE_SEQUENCE_DOCUMENTATION",
    "ArtificeCheckpointError",
    "ArtificeCheckpointDefinition",
    "ArtificeCheckpointResult",
    "ArtificeCheckpointState",
    "ArtificeRomCompatibility",
    "ArtificeUsbRetryExhaustedError",
    "DEFAULT_ARTIFICE_BASELINE_PATH",
    "DEFAULT_ARTIFICE_HASHES",
    "DEFAULT_ARTIFICE_PATH",
    "DEFAULT_ARTIFICE_CHECKPOINT_VERSION",
    "DEFAULT_ARTIFICE_PROGRAM_NAME",
    "JailbreakIndicators",
    "JailbreakResult",
    "SUPPORTED_ARTIFICE_FIRMWARE_FAMILIES",
    "SUPPORTED_ARTIFICE_OS_PREFIXES",
    "TransferManifestEntry",
    "build_alpha_program_launch_sequence",
    "build_artifice_shell_exit_sequence",
    "build_artifice_shell_launch_sequence",
    "evaluate_artifice_rom_compatibility",
    "build_tios_program_menu_sequence",
    "evaluate_jailbreak_indicators",
    "get_artifice_checkpoint_definitions",
    "load_artifice_expected_hashes",
    "resolve_artifice_path",
    "run_artifice_jailbreak",
    "stage_artifice_transfer_package",
]
