"""
Shared infrastructure for the hardware-equivalent LCD text suite.

This module backs BOTH:

  * tests/emulator/test_lcd_text_emulator.py (pytest verify mode)
  * tests/emulator/capture_lcd_text_baseline.py (manual baseline capture)

It is the single owner of:
  * Absolute-path resolution for the headless emulator binary, ROM, and
    CalcEngine .8xp bundle. HARD RULE: tools/emulator/cemu-headless/
    exclusively - no PATH lookup, no GUI CEmu, no silent fallback.
  * The cemu-headless JSON-line session lifecycle (subprocess management,
    command dispatch, response parsing).
  * Expression -> TI-84 keypad sequence encoding, reusing the repo-canonical
    scripts/cemu/keypress_sequences.encoder and extending it for the `=`
    character that PR #20's `solve(x^2=4,x)` case needs.
  * Boot + CalcEngine launch + result-CRC capture + return-to-prompt.
  * Baseline JSON load / atomic save.

Context
-------
PR #19 introduced the emulator validation harness: load the real .8xp bundle
into the headless CEmu fork, emulate keypresses via `autotester.exe`, and
validate result screens by CRC.

PR #20 separately added tests/test_lcd_text.py, which validates the SAME 59
expressions by text-substring assertions against the DESKTOP calcengine.exe
--solve binary. It skips the TI-OS layer, module dispatch, and RAM path
where real hardware bugs (like the "2+3 RAM Cleared" regression) live.

This file ports PR #20's 59 expressions to the emulator layer. There is one
unavoidable architectural gap: the current headless binary exposes CRC and
PNG frame capture, but not ASCII text extraction from the LCD. The
repo-level README tracks ASCII extraction as TODO pending native cemu-core
wiring (see tools/emulator/cemu-headless/README.md). Until that lands, the
strictest hardware-equivalent substitute for text-substring assertions is
per-expression CRC comparison against a captured baseline. That is what
this suite implements.

Structure of a validation run
-----------------------------
1. Launch tools/emulator/cemu-headless/cemu-headless.exe (absolute path).
2. load_rom against rom/Rom.rom (or $CALCENGINE_ROM_PATH).
3. load_app against the CalcEngine deployment directory
   (deploy/hardware-ready by convention, else bin/core/).
4. Step until the boot screen is up.
5. Press clear, then PRGM -> navigate to CALCENG -> enter -> wait for the
   CalcEngine prompt to render.
6. For each of the 59 expressions:
     a. Type the expression via ALPHA-encoded key sequence.
     b. Press ENTER, wait for the result to render.
     c. Capture screen CRC, return it to the caller.
     d. Press CLEAR to return to the CalcEngine prompt for the next expr.

Baseline capture
----------------
`capture_lcd_text_baseline.py` runs the same pipeline in write mode: it
captures the CRC after each expression, appends it to that expression's
`expected_crcs` list in lcd_text_emulator_baseline.json (deduped), and
writes the file atomically. Multiple capture runs can accumulate baselines
for render-timing flap (same pattern used by autotest.comprehensive).

Verify mode (pytest)
--------------------
The pytest file loads the baseline JSON once per session, spins up a single
cemu-headless session (in the session-scoped fixture), and runs 59 test
functions. Each test asserts the CRC of its result screen appears in the
baseline. If the baseline list is empty (never captured), the test SKIPS
with a clear message. If the baseline is populated and the CRC does not
match, the test FAILS - which is the signal for a real hardware-path bug.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

# ---------------------------------------------------------------------------
# Path resolution - absolute paths only, no PATH lookup, no GUI fallback.
# ---------------------------------------------------------------------------

REPO_ROOT: Path = Path(__file__).resolve().parents[2]
"""Repository root, computed from this file's location."""

# The headless fork is the ONLY acceptable emulator binary for this suite.
# HARD RULE (user-enforced 2026-04-13): GUI CEmu must never launch. We pin
# the path to the in-tree fork and bake an existence assertion into the
# suite so it hard-fails rather than fall through to anything on PATH.
HEADLESS_PRIMARY: Path = REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "cemu-headless.exe"
"""Absolute pinned path to the headless fork."""

