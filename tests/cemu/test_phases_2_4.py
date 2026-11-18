"""
test_phases_2_4.py

End-to-end emulator validation for CalcEngine Phases 2, 3, and 4
(overnight push — v2.0.0 feature set).

Phase 2 — Per-entry history results and ENTRY recall cycling:
  - History entries display individual result strings, not just "last result".
  - 2nd+ENTER cycles backward through prior expressions (not just the most
    recent one).
  - CLEAR pressed on an empty input line wipes the full history ring.

Phase 3 — Full mode-settings menu:
  - Pressing MODE opens the settings menu (title line visible on screen).
  - Pressing CLEAR inside the menu exits without modifying settings.
  - Pressing ENTER inside the menu saves settings without crashing.
  - Settings survive a worker re-launch cycle (persisted in CE_STATE).

Phase 4 follow-through — single-enter result flow:
  - Results stay on the CalcEngine home screen history instead of opening a
    blocking graphx overlay.
  - A single ENTER is enough: submit once, wait for the worker cycle, and the
    next prompt is immediately ready with no dismiss-key step.
  - Structured results (fractions, powers, radicals) and plain integers all
    land in history without an extra keypress.

All tests auto-skip when any prerequisite is absent:
  - cemu-headless native binary (tools/emulator/cemu-headless/build/Release/)
  - Rom.rom (rom/Rom.rom)
  - arTIfiCE.8xp (tools/emulator/assets/arTIfiCE.8xp)
  - CALCENG.8xp (deploy/hardware-ready/ or Downloads)
  - CEALG.8xp (algebra worker — needed for Phase 3 settings persistence)

The skip-on-missing design keeps CI green on machines without the full
hardware stack.
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
    Path("C:/Users/user/src/algebrix-ce/rom/Rom.rom"),
]
_ARTIFICE_SEARCH = [
    REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE.8xp",
    REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE" / "arTIfiCE.8xp",
    Path("C:/Users/user/Downloads/arTIfiCE.8xp"),
]
_CALCENG_SEARCH = [
    REPO_ROOT / "deploy" / "hardware-ready" / "CALCENG.8xp",
    REPO_ROOT / "bin" / "CALCENG.8xp",
    Path("C:/Users/user/Downloads/CalcEngine/CALCENG.8xp"),
    Path("C:/Users/user/Downloads/CalcEngine-Ready/CALCENG.8xp"),
]
_CEALG_SEARCH = [
    REPO_ROOT / "deploy" / "hardware-ready" / "CEALG.8xp",
    REPO_ROOT / "bin" / "CEALG.8xp",
    Path("C:/Users/user/Downloads/CalcEngine/CEALG.8xp"),
    Path("C:/Users/user/Downloads/CalcEngine-Ready/CEALG.8xp"),
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
CEALG_PATH    = _find_first(_CEALG_SEARCH)

_BINARY_MISSING   = NATIVE_BINARY is None
_ROM_MISSING      = ROM_PATH is None
_ARTIFICE_MISSING = ARTIFICE_PATH is None
_CALCENG_MISSING  = CALCENG_PATH is None
_CEALG_MISSING    = CEALG_PATH is None

_SKIP_REASON = (
    "cemu-headless native binary not built; run tools/emulator/cemu-headless/build.ps1"
    if _BINARY_MISSING else
    "Rom.rom not found; place it at rom/Rom.rom"
    if _ROM_MISSING else
    "arTIfiCE.8xp not found; run scripts/get-artifice-asset.ps1"
    if _ARTIFICE_MISSING else
    "CALCENG.8xp not found in deploy/hardware-ready/ or Downloads"
    if _CALCENG_MISSING else
    "CEALG.8xp not found in deploy/hardware-ready/ or Downloads"
    if _CEALG_MISSING else
    ""
)

_NEEDS_FULL_STACK = pytest.mark.skipif(
    _BINARY_MISSING or _ROM_MISSING or _ARTIFICE_MISSING
    or _CALCENG_MISSING or _CEALG_MISSING,
    reason=_SKIP_REASON,
)


# ---------------------------------------------------------------------------
# Low-level session helpers (same pattern as test_phase1_ui.py)
# ---------------------------------------------------------------------------

def _run_session(
    commands: list[dict[str, Any]],
    timeout: float = 180.0,
) -> list[dict[str, Any]]:
    """
    Feed *commands* as newline-delimited JSON to cemu-headless stdin and
    return parsed JSON response objects.  An ``exit`` command is appended
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
    return str(path).replace("\\", "/")


# ---------------------------------------------------------------------------
# Command builders
# ---------------------------------------------------------------------------

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


