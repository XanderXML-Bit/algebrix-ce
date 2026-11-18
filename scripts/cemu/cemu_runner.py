from __future__ import annotations

import json
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, MutableMapping, Sequence


CYCLES_PER_MILLISECOND = 48_000


class CEmuRunnerError(RuntimeError):
    """Base exception for CEmu runner failures."""


class CEmuBinaryNotFoundError(CEmuRunnerError):
    """Raised when the configured emulator binary is missing."""


class CEmuProcessTimeoutError(CEmuRunnerError):
    """Raised when the emulator does not respond within the configured timeout."""


class CEmuCommandError(CEmuRunnerError):
    """Raised when the emulator rejects a command."""


class CEmuProcessExitedError(CEmuRunnerError):
    """Raised when the emulator exits unexpectedly before a command can complete."""

    def __init__(
        self,
        message: str,
        *,
        returncode: int | None,
        stdout_tail: Sequence[str] = (),
        stderr_tail: Sequence[str] = (),
    ) -> None:
        self.returncode = returncode
        self.stdout_tail = tuple(stdout_tail)
        self.stderr_tail = tuple(stderr_tail)
        super().__init__(message)


class CEmuRomLoadError(CEmuCommandError):
    """Raised when CEmu rejects the configured ROM image."""


class CEmuArchiveTransferError(CEmuCommandError):
    """Raised when a transfer into the calculator archive fails."""


class CEmuArchiveCapacityError(CEmuArchiveTransferError):
    """Raised when the requested archive transfer exceeds available capacity."""


@dataclass(frozen=True)
class CEmuRunnerConfig:
    executable_path: Path | str
    rom_path: Path | str
    autotester_path: Path | str | None = None
    arguments: tuple[str, ...] = ()
    cwd: Path | str | None = None
    env: Mapping[str, str] | None = None
    startup_timeout_s: float = 10.0
    command_timeout_s: float = 15.0
    shutdown_timeout_s: float = 5.0
    boot_timeout_s: float = 20.0
    ready_poll_interval_s: float = 0.1
    default_boot_delay_ms: int = 4000
    create_no_window: bool = True

    def resolved_command(self) -> list[str]:
        executable = Path(self.executable_path)
        return [str(executable), *self.arguments]

    def resolved_cwd(self) -> str | None:
        return str(Path(self.cwd)) if self.cwd is not None else None

    def resolved_env(self) -> MutableMapping[str, str]:
        merged = dict(os.environ)
        if self.env:
            merged.update(self.env)
        return merged


@dataclass(frozen=True)
class CEmuCommandResult:
    command: str
    payload: Mapping[str, Any]
    raw: Mapping[str, Any]


@dataclass(frozen=True)
class CEmuSessionSummary:
    boot_result: CEmuCommandResult
    loaded_programs: tuple[Path, ...]
    state: Mapping[str, Any]
    boot_duration_ms: int
    wait_for_boot_duration_ms: int
    boot_state_poll_count: int


@dataclass(frozen=True)
class CEmuBootWaitResult:
    state_result: CEmuCommandResult
    duration_ms: int
    poll_count: int


def _normalize_event(event: Any) -> tuple[str, str | None, int]:
    if hasattr(event, "kind"):
        kind = str(getattr(event, "kind"))
        key = getattr(event, "key", None)
        duration_ms = int(getattr(event, "duration_ms", 0))
        return kind, key, duration_ms

    if isinstance(event, str):
        return "key", event, 0

    if isinstance(event, Mapping):
        kind = str(event.get("kind", "key"))
        key = event.get("key")
        duration_ms = int(event.get("duration_ms", 0))
        return kind, None if key is None else str(key), duration_ms

    raise TypeError(f"Unsupported key event: {event!r}")


def _running_under_wsl() -> bool:
    if os.environ.get("WSL_DISTRO_NAME"):
        return True
    try:
        release = os.uname().release.lower()
    except AttributeError:
        return False
    return "microsoft" in release or "wsl" in release