HEADLESS_FALLBACK_NATIVE: Path = (
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build_native" / "cemu-headless.exe"
)
"""Only acceptable fallback: the native-core build under the same fork directory.
Still inside tools/emulator/cemu-headless/ - still the headless fork, not GUI CEmu."""

HEADLESS_CANDIDATES: tuple[Path, ...] = (HEADLESS_PRIMARY, HEADLESS_FALLBACK_NATIVE)
"""Ordered candidates. Both live under tools/emulator/cemu-headless/."""

ROM_CANDIDATES: tuple[Path, ...] = (
    Path(os.environ.get("CALCENGINE_ROM_PATH", "")) if os.environ.get("CALCENGINE_ROM_PATH") else Path(""),
    REPO_ROOT / "rom" / "Rom.rom",
)

BUNDLE_CANDIDATES: tuple[Path, ...] = (
    Path(os.environ.get("CALCENGINE_BUNDLE_DIR", "")) if os.environ.get("CALCENGINE_BUNDLE_DIR") else Path(""),
    REPO_ROOT / "deploy" / "hardware-ready",
    REPO_ROOT / "bin" / "core",
)

AUTOTESTER_CANDIDATES: tuple[Path, ...] = (
    Path(os.environ.get("CEMU_HEADLESS_AUTOTESTER_PATH", "")) if os.environ.get("CEMU_HEADLESS_AUTOTESTER_PATH") else Path(""),
    Path(os.environ.get("CEMU_AUTOTESTER_PATH", "")) if os.environ.get("CEMU_AUTOTESTER_PATH") else Path(""),
    Path.home() / "Tools" / "CEmu" / "v2.0" / "autotester.exe",
    Path.home() / "Tools" / "CEmu" / "autotester.exe",
    Path("C:/Tools/CEmu/v2.0/autotester.exe"),
    Path("C:/Tools/CEmu/autotester.exe"),
)

ARTIFICE_CANDIDATES: tuple[Path, ...] = (
    Path(os.environ.get("CALCENGINE_ARTIFICE_PATH", "")) if os.environ.get("CALCENGINE_ARTIFICE_PATH") else Path(""),
    REPO_ROOT / "tools" / "emulator" / "assets" / "artifice-v2.8xp",
    REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE.8xp",
)
"""arTIfiCE jailbreak binary candidates. Renamed A.8xp at transfer time so it
sorts first in the PRGM menu and the 3-ENTER launch sequence reaches CALCENG."""

BASELINE_PATH: Path = REPO_ROOT / "tests" / "emulator" / "lcd_text_emulator_baseline.json"


def _first_existing(candidates: Iterable[Path]) -> Path | None:
    for candidate in candidates:
        s = str(candidate)
        # Path("") stringifies to "." and always exists - skip it.
        if not s or s == ".":
            continue
        try:
            if candidate.exists():
                return candidate.resolve()
        except OSError:
            continue
    return None


def resolve_headless_binary() -> Path:
    """
    Return the absolute, resolved path to the headless fork binary.

    Hard-fails if no binary is found under tools/emulator/cemu-headless/.
    Does NOT fall through to PATH, CEmu.exe, or any GUI install.
    """
    binary = _first_existing(HEADLESS_CANDIDATES)
    if binary is None:
        raise FileNotFoundError(
            "Headless CEmu binary not found. Checked (absolute paths only): "
            + "; ".join(str(c) for c in HEADLESS_CANDIDATES)
            + ". Build it via tools/emulator/cemu-headless/build.ps1. This suite"
            " refuses to fall through to PATH or to the GUI CEmu install; the"
            " hard rule is headless-fork only."
        )
    # Final safety: the resolved path must live under tools/emulator/cemu-headless/.
    try:
        binary.relative_to((REPO_ROOT / "tools" / "emulator" / "cemu-headless").resolve())
    except ValueError as exc:
        raise RuntimeError(
            f"Resolved headless binary {binary} is outside tools/emulator/cemu-headless/."
            " This suite refuses to launch anything else - hard rule: headless fork only."
        ) from exc
    return binary