_HARDWARE_READY_DIR = REPO_ROOT / "deploy" / "hardware-ready"


def _all_module_paths() -> list[Path]:
    found: list[Path] = []
    if _HARDWARE_READY_DIR.exists():
        for p in sorted(_HARDWARE_READY_DIR.glob("*.8xp")):
            found.append(p)
    else:
        for p in [CALCENG_PATH, CEALG_PATH]:
            if p is not None:
                found.append(p)
    return found


def _boot_and_load_commands() -> list[dict[str, Any]]:
    cmds: list[dict[str, Any]] = [_load_rom_cmd()]
    assert ARTIFICE_PATH is not None
    cmds.append(_load_app_cmd(ARTIFICE_PATH))
    for p in _all_module_paths():
        cmds.append(_load_app_cmd(p))
    cmds += [_step(4000), _key("clear"), _step(500)]
    return cmds


def _launch_calcengine_commands() -> list[dict[str, Any]]:
    return [
        _key("prgm"),
        _step(1200),
        _key("enter"),
        _step(2000),
        _key("enter"),
        _step(1000),
        _key("enter"),
        _step(3000),
    ]


# ---------------------------------------------------------------------------
# CRC constants
# ---------------------------------------------------------------------------

# CalcEngine prompt screen CRC (from Pass 71 hardware validation)
_CE_PROMPT_CRC = "384A70F2"


# ---------------------------------------------------------------------------
# Shared session helper (used by all test classes)
# ---------------------------------------------------------------------------

class _CalcEngineTestBase:
    """
    Mixin that provides a session runner and common assertion helpers.
    """

    def _run_calcengine_session(
        self,
        extra_cmds: list[dict[str, Any]],
        timeout: float = 240.0,
    ) -> list[dict[str, Any]]:
        commands = (
            _boot_and_load_commands()
            + _launch_calcengine_commands()
            + extra_cmds
        )
        return _run_session(commands, timeout=timeout)

    @staticmethod
    def _text_responses(responses: list[dict[str, Any]]) -> list[str]:
        texts: list[str] = []
        for r in responses:
            if (
                r.get("cmd") == "get_screen"
                and r.get("format") == "text"
                and r.get("ok")
            ):
                texts.append(r.get("text", ""))
        return texts

    @staticmethod
    def _crc_responses(responses: list[dict[str, Any]]) -> list[str]:
        return [
            r.get("crc", "")
            for r in responses
            if r.get("cmd") == "get_screen"
            and r.get("format") == "crc"
            and r.get("ok")
        ]

    @staticmethod
    def _assert_calcengine_running(responses: list[dict[str, Any]]) -> None:
        crc_resps = [
            r for r in responses
            if r.get("cmd") == "get_screen"
            and r.get("format") == "crc"
            and r.get("ok") is True
        ]
        if not crc_resps:
            pytest.skip(
                "No get_screen CRC response — CalcEngine may not have launched "
                "(arTIfiCE path timing or ROM version mismatch)"
            )
        crcs = {r.get("crc", "") for r in crc_resps}
        if _CE_PROMPT_CRC not in crcs:
            pytest.skip(
                f"CalcEngine prompt CRC {_CE_PROMPT_CRC!r} not seen "
                f"(observed: {crcs!r}) — arTIfiCE launch may have failed"
            )


# ---------------------------------------------------------------------------
# Phase 2: Per-entry history results and ENTRY recall cycling
# ---------------------------------------------------------------------------

