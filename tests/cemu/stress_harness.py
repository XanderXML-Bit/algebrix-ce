from __future__ import annotations

import json
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from cemu_runner import CEmuCommandError, CEmuProcessTimeoutError, CEmuRunner
from fake_backend import build_fake_cemu_runner, fake_crc_for_keys
from keypress_sequences import KeyAction
from pipeline_support import TransferPackage
from rom_validation import RomValidationResult

from artifice_jailbreak import (
    build_artifice_shell_exit_sequence,
    build_artifice_shell_launch_sequence,
    build_tios_program_menu_sequence,
)


@dataclass(frozen=True)
class DummyCommandResult:
    command: str
    payload: Mapping[str, object]
    raw: Mapping[str, object]


@dataclass(frozen=True)
class ArchiveEntry:
    name: str
    size_bytes: int


@dataclass
class AppVarEntry:
    name: str
    payload: bytes
    archived: bool
    vat_id: int
    generation: int
    write_count: int

    @property
    def size_bytes(self) -> int:
        return len(self.payload)


def _result(command: str, payload: Mapping[str, object]) -> DummyCommandResult:
    materialized = dict(payload)
    return DummyCommandResult(command, materialized, materialized)


def _iter_transfer_entries(path: Path) -> list[ArchiveEntry]:
    if path.is_dir():
        candidates = sorted(
            (
                candidate
                for candidate in path.iterdir()
                if candidate.is_file() and candidate.suffix.lower() in {".8xp", ".8xv"}
            ),
            key=lambda candidate: candidate.name.lower(),
        )
        return [
            ArchiveEntry(name=candidate.name, size_bytes=candidate.stat().st_size)
            for candidate in candidates
        ]

    if path.is_file() and path.suffix.lower() in {".8xp", ".8xv"}:
        return [ArchiveEntry(name=path.name, size_bytes=path.stat().st_size)]

    raise CEmuCommandError(f"load_app failed: unsupported load_app path '{path}'")


def _normalize_target_name(entries: Sequence[ArchiveEntry], requested_target: object | None) -> str:
    if requested_target is not None and str(requested_target).strip():
        return str(requested_target).strip().upper()
    if not entries:
        return ""
    return Path(entries[0].name).stem.upper()[:8]


def _normalize_subset_request(value: str) -> str:
    return str(value).strip().upper()


def _entry_matches_subset_request(entry_name: str, requested_names: set[str]) -> bool:
    normalized_name = entry_name.upper()
    normalized_stem = Path(entry_name).stem.upper()
    return normalized_name in requested_names or normalized_stem in requested_names


def stage_transfer_subset(
    package: TransferPackage,
    destination_dir: Path,
    *,
    module_names: Sequence[str],
    runtime_names: Sequence[str] | None = None,
) -> Path:
    destination_dir.mkdir(parents=True, exist_ok=True)
    for child in destination_dir.iterdir():
        if child.is_file():
            child.unlink()

    requested_module_names = {
        _normalize_subset_request(name)
        for name in module_names
        if str(name).strip()
    }
    if not requested_module_names:
        raise ValueError("At least one module must be selected for a staged subset.")

    requested_runtime_names = None
    if runtime_names is not None:
        requested_runtime_names = {
            _normalize_subset_request(name)
            for name in runtime_names
            if str(name).strip()
        }

    selected_module_entries = [
        entry
        for entry in package.module_entries
        if _entry_matches_subset_request(entry.name, requested_module_names)
    ]
    missing_module_names = sorted(
        requested_module_names
        - {
            entry.name.upper()
            for entry in selected_module_entries
        }
        - {
            Path(entry.name).stem.upper()
            for entry in selected_module_entries
        }
    )
    if missing_module_names:
        raise KeyError(
            "Requested module subset entries were not found in the transfer package: "
            + ", ".join(missing_module_names)
        )

    if requested_runtime_names is None:
        selected_runtime_entries = list(package.runtime_entries)
    else:
        selected_runtime_entries = [
            entry
            for entry in package.runtime_entries
            if _entry_matches_subset_request(entry.name, requested_runtime_names)
        ]
        missing_runtime_names = sorted(
            requested_runtime_names
            - {
                entry.name.upper()
                for entry in selected_runtime_entries
            }
            - {
                Path(entry.name).stem.upper()
                for entry in selected_runtime_entries
            }
        )
        if missing_runtime_names:
            raise KeyError(
                "Requested runtime subset entries were not found in the transfer package: "
                + ", ".join(missing_runtime_names)
            )

    for entry in [*selected_module_entries, *selected_runtime_entries]:
        destination_path = destination_dir / entry.name
        destination_path.write_bytes(package.resolved_paths[entry.name].read_bytes())

    return destination_dir