def resolve_rom() -> Path | None:
    """Absolute path to Rom.rom, or None if absent (lets tests skip cleanly)."""
    return _first_existing(ROM_CANDIDATES)


def resolve_bundle() -> Path | None:
    """
    Absolute path to the CalcEngine .8xp bundle directory, or None if absent.
    A directory is acceptable because cmd_load_app in main.c supports
    directory enumeration.
    """
    return _first_existing(BUNDLE_CANDIDATES)


def resolve_autotester() -> Path | None:
    """
    Absolute path to autotester.exe. cemu-headless can auto-resolve this
    from env vars and common install paths, so returning None is fine -
    the binary will handle it. We pass an explicit path when we have one
    to remove ambiguity.
    """
    return _first_existing(AUTOTESTER_CANDIDATES)


def resolve_artifice() -> Path | None:
    """
    Absolute path to the arTIfiCE jailbreak .8xp, or None if not found.
    CalcEngine modules are ASM programs that require arTIfiCE on TI-OS 5.6+
    to launch from the PRGM menu. The binary is renamed to A.8xp at transfer
    time so the PRGM list puts it before all CalcEngine modules alphabetically.
    """
    return _first_existing(ARTIFICE_CANDIDATES)


def sha256_file(path: Path) -> str | None:
    try:
        hasher = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(65536), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return None


# ---------------------------------------------------------------------------
# Keypad encoding - reuse the canonical encoder, extend for `=`.
# ---------------------------------------------------------------------------

# Ensure scripts/cemu is importable before we touch keypress_sequences.
_SCRIPTS_CEMU = str(REPO_ROOT / "scripts" / "cemu")
if _SCRIPTS_CEMU not in sys.path:
    sys.path.insert(0, _SCRIPTS_CEMU)

import keypress_sequences as _kps  # noqa: E402  (path munging above)


# The canonical encoder in scripts/cemu/keypress_sequences.py covers every
# character in 58 of the 59 expressions. Only `solve(x^2=4,x)` contains the
# `=` character, which on the TI-84 CE keypad is produced via the [2nd]
# [math] (TEST menu) -> option 1 sequence. Rather than mutate the shared
# encoder (and risk perturbing unrelated fidelity suites), we post-process
# the token stream locally.

_EQUALS_SUBSTITUTION: tuple[str, ...] = ("2nd", "math", "1", "delay|350")
"""Tokens emitted for `=`: open TEST menu, select 1:=, settle."""


def encode_expression_tokens(expression_text: str) -> tuple[str, ...]:
    """
    Convert a CalcEngine expression string into a tuple of inject_key
    key names (plus `delay|ms` markers), suitable for the headless binary's
    inject_key command.

    Reuses scripts/cemu/keypress_sequences.expression_tokens for everything
    except `=`, which is handled locally because the TI-84 keypad does not
    expose `=` as a single key.
    """
    if not expression_text:
        return ()

    # Split around `=`, encode each segment via the canonical encoder, and
    # stitch them back together with the TEST-menu sequence.
    if "=" not in expression_text:
        return _kps.expression_tokens(expression_text)

    segments = expression_text.split("=")
    stitched: list[str] = []
    for index, segment in enumerate(segments):
        if segment:
            stitched.extend(_kps.expression_tokens(segment))
        if index < len(segments) - 1:
            stitched.extend(_EQUALS_SUBSTITUTION)
    return tuple(stitched)


# ---------------------------------------------------------------------------
# Baseline JSON load / atomic save.
# ---------------------------------------------------------------------------

def load_baseline() -> dict[str, Any]:
    """Read tests/emulator/lcd_text_emulator_baseline.json as a dict."""
    return json.loads(BASELINE_PATH.read_text(encoding="utf-8"))


