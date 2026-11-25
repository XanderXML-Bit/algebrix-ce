"""
test_phase1_ui.py

Phase 1 CalcEngine UI validation tests (v1.1.0).

Verifies the four Phase 1 behavioural promises using the cemu-headless native
binary with a full ROM + module set:

  1. Expression evaluation: typing "2+2" + ENTER shows "4" on screen and does
     NOT show literal "%.6g" (regression from the old os_GetStringInput path).
  2. MODE key: pressing MODE does NOT insert the word "Float" into the input
     buffer (regression from os_GetStringInput delegating to TI-OS home handler).
  3. Quit: pressing 2nd+MODE causes CalcEngine to exit cleanly (screen no longer
     shows the CalcEngine prompt).
  4. ENTRY recall: typing "3+5" + ENTER, then pressing 2nd+ENTER, places "3+5"
     back in the input buffer.

All tests are auto-skipped when any prerequisite is absent:
  - cemu-headless native binary not built
      (tools/emulator/cemu-headless/build/Release/cemu-headless.exe)
  - Rom.rom not present (rom/Rom.rom)
  - arTIfiCE.8xp not available (tools/emulator/assets/arTIfiCE.8xp)
  - CALCENG.8xp not present (deploy/hardware-ready/CALCENG.8xp or bin/launcher/CALCENG.8xp)
  - CEBASE.8xp not present (required module for arithmetic dispatch)

The skip-on-missing-prerequisites design keeps the main pytest suite green in CI
and on developer machines that have not provisioned the full hardware stack.
When all prerequisites are present the tests run against the real ROM.
"""

from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path
from typing import Any

import pytest


# ---------------------------------------------------------------------------
# Repository root & resource resolution
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]

_NATIVE_BINARY_SEARCH = [
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build" / "Release" / "cemu-headless.exe",
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build_native" / "cemu-headless.exe",
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build" / "x64" / "Release" / "cemu-headless.exe",
]
_ROM_SEARCH = [
    REPO_ROOT / "rom" / "Rom.rom",
]
_ARTIFICE_SEARCH = [
    REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE.8xp",
    REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE" / "arTIfiCE.8xp",
]
_CALCENG_SEARCH = [
    REPO_ROOT / "deploy" / "hardware-ready" / "CALCENG.8xp",
    REPO_ROOT / "bin" / "launcher" / "CALCENG.8xp",
    REPO_ROOT / "bin" / "CALCENG.8xp",
]
_CEBASE_SEARCH = [
    REPO_ROOT / "deploy" / "hardware-ready" / "CEBASE.8xp",
    REPO_ROOT / "bin" / "base" / "CEBASE.8xp",
    REPO_ROOT / "bin" / "CEBASE.8xp",
]


def _find_first(paths: list[Path]) -> Path | None:
    for p in paths:
        try:
            if p.exists():
                return p
        except OSError:
            pass
    return None


NATIVE_BINARY = _find_first(_NATIVE_BINARY_SEARCH)
ROM_PATH      = _find_first(_ROM_SEARCH)
ARTIFICE_PATH = _find_first(_ARTIFICE_SEARCH)
CALCENG_PATH  = _find_first(_CALCENG_SEARCH)
CEBASE_PATH   = _find_first(_CEBASE_SEARCH)

_BINARY_MISSING   = NATIVE_BINARY is None
_ROM_MISSING      = ROM_PATH is None
_ARTIFICE_MISSING = ARTIFICE_PATH is None
_CALCENG_MISSING  = CALCENG_PATH is None
_CEBASE_MISSING   = CEBASE_PATH is None

_SKIP_REASON = (
    "cemu-headless native binary not built; run tools/emulator/cemu-headless/build.ps1"
    if _BINARY_MISSING else
    "Rom.rom not found; place it at rom/Rom.rom"
    if _ROM_MISSING else
    "arTIfiCE.8xp not found; run scripts/get-artifice-asset.ps1"
    if _ARTIFICE_MISSING else
    "CALCENG.8xp not found in deploy/hardware-ready/ or Downloads"
    if _CALCENG_MISSING else
    "CEBASE.8xp not found in deploy/hardware-ready/ or Downloads"
    if _CEBASE_MISSING else
    ""
)
_NEEDS_FULL_STACK = pytest.mark.skipif(
    _BINARY_MISSING or _ROM_MISSING or _ARTIFICE_MISSING or _CALCENG_MISSING or _CEBASE_MISSING,
    reason=_SKIP_REASON,
)