def write_synthetic_rom(
    path: Path,
    *,
    os_version: str,
    boot_version: str,
    model_signature: str = "TI-84 Plus CE",
    python_signature: bool = True,
) -> Path:
    payload = bytearray(b"\x00" * 4_194_304)
    payload[1_024 : 1_024 + len(model_signature)] = model_signature.encode("ascii")
    if python_signature:
        payload[4_096 : 4_096 + len(b"Python")] = b"Python"
    payload[8_192 : 8_192 + len(os_version)] = os_version.encode("ascii")
    payload[12_288 : 12_288 + len(boot_version)] = boot_version.encode("ascii")
    path.write_bytes(bytes(payload))
    return path


def write_fake_artifice_baseline(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    shell_keys = [
        str(action.key)
        for action in build_artifice_shell_launch_sequence()
        if action.kind == "key"
    ]
    quit_keys = [
        *shell_keys,
        *(
            str(action.key)
            for action in build_artifice_shell_exit_sequence()
            if action.kind == "key"
        ),
    ]
    menu_keys = [
        *quit_keys,
        *(
            str(action.key)
            for action in build_tios_program_menu_sequence()
            if action.kind == "key"
        ),
    ]
    payload = {
        "screens": [
            {"step": "artifice_shell_loaded", "hash": fake_crc_for_keys(*(shell_keys or ("idle",)))},
            {"step": "artifice_quit_transition", "hash": fake_crc_for_keys(*(quit_keys or ("idle",)))},
            {"step": "ti_os_program_menu_open", "hash": fake_crc_for_keys(*(menu_keys or ("idle",)))},
        ]
    }
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


class FakeCEmuStressRunner:
    _APPVAR_ARCHIVE_OVERHEAD_BYTES = 16

    def __init__(self, delegate: CEmuRunner, transfer_package: TransferPackage) -> None:
        self._delegate = delegate
        self._transfer_package = transfer_package
        self._archive_entries: list[ArchiveEntry] = []
        self._archive_used_bytes = 0
        self._archive_capacity_bytes = max(
            262_144,
            sum(entry.size_bytes for entry in transfer_package.module_entries) * 3,
        )
        self._archive_fragments: list[int] = []
        self._filler_entries: list[ArchiveEntry] = []
        self._defrag_count = 0
        self._load_app_history: list[str] = []
        self._active_target = ""
        self._target_is_asm = True
        self._transfer_snapshot: tuple[list[ArchiveEntry], int, str, bool] | None = None
        self._transfer_interrupt_stages: list[str] = []
        self._transfer_rollbacks = 0
        self._last_transfer_stage = ""
        self._module_states: dict[str, dict[str, object]] = {}
        self._shared_lists: dict[str, list[int]] = {}
        self._shared_list_sources: dict[str, str] = {}
        self._module_history: list[str] = []
        self._appvars: dict[str, AppVarEntry] = {}
        self._vat_tombstone_count = 0
        self._next_vat_id = 1
        self._next_appvar_generation = 1
        self._heap_limit_bytes = max(131_072, sum(entry.size_bytes for entry in transfer_package.module_entries) * 2)
        self._current_heap_bytes = 0
        self._peak_heap_bytes = 0
        self._oom_count = 0
        self._heap_corruption_detected = False
        self._battery_low = False
        self._rom_path_override = delegate.rom_path
        self._rom_validation: RomValidationResult | None = None
        self._executing_module = ""
        self._execution_release_event: threading.Event | None = None
        self._queue_artifice_requests = False
        self._queued_jailbreaks = 0
        self._rejected_jailbreaks = 0
        self._execution_lock = threading.Lock()
        self._transfer_lock = threading.Lock()
        self._artifice_conflict_event = threading.Event()

    @property
    def rom_path(self) -> Path:
        return self._rom_path_override

    @property
    def autotester_path(self) -> Path | None:
        return self._delegate.autotester_path

    @property
    def is_running(self) -> bool:
        return self._delegate.is_running

    @property
    def stdout_lines(self) -> tuple[str, ...]:
        return self._delegate.stdout_lines

    @property
    def stderr_lines(self) -> tuple[str, ...]:
        return self._delegate.stderr_lines

    @property
    def queue_artifice_requests(self) -> bool:
        return self._queue_artifice_requests

    @queue_artifice_requests.setter
    def queue_artifice_requests(self, value: bool) -> None:
        self._queue_artifice_requests = value

    @property
    def queued_jailbreaks(self) -> int:
        return self._queued_jailbreaks

    @property
    def rejected_jailbreaks(self) -> int:
        return self._rejected_jailbreaks

    @property
    def load_app_history(self) -> tuple[str, ...]:
        return tuple(self._load_app_history)

    def wait_for_artifice_conflict(self, timeout_s: float) -> bool:
        return self._artifice_conflict_event.wait(timeout=timeout_s)

    @property
    def transfer_rollbacks(self) -> int:
        return self._transfer_rollbacks

    def start(self) -> None:
        self._delegate.start()

    def shutdown(self) -> None:
        self._delegate.shutdown()

    def load_rom(self, timeout_s: float | None = None) -> DummyCommandResult:
        delegate_result = self._delegate.load_rom(timeout_s=timeout_s)
        payload = dict(delegate_result.payload)
        payload["path"] = str(self._rom_path_override)
        return _result("load_rom", payload)

    def boot(
        self,
        program_paths: Sequence[Path | str] = (),
        ready_sequence: Sequence[KeyAction] = (),
        timeout_s: float | None = None,
    ) -> object:
        summary = self._delegate.boot(program_paths=program_paths, ready_sequence=ready_sequence, timeout_s=timeout_s)
        loaded_entries: list[ArchiveEntry] = []
        for program_path in program_paths:
            loaded_entries.extend(_iter_transfer_entries(Path(program_path)))
        if loaded_entries:
            self._set_archive_entries(loaded_entries)
            self._active_target = _normalize_target_name(loaded_entries, None)
            self._target_is_asm = True
        return summary

    def reset(self, timeout_s: float | None = None) -> DummyCommandResult:
        delegate_result = self._delegate.reset(timeout_s=timeout_s)
        if self._transfer_snapshot is not None:
            restored_entries, restored_used_bytes, restored_target, restored_target_is_asm = self._transfer_snapshot
            self._archive_entries = list(restored_entries)
            self._archive_used_bytes = restored_used_bytes
            self._active_target = restored_target
            self._target_is_asm = restored_target_is_asm
            self._transfer_rollbacks += 1
            self._transfer_snapshot = None
        self._executing_module = ""
        self._execution_release_event = None
        return _result("reset", dict(delegate_result.payload))

    def send_sequence(
        self,
        events: Sequence[Any],
        timeout_s: float | None = None,
    ) -> list[object]:
        return self._delegate.send_sequence(events, timeout_s=timeout_s)

    def capture_screen(self, format_name: str = "crc", timeout_s: float | None = None) -> object:
        return self._delegate.capture_screen(format_name=format_name, timeout_s=timeout_s)

    def snapshot(self, path: Path | str, timeout_s: float | None = None) -> object:
        result = self._delegate.snapshot(path, timeout_s=timeout_s)
        snapshot_path = Path(path)
        if snapshot_path.suffix.lower() == ".json" and snapshot_path.exists():
            payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
            payload["transfer_files"] = [entry.name for entry in self._archive_entries]
            payload["app_count"] = len(self._archive_entries)
            payload["active_target"] = self._active_target
            payload["archive_fragment_count"] = len(self._archive_fragments)
            snapshot_path.write_text(json.dumps(payload), encoding="utf-8")
        return result

    def get_state(self, timeout_s: float | None = None) -> DummyCommandResult:
        delegate_payload = dict(self._delegate.get_state(timeout_s=timeout_s).payload)
        if self._rom_validation is not None:
            delegate_payload["os_version"] = self._rom_validation.os_version
            delegate_payload["firmware_family"] = self._rom_validation.firmware_family
            delegate_payload["rom_model"] = self._rom_validation.model
        appvar_archive_bytes = self._appvar_archive_bytes()
        archive_used_bytes = self._archive_used_bytes + appvar_archive_bytes
        delegate_payload.update(
            {
                "app_count": len(self._archive_entries),
                "target_name": self._active_target,
                "target_is_asm": self._target_is_asm,
                "archive_entry_names": [entry.name for entry in self._archive_entries],
                "archive_used_bytes": archive_used_bytes,
                "archive_capacity_bytes": self._archive_capacity_bytes,
                "archive_free_bytes": self._archive_capacity_bytes - archive_used_bytes,
                "archive_fragment_count": len(self._archive_fragments),
                "archive_fragment_bytes": list(self._archive_fragments),
                "defrag_count": self._defrag_count,
                "transfer_rollbacks": self._transfer_rollbacks,
                "last_transfer_stage": self._last_transfer_stage,
                "shared_lists": dict(self._shared_lists),
                "shared_list_sources": dict(self._shared_list_sources),
                "module_history": list(self._module_history),
                "current_heap_bytes": self._current_heap_bytes,
                "peak_heap_bytes": self._peak_heap_bytes,
                "oom_count": self._oom_count,
                "heap_corruption_detected": self._heap_corruption_detected,
                "battery_low": self._battery_low,
                "executing_module": self._executing_module,
                "queued_jailbreaks": self._queued_jailbreaks,
                "rejected_jailbreaks": self._rejected_jailbreaks,
                "appvar_count": len(self._appvars),
                "appvar_names": sorted(self._appvars),
                "appvar_bytes_used": sum(entry.size_bytes for entry in self._appvars.values()),
                "appvar_archive_bytes": appvar_archive_bytes,
                "vat_entry_count": len(self._appvars),
                "vat_tombstone_count": self._vat_tombstone_count,
                "vat_next_id": self._next_vat_id,
                "appvars": {
                    name: self._appvar_info(entry)
                    for name, entry in sorted(self._appvars.items())
                },
            }
        )
        return _result("get_state", delegate_payload)

    def create_appvar(
        self,
        name: str,
        payload: bytes | bytearray | memoryview = b"",
        *,
        archived: bool = True,
    ) -> dict[str, object]:
        normalized_name = self._normalize_appvar_name(name)
        if normalized_name in self._appvars:
            raise FileExistsError(f"AppVar '{normalized_name}' already exists.")

        payload_bytes = bytes(payload)
        required_archive_bytes = self._appvar_storage_bytes(payload_bytes, archived=archived)
        if required_archive_bytes > self._archive_free_bytes():
            raise MemoryError(
                f"AppVar '{normalized_name}' requires {required_archive_bytes} archive bytes, "
                f"but only {self._archive_free_bytes()} are free."
            )

        entry = AppVarEntry(
            name=normalized_name,
            payload=payload_bytes,
            archived=archived,
            vat_id=self._next_vat_id,
            generation=self._next_appvar_generation,
            write_count=1,
        )
        self._appvars[normalized_name] = entry
        self._next_vat_id += 1
        self._next_appvar_generation += 1
        return self._appvar_info(entry)

    def read_appvar(self, name: str) -> bytes:
        entry = self._require_appvar(name)
        return bytes(entry.payload)

    def write_appvar(
        self,
        name: str,
        payload: bytes | bytearray | memoryview,
        *,
        archived: bool | None = None,
        create_if_missing: bool = False,
    ) -> dict[str, object]:
        normalized_name = self._normalize_appvar_name(name)
        if normalized_name not in self._appvars:
            if not create_if_missing:
                raise FileNotFoundError(f"AppVar '{normalized_name}' does not exist.")
            return self.create_appvar(
                normalized_name,
                payload,
                archived=True if archived is None else archived,
            )

        entry = self._appvars[normalized_name]
        payload_bytes = bytes(payload)
        archived_flag = entry.archived if archived is None else bool(archived)
        old_archive_bytes = self._appvar_storage_bytes(entry.payload, archived=entry.archived)
        new_archive_bytes = self._appvar_storage_bytes(payload_bytes, archived=archived_flag)
        additional_archive_bytes = max(0, new_archive_bytes - old_archive_bytes)
        if additional_archive_bytes > self._archive_free_bytes():
            raise MemoryError(
                f"AppVar '{normalized_name}' write requires {additional_archive_bytes} additional archive bytes, "
                f"but only {self._archive_free_bytes()} are free."
            )

        entry.payload = payload_bytes
        entry.archived = archived_flag
        entry.write_count += 1
        return self._appvar_info(entry)

    def delete_appvar(self, name: str) -> dict[str, object]:
        entry = self._require_appvar(name)
        freed_archive_bytes = self._appvar_storage_bytes(entry.payload, archived=entry.archived)
        removed = self._appvars.pop(entry.name)
        self._vat_tombstone_count += 1
        payload = self._appvar_info(removed)
        payload["deleted"] = True
        payload["freed_archive_bytes"] = freed_archive_bytes
        return payload

    def get_appvar_info(self, name: str) -> dict[str, object]:
        return self._appvar_info(self._require_appvar(name))

    def list_appvars(self) -> tuple[dict[str, object], ...]:
        return tuple(self._appvar_info(entry) for _, entry in sorted(self._appvars.items()))

    def _send_command(
        self,
        payload: Mapping[str, Any],
        timeout_s: float | None = None,
    ) -> DummyCommandResult:
        if str(payload.get("cmd")) != "load_app":
            return _result(
                str(payload.get("cmd", "")),
                dict(self._delegate._send_command(payload, timeout_s=timeout_s).payload),  # type: ignore[attr-defined]
            )

        with self._transfer_lock:
            requested_target = _normalize_target_name(
                _iter_transfer_entries(Path(payload["path"])),
                payload.get("target_name"),
            )
            requested_target_is_asm = bool(payload.get("target_is_asm", True))
            self._handle_execution_conflict(
                target_name=requested_target,
                target_is_asm=requested_target_is_asm,
                timeout_s=timeout_s,
            )
            if self._battery_low:
                raise CEmuCommandError("load_app failed: battery too low for archive transfer")

            entries = _iter_transfer_entries(Path(payload["path"]))
            self._transfer_snapshot = (
                list(self._archive_entries),
                self._archive_used_bytes,
                self._active_target,
                self._target_is_asm,
            )
            if self._transfer_interrupt_stages:
                stage = self._transfer_interrupt_stages.pop(0)
                self._last_transfer_stage = stage
                self._apply_partial_transfer(
                    entries,
                    stage=stage,
                    target_name=requested_target,
                    target_is_asm=requested_target_is_asm,
                )
                raise CEmuProcessTimeoutError(
                    f"Synthetic power-cycle interrupted the USB transfer during stage '{stage}'."
                )

            delegate_result = self._delegate._send_command(payload, timeout_s=timeout_s)  # type: ignore[attr-defined]
            self._apply_transfer(entries, target_name=requested_target, target_is_asm=requested_target_is_asm)
            self._load_app_history.append(requested_target)
            return _result("load_app", dict(delegate_result.payload))

    def configure_transfer_interrupts(self, *stages: str) -> None:
        self._transfer_interrupt_stages = [str(stage) for stage in stages]

    def set_battery_low(self, value: bool) -> None:
        self._battery_low = value

    def swap_rom(self, rom_path: Path, validation: RomValidationResult) -> None:
        self._rom_path_override = rom_path
        self._rom_validation = validation

    def archive_digest(self) -> tuple[tuple[str, int], ...]:
        return tuple((entry.name, entry.size_bytes) for entry in self._archive_entries)

    def verify_archive_integrity(
        self,
        *,
        expected_entry_names: Sequence[str] | None = None,
    ) -> dict[str, object]:
        expected_names = list(expected_entry_names or [
            *(entry.name for entry in self._transfer_package.module_entries),
            *(entry.name for entry in self._transfer_package.runtime_entries),
        ])
        expected_name_set = set(expected_names)
        actual_names = [entry.name for entry in self._archive_entries]
        actual_name_set = set(actual_names)
        name_counts: dict[str, int] = {}
        for name in actual_names:
            name_counts[name] = name_counts.get(name, 0) + 1
        duplicate_entry_names = sorted(
            name
            for name, count in name_counts.items()
            if count > 1
        )
        expected_size_by_name = {
            entry.name: entry.size_bytes
            for entry in (*self._transfer_package.module_entries, *self._transfer_package.runtime_entries)
        }
        mismatched_size_names = sorted(
            entry.name
            for entry in self._archive_entries
            if entry.name in expected_size_by_name
            and expected_size_by_name[entry.name] != entry.size_bytes
        )
        missing_entry_names = sorted(expected_name_set - actual_name_set)
        unexpected_entry_names = sorted(actual_name_set - expected_name_set)
        return {
            "success": not duplicate_entry_names and not mismatched_size_names and not missing_entry_names and not unexpected_entry_names,
            "expected_entry_names": expected_names,
            "actual_entry_names": actual_names,
            "missing_entry_names": missing_entry_names,
            "unexpected_entry_names": unexpected_entry_names,
            "duplicate_entry_names": duplicate_entry_names,
            "mismatched_size_names": mismatched_size_names,
            "archive_digest": self.archive_digest(),
            "archive_used_bytes": self._archive_used_bytes + self._appvar_archive_bytes(),
            "archive_capacity_bytes": self._archive_capacity_bytes,
        }

    def validate_boot_state(
        self,
        *,
        expected_module_names: Sequence[str],
        expected_runtime_names: Sequence[str],
        expected_target_name: str | None = None,
        expected_target_is_asm: bool = True,
        timeout_s: float | None = None,
    ) -> dict[str, object]:
        state = dict(self.get_state(timeout_s=timeout_s).payload)
        expected_entry_names = [*expected_module_names, *expected_runtime_names]
        integrity = self.verify_archive_integrity(expected_entry_names=expected_entry_names)
        missing_module_names = sorted(
            set(expected_module_names) - set(state["archive_entry_names"])
        )
        missing_runtime_names = sorted(
            set(expected_runtime_names) - set(state["archive_entry_names"])
        )
        success = (
            bool(state.get("rom_loaded"))
            and bool(state.get("running"))
            and not missing_module_names
            and not missing_runtime_names
            and not integrity["unexpected_entry_names"]
            and not integrity["duplicate_entry_names"]
            and not integrity["mismatched_size_names"]
            and (
                expected_target_name is None
                or str(state.get("target_name", "")) == expected_target_name
            )
            and bool(state.get("target_is_asm", False)) is bool(expected_target_is_asm)
        )
        return {
            "success": success,
            "state": state,
            "integrity": integrity,
            "missing_module_names": missing_module_names,
            "missing_runtime_names": missing_runtime_names,
        }

    def set_archive_capacity_bytes(self, capacity_bytes: int) -> None:
        normalized_capacity = int(capacity_bytes)
        minimum_required = self._archive_used_bytes + self._appvar_archive_bytes()
        if normalized_capacity < minimum_required:
            raise ValueError(
                f"Archive capacity {normalized_capacity} bytes is smaller than the current in-use payload "
                f"of {minimum_required} bytes."
            )
        self._archive_capacity_bytes = normalized_capacity

    def simulate_archive_fragmentation(
        self,
        *,
        fill_ratio: float,
        delete_middle_count: int,
    ) -> dict[str, object]:
        target_used_bytes = int(self._archive_capacity_bytes * fill_ratio)
        filler_index = 0
        while self._archive_used_bytes < target_used_bytes:
            filler_size = min(4_096, max(512, target_used_bytes - self._archive_used_bytes))
            filler_entry = ArchiveEntry(name=f"FILLER{filler_index:02d}.8xp", size_bytes=filler_size)
            self._filler_entries.append(filler_entry)
            self._archive_entries.append(filler_entry)
            self._archive_used_bytes += filler_size
            filler_index += 1

        base_module_entries = [
            entry
            for entry in self._archive_entries
            if entry.name.lower().endswith(".8xp") and not entry.name.upper().startswith("FILLER")
        ]
        delete_count = min(max(delete_middle_count, 0), len(base_module_entries))
        start_index = max(0, (len(base_module_entries) // 2) - (delete_count // 2))
        deleted_entries = base_module_entries[start_index : start_index + delete_count]
        deleted_names = {entry.name for entry in deleted_entries}
        self._archive_entries = [
            entry for entry in self._archive_entries if entry.name not in deleted_names
        ]
        self._archive_fragments.extend(entry.size_bytes for entry in deleted_entries)
        self._archive_used_bytes = sum(entry.size_bytes for entry in self._archive_entries)
        return {
            "success": True,
            "fill_ratio": fill_ratio,
            "deleted_entries": [entry.name for entry in deleted_entries],
            "fragment_count": len(self._archive_fragments),
            "archive_used_bytes": self._archive_used_bytes,
            "archive_free_bytes": self._archive_capacity_bytes - self._archive_used_bytes,
        }

    def defragment_archive(self) -> dict[str, object]:
        before_digest = self.archive_digest()
        if self._battery_low:
            return {
                "success": False,
                "aborted": True,
                "reason": "battery_low",
                "archive_digest": before_digest,
                "fragment_count": len(self._archive_fragments),
            }
        self._archive_entries.sort(key=lambda entry: entry.name.lower())
        self._archive_fragments.clear()
        self._defrag_count += 1
        self._archive_used_bytes = sum(entry.size_bytes for entry in self._archive_entries)
        return {
            "success": True,
            "aborted": False,
            "archive_digest": self.archive_digest(),
            "fragment_count": 0,
            "defrag_count": self._defrag_count,
        }

    def load_modules_until_archive_rejects(
        self,
        module_names: Sequence[str],
        *,
        repeat: int = 1,
    ) -> dict[str, object]:
        normalized_repeat = max(1, int(repeat))
        requested_modules = [str(name).upper()[:8] for name in module_names if str(name).strip()]
        if not requested_modules:
            raise ValueError("At least one module name is required.")

        loaded_entry_names: list[str] = []
        loaded_count = 0
        rejected_module_name = ""
        rejected_size_bytes = 0
        synthetic_index = 0

        for _ in range(normalized_repeat):
            for module_name in requested_modules:
                module_size = self._lookup_module_size(module_name)
                projected_bytes = (
                    self._archive_used_bytes
                    + self._appvar_archive_bytes()
                    + module_size
                )
                if projected_bytes > self._archive_capacity_bytes:
                    rejected_module_name = module_name
                    rejected_size_bytes = module_size
                    return {
                        "loaded_count": loaded_count,
                        "loaded_entry_names": list(loaded_entry_names),
                        "rejected_module_name": rejected_module_name,
                        "rejected_size_bytes": rejected_size_bytes,
                        "archive_used_bytes": self._archive_used_bytes + self._appvar_archive_bytes(),
                        "archive_free_bytes": self._archive_free_bytes(),
                        "archive_capacity_bytes": self._archive_capacity_bytes,
                    }

                synthetic_name = f"{module_name[:4]}{synthetic_index:04d}.8xp"
                self._archive_entries.append(ArchiveEntry(name=synthetic_name, size_bytes=module_size))
                self._archive_used_bytes += module_size
                loaded_entry_names.append(synthetic_name)
                loaded_count += 1
                synthetic_index += 1

        return {
            "loaded_count": loaded_count,
            "loaded_entry_names": list(loaded_entry_names),
            "rejected_module_name": rejected_module_name,
            "rejected_size_bytes": rejected_size_bytes,
            "archive_used_bytes": self._archive_used_bytes + self._appvar_archive_bytes(),
            "archive_free_bytes": self._archive_free_bytes(),
            "archive_capacity_bytes": self._archive_capacity_bytes,
        }

    def execute_module(
        self,
        module_name: str,
        *,
        action: str,
        list_name: str = "L1",
        list_values: Sequence[int] = (),
        scratch_value: str = "",
        heap_multiplier: float = 1.0,
    ) -> dict[str, object]:
        normalized_name = module_name.upper()
        self._assert_module_present(normalized_name)
        self._module_history.append(normalized_name)
        self._active_target = normalized_name
        self._target_is_asm = True
        module_state = self._module_states.setdefault(normalized_name, {})
        allocation_bytes = max(
            self._lookup_module_size(normalized_name),
            int(self._lookup_module_size(normalized_name) * max(heap_multiplier, 1.0)),
        )

        self._current_heap_bytes += allocation_bytes
        self._peak_heap_bytes = max(self._peak_heap_bytes, self._current_heap_bytes)
        if self._current_heap_bytes > self._heap_limit_bytes:
            self._oom_count += 1
            self._heap_corruption_detected = True
            self._current_heap_bytes -= allocation_bytes
            raise MemoryError(
                f"Module '{normalized_name}' exceeded the simulated heap limit during action '{action}'."
            )

        try:
            if action == "write_scratch":
                module_state["scratch"] = scratch_value
                return self._module_result(
                    normalized_name,
                    action,
                    scratch=module_state["scratch"],
                    allocation_bytes=allocation_bytes,
                )
            if action == "read_scratch":
                return self._module_result(
                    normalized_name,
                    action,
                    scratch=str(module_state.get("scratch", "")),
                    allocation_bytes=allocation_bytes,
                )
            if action == "produce_list":
                self._shared_lists[list_name] = [int(value) for value in list_values]
                self._shared_list_sources[list_name] = normalized_name
                return self._module_result(
                    normalized_name,
                    action,
                    shared_list=list(self._shared_lists[list_name]),
                    source_module=normalized_name,
                    allocation_bytes=allocation_bytes,
                )
            if action == "consume_list":
                values = list(self._shared_lists.get(list_name, ()))
                if not values:
                    raise KeyError(
                        f"Module '{normalized_name}' could not find shared list '{list_name}'."
                    )
                return self._module_result(
                    normalized_name,
                    action,
                    shared_list=values,
                    source_module=self._shared_list_sources.get(list_name, ""),
                    checksum=sum(values),
                    allocation_bytes=allocation_bytes,
                )
            if action == "self_test":
                return self._module_result(
                    normalized_name,
                    action,
                    allocation_bytes=allocation_bytes,
                )
            if action == "allocate_heap":
                return self._module_result(
                    normalized_name,
                    action,
                    allocation_bytes=allocation_bytes,
                )
            raise ValueError(f"Unsupported simulated module action '{action}'.")
        finally:
            self._current_heap_bytes -= allocation_bytes

    def hold_module_execution(
        self,
        module_name: str,
        *,
        started_event: threading.Event,
        release_event: threading.Event,
    ) -> None:
        normalized_name = module_name.upper()
        self._assert_module_present(normalized_name)
        with self._execution_lock:
            self._executing_module = normalized_name
            self._execution_release_event = release_event
            self._module_history.append(normalized_name)
            self._active_target = normalized_name
            started_event.set()
            release_event.wait(timeout=5.0)
            self._executing_module = ""
            self._execution_release_event = None

    def _handle_execution_conflict(
        self,
        *,
        target_name: str,
        target_is_asm: bool,
        timeout_s: float | None,
    ) -> None:
        if target_name != "A" or target_is_asm or not self._executing_module:
            return
        self._artifice_conflict_event.set()
        if not self._queue_artifice_requests:
            self._rejected_jailbreaks += 1
            raise CEmuCommandError(
                f"load_app failed: cannot start arTIfiCE while module '{self._executing_module}' is executing"
            )
        self._queued_jailbreaks += 1
        release_event = self._execution_release_event
        if release_event is None:
            raise CEmuCommandError("load_app failed: execution queue was requested without a release event")
        wait_timeout = 2.0 if timeout_s is None else max(0.1, float(timeout_s))
        if not release_event.wait(wait_timeout):
            raise CEmuProcessTimeoutError(
                "Queued arTIfiCE transfer timed out while waiting for the executing module to yield."
            )

    def _apply_partial_transfer(
        self,
        entries: Sequence[ArchiveEntry],
        *,
        stage: str,
        target_name: str,
        target_is_asm: bool,
    ) -> None:
        stage_map = {
            "handshake": max(1, len(entries) // 8),
            "payload": max(1, len(entries) // 2),
            "commit": max(1, len(entries) - 1),
        }
        partial_count = stage_map.get(stage, max(1, len(entries) // 3))
        self._set_archive_entries(entries[:partial_count])
        self._active_target = target_name
        self._target_is_asm = target_is_asm

    def _apply_transfer(
        self,
        entries: Sequence[ArchiveEntry],
        *,
        target_name: str,
        target_is_asm: bool,
    ) -> None:
        self._set_archive_entries(entries)
        self._active_target = target_name
        self._target_is_asm = target_is_asm
        self._transfer_snapshot = None
        self._last_transfer_stage = ""

    def _set_archive_entries(self, entries: Sequence[ArchiveEntry]) -> None:
        required_bytes = sum(entry.size_bytes for entry in entries) + self._appvar_archive_bytes()
        if required_bytes > self._archive_capacity_bytes:
            raise CEmuCommandError(
                f"load_app failed: archive requires {required_bytes} bytes, "
                f"but capacity is limited to {self._archive_capacity_bytes} bytes"
            )
        self._archive_entries = list(entries)
        self._archive_used_bytes = sum(entry.size_bytes for entry in entries)

    def _normalize_appvar_name(self, name: str) -> str:
        normalized = str(name).strip().upper()
        if not normalized:
            raise ValueError("AppVar name must not be empty.")
        if len(normalized) > 8:
            raise ValueError(f"AppVar name '{normalized}' exceeds the 8-character VAT limit.")
        return normalized

    def _require_appvar(self, name: str) -> AppVarEntry:
        normalized_name = self._normalize_appvar_name(name)
        try:
            return self._appvars[normalized_name]
        except KeyError as exc:
            raise FileNotFoundError(f"AppVar '{normalized_name}' does not exist.") from exc

    def _appvar_storage_bytes(self, payload: bytes, *, archived: bool) -> int:
        if not archived:
            return 0
        return len(payload) + self._APPVAR_ARCHIVE_OVERHEAD_BYTES

    def _appvar_archive_bytes(self) -> int:
        return sum(
            self._appvar_storage_bytes(entry.payload, archived=entry.archived)
            for entry in self._appvars.values()
        )

    def _archive_free_bytes(self) -> int:
        return self._archive_capacity_bytes - self._archive_used_bytes - self._appvar_archive_bytes()

    def _appvar_info(self, entry: AppVarEntry) -> dict[str, object]:
        return {
            "name": entry.name,
            "size_bytes": entry.size_bytes,
            "archived": entry.archived,
            "archive_bytes": self._appvar_storage_bytes(entry.payload, archived=entry.archived),
            "vat_id": entry.vat_id,
            "generation": entry.generation,
            "write_count": entry.write_count,
            "payload_hex": entry.payload.hex().upper(),
        }

    def _assert_module_present(self, module_name: str) -> None:
        present_names = {Path(entry.name).stem.upper() for entry in self._archive_entries if entry.name.lower().endswith(".8xp")}
        if module_name[:8] not in present_names:
            raise KeyError(f"Module '{module_name}' is not present in the simulated archive.")

    def _lookup_module_size(self, module_name: str) -> int:
        normalized = module_name[:8]
        for entry in self._transfer_package.module_entries:
            if Path(entry.name).stem.upper() == normalized:
                return entry.size_bytes
        return 4096

    def _module_result(
        self,
        module_name: str,
        action: str,
        *,
        allocation_bytes: int,
        scratch: str = "",
        shared_list: Sequence[int] = (),
        source_module: str = "",
        checksum: int = 0,
    ) -> dict[str, object]:
        crc_seed = [module_name, action]
        if scratch:
            crc_seed.append(scratch)
        if shared_list:
            crc_seed.extend(str(value) for value in shared_list)
        return {
            "module_name": module_name,
            "action": action,
            "scratch": scratch,
            "shared_list": list(shared_list),
            "source_module": source_module,
            "checksum": checksum,
            "screen_crc": fake_crc_for_keys(*crc_seed),
            "allocation_bytes": allocation_bytes,
            "heap_after_bytes": self._current_heap_bytes - allocation_bytes,
            "heap_peak_bytes": self._peak_heap_bytes,
            "heap_corruption_detected": self._heap_corruption_detected,
        }

    def __getattr__(self, name: str) -> Any:
        return getattr(self._delegate, name)


def build_fake_cemu_stress_runner(
    tmp_path: Path,
    rom_path: Path,
    *,
    transfer_package: TransferPackage,
    command_timeout_s: float = 3.0,
    boot_timeout_s: float = 5.0,
) -> FakeCEmuStressRunner:
    tmp_path.mkdir(parents=True, exist_ok=True)
    delegate = build_fake_cemu_runner(
        tmp_path,
        rom_path,
        command_timeout_s=command_timeout_s,
        boot_timeout_s=boot_timeout_s,
    )
    return FakeCEmuStressRunner(delegate, transfer_package)