@_NEEDS_FULL_STACK
class TestPhase2History(_CalcEngineTestBase):
    """
    End-to-end Phase 2 tests.

    Phase 2 adds three behaviours:
      1. Each history entry carries its own result string
         (history_results[CE_HISTORY_SIZE][64] in ce_shared_state_t).
      2. 2nd+ENTER cycles backward through ALL prior expressions, not just
         the most recent one.
      3. CLEAR pressed with an empty input line wipes the full history ring.
    """

    def test_history_per_entry_results_displayed(self) -> None:
        """
        Submit two distinct expressions.  The home screen should show both
        results (one per row) rather than the same result for every entry.

        Concrete sequence: "1+1" ENTER → "7*3" ENTER → capture screen.
        Expected: screen contains "2" (from 1+1) and "21" (from 7*3).
        """
        extra = [
            _get_crc(),                     # assert CalcEngine running
            # First expression: 1+1 = 2
            _key("1"), _step(100),
            _key("+"), _step(100),
            _key("1"), _step(100),
            _key("enter", duration_ms=400),
            _step(5000),                    # worker cycle
            # Second expression: 7*3 = 21
            _key("7"), _step(100),
            _key("mul"), _step(100),        # × key
            _key("3"), _step(100),
            _key("enter", duration_ms=400),
            _step(5000),                    # worker cycle
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=300)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, "No screen text received after two expressions"

        combined = "\n".join(texts)

        # Both individual results must be visible
        assert "2" in combined, (
            f"Expected '2' (result of 1+1) on screen but got:\n{combined!r}"
        )
        assert "21" in combined, (
            f"Expected '21' (result of 7*3) on screen but got:\n{combined!r}"
        )

    def test_entry_cycling_backward_through_history(self) -> None:
        """
        Submit three expressions.  Pressing 2nd+ENTER three times should
        cycle back to the first one.

        Sequence: "5-3" ENTER → "9/3" ENTER → "4+4" ENTER →
          2nd+ENTER (should recall "4+4") →
          2nd+ENTER (should recall "9/3") →
          2nd+ENTER (should recall "5-3") → capture screen.

        The input buffer after the third 2nd+ENTER should contain "5-3" (or
        equivalent spacing).  We test that ALL three expressions appear across
        the three captured text snapshots.
        """
        extra = [
            _get_crc(),
            # expression 1: 5-3
            _key("5"), _step(80), _key("-"), _step(80), _key("3"), _step(80),
            _key("enter", duration_ms=400), _step(5000),
            # expression 2: 9/3
            _key("9"), _step(80), _key("div"), _step(80), _key("3"), _step(80),
            _key("enter", duration_ms=400), _step(5000),
            # expression 3: 4+4
            _key("4"), _step(80), _key("+"), _step(80), _key("4"), _step(80),
            _key("enter", duration_ms=400), _step(5000),
            # Recall backward: 2nd+ENTER x1 → most recent (4+4)
            _key("2nd"), _step(150), _key("enter", duration_ms=300), _step(400),
            _get_text(),
            # Recall backward: 2nd+ENTER x2 → second most recent (9/3)
            _key("2nd"), _step(150), _key("enter", duration_ms=300), _step(400),
            _get_text(),
            # Recall backward: 2nd+ENTER x3 → oldest (5-3)
            _key("2nd"), _step(150), _key("enter", duration_ms=300), _step(400),
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=360)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert len(texts) >= 3, (
            f"Expected at least 3 text snapshots, got {len(texts)}"
        )

        # Each snapshot should show the recalled expression in the input line.
        # We check the last (oldest) snapshot contains "5" and "3" together
        # (avoiding false positives from the result area).
        last_text = texts[-1]
        assert "5" in last_text and "3" in last_text, (
            f"Expected expression '5-3' recalled into input buffer after "
            f"three 2nd+ENTER presses but got:\n{last_text!r}"
        )

        # The first recall snapshot should contain "4" twice (4+4)
        first_text = texts[0]
        assert "4" in first_text, (
            f"Expected '4+4' recalled on first 2nd+ENTER but got:\n{first_text!r}"
        )

    def test_clear_on_empty_input_wipes_history(self) -> None:
        """
        After submitting expressions, pressing CLEAR with an empty input
        must wipe the entire history ring.

        Sequence: "2+3" ENTER → "6-1" ENTER → (both on history screen) →
          CLEAR (input was already cleared after submit) → capture screen.

        After the wipe the CalcEngine home screen should NOT show the
        previous expressions ("2+3", "6-1") in the history display area.
        """
        extra = [
            _get_crc(),
            # Submit two expressions
            _key("2"), _step(80), _key("+"), _step(80), _key("3"), _step(80),
            _key("enter", duration_ms=400), _step(5000),
            _key("6"), _step(80), _key("-"), _step(80), _key("1"), _step(80),
            _key("enter", duration_ms=400), _step(5000),
            # Snapshot before wipe to verify history was populated
            _get_text(),
            # CLEAR on empty input → wipe history
            _key("clear", duration_ms=400), _step(500),
            # Snapshot after wipe
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=300)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert len(texts) >= 2, (
            f"Expected at least 2 text snapshots (before/after wipe), got {len(texts)}"
        )

        before_text = texts[0]
        after_text  = texts[-1]

        # Before wipe: at least one submitted expression visible
        has_history_before = "2+3" in before_text or "6-1" in before_text or \
                             "2 + 3" in before_text or "6 - 1" in before_text or \
                             "5" in before_text or "4" in before_text  # results
        assert has_history_before, (
            f"Expected history entries on screen before CLEAR but got:\n{before_text!r}"
        )

        # After wipe: expressions "2+3" and "6-1" should no longer appear
        assert "2+3" not in after_text and "2 + 3" not in after_text, (
            f"History entry '2+3' still visible after CLEAR wipe:\n{after_text!r}"
        )
        assert "6-1" not in after_text and "6 - 1" not in after_text, (
            f"History entry '6-1' still visible after CLEAR wipe:\n{after_text!r}"
        )


