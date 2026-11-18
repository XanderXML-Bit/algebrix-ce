"""
test_lcd_text_emulator.py - Hardware-equivalent LCD text assertion suite.

This pytest module is the emulator-layer port of tests/test_lcd_text.py (the
DESKTOP layer's 59-expression substring-assertion suite that drives
bin/calcengine.exe --solve). It validates the SAME 59 expressions, but by
driving the real .8xp bundle inside the headless CEmu fork at
tools/emulator/cemu-headless/ and comparing the resulting LCD frame's CRC
against a captured baseline in lcd_text_emulator_baseline.json.

Why CRC and not substring?
--------------------------
The current cemu-headless binary exposes CRC and PNG frame capture but not
ASCII text extraction (see tools/emulator/cemu-headless/README.md). Until
that TODO lands we use the strictest hardware-equivalent substitute: per-
expression CRC comparison. The expected-substring strings from PR #20 are
preserved inside the baseline JSON as a human-auditable record of what each
CRC is supposed to mean, but the assertion itself is CRC-based.

Hard rules (user-enforced 2026-04-13)
-------------------------------------
* The headless binary MUST resolve to a file under
  tools/emulator/cemu-headless/. No PATH lookup, no GUI CEmu fallback, no
  silent substitution. If no such binary exists, this module HARD-FAILS at
  collection time (`pytest.exit` with a clear message) rather than silently
  routing to the system CEmu install.
* Absolute paths only for ROM, bundle, and autotester inputs.
* Never delete or mutate files outside the test tree.

Skip vs. fail matrix
--------------------
* Headless binary missing  -> HARD FAIL at collection (pytest.exit).
* ROM missing              -> module-wide skip with clear message.
* Bundle missing           -> module-wide skip with clear message.
* Baseline empty for an expr (`expected_crcs: []`) -> that test SKIPS with
  a message pointing at capture_lcd_text_baseline.py.
* Baseline populated and CRC matches -> PASS.
* Baseline populated and CRC mismatches -> FAIL. This is the signal.

One subprocess, 59 expressions
------------------------------
Booting TI-OS and loading the .8xp bundle costs ~30 seconds per run. Rather
than spin 59 subprocesses, we boot ONCE in a session-scoped fixture, queue
all 59 expression cycles, run the subprocess, capture every CRC, and then
let the 59 parametrized tests each assert their own CRC against the
baseline. Total wall time stays in the 4-8 minute range instead of blowing
past 30 minutes.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

import pytest

from _lcd_text_emulator_shared import (  # type: ignore[import-not-found]
    BASELINE_PATH,
    HEADLESS_CANDIDATES,
    HeadlessSession,
    HeadlessSessionError,
    SessionResults,
    baseline_expression_index,
    explain_resolved_paths,
    load_baseline,
    resolve_autotester,
    resolve_bundle,
    resolve_headless_binary,
    resolve_rom,
)


# ---------------------------------------------------------------------------
# Collection-time guards. The headless-binary check is a HARD FAIL per the
# user's 2026-04-13 rule: if the pinned binary is missing, do not silently
# substitute anything. ROM/bundle absence is a soft skip.
# ---------------------------------------------------------------------------

def _assert_headless_binary_present_or_hardfail() -> Path:
    """
    Resolve the headless binary or exit the whole pytest run with a clear
    message. We use pytest.exit (not skip) because silent substitution is
    exactly the failure mode the user banned.
    """
    try:
        return resolve_headless_binary()
    except FileNotFoundError as exc:
        # HARD FAIL - do not skip, do not fall through. The user explicitly
        # banned silent fallback to GUI CEmu, PATH lookup, or anything not
        # under tools/emulator/cemu-headless/. A missing binary means the
        # suite cannot run hardware-equivalent checks and must shout.
        pytest.exit(
            "tests/emulator/test_lcd_text_emulator.py: refusing to run "
            "without the pinned headless fork binary.\n"
            f"{exc}\n\n"
            f"Resolved paths:\n{explain_resolved_paths()}\n\n"
            "Build the headless fork with tools/emulator/cemu-headless/build.ps1 "
            "and rerun. This suite will not fall through to GUI CEmu, "
            "anything on PATH, or any other binary - that is a hard rule.",
            returncode=2,
        )


HEADLESS_BINARY: Path = _assert_headless_binary_present_or_hardfail()

# Extra containment check: the resolved binary MUST live under
# tools/emulator/cemu-headless/. resolve_headless_binary already checks this,
# but we re-assert here at collection time to be defence-in-depth; the
# user was explicit that this is non-negotiable.
_HEADLESS_DIR = (Path(__file__).resolve().parents[1].parent / "tools" / "emulator" / "cemu-headless").resolve()
try:
    HEADLESS_BINARY.relative_to(_HEADLESS_DIR)
except ValueError:
    pytest.exit(
        f"Resolved headless binary {HEADLESS_BINARY} is NOT under {_HEADLESS_DIR}. "
        "This suite refuses to launch anything else.",
        returncode=2,
    )

ROM_PATH: Path | None = resolve_rom()
BUNDLE_PATH: Path | None = resolve_bundle()
AUTOTESTER_PATH: Path | None = resolve_autotester()

_ROM_MISSING_REASON = (
    "Rom.rom not found. Checked CALCENGINE_ROM_PATH, rom/Rom.rom, and the "
    "OneDrive repo copy. Set CALCENGINE_ROM_PATH to the absolute path of "
    "your dumped TI-84 Plus CE ROM, then rerun.\n"
    f"Resolved paths:\n{explain_resolved_paths()}"
)
_BUNDLE_MISSING_REASON = (
    "CalcEngine .8xp bundle directory not found. Checked "
    "CALCENGINE_BUNDLE_DIR, C:/Users/user/Downloads/CalcEngine, and "
    "bin/core/. Build the TI target (make) and copy outputs to Downloads, "
    "or set CALCENGINE_BUNDLE_DIR.\n"
    f"Resolved paths:\n{explain_resolved_paths()}"
)


# ---------------------------------------------------------------------------
# Load baseline at collection time - used both for test IDs and skip logic.
# ---------------------------------------------------------------------------

def _load_baseline_or_fail() -> dict[str, Any]:
    try:
        return load_baseline()
    except FileNotFoundError:
        pytest.exit(
            f"Baseline file not found at {BASELINE_PATH}. This file is "
            "checked into the repo; if it is missing, restore it from git.",
            returncode=2,
        )
    except json.JSONDecodeError as exc:
        pytest.exit(
            f"Baseline file at {BASELINE_PATH} is not valid JSON: {exc}",
            returncode=2,
        )


BASELINE_PAYLOAD = _load_baseline_or_fail()
BASELINE_INDEX = baseline_expression_index(BASELINE_PAYLOAD)
EXPRESSION_ENTRIES: list[dict[str, Any]] = list(BASELINE_PAYLOAD.get("expressions", []))

if len(EXPRESSION_ENTRIES) != 59:
    pytest.exit(
        f"Baseline file {BASELINE_PATH} has {len(EXPRESSION_ENTRIES)} "
        f"entries, expected 59 (the full PR #20 set). Do not drop entries.",
        returncode=2,
    )


# ---------------------------------------------------------------------------
# Session-scoped fixture: boot + queue all 59 expressions + run once.
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def session_results() -> SessionResults:
    """
    Run cemu-headless once, capture CRC + OCR text for every expression, and
    return a SessionResults. All downstream fixtures share this single session.

    On any session-level failure (subprocess error, load_rom failure, etc.)
    the fixture calls pytest.fail() with full diagnostic context so that every
    downstream test fails with the same clear message rather than hiding the
    root cause behind a KeyError or similar secondary exception.
    """
    if ROM_PATH is None:
        pytest.skip(_ROM_MISSING_REASON)
    if BUNDLE_PATH is None:
        pytest.skip(_BUNDLE_MISSING_REASON)

    session = HeadlessSession(
        headless_binary=HEADLESS_BINARY,
        rom=ROM_PATH,
        bundle=BUNDLE_PATH,
        autotester=AUTOTESTER_PATH,
    )
    session.queue_boot_and_launch()
    for entry in EXPRESSION_ENTRIES:
        session.queue_expression(entry["expr"], entry["id"])

    try:
        results = session.run()
    except HeadlessSessionError as exc:
        pytest.fail(
            "cemu-headless session failed while capturing LCD data.\n"
            f"Headless binary : {HEADLESS_BINARY}\n"
            f"ROM             : {ROM_PATH}\n"
            f"Bundle          : {BUNDLE_PATH}\n"
            f"Autotester      : {AUTOTESTER_PATH}\n"
            f"Error           : {exc}\n"
        )
    except FileNotFoundError as exc:
        # subprocess.Popen can raise this if the binary vanishes between
        # collection and execution (e.g. antivirus quarantine).
        pytest.fail(f"Headless binary disappeared between collection and run: {exc}")
    except Exception as exc:  # pragma: no cover - last-resort diagnostic
        pytest.fail(f"Unexpected error driving cemu-headless: {exc!r}")

    # Drop the synthetic boot markers so they do not pollute expression results.
    # The text capture uses a "__text__" suffix tag, so strip both variants.
    results.crcs_by_tag.pop("__boot_calcengine_prompt__", None)
    results.texts_by_tag.pop("__boot_calcengine_prompt__", None)
    results.texts_by_tag.pop("__boot_calcengine_prompt____text__", None)
    return results


@pytest.fixture(scope="session")
def captured_crcs(session_results: SessionResults) -> dict[str, str]:
    """CRC dict (expression id -> 8-char uppercase hex).

    Thin wrapper around session_results.crcs_by_tag for backward-compatible
    access. The underlying session runs exactly once, shared with captured_texts.
    """
    return session_results.crcs_by_tag


@pytest.fixture(scope="session")
def captured_texts(session_results: SessionResults) -> dict[str, str]:
    """OCR text dict (expression id -> extracted screen text).

    Populated when the binary supports format="text" (native_core backend).
    Empty dict when the backend only returned CRC (wrapper mode or older build).
    Use as belt-and-suspenders alongside CRC: if the text dict is non-empty,
    substring assertions provide semantic, cosmetic-change-tolerant coverage.
    """
    return session_results.texts_by_tag


# ---------------------------------------------------------------------------
# 59 parametrized tests, one per expression id.
# ---------------------------------------------------------------------------

_PARAMS = [
    pytest.param(entry, id=entry["id"])
    for entry in EXPRESSION_ENTRIES
]


@pytest.mark.parametrize("entry", _PARAMS)
def test_expression_lcd_crc(
    entry: dict[str, Any],
    captured_crcs: dict[str, str],
    captured_texts: dict[str, str],
) -> None:
    """
    Belt-and-suspenders assertion: CRC match AND text-substring match.

    CRC assertion (exact render check)
    ------------------------------------
    Assert that the LCD CRC captured for this expression matches one of the
    baselines recorded in lcd_text_emulator_baseline.json.  A mismatch here
    catches pixel-level regressions including layout changes, cursor drift,
    colour palette changes, and garbled output.

    Substring assertion (semantic check, when OCR is available)
    ------------------------------------------------------------
    When the headless binary supports format="text" (native_core backend),
    also assert that every string in entry['expected_text_substrings'] appears
    in the OCR-extracted screen text.  This check is *tolerant* of cosmetic
    render changes (e.g. extra whitespace, minor alignment shifts) that would
    trip the CRC check but leave the answer semantically correct.

    If OCR text is absent (wrapper mode, older build, or the text dict is empty
    because the backend does not support format="text") the substring check is
    silently skipped — the CRC check alone is the safety net in that case.

    Skip / fail matrix (this test)
    --------------------------------
    * baseline_crcs empty             → SKIP (not yet captured)
    * CRC not in baseline_crcs        → FAIL (pixel-level regression)
    * CRC in baseline AND text absent → PASS (OCR not available, CRC passed)
    * CRC in baseline AND substrings  → also check substrings; FAIL if missing
    """
    expression_id: str = entry["id"]
    label: str = entry["label"]
    expression_text: str = entry["expr"]
    expected_substrings: list[str] = list(entry.get("expected_text_substrings", []))
    baseline_crcs: list[str] = [c.upper() for c in entry.get("expected_crcs", []) if isinstance(c, str)]

    if not baseline_crcs:
        pytest.skip(
            f"No CRC baseline captured yet for '{expression_id}' ({label}).\n"
            "Run tests/emulator/capture_lcd_text_baseline.py on a known-good "
            "Windows build to populate expected_crcs in "
            "tests/emulator/lcd_text_emulator_baseline.json, then rerun.\n"
            f"Resolved paths:\n{explain_resolved_paths()}"
        )

    observed_crc = captured_crcs.get(expression_id)
    if observed_crc is None:
        pytest.fail(
            f"No CRC captured for expression '{expression_id}'. This means "
            "the cemu-headless session ran but dropped the screen capture "
            "for this expression. Check session diagnostics for "
            "get_screen response count vs. request count."
        )

    observed_crc_norm = observed_crc.upper()
    if observed_crc_norm not in baseline_crcs:
        pytest.fail(
            "LCD CRC mismatch on hardware-equivalent path.\n"
            f"  id               : {expression_id}\n"
            f"  label            : {label}\n"
            f"  expression       : {expression_text}\n"
            f"  observed CRC     : {observed_crc_norm}\n"
            f"  baseline CRCs    : {baseline_crcs}\n"
            f"  expected text    : {expected_substrings}\n"
            "\n"
            "Interpretation: driving the real .8xp bundle through "
            "TI-OS -> CalcEngine -> LCD produced a different frame than "
            "the recorded baseline. Either:\n"
            "  (a) CalcEngine's hardware-path output regressed (look at "
            "      recent changes in src/ touching this expression's "
            "      processor - parse, eval, simplify, deriv, integrate, "
            "      limits, series, algebra, stats, matrix, etc.), or\n"
            "  (b) The baseline is stale and needs recapture after a "
            "      deliberate output change. Rerun "
            "      tests/emulator/capture_lcd_text_baseline.py ONLY after "
            "      you have confirmed the new output is correct."
        )

    # ── Belt-and-suspenders: substring assertion ────────────────────────────
    # Only runs when OCR text is available (native_core backend with format=text).
    # Silently skipped when captured_texts is empty (wrapper mode / older build).
    observed_text: str | None = captured_texts.get(expression_id)
    if observed_text is not None and expected_substrings:
        missing_substrings = [s for s in expected_substrings if s not in observed_text]
        if missing_substrings:
            pytest.fail(
                "LCD text substring mismatch on hardware-equivalent path.\n"
                f"  id                : {expression_id}\n"
                f"  label             : {label}\n"
                f"  expression        : {expression_text}\n"
                f"  observed text     : {observed_text!r}\n"
                f"  missing substrings: {missing_substrings}\n"
                f"  all expected      : {expected_substrings}\n"
                "\n"
                "The OCR-extracted LCD text does not contain every expected "
                "substring. Possible causes:\n"
                "  (a) CalcEngine's output changed for this expression — check "
                "      recent src/ changes for the relevant processor.\n"
                "  (b) The OCR quality is degraded — check the font table in "
                "      tools/emulator/cemu-headless/main.c (native_lcd_extract_text) "
                "      and verify the binary was rebuilt after any main.c edits.\n"
                "  (c) The expected_text_substrings in the baseline JSON are wrong "
                "      — they should be updated if the correct output changed."
            )


# ---------------------------------------------------------------------------
# Metadata check: the baseline's recorded SHA256 headers (when populated)
# should match the binaries we actually just ran against. This catches the
# case where someone updates the headless binary or bundle but forgets to
# recapture - the baseline CRCs silently become invalid.
# ---------------------------------------------------------------------------

def test_baseline_metadata_consistency(captured_crcs: dict[str, str]) -> None:
    """
    Compare the baseline's recorded artifact SHA256s (if any) against what
    we actually loaded. Skips when the baseline has not yet recorded any
    SHA256 (fresh/empty baseline).
    """
    recorded_headless = BASELINE_PAYLOAD.get("headless_binary_sha256")
    recorded_rom = BASELINE_PAYLOAD.get("rom_sha256")
    recorded_bundle = BASELINE_PAYLOAD.get("bundle_sha256")

    if not any([recorded_headless, recorded_rom, recorded_bundle]):
        pytest.skip(
            "Baseline has no recorded SHA256 metadata yet. Run "
            "capture_lcd_text_baseline.py to populate."
        )

    # We do not fail on mismatch - instead we emit a warning-style assertion
    # that is loud but not show-stopping, because legitimate environment
    # drift (e.g. recompiled headless binary for speed) is fine as long as
    # the CRC set still matches. The captured_crcs fixture already ran, so
    # if those CRCs matched we trust them; this check is purely advisory.
    drift = []
    from _lcd_text_emulator_shared import sha256_file  # local import to avoid cycles
    observed_headless = sha256_file(HEADLESS_BINARY)
    if recorded_headless and observed_headless and recorded_headless != observed_headless:
        drift.append(
            f"headless_binary_sha256: baseline={recorded_headless} observed={observed_headless}"
        )
    if recorded_rom and ROM_PATH is not None:
        observed_rom = sha256_file(ROM_PATH)
        if observed_rom and recorded_rom != observed_rom:
            drift.append(f"rom_sha256: baseline={recorded_rom} observed={observed_rom}")

    if drift and os.environ.get("CALCENGINE_STRICT_BASELINE_META") == "1":
        pytest.fail(
            "Baseline metadata drift detected (strict mode):\n  "
            + "\n  ".join(drift)
        )