def save_baseline_atomic(payload: dict[str, Any]) -> None:
    """
    Atomically overwrite the baseline file. We write to a sibling temp
    file and rename, so a crashed capture run never leaves a half-written
    baseline.
    """
    # Normalize key ordering for stable diffs across capture runs.
    serialized = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
    directory = BASELINE_PATH.parent
    with tempfile.NamedTemporaryFile(
        "w", dir=str(directory), delete=False, encoding="utf-8", suffix=".tmp"
    ) as handle:
        handle.write(serialized)
        temp_path = Path(handle.name)
    os.replace(str(temp_path), str(BASELINE_PATH))


def baseline_expression_index(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Return a dict mapping expression id -> baseline record."""
    return {entry["id"]: entry for entry in payload.get("expressions", [])}


# ---------------------------------------------------------------------------
# Headless session wrapper.
# ---------------------------------------------------------------------------

@dataclass
class SessionArtifacts:
    """Bookkeeping recorded during a session for diagnostics."""

    headless_binary: Path
    rom: Path
    bundle: Path
    autotester: Path | None
    headless_sha256: str | None
    rom_sha256: str | None
    bundle_sha256: str | None
    commands: list[dict[str, Any]] = field(default_factory=list)
    responses: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class SessionResults:
    """Results returned by HeadlessSession.run().

    crcs_by_tag  : mapping of capture tag -> CRC hex string (8 upper-case chars).
                   Always populated; used for exact-render regression detection.
    texts_by_tag : mapping of capture tag -> OCR-extracted screen text.
                   Populated when the binary supports format="text" (native_core
                   backend). Empty dict when the backend only returned CRC.
    """

    crcs_by_tag: dict[str, str]
    texts_by_tag: dict[str, str]


class HeadlessSessionError(RuntimeError):
    """Raised when the cemu-headless subprocess misbehaves."""


class HeadlessSession:
    """
    Drive a single cemu-headless subprocess end-to-end.

    Rather than open/close a subprocess per expression (which would cost
    ~30s of boot + app transfer per test), we boot once, park at the
    CalcEngine prompt, and dispatch 59 expression cycles inside the same
    process. Each cycle: type -> enter -> step -> get_screen -> clear.
    """

    # Timing defaults picked to match the patterns in
    # tests/emulator/autotest.comprehensive.json, which already has
    # battle-tested timings for CalcEngine expression entry.
    DEFAULT_KEY_DURATION_MS: int = 80
    AFTER_KEY_SETTLE_MS: int = 60
    AFTER_ENTER_RESULT_MS: int = 2200
    AFTER_CLEAR_RETURN_MS: int = 450
    BOOT_SETTLE_MS: int = 4000
    CALCENGINE_LAUNCH_SETTLE_MS: int = 4500
    SESSION_TIMEOUT_S: float = 900.0

    def __init__(
        self,
        headless_binary: Path,
        rom: Path,
        bundle: Path,
        autotester: Path | None = None,
        artifice: Path | None = None,
    ) -> None:
        self.headless_binary = headless_binary
        self.rom = rom
        self.bundle = bundle
        self.autotester = autotester
        # Auto-resolve arTIfiCE if caller did not provide it.
        self.artifice: Path | None = artifice if artifice is not None else resolve_artifice()
        self._commands: list[dict[str, Any]] = []
        self._artifice_tmp: Path | None = None  # temp A.8xp; held alive until run() completes
        self.artifacts = SessionArtifacts(
            headless_binary=headless_binary,
            rom=rom,
            bundle=bundle,
            autotester=autotester,
            headless_sha256=sha256_file(headless_binary),
            rom_sha256=sha256_file(rom),
            bundle_sha256=None,  # Bundle is a directory; digest on demand if needed.
        )

    # ---- command building ------------------------------------------------

    def _cmd(self, **fields: Any) -> None:
        self._commands.append(fields)

    def _key(self, key: str, duration_ms: int | None = None) -> None:
        self._cmd(
            cmd="inject_key",
            key=key,
            duration_ms=int(duration_ms if duration_ms is not None else self.DEFAULT_KEY_DURATION_MS),
        )

    def _step(self, cycles_ms: int) -> None:
        self._cmd(cmd="step", cycles_ms=int(cycles_ms))

    def _capture_marker(self, marker_name: str) -> None:
        """Emit a format="crc" get_screen request. Works in both native and
        wrapper mode. The CRC is the primary regression signal."""
        self._cmd(cmd="get_screen", format="crc", tag=marker_name)

    def _capture_text_marker(self, marker_name: str) -> None:
        """Emit a format="text" get_screen request (native_core backend only).

        Uses the tag ``<marker_name>__text__`` so run() can distinguish these
        optional requests from the mandatory CRC requests. Wrapper-mode error
        responses for this tag are silently swallowed — the CRC capture is the
        authoritative regression signal; text enables belt-and-suspenders
        substring assertions when the binary supports it.
        """
        self._cmd(cmd="get_screen", format="text", tag=marker_name + "__text__")

    # ---- public wiring ---------------------------------------------------

    def queue_boot_and_launch(self) -> None:
        """Queue the one-time preamble: load ROM + bundle, boot TI-OS,
        launch CalcEngine via the arTIfiCE jailbreak, park at its prompt.

        The native_core backend requires one load_app command per file
        (directory paths are not accepted). Transfer order:
          1. load_rom  — boots TI-OS (NATIVE_BOOT_TICKS internally)
          2. *.8xv     — LibLoad, GRAPHX, FILEIOC, KEYPADC runtime AppVars
          3. A.8xp     — arTIfiCE renamed so PRGM lists it first alphabetically
          4. *.8xp     — CalcEngine modules (CALCENG, CEALG, …)
        Launch sequence (arTIfiCE jailbreak, works on TI-OS 5.6+):
          PRGM → ENTER (paste prgmA) → ENTER (run arTIfiCE) → ENTER (CALCENG)
        """
        # Step 1 — load ROM (native backend runs NATIVE_BOOT_TICKS internally)
        load_rom_cmd: dict[str, Any] = {"cmd": "load_rom", "path": str(self.rom)}
        if self.autotester is not None:
            load_rom_cmd["autotester_path"] = str(self.autotester)
        self._commands.append(load_rom_cmd)

        # Step 2 — transfer .8xv runtime AppVars (one load_app per file)
        bundle_path = Path(self.bundle)
        for xv in sorted(bundle_path.glob("*.8xv")):
            self._commands.append({"cmd": "load_app", "path": str(xv)})

        # Step 3 — transfer arTIfiCE renamed as A.8xp so it is first
        # alphabetically in the PRGM menu (required by the 3-ENTER sequence).
        if self.artifice is not None:
            tmp_dir = Path(tempfile.mkdtemp(prefix="calcengine_artifice_"))
            a_path = tmp_dir / "A.8xp"
            shutil.copy2(str(self.artifice), str(a_path))
            self._artifice_tmp = a_path  # keep reference alive until run() finishes
            self._commands.append({"cmd": "load_app", "path": str(a_path)})

        # Step 4 — transfer all .8xp CalcEngine modules (one load_app per file)
        for xp in sorted(bundle_path.glob("*.8xp")):
            self._commands.append({"cmd": "load_app", "path": str(xp)})

        # Allow the OS to settle after all transfers.
        self._step(self.BOOT_SETTLE_MS)

        # Clear any splash / residual state.
        self._key("clear")
        self._step(500)

        # arTIfiCE jailbreak launch sequence:
        #   PRGM  → open program list (A.8xp = arTIfiCE is first entry)
        #   ENTER → paste prgmA to home screen
        #   ENTER → execute prgmA → arTIfiCE shell opens
        #   ENTER → select first entry in arTIfiCE shell (CALCENG)
        self._key("prgm")
        self._step(1200)
        self._key("enter")   # paste prgmA to home
        self._step(1000)
        self._key("enter")   # run prgmA → arTIfiCE shell
        self._step(4000)
        self._key("enter")   # launch CALCENG from arTIfiCE shell
        self._step(self.CALCENGINE_LAUNCH_SETTLE_MS)
        self._capture_marker("__boot_calcengine_prompt__")
        self._capture_text_marker("__boot_calcengine_prompt__")

    def queue_expression(self, expression_text: str, capture_tag: str) -> None:
        """
        Queue the keypress sequence for one expression and request a CRC
        capture after the result has rendered. Finishes by pressing CLEAR
        so the next expression starts from a clean prompt.
        """
        tokens = encode_expression_tokens(expression_text)
        for token in tokens:
            if token.startswith("delay|"):
                # Inline delay marker from the `=` substitution.
                try:
                    delay_ms = int(token.split("|", 1)[1])
                except ValueError:
                    delay_ms = 250
                self._step(delay_ms)
                continue
            self._key(token)
            self._step(self.AFTER_KEY_SETTLE_MS)
        self._step(200)  # settle before ENTER
        self._key("enter")
        self._step(self.AFTER_ENTER_RESULT_MS)
        self._capture_marker(capture_tag)          # CRC — works in both modes
        self._capture_text_marker(capture_tag)     # OCR text — native_core only; errors gracefully skipped
        # Return to prompt for the next expression.
        self._key("clear")
        self._step(self.AFTER_CLEAR_RETURN_MS)

    # ---- execution -------------------------------------------------------

    def run(self) -> SessionResults:
        """
        Execute every queued command in a single cemu-headless subprocess.

        Returns a SessionResults with:
          crcs_by_tag  : capture tag -> CRC hex string (always populated)
          texts_by_tag : capture tag -> OCR screen text (populated when the
                         binary supports format="text"; empty otherwise)

        Raises HeadlessSessionError on protocol-level failures (bad exit,
        load_rom failure, load_app failure, etc.).
        """
        if not self._commands:
            raise HeadlessSessionError(
                "No commands queued. Call queue_boot_and_launch() first."
            )
        commands = list(self._commands)
        commands.append({"cmd": "exit"})

        # cemu-headless does not expose a `tag` field in its get_screen
        # response, so we correlate by emission order: the Nth get_screen
        # response belongs to the Nth capture_marker call we made.
        #
        # Tags ending in "__text__" mark optional OCR captures (format="text").
        # These only succeed in native_core mode; wrapper-mode errors are swallowed
        # rather than raising, so that CRC-only sessions still complete normally.
        ordered_tags: list[str] = [
            str(cmd.get("tag"))
            for cmd in commands
            if cmd.get("cmd") == "get_screen"
        ]
        _TEXT_TAG_SUFFIX = "__text__"

        # Strip our synthetic `tag` field before sending - the binary does
        # not accept unknown fields gracefully in all versions.
        wire_commands = [
            {k: v for k, v in cmd.items() if k != "tag"}
            for cmd in commands
        ]
        input_text = "\n".join(json.dumps(c) for c in wire_commands) + "\n"

        try:
            completed = subprocess.run(
                [str(self.headless_binary)],
                input=input_text,
                capture_output=True,
                text=True,
                timeout=self.SESSION_TIMEOUT_S,
            )
        finally:
            # Clean up the temp arTIfiCE file now that the subprocess has
            # finished (or errored). The file must exist during subprocess
            # execution, so we defer cleanup until here.
            if self._artifice_tmp is not None:
                try:
                    shutil.rmtree(str(self._artifice_tmp.parent), ignore_errors=True)
                except OSError:
                    pass

        responses: list[dict[str, Any]] = []
        for line in completed.stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                responses.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        self.artifacts.commands = wire_commands
        self.artifacts.responses = responses

        # Check load_rom succeeded.
        rom_resp = next((r for r in responses if r.get("cmd") == "load_rom"), None)
        if rom_resp is None or rom_resp.get("ok") is not True:
            raise HeadlessSessionError(
                f"load_rom did not succeed. rom={self.rom}. response={rom_resp!r}. "
                f"stderr={completed.stderr!r}"
            )

        # Check EVERY load_app succeeded (one per file; directory paths are
        # not accepted by the native_core backend).
        app_responses = [r for r in responses if r.get("cmd") == "load_app"]
        if not app_responses:
            raise HeadlessSessionError(
                f"No load_app responses received. bundle={self.bundle}. "
                f"stderr={completed.stderr!r}"
            )
        for i, app_resp in enumerate(app_responses):
            if app_resp.get("ok") is not True:
                path_hint = app_resp.get("path", "?")
                raise HeadlessSessionError(
                    f"load_app #{i + 1} did not succeed for '{path_hint}'. "
                    f"response={app_resp!r}. stderr={completed.stderr!r}"
                )

        # Pair up screen responses with tags in order. Extract CRC from plain
        # captures and OCR text from the optional "__text__"-suffixed captures.
        crcs_by_tag: dict[str, str] = {}
        texts_by_tag: dict[str, str] = {}
        screen_responses = [r for r in responses if r.get("cmd") == "get_screen"]

        # Count required (CRC) and optional (text) requests separately.
        # The total must match, but text errors must NOT raise.
        if len(screen_responses) < len(ordered_tags):
            raise HeadlessSessionError(
                f"Expected {len(ordered_tags)} get_screen responses, saw {len(screen_responses)}. "
                f"Last stderr={completed.stderr!r}"
            )
        for tag, resp in zip(ordered_tags, screen_responses):
            is_text_request = tag.endswith(_TEXT_TAG_SUFFIX)
            base_tag = tag[: -len(_TEXT_TAG_SUFFIX)] if is_text_request else tag

            if resp.get("ok") is not True:
                if is_text_request:
                    # format="text" unsupported (wrapper mode or older binary).
                    # Silently skip — CRC captures still succeed; substring
                    # assertions in the test will be a no-op for this run.
                    continue
                raise HeadlessSessionError(
                    f"get_screen for tag '{tag}' failed: {resp!r}"
                )

            if not is_text_request:
                # Primary CRC response — must be well-formed.
                crc = resp.get("crc")
                if not isinstance(crc, str) or len(crc) != 8:
                    raise HeadlessSessionError(
                        f"get_screen for tag '{tag}' returned malformed CRC: {resp!r}"
                    )
                crcs_by_tag[base_tag] = crc.upper()
            else:
                # Optional text response (format="text" succeeds in native_core).
                # May also include a CRC field — we ignore it here since the
                # dedicated CRC capture already recorded it.
                text = resp.get("text")
                if isinstance(text, str):
                    texts_by_tag[base_tag] = text

        return SessionResults(crcs_by_tag=crcs_by_tag, texts_by_tag=texts_by_tag)


# ---------------------------------------------------------------------------
# Diagnostic helpers.
# ---------------------------------------------------------------------------

def explain_resolved_paths() -> str:
    """Human-readable summary of where the suite will look for its inputs.
    Used by both the pytest skip messages and the capture script's header."""
    binary = _first_existing(HEADLESS_CANDIDATES)
    rom = resolve_rom()
    bundle = resolve_bundle()
    autotester = resolve_autotester()
    artifice = resolve_artifice()
    lines = [
        f"  headless binary : {binary if binary else '<not found>'}",
        f"  rom             : {rom if rom else '<not found>'}",
        f"  bundle          : {bundle if bundle else '<not found>'}",
        f"  autotester.exe  : {autotester if autotester else '<delegated to cemu-headless auto-resolve>'}",
        f"  arTIfiCE.8xp    : {artifice if artifice else '<not found — launch will fail>'}",
        f"  baseline JSON   : {BASELINE_PATH}",
    ]
    return "\n".join(lines)