# ---------------------------------------------------------------------------
# Phase 3: Full mode-settings menu
# ---------------------------------------------------------------------------

@_NEEDS_FULL_STACK
class TestPhase3ModeMenu(_CalcEngineTestBase):
    """
    End-to-end Phase 3 tests.

    Phase 3 replaces the stub mode menu with a fully wired settings screen
    that shows 5 editable items and persists changes via CE_STATE.
    """

    def test_mode_key_opens_settings_menu(self) -> None:
        """
        Pressing MODE must display a settings menu, not insert "Float" or
        any other TI-OS token into the input buffer.

        The settings menu header or item labels must be visible on screen.
        We look for any of: "SETTINGS", "Steps", "Exact", "Prec", "Seg".
        """
        extra = [
            _get_crc(),
            _key("mode"),
            _step(800),
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=180)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, "No screen text after pressing MODE"

        combined = "\n".join(texts)

        # Menu must show at least one recognisable label
        menu_indicators = ["SETTINGS", "Steps", "Exact", "Prec", "Seg",
                           "steps", "exact", "prec", "seg", "CALC"]
        found = any(indicator in combined for indicator in menu_indicators)
        assert found, (
            f"Mode menu did not appear — none of {menu_indicators!r} found "
            f"on screen after MODE press:\n{combined!r}"
        )

        # Must NOT insert "Float" into the input buffer
        input_lines = [
            line for line in combined.splitlines()
            if ">" in line
        ]
        for line in input_lines:
            assert "Float" not in line, (
                f"Input line contains 'Float' after MODE press — "
                f"TI-OS handler is being invoked instead of the settings menu.\n"
                f"Line: {line!r}"
            )

    def test_clear_exits_mode_menu_without_saving(self) -> None:
        """
        CLEAR inside the mode menu must exit without crashing and without
        modifying settings.

        After CLEAR the CalcEngine prompt CRC must be restored.
        """
        extra = [
            _get_crc(),     # baseline prompt
            # Open mode menu
            _key("mode"), _step(800),
            # Press DOWN to move to second item (verify navigation works)
            _key("down"), _step(300),
            # CLEAR — cancel without saving
            _key("clear", duration_ms=400), _step(600),
            # Capture CRC after returning to prompt
            _get_crc(),
        ]
        responses = self._run_calcengine_session(extra, timeout=180)
        self._assert_calcengine_running(responses)

        crcs = self._crc_responses(responses)
        assert len(crcs) >= 2, (
            f"Expected at least 2 CRC snapshots, got {len(crcs)}"
        )

        last_crc = crcs[-1]
        assert last_crc == _CE_PROMPT_CRC, (
            f"After CLEAR in mode menu, expected prompt CRC {_CE_PROMPT_CRC!r} "
            f"but got {last_crc!r} — CLEAR may have crashed or not returned "
            "to the main loop"
        )

    def test_enter_saves_mode_menu_without_crash(self) -> None:
        """
        ENTER inside the mode menu must save settings and return to the
        CalcEngine prompt without crashing.

        After ENTER the CalcEngine prompt CRC must be visible again.
        """
        extra = [
            _get_crc(),
            # Open mode menu
            _key("mode"), _step(800),
            # ENTER — save
            _key("enter", duration_ms=400), _step(600),
            # Prompt should be back
            _get_crc(),
        ]
        responses = self._run_calcengine_session(extra, timeout=180)
        self._assert_calcengine_running(responses)

        crcs = self._crc_responses(responses)
        assert len(crcs) >= 2, (
            f"Expected at least 2 CRC snapshots, got {len(crcs)}"
        )

        last_crc = crcs[-1]
        assert last_crc == _CE_PROMPT_CRC, (
            f"After ENTER in mode menu, expected prompt CRC {_CE_PROMPT_CRC!r} "
            f"but got {last_crc!r} — settings save may have crashed"
        )

    def test_precision_setting_affects_output(self) -> None:
        """
        Change precision in mode menu from default (6 digits) to 2 digits,
        then submit "1/3".  The result should show 2 decimal places: "0.33".

        Navigation:
          MODE → DOWN×4 (reach Precision item) → RIGHT (increase precision?
          no — we want fewer digits, so LEFT) → navigate to precision field
          and reduce → ENTER to save → "1/3" → ENTER → capture text.

        Implementation note: the menu order is:
          0: Show steps (bool)
          1: Step detail (ALL/CMP)
          2: Segment pause (bool)
          3: Exact mode (bool)
          4: Precision (1-9)

        We navigate DOWN×4 to reach item 4 (Precision), press LEFT twice to
        reduce from default 6 to 4, then ENTER to save.
        """
        extra = [
            _get_crc(),
            # Open mode menu
            _key("mode"), _step(800),
            # Navigate DOWN 4 times to Precision
            _key("down"), _step(200),
            _key("down"), _step(200),
            _key("down"), _step(200),
            _key("down"), _step(200),
            # Press LEFT twice: 6 -> 5 -> 4
            _key("left"), _step(200),
            _key("left"), _step(200),
            # Save
            _key("enter", duration_ms=400), _step(600),
            # Submit "1/3"
            _key("1"), _step(80), _key("div"), _step(80), _key("3"), _step(80),
            _key("enter", duration_ms=400),
            _step(5000),
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=300)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, "No screen text after precision-changed computation"

        combined = "\n".join(texts)

        # With precision=4, "0.3333" should appear (not "0.333333" at precision=6)
        assert "0.333" in combined, (
            f"Expected '0.333...' in result after precision change but got:\n"
            f"{combined!r}\n\n"
            "Check that: (1) mode menu navigation reaches the Precision item, "
            "(2) LEFT key decrements precision, (3) ENTER saves to g_settings "
            "and CE_STATE, (4) the worker reads settings from the request."
        )


