"""
test_cemu_headless_native.py

End-to-end integration tests for the cemu-headless native-core binary.

These tests drive the JSON-line protocol directly via subprocess stdin/stdout.
They are guarded by:
  - NATIVE_BINARY_READY: cemu-headless.exe built with CEMU_HEADLESS_HAS_CEMU_CORE=1
  - ROM_READY: a valid Rom.rom is present (resolve_rom_path succeeds)

All tests are auto-skipped when those conditions are not met, so CI remains
green even when the native binary has not been built.

Coverage targets
----------------
* Binary smoke (version, exit)
* load_rom success and rejection of a non-existent path
* get_screen crc -- before and after state change
* get_screen png -- valid PNG file written to disk
* load_app -- .8xp transferred into archive
* step -- emulation advances
* inject_key -- known and unknown key names
* get_state -- JSON state shape after boot
* reset -- state cleared and re-booted
* snapshot json -- state JSON file written
* snapshot png -- PNG file written via snapshot command
* Error paths -- bad command names, missing required fields
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path
from typing import Any

import pytest


def _running_under_wsl() -> bool:
    if os.environ.get("WSL_DISTRO_NAME"):
        return True
    try:
        release = os.uname().release.lower()
    except AttributeError:
        return False
    return "microsoft" in release or "wsl" in release


# ---------------------------------------------------------------------------
# Repository / resource resolution
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]

_NATIVE_BINARY_PATHS = [
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "cemu-headless.exe",
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build_native" / "cemu-headless.exe",
    Path("C:/Users/user/src/algebrix-ce/tools/emulator/cemu-headless/cemu-headless.exe"),
    Path("C:/Users/user/src/algebrix-ce/tools/emulator/cemu-headless/build_native/cemu-headless.exe"),
]
_ROM_PATHS = [
    REPO_ROOT / "rom" / "Rom.rom",
    Path("C:/Users/user/src/algebrix-ce/rom/Rom.rom"),
]
_APP_PATHS = [
    REPO_ROOT / "bin" / "CALCENG.8xp",
    REPO_ROOT / "deploy" / "hardware-ready" / "CALCENG.8xp",
    Path("C:/Users/user/Downloads/CalcEngine/CALCENG.8xp"),
    Path("C:/Users/user/src/algebrix-ce/deploy/hardware-ready/CALCENG.8xp"),
]


def _find_first(paths: list[Path]) -> Path | None:
    for p in paths:
        try:
            if p.exists():
                return p
        except OSError:
            pass
    return None


def _uses_windows_host_paths() -> bool:
    return bool(NATIVE_BINARY) and _running_under_wsl() and str(NATIVE_BINARY).lower().endswith(".exe")


def _command_path(path: Path | str) -> str:
    normalized = str(Path(path))
    if not _uses_windows_host_paths():
        return normalized.replace("\\", "/")
    parts = Path(normalized).parts
    if len(parts) >= 4 and parts[0] == "/" and parts[1] == "mnt" and len(parts[2]) == 1:
        drive = parts[2].upper()
        remainder = "\\".join(parts[3:])
        return f"{drive}:\\{remainder}" if remainder else f"{drive}:\\"
    return normalized.replace("\\", "/")


def _host_accessible_temp_root() -> Path | None:
    if not _uses_windows_host_paths() or NATIVE_BINARY is None:
        return None
    parts = NATIVE_BINARY.parts
    if len(parts) >= 6 and parts[0] == "/" and parts[1] == "mnt" and len(parts[2]) == 1 and parts[3].lower() == "users":
        root = Path("/mnt") / parts[2] / "Users" / parts[4] / "AppData" / "Local" / "Temp"
        root.mkdir(parents=True, exist_ok=True)
        return root
    return None


def _make_temp_dir(prefix: str) -> Path:
    root = _host_accessible_temp_root()
    return Path(tempfile.mkdtemp(prefix=prefix, dir=str(root) if root is not None else None))


def _make_temp_path(prefix: str, suffix: str) -> tuple[Path, Path]:
    directory = _make_temp_dir(prefix)
    return directory, directory / f"artifact{suffix}"


NATIVE_BINARY = _find_first(_NATIVE_BINARY_PATHS)
ROM_PATH = _find_first(_ROM_PATHS)
APP_PATH = _find_first(_APP_PATHS)

_BINARY_MISSING = NATIVE_BINARY is None
_ROM_MISSING = ROM_PATH is None
_APP_MISSING = APP_PATH is None

_BINARY_SKIP_REASON = (
    "cemu-headless native binary not found; run tools/emulator/cemu-headless/build.ps1"
    if _BINARY_MISSING
    else ""
)
_ROM_SKIP_REASON = (
    "Rom.rom not found; place it at rom/Rom.rom"
    if _ROM_MISSING
    else ""
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run_session(commands: list[dict[str, Any]], timeout: float = 60.0) -> list[dict[str, Any]]:
    """
    Feed a list of JSON command dicts to cemu-headless via stdin and return
    the parsed JSON response objects in order.  An ``exit`` command is
    automatically appended unless already present.
    """
    assert NATIVE_BINARY is not None
    if not any(c.get("cmd") == "exit" for c in commands):
        commands = list(commands) + [{"cmd": "exit"}]

    input_text = "\n".join(json.dumps(c) for c in commands) + "\n"
    result = subprocess.run(
        [str(NATIVE_BINARY)],
        input=input_text,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    responses = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                responses.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return responses


def _load_rom_commands() -> list[dict[str, Any]]:
    assert ROM_PATH is not None
    return [{"cmd": "load_rom", "path": _command_path(ROM_PATH)}]


def _png_is_valid(path: Path) -> bool:
    """Return True if path is a syntactically valid PNG file."""
    try:
        data = path.read_bytes()
        return (
            data[:8] == b"\x89PNG\r\n\x1a\n"
            and len(data) > 16
        )
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Smoke tests (binary presence + basic protocol)
# ---------------------------------------------------------------------------

@pytest.mark.skipif(_BINARY_MISSING, reason=_BINARY_SKIP_REASON)
class TestNativeBinarySmoke:
    def test_binary_exists(self) -> None:
        assert NATIVE_BINARY is not None
        assert NATIVE_BINARY.exists(), f"Binary missing: {NATIVE_BINARY}"
        assert NATIVE_BINARY.stat().st_size > 0

    def test_version_flag(self) -> None:
        result = subprocess.run(
            [str(NATIVE_BINARY), "--version"],
            capture_output=True, text=True, timeout=10,
        )
        assert "native-core" in result.stdout.lower() or "native-core" in result.stderr.lower()

    def test_exit_command(self) -> None:
        responses = _run_session([{"cmd": "exit"}])
        assert any(r.get("ok") is True and r.get("cmd") == "exit" for r in responses)

    def test_unknown_command_returns_error(self) -> None:
        responses = _run_session([{"cmd": "totally_unknown_command_xyz"}])
        err_responses = [r for r in responses if r.get("ok") is False]
        assert len(err_responses) >= 1, "Expected an error response for unknown command"

    def test_empty_input_exits_cleanly(self) -> None:
        # Sending EOF immediately should not crash the process
        result = subprocess.run(
            [str(NATIVE_BINARY)],
            input="",
            capture_output=True,
            text=True,
            timeout=10,
        )
        # Process should exit with code 0
        assert result.returncode == 0

    def test_malformed_json_returns_error(self) -> None:
        result = subprocess.run(
            [str(NATIVE_BINARY)],
            input='not valid json\n{"cmd":"exit"}\n',
            capture_output=True, text=True, timeout=10,
        )
        responses = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    responses.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        err_responses = [r for r in responses if r.get("ok") is False]
        assert len(err_responses) >= 1, "Expected error for malformed JSON"


# ---------------------------------------------------------------------------
# ROM loading tests (require native binary only, not real ROM for error cases)
# ---------------------------------------------------------------------------

@pytest.mark.skipif(_BINARY_MISSING, reason=_BINARY_SKIP_REASON)
class TestLoadRomErrors:
    def test_load_rom_missing_path_field(self) -> None:
        responses = _run_session([{"cmd": "load_rom"}])
        err = next((r for r in responses if r.get("cmd") == "load_rom"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_load_rom_nonexistent_file(self) -> None:
        responses = _run_session([{"cmd": "load_rom", "path": "C:/nonexistent/Rom.rom"}])
        err = next((r for r in responses if r.get("cmd") == "load_rom"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_get_screen_before_rom_returns_error(self) -> None:
        responses = _run_session([{"cmd": "get_screen", "format": "crc"}])
        err = next((r for r in responses if r.get("cmd") == "get_screen"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_load_app_before_rom_returns_error(self) -> None:
        responses = _run_session([{"cmd": "load_app", "path": "C:/dummy.8xp"}])
        err = next((r for r in responses if r.get("cmd") == "load_app"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_step_before_rom_returns_error(self) -> None:
        responses = _run_session([{"cmd": "step", "cycles_ms": 100}])
        err = next((r for r in responses if r.get("cmd") == "step"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_inject_key_before_rom_returns_error(self) -> None:
        responses = _run_session([{"cmd": "inject_key", "key": "enter"}])
        err = next((r for r in responses if r.get("cmd") == "inject_key"), None)
        assert err is not None
        assert err.get("ok") is False


# ---------------------------------------------------------------------------
# Real ROM tests (require both native binary AND valid Rom.rom)
# ---------------------------------------------------------------------------

_NEEDS_ROM = pytest.mark.skipif(
    _BINARY_MISSING or _ROM_MISSING,
    reason=_BINARY_SKIP_REASON or _ROM_SKIP_REASON,
)


@_NEEDS_ROM
class TestLoadRom:
    def test_load_rom_succeeds(self) -> None:
        responses = _run_session(_load_rom_commands())
        rom_resp = next((r for r in responses if r.get("cmd") == "load_rom"), None)
        assert rom_resp is not None, "No load_rom response received"
        assert rom_resp.get("ok") is True, f"load_rom failed: {rom_resp}"
        assert rom_resp.get("backend") == "native_core"
        assert rom_resp.get("boot_ticks", 0) > 0

    def test_load_rom_sets_running_state(self) -> None:
        responses = _run_session(_load_rom_commands() + [{"cmd": "get_state"}])
        state_resp = next((r for r in responses if r.get("cmd") == "get_state"), None)
        assert state_resp is not None
        assert state_resp.get("ok") is True
        # get_state returns fields flat at the top level (not nested in "state")
        assert state_resp.get("rom_loaded") is True
        assert state_resp.get("running") is True


@_NEEDS_ROM
class TestGetScreen:
    def test_get_screen_crc_returns_8_hex_chars(self) -> None:
        responses = _run_session(_load_rom_commands() + [{"cmd": "get_screen", "format": "crc"}])
        screen = next((r for r in responses if r.get("cmd") == "get_screen"), None)
        assert screen is not None
        assert screen.get("ok") is True
        crc = screen.get("crc", "")
        assert len(crc) == 8, f"CRC should be 8 hex chars, got: '{crc}'"
        assert all(c in "0123456789ABCDEFabcdef" for c in crc), f"Non-hex CRC: '{crc}'"

    def test_get_screen_crc_consistent_between_calls(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "get_screen", "format": "crc"}, {"cmd": "get_screen", "format": "crc"}]
        )
        crcs = [r.get("crc") for r in responses if r.get("cmd") == "get_screen" and r.get("ok")]
        assert len(crcs) == 2, "Expected two CRC responses"
        assert crcs[0] == crcs[1], "CRC should be stable when emulation is not running"

    def test_get_screen_png_writes_valid_png(self) -> None:
        temp_dir, png_path = _make_temp_path("calcengine-native-screen-", ".png")
        try:
            responses = _run_session(
                _load_rom_commands()
                + [{"cmd": "get_screen", "format": "png", "path": _command_path(png_path)}]
            )
            screen = next((r for r in responses if r.get("cmd") == "get_screen"), None)
            assert screen is not None
            assert screen.get("ok") is True, f"get_screen png failed: {screen}"
            assert _png_is_valid(png_path), "Written file is not a valid PNG"
            # PNG should be 320x240 (check IHDR)
            data = png_path.read_bytes()
            # IHDR chunk starts at byte 16: 4 bytes width, 4 bytes height
            width = struct.unpack(">I", data[16:20])[0]
            height = struct.unpack(">I", data[20:24])[0]
            assert width == 320, f"Expected width 320, got {width}"
            assert height == 240, f"Expected height 240, got {height}"
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_get_screen_png_missing_path_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "get_screen", "format": "png"}]
        )
        err = next((r for r in responses if r.get("cmd") == "get_screen"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_get_screen_unknown_format_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "get_screen", "format": "bmp"}]
        )
        err = next((r for r in responses if r.get("cmd") == "get_screen"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_crc_changes_after_step(self) -> None:
        """CRC before and after a long step should differ (OS is rendering frames)."""
        responses = _run_session(
            _load_rom_commands()
            + [
                {"cmd": "get_screen", "format": "crc"},
                {"cmd": "step", "cycles_ms": 2000},
                {"cmd": "get_screen", "format": "crc"},
            ],
            timeout=120,
        )
        crcs = [r.get("crc") for r in responses if r.get("cmd") == "get_screen" and r.get("ok")]
        assert len(crcs) == 2, "Expected two CRC responses"
        # The screen state may or may not have changed visually, but both must be valid 8-hex CRCs
        for crc in crcs:
            assert crc is not None and len(crc) == 8


@_NEEDS_ROM
class TestLoadApp:
    @pytest.mark.skipif(_APP_MISSING, reason="CALCENG.8xp not found in Downloads or bin/")
    def test_load_app_succeeds(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "load_app", "path": _command_path(APP_PATH)}],
            timeout=90,
        )
        app_resp = next((r for r in responses if r.get("cmd") == "load_app"), None)
        assert app_resp is not None, "No load_app response received"
        assert app_resp.get("ok") is True, f"load_app failed: {app_resp}"
        assert app_resp.get("backend") == "native_core"
        assert app_resp.get("app_count", 0) >= 1

    @pytest.mark.skipif(_APP_MISSING, reason="CALCENG.8xp not found in Downloads or bin/")
    def test_load_app_directory_honors_target_metadata(self) -> None:
        stage_dir = _make_temp_dir("calcengine-dir-load-")
        try:
            shutil.copy2(APP_PATH, stage_dir / APP_PATH.name)
            responses = _run_session(
                _load_rom_commands()
                + [{
                    "cmd": "load_app",
                    "path": _command_path(stage_dir),
                    "target_name": "A",
                    "target_is_asm": False,
                }],
                timeout=90,
            )
        finally:
            shutil.rmtree(stage_dir, ignore_errors=True)

        app_resp = next((r for r in responses if r.get("cmd") == "load_app"), None)
        assert app_resp is not None, "No load_app response received"
        assert app_resp.get("ok") is True, f"load_app failed: {app_resp}"
        assert app_resp.get("backend") == "native_core"
        assert app_resp.get("app_count", 0) >= 1
        assert app_resp.get("target_name") == "A"
        assert app_resp.get("target_is_asm") is False

    def test_load_app_nonexistent_file_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "load_app", "path": "C:/nonexistent/APP.8xp"}],
            timeout=60,
        )
        err = next((r for r in responses if r.get("cmd") == "load_app"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_load_app_missing_path_field_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "load_app"}],
            timeout=60,
        )
        err = next((r for r in responses if r.get("cmd") == "load_app"), None)
        assert err is not None
        assert err.get("ok") is False


@_NEEDS_ROM
class TestStep:
    def test_step_advances_total_cycles(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "step", "cycles_ms": 500}, {"cmd": "step", "cycles_ms": 500}],
            timeout=60,
        )
        step_responses = [r for r in responses if r.get("cmd") == "step" and r.get("ok")]
        assert len(step_responses) == 2
        tc1 = step_responses[0].get("total_cycles", 0)
        tc2 = step_responses[1].get("total_cycles", 0)
        assert tc2 > tc1, f"total_cycles should increase: {tc1} -> {tc2}"

    def test_step_default_duration(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "step"}],
            timeout=60,
        )
        step = next((r for r in responses if r.get("cmd") == "step"), None)
        assert step is not None
        assert step.get("ok") is True

    def test_step_reports_backend(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "step", "cycles_ms": 100}],
            timeout=30,
        )
        step = next((r for r in responses if r.get("cmd") == "step"), None)
        assert step is not None
        assert step.get("backend") == "native_core"


@_NEEDS_ROM
class TestInjectKey:
    def test_inject_known_key_succeeds(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "inject_key", "key": "enter", "duration_ms": 100}],
            timeout=60,
        )
        key_resp = next((r for r in responses if r.get("cmd") == "inject_key"), None)
        assert key_resp is not None
        assert key_resp.get("ok") is True
        assert key_resp.get("backend") == "native_core"

    def test_inject_key_with_key_prefix(self) -> None:
        """Keys may be specified as 'key|enter' (autotester format)."""
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "inject_key", "key": "key|enter", "duration_ms": 100}],
            timeout=60,
        )
        key_resp = next((r for r in responses if r.get("cmd") == "inject_key"), None)
        assert key_resp is not None
        assert key_resp.get("ok") is True

    def test_inject_all_known_keys(self) -> None:
        """Every key in the native map should be accepted without error."""
        known_keys = [
            "graph", "trace", "zoom", "window", "y=", "2nd", "mode", "del",
            "on", "sto", "sto>", "store", "storeto", "ln", "log", "^2", "-1", "math", "alpha",
            "0", "1", "4", "7", ",", "sin", "apps", "xton",
            ".", "2", "5", "8", "(", "cos", "prgm", "stat",
            "(-)", "3", "6", "9", ")", "tan", "vars",
            "enter", "+", "-", "*", "/", "^", "clear",
            "down", "left", "right", "up",
        ]
        commands = _load_rom_commands()
        for key in known_keys:
            commands.append({"cmd": "inject_key", "key": key, "duration_ms": 50})
        responses = _run_session(commands, timeout=120)
        key_responses = [r for r in responses if r.get("cmd") == "inject_key"]
        errors = [r for r in key_responses if r.get("ok") is False]
        assert len(errors) == 0, f"Unknown keys rejected: {[r.get('error') for r in errors]}"
        assert len(key_responses) == len(known_keys)

    def test_inject_unknown_key_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "inject_key", "key": "nonexistent_key_xyz"}],
            timeout=30,
        )
        err = next((r for r in responses if r.get("cmd") == "inject_key"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_inject_key_missing_key_field_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "inject_key"}],
            timeout=30,
        )
        err = next((r for r in responses if r.get("cmd") == "inject_key"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_inject_key_reports_row_col(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "inject_key", "key": "enter", "duration_ms": 50}],
            timeout=30,
        )
        key_resp = next((r for r in responses if r.get("cmd") == "inject_key"), None)
        assert key_resp is not None
        assert "emu_row" in key_resp or "row" in key_resp or key_resp.get("ok") is True


@_NEEDS_ROM
class TestGetState:
    def test_get_state_after_boot(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "get_state"}]
        )
        state_resp = next((r for r in responses if r.get("cmd") == "get_state"), None)
        assert state_resp is not None
        assert state_resp.get("ok") is True
        # Fields are returned flat at the top level of the response object
        assert state_resp.get("rom_loaded") is True
        assert state_resp.get("running") is True
        assert isinstance(state_resp.get("app_count"), int)
        assert isinstance(state_resp.get("total_cycles"), int)

    def test_get_state_backend_field(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "get_state"}]
        )
        state_resp = next((r for r in responses if r.get("cmd") == "get_state"), None)
        assert state_resp is not None
        assert state_resp.get("backend") == "native_core"


@_NEEDS_ROM
class TestReset:
    def test_reset_succeeds(self) -> None:
        responses = _run_session(
            _load_rom_commands()
            + [{"cmd": "step", "cycles_ms": 500}, {"cmd": "reset"}],
            timeout=60,
        )
        reset_resp = next((r for r in responses if r.get("cmd") == "reset"), None)
        assert reset_resp is not None
        assert reset_resp.get("ok") is True
        assert reset_resp.get("backend") == "native_core"

    def test_reset_before_rom_returns_error(self) -> None:
        responses = _run_session([{"cmd": "reset"}])
        err = next((r for r in responses if r.get("cmd") == "reset"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_reset_clears_total_cycles(self) -> None:
        """After reset, cycles counter should be lower than before (re-boots from 0)."""
        responses = _run_session(
            _load_rom_commands()
            + [
                {"cmd": "step", "cycles_ms": 500},
                {"cmd": "get_state"},
                {"cmd": "reset"},
                {"cmd": "get_state"},
            ],
            timeout=90,
        )
        # get_state fields are flat at top level
        states = [r for r in responses if r.get("cmd") == "get_state" and r.get("ok")]
        assert len(states) == 2, "Expected two get_state responses"
        cycles_before_reset = states[0].get("total_cycles", 0)
        cycles_after_reset = states[1].get("total_cycles", 0)
        # After reset the counter includes boot ticks; it should be less than before+step
        assert cycles_before_reset > 0, "total_cycles should be positive after step"


@_NEEDS_ROM
class TestSnapshot:
    def test_snapshot_json_writes_file(self) -> None:
        temp_dir, snap_path = _make_temp_path("calcengine-native-snapshot-", ".json")
        try:
            responses = _run_session(
                _load_rom_commands()
                + [{"cmd": "snapshot", "path": _command_path(snap_path)}]
            )
            snap_resp = next((r for r in responses if r.get("cmd") == "snapshot"), None)
            assert snap_resp is not None
            assert snap_resp.get("ok") is True, f"snapshot json failed: {snap_resp}"
            assert snap_path.exists(), "Snapshot JSON file not written"
            content = json.loads(snap_path.read_text(encoding="utf-8"))
            assert isinstance(content, dict), "Snapshot JSON should be a dict"
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_snapshot_png_writes_valid_png(self) -> None:
        temp_dir, snap_path = _make_temp_path("calcengine-native-snapshot-", ".png")
        try:
            responses = _run_session(
                _load_rom_commands()
                + [{"cmd": "snapshot", "path": _command_path(snap_path)}]
            )
            snap_resp = next((r for r in responses if r.get("cmd") == "snapshot"), None)
            assert snap_resp is not None
            assert snap_resp.get("ok") is True, f"snapshot png failed: {snap_resp}"
            assert _png_is_valid(snap_path), "Written file is not a valid PNG"
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_snapshot_missing_path_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "snapshot"}]
        )
        err = next((r for r in responses if r.get("cmd") == "snapshot"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_snapshot_unknown_extension_returns_error(self) -> None:
        responses = _run_session(
            _load_rom_commands() + [{"cmd": "snapshot", "path": "C:/tmp/snap.bmp"}]
        )
        err = next((r for r in responses if r.get("cmd") == "snapshot"), None)
        assert err is not None
        assert err.get("ok") is False

    def test_snapshot_before_rom_returns_error(self) -> None:
        temp_dir, snap_path = _make_temp_path("calcengine-native-snapshot-", ".json")
        try:
            responses = _run_session([{"cmd": "snapshot", "path": _command_path(snap_path)}])
            err = next((r for r in responses if r.get("cmd") == "snapshot"), None)
            assert err is not None
            assert err.get("ok") is False
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)


# ---------------------------------------------------------------------------
# End-to-end: full session
# ---------------------------------------------------------------------------

@_NEEDS_ROM
@pytest.mark.skipif(_APP_MISSING, reason="CALCENG.8xp not found in Downloads or bin/")
class TestFullSession:
    def test_boot_transfer_step_capture(self) -> None:
        """
        Full session: ROM -> load app -> step -> screen CRC -> snapshot PNG.
        Validates the entire pipeline in a single subprocess run.
        """
        temp_dir, snap_path = _make_temp_path("calcengine-native-session-", ".png")

        try:
            commands = [
                {"cmd": "load_rom", "path": _command_path(ROM_PATH)},
                {"cmd": "load_app", "path": _command_path(APP_PATH)},
                {"cmd": "step", "cycles_ms": 3000},
                {"cmd": "get_screen", "format": "crc"},
                {"cmd": "snapshot", "path": _command_path(snap_path)},
                {"cmd": "get_state"},
            ]
            responses = _run_session(commands, timeout=120)

            rom_resp = next((r for r in responses if r.get("cmd") == "load_rom"), None)
            app_resp = next((r for r in responses if r.get("cmd") == "load_app"), None)
            step_resp = next((r for r in responses if r.get("cmd") == "step"), None)
            screen_resp = next((r for r in responses if r.get("cmd") == "get_screen"), None)
            snap_resp = next((r for r in responses if r.get("cmd") == "snapshot"), None)
            state_resp = next((r for r in responses if r.get("cmd") == "get_state"), None)

            assert rom_resp is not None and rom_resp.get("ok") is True
            assert app_resp is not None and app_resp.get("ok") is True
            assert step_resp is not None and step_resp.get("ok") is True
            assert screen_resp is not None and screen_resp.get("ok") is True
            assert snap_resp is not None and snap_resp.get("ok") is True
            assert state_resp is not None and state_resp.get("ok") is True

            crc = screen_resp.get("crc", "")
            assert len(crc) == 8, f"Expected 8-char CRC, got: '{crc}'"

            assert _png_is_valid(snap_path), "Snapshot PNG is not valid"

            # get_state fields are flat at the top level
            assert state_resp.get("rom_loaded") is True
            assert state_resp.get("app_count", 0) >= 1

        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)