def _path_for_host_process(path: Path | str, executable_path: Path | str) -> str:
    normalized = str(Path(path))
    executable = str(Path(executable_path))
    if not (_running_under_wsl() and executable.lower().endswith('.exe')):
        return normalized
    parts = Path(normalized).parts
    if len(parts) >= 4 and parts[0] == '/' and parts[1] == 'mnt' and len(parts[2]) == 1:
        drive = parts[2].upper()
        remainder = '\\'.join(parts[3:])
        return f"{drive}:\\{remainder}" if remainder else f"{drive}:\\"
    return normalized


def _path_is_host_accessible(path: Path | str, executable_path: Path | str) -> bool:
    executable = str(Path(executable_path))
    candidate = Path(path)
    if not (_running_under_wsl() and executable.lower().endswith('.exe')):
        return True
    parts = candidate.parts
    return len(parts) >= 4 and parts[0] == '/' and parts[1] == 'mnt' and len(parts[2]) == 1


def _host_accessible_temp_roots(executable_path: Path | str) -> tuple[Path, ...]:
    executable = Path(executable_path)
    if not (_running_under_wsl() and executable.suffix.lower() == ".exe"):
        return ()

    candidates: list[Path] = []
    parts = executable.parts
    if len(parts) >= 4 and parts[0] == "/" and parts[1] == "mnt" and len(parts[2]) == 1:
        drive_root = Path("/mnt") / parts[2]
        if len(parts) >= 6 and parts[3].lower() == "users":
            candidates.append(drive_root / "Users" / parts[4] / "AppData" / "Local" / "Temp")
        candidates.append(drive_root / "Temp")
    candidates.append(executable.parent)

    unique_candidates: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        candidate_text = str(candidate)
        if candidate_text in seen:
            continue
        seen.add(candidate_text)
        unique_candidates.append(candidate)
    return tuple(unique_candidates)


def create_host_accessible_temp_dir(prefix: str, executable_path: Path | str) -> Path:
    for root in _host_accessible_temp_roots(executable_path):
        try:
            root.mkdir(parents=True, exist_ok=True)
            return Path(tempfile.mkdtemp(prefix=prefix, dir=str(root)))
        except OSError:
            continue
    return Path(tempfile.mkdtemp(prefix=prefix))