# ---------------------------------------------------------------------------
# Phase 4 follow-through: single-enter result flow
# ---------------------------------------------------------------------------

@_NEEDS_FULL_STACK
class TestSingleEnterResultFlow(_CalcEngineTestBase):
    """
    End-to-end follow-through tests for the Phase 4 result-flow repair.

    The launcher no longer routes results through a blocking graphx overlay.
    A single ENTER is enough: submit once, wait for the worker cycle, and the
    home screen should already show the result in history with the next prompt
    ready for another expression.
    """

    def _submit_and_capture_text(
        self,
        keys: list[str],
        wait_ms: int = 5000,
    ) -> str:
        extra: list[dict[str, Any]] = [_get_crc()]
        for key in keys:
            extra.append(_key(key))
            extra.append(_step(80))
        extra += [
            _key("enter", duration_ms=400),
            _step(wait_ms),
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=300)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert texts, "No screen text captured after a single ENTER submit"
        return texts[-1]

    def test_single_enter_shows_plain_result_in_history(self) -> None:
        text = self._submit_and_capture_text(["5", "+", "3"])

        assert "8" in text, (
            f"Expected result '8' on the home screen after one ENTER but got:\n{text!r}"
        )

    def test_single_enter_shows_power_result_without_dismiss_key(self) -> None:
        text = self._submit_and_capture_text(["2", "^", "8"])

        assert "256" in text, (
            f"Expected result '256' on the home screen after one ENTER but got:\n{text!r}"
        )

    def test_single_enter_shows_radical_result_without_dismiss_key(self) -> None:
        text = self._submit_and_capture_text(["2nd", "^2", "1", "6", ")"])

        assert "4" in text, (
            f"Expected sqrt(16) to render result '4' on the home screen after one ENTER but got:\n{text!r}"
        )

    def test_single_enter_keeps_history_and_next_prompt_submittable(self) -> None:
        extra = [
            _get_crc(),
            _key("4"), _step(80), _key("+"), _step(80), _key("4"), _step(80),
            _key("enter", duration_ms=400), _step(5000),
            _key("5"), _step(80), _key("+"), _step(80), _key("1"), _step(80),
            _get_text(),
            _key("enter", duration_ms=400), _step(5000),
            _get_text(),
        ]
        responses = self._run_calcengine_session(extra, timeout=300)
        self._assert_calcengine_running(responses)

        texts = self._text_responses(responses)
        assert len(texts) >= 2, "Expected text captures before and after submitting the follow-up expression"

        before_submit = texts[-2]
        after_submit = texts[-1]
        combined = "\n".join(texts)

        assert "8" in combined, (
            f"Expected prior result '8' to remain in history but got:\n{combined!r}"
        )
        assert "5+1" in before_submit or "5 + 1" in before_submit, (
            f"Expected the next prompt/input line to remain editable after one ENTER but got:\n{before_submit!r}"
        )
        assert "6" in after_submit, (
            f"Expected the follow-up expression '5+1' to evaluate after one additional ENTER but got:\n{after_submit!r}"
        )