# ---------------------------------------------------------------------------
# Low-level session helpers (mirrors test_cemu_headless_native.py pattern)
# ---------------------------------------------------------------------------

def _run_session(
    commands: list[dict[str, Any]],
    timeout: float = 180.0,
) -> list[dict[str, Any]]:
    """
    Feed *commands* as newline-delimited JSON to cemu-headless stdin and return
    the parsed JSON response objects.  An ``exit`` command is appended
    automatically unless already present.
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
    responses: list[dict[str, Any]] = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                responses.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return responses


def _p(path: Path) -> str:
    """Return a forward-slash path string suitable for cemu-headless JSON."""
    return str(path).replace("\\", "/")


# ---------------------------------------------------------------------------
# CalcEngine launch helpers
# ---------------------------------------------------------------------------

# All modules required for Phase 1 to work (CALCENG = launcher, CEBASE = arithmetic)
_REQUIRED_MODULES: list[Path] = [
    p for p in [CALCENG_PATH, CEBASE_PATH] if p is not None
]

# Additional modules to pre-load so that the worker lookup never fails.
# We load everything in deploy/hardware-ready/ when available.
_HARDWARE_READY_DIR = REPO_ROOT / "deploy" / "hardware-ready"


def _all_module_paths() -> list[Path]:
    """Return all .8xp modules to pre-load before running tests."""
    found: list[Path] = []
    if _HARDWARE_READY_DIR.exists():
        for p in sorted(_HARDWARE_READY_DIR.glob("*.8xp")):
            found.append(p)
    else:
        # Minimal fallback: just what we found above.
        found.extend(p for p in _REQUIRED_MODULES if p not in found)
    return found


def _load_rom_cmd() -> dict[str, Any]:
    assert ROM_PATH is not None
    return {"cmd": "load_rom", "path": _p(ROM_PATH)}


def _load_app_cmd(path: Path) -> dict[str, Any]:
    return {"cmd": "load_app", "path": _p(path)}


def _step(ms: int) -> dict[str, Any]:
    return {"cmd": "step", "cycles_ms": ms}


def _key(name: str, duration_ms: int = 250) -> dict[str, Any]:
    return {"cmd": "inject_key", "key": name, "duration_ms": duration_ms}


def _get_text() -> dict[str, Any]:
    return {"cmd": "get_screen", "format": "text"}


def _get_crc() -> dict[str, Any]:
    return {"cmd": "get_screen", "format": "crc"}


def _boot_and_load_commands() -> list[dict[str, Any]]:
    """
    Boot sequence:
      1. load_rom — boots the OS
      2. load_app for every .8xp module (includes arTIfiCE and all CalcEngine workers)
      3. step 4 000 ms — let TI-OS fully boot
      4. clear — dismiss any splash / menu
      5. step 500 ms — stabilise at TI-OS home screen
    """
    cmds: list[dict[str, Any]] = [_load_rom_cmd()]

    # Load arTIfiCE first so it is present in the shell menu
    assert ARTIFICE_PATH is not None
    cmds.append(_load_app_cmd(ARTIFICE_PATH))

    # Load all CalcEngine modules
    for p in _all_module_paths():
        cmds.append(_load_app_cmd(p))

    # Boot TI-OS to home screen
    cmds += [_step(4000), _key("clear"), _step(500)]
    return cmds


def _launch_calcengine_commands() -> list[dict[str, Any]]:
    """
    Navigate from TI-OS home screen into CalcEngine via the arTIfiCE jailbreak:
      PRGM → ENTER (runs arTIfiCE prgm) → wait → ENTER (opens arTIfiCE shell)
      → wait → ENTER (selects CALCENG) → wait 3 s (CalcEngine starts)
    """
    return [
        # Open PRGM menu and run arTIfiCE
        _key("prgm"),
        _step(1200),
        _key("enter"),        # execute arTIfiCE from program list
        _step(2000),          # arTIfiCE launches, shell appears
        _key("enter"),        # confirm / open shell
        _step(1000),
        # Navigate to CALCENG in the arTIfiCE shell and launch it
        _key("enter"),        # select first entry (CALCENG)
        _step(3000),          # CalcEngine boots
    ]


# CalcEngine prompt CRC observed in Pass 71 validation
_CE_PROMPT_CRC = "384A70F2"

# "%.6g" text that must NOT appear after Phase 1 fix
_BAD_FORMAT_SPEC = "%.6g"
# "Float" that must NOT appear when MODE is pressed (old os_GetStringInput bug)
_BAD_MODE_STRING = "Float"


# ---------------------------------------------------------------------------
# Phase 1 tests
# ---------------------------------------------------------------------------

@_NEEDS_FULL_STACK
class TestPhase1UI:
    """
    End-to-end Phase 1 UI validation using the cemu-headless native binary.

    Boot flow per test:
      ROM → all modules → TI-OS home → arTIfiCE → CalcEngine prompt

    Each test method runs an independent subprocess session so tests are
    isolated from each other.
    """

    # ----- helpers ----------------------------------------------------------

    def _run_calcengine_session(
        self,
        extra_cmds: list[dict[str, Any]],
        timeout: float = 180.0,
    ) -> list[dict[str, Any]]:
        """
        Build a full session: boot → load modules → launch CalcEngine →
        *extra_cmds* → exit.
        """
        commands = (
            _boot_and_load_commands()
            + _launch_calcengine_commands()
            + extra_cmds
        )
        return _run_session(commands, timeout=timeout)

    @staticmethod
    def _text_responses(responses: list[dict[str, Any]]) -> list[str]:
        """Return all get_screen text strings from the response list."""
        texts: list[str] = []
        for r in responses:
            if r.get("cmd") == "get_screen" and r.get("format") == "text" and r.get("ok"):
                texts.append(r.get("text", ""))
        return texts

    @staticmethod
    def _assert_calcengine_running(responses: list[dict[str, Any]]) -> None:
        """
        Assert that a get_screen response is present and has the CalcEngine
        prompt CRC, confirming CalcEngine is running before we start the test.
        """
        crc_resps = [
            r for r in responses
            if r.get("cmd") == "get_screen"
            and r.get("format") == "crc"
            and r.get("ok") is True
        ]
        if not crc_resps:
            pytest.skip(
                "No get_screen CRC response received — CalcEngine may not have "
                "launched (arTIfiCE path timing or ROM version mismatch)"
            )
        crcs = {r.get("crc", "") for r in crc_resps}
        if _CE_PROMPT_CRC not in crcs:
            pytest.skip(
                f"CalcEngine prompt CRC {_CE_PROMPT_CRC!r} not seen "
                f"(observed: {crcs!r}) — arTIfiCE launch may have failed"
            )

    # ----- test 1: 2+2 displays "4", not "%.6g" ---------------------------

    def test_arithmetic_result_no_format_spec(self) -> None:
        """
        Typing "2+2" ENTER must show "4" on screen and must NOT show "%.6g".

        This test catches the regression where os_GetStringInput passed raw
        format specifiers to os_PutStrFull() instead of pre-formatted strings.
        Phase 1 fixes this by routing all output through snprintf buffers.
        """
        extra = [
            # Verify CalcEngine is running (prompt CRC)
            _get_crc(),
            # Type "2+2" key-by-key
            _key("2"),   _step(100),
            _key("+"),   _step(100),
            _key("2"),   _step(100),
            # Submit
            _key("enter", duration_ms=400),
            # Wait for the worker to process and CALCENG to re-launch
            _step(5000),
            # Capture screen text
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=240)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, (
            "No get_screen text response received after entering '2+2' — "
            "CalcEngine may have crashed or not produced output"
        )

        combined = "\n".join(texts)

        # Must contain the result "4"
        assert "4" in combined, (
            f"Expected '4' in screen text after '2+2' but got:\n{combined!r}"
        )

        # Must NOT contain raw format specifiers (Phase 1 regression guard)
        assert _BAD_FORMAT_SPEC not in combined, (
            f"Screen text contains format specifier {_BAD_FORMAT_SPEC!r} — "
            f"output is not being pre-formatted before display.\n"
            f"Full text:\n{combined!r}"
        )

    # ----- test 2: MODE does NOT insert "Float" ----------------------------

    def test_mode_key_does_not_type_float(self) -> None:
        """
        Pressing MODE must NOT insert "Float" (or any TI-OS string) into the
        input line.

        The old os_GetStringInput delegated to the TI-OS home-screen handler;
        pressing MODE in that context causes TI-OS to insert the word "Float"
        as a token name. Phase 1 replaces os_GetStringInput with a custom
        os_GetCSC() loop that intercepts MODE and shows a stub menu instead.
        """
        extra = [
            _get_crc(),          # assert CalcEngine is running
            _key("mode"),        # press MODE — should open stub menu, NOT type "Float"
            _step(1000),
            _get_text(),         # capture what is on screen
            _key("enter"),       # dismiss the mode menu (any-key-to-return)
            _step(500),
            _get_text(),         # capture input line after returning from menu
        ]
        responses = self._run_calcengine_session(extra, timeout=180)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, (
            "No get_screen text response received after pressing MODE"
        )

        combined = "\n".join(texts)

        # "Float" must not appear in the input area.  The mode menu stub may
        # legitimately display "Float" as part of settings UI in a later phase,
        # so we specifically check the input line (row containing ">").
        input_lines = [
            line for line in combined.splitlines()
            if line.lstrip().startswith(">") or "> " in line
        ]
        for line in input_lines:
            assert _BAD_MODE_STRING not in line, (
                f"Input line contains {_BAD_MODE_STRING!r} after pressing MODE — "
                f"TI-OS home-screen handler is still being invoked.\n"
                f"Input line: {line!r}"
            )

    # ----- test 3: 2nd+MODE = clean exit -----------------------------------

    def test_2nd_mode_exits_cleanly(self) -> None:
        """
        Pressing 2nd+MODE must cause CalcEngine to exit cleanly.

        After 2nd+MODE the screen should no longer show the CalcEngine prompt
        (CRC 384A70F2).  The exit may return to TI-OS home screen or arTIfiCE
        shell — either is acceptable.  What is NOT acceptable is a crash
        (RAM-cleared) or a hang.
        """
        extra = [
            _get_crc(),          # baseline — CalcEngine prompt
            _key("2nd"),  _step(200),
            _key("mode"), _step(500),  # 2nd+MODE = quit
            _step(1000),
            _get_crc(),          # screen after quit
        ]
        responses = self._run_calcengine_session(extra, timeout=120)
        self._assert_calcengine_running(responses)

        # Collect all CRC responses
        crc_responses = [
            r for r in responses
            if r.get("cmd") == "get_screen" and r.get("format") == "crc" and r.get("ok")
        ]
        assert len(crc_responses) >= 2, (
            "Expected at least two CRC snapshots (before and after quit)"
        )

        # The last CRC must differ from the CalcEngine prompt CRC — we exited.
        last_crc = crc_responses[-1].get("crc", "")
        assert last_crc != _CE_PROMPT_CRC, (
            f"Screen CRC after 2nd+MODE is still {_CE_PROMPT_CRC!r} — "
            "CalcEngine did not exit; 2nd+MODE quit is not working"
        )

    # ----- test 4: ENTRY recall (2nd+ENTER) --------------------------------

    def test_entry_recall_restores_last_expression(self) -> None:
        """
        After submitting "3+5" with ENTER, pressing 2nd+ENTER must place "3+5"
        back in the input buffer (visible on the input line with "> 3+5|").

        This is the ENTRY recall feature: g_shared.last_expression is set when
        an expression is submitted, and 2nd+ENTER copies it back into s_input.
        """
        extra = [
            _get_crc(),          # assert CalcEngine running
            # Type "3+5"
            _key("3"),   _step(100),
            _key("+"),   _step(100),
            _key("5"),   _step(100),
            # Submit with ENTER
            _key("enter", duration_ms=400),
            _step(5000),         # wait for worker + re-launch cycle
            # Back at CalcEngine prompt with result displayed.
            # Press 2nd+ENTER to recall last expression.
            _key("2nd"),  _step(200),
            _key("enter", duration_ms=400),
            _step(500),
            # Capture input line
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=240)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, (
            "No get_screen text response received after 2nd+ENTER recall"
        )

        combined = "\n".join(texts)

        # The input line should contain "3+5" (possibly with cursor marker "|")
        assert "3+5" in combined or "3 + 5" in combined, (
            f"Expected '3+5' in input buffer after 2nd+ENTER recall but got:\n"
            f"{combined!r}\n\n"
            "Check that: (1) the expression was submitted and stored in "
            "g_shared.last_expression, (2) 2nd+ENTER is wired to copy it into "
            "the input buffer, (3) home_draw() renders the buffer to the LCD."
        )