class CEmuRunner:
    def __init__(
        self,
        config: CEmuRunnerConfig,
        process_factory: Callable[..., subprocess.Popen[str]] | None = None,
    ) -> None:
        self.config = config
        self._process_factory = process_factory or subprocess.Popen
        self._process: subprocess.Popen[str] | None = None
        self._stdout_queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self._stdout_backlog: deque[dict[str, Any]] = deque()
        self._stdout_lines: list[str] = []
        self._stderr_lines: list[str] = []
        self._reader_threads: list[threading.Thread] = []
        self._ready_event = threading.Event()
        self._command_lock = threading.Lock()
        self._loaded_programs: list[Path] = []

    @property
    def executable_path(self) -> Path:
        return Path(self.config.executable_path)

    @property
    def rom_path(self) -> Path:
        return Path(self.config.rom_path)

    @property
    def autotester_path(self) -> Path | None:
        if self.config.autotester_path is None:
            return None
        return Path(self.config.autotester_path)

    @property
    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    @property
    def stdout_lines(self) -> tuple[str, ...]:
        return tuple(self._stdout_lines)

    @property
    def stderr_lines(self) -> tuple[str, ...]:
        return tuple(self._stderr_lines)

    def create_host_accessible_temp_dir(self, prefix: str) -> Path:
        return create_host_accessible_temp_dir(prefix, self.executable_path)

    @classmethod
    def default_binary_candidates(cls, repo_root: Path | str | None = None) -> list[Path]:
        candidates: list[Path] = []
        env_value = os.environ.get("CEMU_HEADLESS_PATH") or os.environ.get("CEMU_BINARY_PATH")
        if env_value:
            candidates.append(Path(env_value))

        if repo_root is not None:
            root = Path(repo_root)
            # Native cmake build (NMake / build.ps1 output)
            candidates.append(root / "tools" / "emulator" / "cemu-headless" / "cemu-headless.exe")
            candidates.append(root / "tools" / "emulator" / "cemu-headless" / "build_native" / "cemu-headless.exe")
            # MSBuild multi-config output (Visual Studio generator)
            candidates.append(root / "tools" / "emulator" / "cemu-headless" / "build" / "Release" / "cemu-headless.exe")
            # POSIX / Linux single-config output
            candidates.append(root / "tools" / "emulator" / "cemu-headless" / "build" / "cemu-headless")

        return candidates

    @classmethod
    def resolve_default_binary(cls, repo_root: Path | str | None = None) -> Path | None:
        for candidate in cls.default_binary_candidates(repo_root=repo_root):
            if candidate.exists():
                return candidate
        return None

    def start(self) -> None:
        if self.is_running:
            return

        executable = self.executable_path
        if not executable.exists():
            raise CEmuBinaryNotFoundError(f"CEmu binary does not exist: {executable}")
        if not self.rom_path.exists():
            raise CEmuBinaryNotFoundError(f"ROM path does not exist: {self.rom_path}")
        if self.autotester_path is not None and not self.autotester_path.exists():
            raise CEmuBinaryNotFoundError(f"AutoTester path does not exist: {self.autotester_path}")

        creation_flags = 0
        if os.name == "nt":
            creation_flags |= getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            if self.config.create_no_window:
                creation_flags |= getattr(subprocess, "CREATE_NO_WINDOW", 0)

        self._ready_event.clear()
        self._stdout_lines.clear()
        self._stderr_lines.clear()
        self._stdout_backlog.clear()
        self._loaded_programs.clear()

        self._process = self._process_factory(
            self.config.resolved_command(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            cwd=self.config.resolved_cwd(),
            env=self.config.resolved_env(),
            creationflags=creation_flags,
        )

        self._start_reader_threads()
        self._wait_for_ready(timeout_s=self.config.startup_timeout_s)
        self.get_state(timeout_s=self.config.command_timeout_s)

    def _start_reader_threads(self) -> None:
        assert self._process is not None
        assert self._process.stdout is not None
        assert self._process.stderr is not None

        self._reader_threads = [
            threading.Thread(
                target=self._read_stdout,
                args=(self._process.stdout,),
                name="cemu-runner-stdout",
                daemon=True,
            ),
            threading.Thread(
                target=self._read_stderr,
                args=(self._process.stderr,),
                name="cemu-runner-stderr",
                daemon=True,
            ),
        ]
        for thread in self._reader_threads:
            thread.start()

    def _read_stdout(self, pipe: Any) -> None:
        for raw_line in iter(pipe.readline, ""):
            line = raw_line.rstrip("\r\n")
            if not line:
                continue
            self._stdout_lines.append(line)
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(payload, dict):
                self._stdout_queue.put(payload)

    def _read_stderr(self, pipe: Any) -> None:
        for raw_line in iter(pipe.readline, ""):
            line = raw_line.rstrip("\r\n")
            if not line:
                continue
            self._stderr_lines.append(line)
            if "ready" in line.lower():
                self._ready_event.set()

    def _wait_for_ready(self, timeout_s: float) -> None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self._ready_event.wait(timeout=self.config.ready_poll_interval_s):
                return
            if self._process is None:
                break
            if self._process.poll() is not None:
                raise self._build_process_exited_error("CEmu exited before reporting ready state.")
        raise CEmuProcessTimeoutError(
            f"CEmu did not report a ready state within {timeout_s:.1f}s."
        )

    def _tail_lines(self, lines: Sequence[str], limit: int = 20) -> tuple[str, ...]:
        return tuple(str(line) for line in lines[-limit:] if str(line).strip())

    def _build_process_exited_error(self, message: str) -> CEmuProcessExitedError:
        returncode = self._process.poll() if self._process is not None else None
        stdout_tail = self._tail_lines(self._stdout_lines)
        stderr_tail = self._tail_lines(self._stderr_lines)
        detail_parts = [message]
        if returncode is not None:
            detail_parts.append(f"Return code: {returncode}.")
        if stdout_tail:
            detail_parts.append("Stdout tail: " + " | ".join(stdout_tail))
        if stderr_tail:
            detail_parts.append("Stderr tail: " + " | ".join(stderr_tail))
        return CEmuProcessExitedError(
            " ".join(detail_parts),
            returncode=returncode,
            stdout_tail=stdout_tail,
            stderr_tail=stderr_tail,
        )

    def _classify_command_error(self, command_name: str, error_message: str) -> CEmuCommandError:
        normalized_message = error_message.lower()
        if command_name == "load_rom" and any(
            token in normalized_message
            for token in ("corrupt", "invalid", "signature", "firmware", "rom", "checksum")
        ):
            return CEmuRomLoadError(f"{command_name} failed: {error_message}")
        if command_name == "load_app":
            if any(
                token in normalized_message
                for token in ("archive requires", "capacity", "too large", "out of memory")
            ):
                return CEmuArchiveCapacityError(f"{command_name} failed: {error_message}")
            return CEmuArchiveTransferError(f"{command_name} failed: {error_message}")
        return CEmuCommandError(f"{command_name} failed: {error_message}")

    def _send_command(
        self,
        payload: Mapping[str, Any],
        timeout_s: float | None = None,
    ) -> CEmuCommandResult:
        if not self.is_running:
            raise CEmuRunnerError("CEmu process is not running.")

        timeout = self.config.command_timeout_s if timeout_s is None else timeout_s
        command_name = str(payload["cmd"])

        assert self._process is not None
        assert self._process.stdin is not None

        encoded = json.dumps(payload, separators=(",", ":"))
        with self._command_lock:
            try:
                self._process.stdin.write(encoded + "\n")
                self._process.stdin.flush()
            except (BrokenPipeError, OSError) as exc:
                if self._process.poll() is not None:
                    raise self._build_process_exited_error(
                        f"CEmu exited before accepting '{command_name}'."
                    ) from exc
                raise CEmuRunnerError(
                    f"CEmu could not accept '{command_name}': {exc}"
                ) from exc
            raw = self._wait_for_command_response(command_name=command_name, timeout_s=timeout)

        if not raw.get("ok", False):
            error_message = str(raw.get("error", "unknown CEmu error"))
            raise self._classify_command_error(command_name, error_message)

        return CEmuCommandResult(command=command_name, payload=raw, raw=raw)

    def _wait_for_command_response(self, command_name: str, timeout_s: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for _ in range(len(self._stdout_backlog)):
                candidate = self._stdout_backlog.popleft()
                if candidate.get("cmd") == command_name:
                    return candidate
                self._stdout_backlog.append(candidate)

            remaining = max(0.0, deadline - time.monotonic())
            try:
                message = self._stdout_queue.get(timeout=min(0.1, remaining))
            except queue.Empty:
                if self._process is not None and self._process.poll() is not None:
                    raise self._build_process_exited_error(
                        f"CEmu exited while waiting for '{command_name}'."
                    )
                continue

            if message.get("cmd") == command_name:
                return message
            self._stdout_backlog.append(message)

        raise CEmuProcessTimeoutError(
            f"CEmu did not respond to '{command_name}' within {timeout_s:.1f}s."
        )

    def load_rom(self, timeout_s: float | None = None) -> CEmuCommandResult:
        payload: dict[str, Any] = {
            "cmd": "load_rom",
            "path": _path_for_host_process(self.rom_path, self.executable_path),
        }
        if self.autotester_path is not None:
            payload["autotester_path"] = _path_for_host_process(self.autotester_path, self.executable_path)
        return self._send_command(payload, timeout_s=timeout_s)

    def load_program(self, program_path: Path | str, timeout_s: float | None = None) -> CEmuCommandResult:
        result = self._send_command(
            {"cmd": "load_app", "path": _path_for_host_process(Path(program_path), self.executable_path)},
            timeout_s=timeout_s,
        )
        self._loaded_programs.append(Path(program_path))
        return result

    def load_program_with_target(
        self,
        program_path: Path | str,
        *,
        target_name: str,
        target_is_asm: bool,
        timeout_s: float | None = None,
    ) -> CEmuCommandResult:
        result = self._send_command(
            {
                "cmd": "load_app",
                "path": _path_for_host_process(Path(program_path), self.executable_path),
                "target_name": target_name,
                "target_is_asm": target_is_asm,
            },
            timeout_s=timeout_s,
        )
        self._loaded_programs.append(Path(program_path))
        return result

    def load_programs(
        self,
        program_paths: Iterable[Path | str],
        timeout_s: float | None = None,
    ) -> list[CEmuCommandResult]:
        return [self.load_program(path, timeout_s=timeout_s) for path in program_paths]

    def step_cycles(self, cycles: int, timeout_s: float | None = None) -> CEmuCommandResult:
        if cycles <= 0:
            raise ValueError("cycles must be positive")
        return self._send_command({"cmd": "step", "cycles": cycles}, timeout_s=timeout_s)

    def step_delay_ms(self, delay_ms: int, timeout_s: float | None = None) -> CEmuCommandResult:
        if delay_ms <= 0:
            raise ValueError("delay_ms must be positive")
        return self.step_cycles(delay_ms * CYCLES_PER_MILLISECOND, timeout_s=timeout_s)

    def inject_key(
        self,
        key: str,
        duration_ms: int = 0,
        timeout_s: float | None = None,
    ) -> CEmuCommandResult:
        payload = {"cmd": "inject_key", "key": key}
        if duration_ms > 0:
            payload["duration_ms"] = duration_ms
        return self._send_command(payload, timeout_s=timeout_s)

    def send_sequence(
        self,
        events: Sequence[Any],
        timeout_s: float | None = None,
    ) -> list[CEmuCommandResult]:
        results: list[CEmuCommandResult] = []
        for event in events:
            kind, key, duration_ms = _normalize_event(event)
            if kind == "key":
                if key is None:
                    raise ValueError(f"Key event is missing a key: {event!r}")
                results.append(self.inject_key(key=key, duration_ms=duration_ms, timeout_s=timeout_s))
                continue
            if kind == "pause":
                results.append(self.step_delay_ms(delay_ms=duration_ms, timeout_s=timeout_s))
                continue
            raise ValueError(f"Unsupported event kind '{kind}' in sequence.")
        return results

    def capture_screen(self, format_name: str = "crc", timeout_s: float | None = None) -> CEmuCommandResult:
        return self._send_command({"cmd": "get_screen", "format": format_name}, timeout_s=timeout_s)

    def snapshot(self, path: Path | str, timeout_s: float | None = None) -> CEmuCommandResult:
        requested_path = Path(path)
        if _path_is_host_accessible(requested_path, self.executable_path):
            return self._send_command(
                {"cmd": "snapshot", "path": _path_for_host_process(requested_path, self.executable_path)},
                timeout_s=timeout_s,
            )

        requested_path.parent.mkdir(parents=True, exist_ok=True)
        host_stage_dir = self.create_host_accessible_temp_dir(prefix="calcengine-snapshot-")
        staged_path = host_stage_dir / requested_path.name
        try:
            result = self._send_command(
                {"cmd": "snapshot", "path": _path_for_host_process(staged_path, self.executable_path)},
                timeout_s=timeout_s,
            )
            if not staged_path.exists():
                raise CEmuCommandError(
                    "CEmu reported a successful snapshot, but the host-accessible artifact was not created: "
                    f"{staged_path}"
                )
            shutil.copy2(staged_path, requested_path)
            rewritten_payload = dict(result.payload)
            rewritten_payload["path"] = str(requested_path)
            rewritten_raw = dict(result.raw)
            rewritten_raw["path"] = str(requested_path)
            return CEmuCommandResult(
                command=result.command,
                payload=rewritten_payload,
                raw=rewritten_raw,
            )
        finally:
            shutil.rmtree(host_stage_dir, ignore_errors=True)

    def get_state(self, timeout_s: float | None = None) -> CEmuCommandResult:
        return self._send_command({"cmd": "get_state"}, timeout_s=timeout_s)

    def reset(self, timeout_s: float | None = None) -> CEmuCommandResult:
        return self._send_command({"cmd": "reset"}, timeout_s=timeout_s)

    def boot(
        self,
        program_paths: Iterable[Path | str] = (),
        ready_sequence: Sequence[Any] = (),
        timeout_s: float | None = None,
    ) -> CEmuSessionSummary:
        boot_started_at = time.perf_counter()
        if not self.is_running:
            self.start()

        boot_timeout = self.config.boot_timeout_s if timeout_s is None else timeout_s
        load_result = self.load_rom(timeout_s=boot_timeout)
        self.load_programs(program_paths, timeout_s=boot_timeout)

        if ready_sequence:
            self.send_sequence(ready_sequence, timeout_s=boot_timeout)
        else:
            self.step_delay_ms(self.config.default_boot_delay_ms, timeout_s=boot_timeout)

        wait_result = self.wait_for_boot_details(timeout_s=boot_timeout)
        return CEmuSessionSummary(
            boot_result=load_result,
            loaded_programs=tuple(self._loaded_programs),
            state=wait_result.state_result.payload,
            boot_duration_ms=round((time.perf_counter() - boot_started_at) * 1000),
            wait_for_boot_duration_ms=wait_result.duration_ms,
            boot_state_poll_count=wait_result.poll_count,
        )

    def wait_for_boot(self, timeout_s: float | None = None) -> CEmuCommandResult:
        return self.wait_for_boot_details(timeout_s=timeout_s).state_result

    def wait_for_boot_details(self, timeout_s: float | None = None) -> CEmuBootWaitResult:
        boot_timeout = self.config.boot_timeout_s if timeout_s is None else timeout_s
        deadline = time.monotonic() + boot_timeout
        last_state: CEmuCommandResult | None = None
        started_at = time.perf_counter()
        poll_count = 0

        while time.monotonic() < deadline:
            poll_count += 1
            last_state = self.get_state(timeout_s=min(2.0, boot_timeout))
            payload = last_state.payload
            if payload.get("rom_loaded") and payload.get("running") and not payload.get("halted"):
                return CEmuBootWaitResult(
                    state_result=last_state,
                    duration_ms=round((time.perf_counter() - started_at) * 1000),
                    poll_count=poll_count,
                )
            time.sleep(self.config.ready_poll_interval_s)

        if last_state is None:
            raise CEmuProcessTimeoutError("Boot polling did not receive any state responses.")
        raise CEmuProcessTimeoutError(
            f"CEmu did not reach a running boot state within {boot_timeout:.1f}s. "
            f"Last state: {json.dumps(last_state.payload, sort_keys=True)}"
        )

    def shutdown(self) -> None:
        process = self._process
        if process is None:
            return

        try:
            if process.poll() is None:
                try:
                    self._send_command({"cmd": "exit"}, timeout_s=min(2.0, self.config.shutdown_timeout_s))
                except CEmuRunnerError:
                    pass

                try:
                    process.wait(timeout=self.config.shutdown_timeout_s)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=max(1.0, self.config.shutdown_timeout_s / 2))
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2.0)
        finally:
            self._process = None

    def __enter__(self) -> "CEmuRunner":
        self.start()
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.shutdown()
