#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
repro_hardware_crash.py -- Attempt to reproduce the hardware crash in the emulator.

CONTEXT
-------
Real TI-84 CE hardware crashes ("validating OS") on expression entry.
Root cause identified (2026-04-14): stack overflow on ITERATION 2 re-entry.

CRASH ANATOMY
-------------
  Iteration 1: CALCENG dispatches expression to CEBASE via os_RunPrgm.
               CEBASE evaluates, writes CE_RESP, sets has_pending_response=1,
               calls os_RunPrgm("CALCENG") to re-launch the launcher.

  Iteration 2: CALCENG re-launches (stack reset by OS).  In main() between
               stages L08 and L09, it calls ce_module_runtime_recover_child_failure.
               That function allocates ce_response_t probe (4510 B) as a local,
               then calls ce_response_read(&probe).  Combined with the caller's
               ce_request_t pending_req (4273 B) already on main()'s stack, the
               total stack depth reaches ~9080 bytes -- far beyond the safe stack
               on the TI-84 CE (256 KB RAM, limited stack area above program BSS).
               Result: "validating OS" watchdog reset.

               ce_module_runtime_recover_child_failure  src/split/module_runtime.c:179
               ce_request_t pending_req in caller        src/core/launcher/main.c:562

  Emulator (CEmu/cemu-headless): does NOT reproduce the crash.  CEmu uses a
  flat host memory model and zeroes all RAM on boot.  It does not enforce the
  eZ80 stack boundaries the same way the OS does, so the overflow writes safely
  into emulator-managed memory and the program continues.

HOW TO RUN
----------
    python tests/emulator/repro_hardware_crash.py [--verbose] [--bundle-dir <path>]
    python tests/emulator/repro_hardware_crash.py --two-expressions [--verbose]

WHAT IT TESTS
-------------
Default (single expression):
  1. Boots TI-OS ROM in CEmu (via autotester.exe or cemu-headless).
  2. Transfers the full CalcEngine bundle (all .8xp + .8xv files).
  3. Launches CALCENG via the CE_KEY_ASM jailbreak sequence.
  4. Captures the CalcEngine prompt CRC.
  5. Types "2" then ENTER.
  6. Waits 4 s — this includes iteration 2 re-entry (the crash site on hardware).
  7. Captures screen CRC.

--two-expressions mode:
  Same as above, then also types "3" ENTER and waits for a second result.
  This confirms the emulator survives iteration 3 as well (hardware would
  never reach this point).

Reports:
  REPRODUCED      — screen CRC is NOT a known CalcEngine CRC.
  NOT_REPRODUCED  — screen CRC IS a known CalcEngine CRC (emulator diverges).
  INCONCLUSIVE    — setup failed (prompt CRC not captured).

KNOWN CRCs
----------
  9F1FC020  CalcEngine prompt screen (historical p00_prompt capture)
  B9B5D5E0  Generic CalcEngine result screen (canonical 2+3=5, all s10 CAS tests)
  128C50A4  CalcEngine boot/home in comprehensive autotest (s00_boot_home)
  03AF06DB  2+3=5 result in comprehensive autotest (s01_add_2_3)
  384A70F2  CalcEngine prompt in native cemu-headless (PR #71 verified)

If the screen after "2 ENTER" shows any of the above CRCs, CalcEngine is still
running and the emulator does NOT reproduce the crash.  This is the expected
result: the stack overflow is hardware-specific.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Ensure UTF-8 output on Windows (cp1252 consoles reject em-dashes etc.)
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------------------
# Known-good CRCs for CalcEngine screens (emulator values)
# ---------------------------------------------------------------------------

CALCENGINE_KNOWN_CRCS: set[str] = {
    "9F1FC020",  # CalcEngine prompt (autotester p00_prompt baseline)
    "B9B5D5E0",  # CalcEngine result screen (canonical 2+3=5, all CAS)
    "128C50A4",  # CalcEngine boot/home (comprehensive autotest s00_boot_home)
    "03AF06DB",  # 2+3=5 result (comprehensive autotest s01_add_2_3)
    "384A70F2",  # CalcEngine prompt (native cemu-headless Pass 71)
    "6828E33C",  # 7-4=3 (certification b02_sub74)
    "F8A7255E",  # 3*4=12 (certification b03_mul34)
    "A2EE5E4B",  # 8/2=4  (certification b04_div82)
}

# ---------------------------------------------------------------------------
# Resource resolution
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]


def _env_path(name: str) -> Path | None:
    value = os.environ.get(name)
    return Path(value).expanduser() if value else None


_ROM_CANDIDATES = [
    REPO_ROOT / "rom" / "Rom.rom",
    _env_path("CALCENGINE_ROM_PATH"),
]

_AUTOTESTER_CANDIDATES = [
    _env_path("CEMU_AUTOTESTER_PATH"),
    REPO_ROOT / "tools" / "CEmu" / "autotester.exe",
]

_HEADLESS_CANDIDATES = [
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build" / "Release" / "cemu-headless.exe",
    REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "cemu-headless.exe",
    _env_path("CEMU_HEADLESS_PATH"),
]

_BUNDLE_CANDIDATES = [
    REPO_ROOT / "deploy" / "hardware-ready",
    _env_path("CALCENGINE_BUNDLE_DIR"),
]


def _find_first(candidates: list[Path | None]) -> Path | None:
    for p in candidates:
        try:
            if p is not None and p.is_file() and p.stat().st_size > 0:
                return p
        except (OSError, ValueError):
            pass
    return None


def _find_first_dir(candidates: list[Path | None]) -> Path | None:
    for p in candidates:
        try:
            if p is not None and p.is_dir():
                return p
        except (OSError, ValueError):
            pass
    return None


# ---------------------------------------------------------------------------
# Autotester-based repro
# ---------------------------------------------------------------------------

def _build_autotester_json(rom_path: Path, bundle_dir: Path) -> dict:
    """
    Build a minimal autotester JSON that:
      1. Boots TI-OS
      2. Launches CALCENG via CE_KEY_ASM jailbreak
      3. Captures prompt CRC (p00_prompt)
      4. Types "2" + ENTER
      5. Waits 3 seconds
      6. Captures result CRC (result_after_2)
    """
    # Collect all transfer files
    transfer_files = sorted([
        str(p).replace("\\", "/")
        for p in bundle_dir.iterdir()
        if p.suffix in (".8xp", ".8xv")
    ])
    if not transfer_files:
        raise FileNotFoundError(f"No .8xp/.8xv files found in {bundle_dir}")

    return {
        "rom": str(rom_path).replace("\\", "/"),
        "transfer_files": transfer_files,
        "target": {"name": "CALCENG", "isASM": True},
        "delay_after_key": 250,
        "delay_after_step": 600,
        "sequence": [
            "action|launch",
            "delay|5000",
            "key|enter",
            "delay|500",
            "hash|p00_prompt",
            # Minimal crash trigger: type "2" + ENTER
            "key|2",
            "delay|300",
            "key|enter",
            "delay|3000",
            "hash|result_after_2",
        ],
        "hashes": {
            "p00_prompt": {
                "description": "CalcEngine prompt should be visible here",
                "start": "vram_start",
                "size": "vram_16_size",
                "expected_CRCs": ["9F1FC020", "128C50A4", "384A70F2"],
                "timeout": 12000,
            },
            "result_after_2": {
                "description": (
                    "After typing '2' + ENTER. "
                    "If CalcEngine still runs: CRC will be a known CalcEngine result CRC. "
                    "If crashed to TI-OS home: CRC will be an unknown value."
                ),
                "start": "vram_start",
                "size": "vram_16_size",
                # Accept ANY known CalcEngine CRC — we don't know exactly which result
                # is shown for the literal expression '2', but it will be in CalcEngine
                # (B9B5D5E0 is the canonical result screen for successful evaluation).
                "expected_CRCs": sorted(CALCENGINE_KNOWN_CRCS),
                "timeout": 8000,
            },
        },
    }


def _parse_crc_from_autotester_output(stdout: str) -> dict[str, str]:
    """
    Parse autotester.exe stdout for CRC observations.
    Autotester outputs lines like:
        Hash #<name> expected [...] got <CRC>
    """
    crcs: dict[str, str] = {}
    pattern = re.compile(
        r"Hash\s*#?(?P<name>[A-Za-z0-9_]+).*?"
        r"(?:got|actual)\s*(?:=\s*)?(?:0x)?(?P<crc>[0-9A-Fa-f]{6,8})",
        re.IGNORECASE,
    )
    for line in stdout.splitlines():
        m = pattern.search(line)
        if m:
            name = m.group("name")
            crc = m.group("crc").upper().zfill(8)
            if name not in crcs:
                crcs[name] = crc
    return crcs


def run_autotester_repro(
    autotester: Path,
    rom: Path,
    bundle_dir: Path,
    verbose: bool = False,
) -> dict:
    """Run the repro via autotester.exe. Returns a result dict."""
    config = _build_autotester_json(rom, bundle_dir)

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", delete=False, encoding="utf-8"
    ) as f:
        json.dump(config, f, indent=2)
        tmp_path = f.name

    try:
        if verbose:
            print(f"  autotester JSON: {tmp_path}")
            print(f"  transfer_files: {len(config['transfer_files'])} files")

        start = time.monotonic()
        proc = subprocess.run(
            [str(autotester), tmp_path],
            capture_output=True,
            text=True,
            timeout=180,
            encoding="utf-8",
            errors="replace",
        )
        elapsed = time.monotonic() - start

        if verbose:
            print(f"  autotester exit code: {proc.returncode}  ({elapsed:.1f}s)")
            for line in proc.stdout.splitlines()[-30:]:
                print(f"  stdout: {line}")
            for line in proc.stderr.splitlines()[-10:]:
                print(f"  stderr: {line}")

        observed = _parse_crc_from_autotester_output(proc.stdout + proc.stderr)

        return {
            "tool": "autotester",
            "exit_code": proc.returncode,
            "elapsed_s": round(elapsed, 1),
            "observed_crcs": observed,
            "stdout_tail": proc.stdout.splitlines()[-20:],
            "stderr_tail": proc.stderr.splitlines()[-10:],
        }
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# cemu-headless-based repro
# ---------------------------------------------------------------------------

def run_headless_repro(
    headless: Path,
    rom: Path,
    bundle_dir: Path,
    verbose: bool = False,
    two_expressions: bool = False,
) -> dict:
    """
    Run the repro via cemu-headless native binary (JSON line protocol).
    This path does NOT go through autotester — it drives the native CEmu core
    directly, which is closer to real hardware behavior.

    If two_expressions=True, also types a second expression after the first
    result appears, to exercise iteration 3 of the re-entry path.
    Note: hardware never reaches this point (it crashes on iteration 2).
    """
    # Transfer directory
    bundle_str = str(bundle_dir).replace("\\", "/")
    rom_str = str(rom).replace("\\", "/")

    # Jailbreak + CalcEngine launch + expression sequence
    # Based on PR #71 verified sequence (commit a44277f):
    #   PRGM -> ENTER (pastes arTIfiCE as 'A'-prefix program)
    #   ENTER (runs arTIfiCE, shell opens)
    #   ENTER (launches CALCENG from shell)
    #
    # ITERATION 2 NOTE: After "2 ENTER", CEBASE evaluates "2", writes CE_RESP,
    # sets has_pending_response=1, then calls os_RunPrgm("CALCENG").  CALCENG
    # re-launches (iteration 2).  On hardware iteration 2 crashes in
    # ce_module_runtime_recover_child_failure (stack overflow, ~9080 bytes).
    # On the emulator this is NOT reproduced: CEmu uses a flat host memory model
    # and does not enforce the eZ80 stack boundary.  The get_screen after "2 ENTER"
    # therefore captures a CalcEngine result CRC (iteration 2 survived on emulator).
    commands = [
        {"cmd": "load_rom", "path": rom_str},
        {"cmd": "load_app", "path": bundle_str},
        # Boot TI-OS
        {"cmd": "step", "cycles_ms": 5000},
        # arTIfiCE jailbreak sequence
        {"cmd": "inject_key", "key": "prgm", "duration_ms": 100},
        {"cmd": "step", "cycles_ms": 1500},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 100},
        {"cmd": "step", "cycles_ms": 2000},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 100},
        {"cmd": "step", "cycles_ms": 2000},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 100},
        {"cmd": "step", "cycles_ms": 3000},
        # Capture prompt
        {"cmd": "get_screen", "format": "crc", "tag": "p00_prompt"},
        # Expression 1: "2" ENTER
        # After this: CEBASE runs, writes CE_RESP, calls os_RunPrgm("CALCENG").
        # CALCENG re-launches (iteration 2).  On hardware this crashes.
        # On emulator: iteration 2 re-entry runs, shows result, returns to prompt.
        {"cmd": "inject_key", "key": "2", "duration_ms": 100},
        {"cmd": "step", "cycles_ms": 300},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 100},
        {"cmd": "step", "cycles_ms": 5000},
        # Capture screen after expression 1 (includes iteration 2 re-entry on emulator)
        {"cmd": "get_screen", "format": "crc", "tag": "result_after_2"},
    ]

    if two_expressions:
        # Expression 2: "3" ENTER
        # Hardware never reaches here.  On emulator: iteration 3 re-entry runs.
        commands += [
            {"cmd": "inject_key", "key": "3", "duration_ms": 100},
            {"cmd": "step", "cycles_ms": 300},
            {"cmd": "inject_key", "key": "enter", "duration_ms": 100},
            {"cmd": "step", "cycles_ms": 5000},
            {"cmd": "get_screen", "format": "crc", "tag": "result_after_3"},
        ]

    commands.append({"cmd": "exit"})

    input_text = "\n".join(json.dumps(c) for c in commands) + "\n"

    if verbose:
        print(f"  cemu-headless commands: {len(commands)}")
        print(f"  ROM: {rom_str}")
        print(f"  bundle: {bundle_str}")

    start = time.monotonic()
    proc = subprocess.run(
        [str(headless)],
        input=input_text,
        capture_output=True,
        text=True,
        timeout=120,
        encoding="utf-8",
        errors="replace",
    )
    elapsed = time.monotonic() - start

    if verbose:
        print(f"  headless exit code: {proc.returncode}  ({elapsed:.1f}s)")
        for line in proc.stdout.splitlines():
            print(f"  stdout: {line}")

    # Parse CRCs from responses tagged "p00_prompt" and "result_after_2"
    observed: dict[str, str] = {}
    last_tag: str | None = None
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        # Tag-aware CRC extraction
        tag = obj.get("tag")
        crc = obj.get("crc", "").upper().zfill(8) if obj.get("crc") else None
        if tag and crc:
            observed[tag] = crc
        elif crc and last_tag:
            observed[last_tag] = crc
            last_tag = None

    return {
        "tool": "cemu-headless",
        "exit_code": proc.returncode,
        "elapsed_s": round(elapsed, 1),
        "observed_crcs": observed,
        "stdout_tail": proc.stdout.splitlines()[-20:],
        "stderr_tail": proc.stderr.splitlines()[-10:],
    }


# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------

def _verdict(observed_crcs: dict[str, str]) -> str:
    """
    Decide REPRODUCED / NOT_REPRODUCED / INCONCLUSIVE based on CRCs.
    """
    prompt_crc = observed_crcs.get("p00_prompt", "")
    result_crc = observed_crcs.get("result_after_2", "")

    # If we never got a prompt CRC, launch failed
    if not prompt_crc:
        return "INCONCLUSIVE — prompt CRC not captured (launch or jailbreak failed)"

    # Check if result is a known CalcEngine CRC
    if result_crc in CALCENGINE_KNOWN_CRCS:
        return (
            f"NOT_REPRODUCED — emulator shows CalcEngine screen after '2 ENTER' "
            f"(CRC {result_crc}). Crash is hardware-only."
        )

    if result_crc:
        return (
            f"REPRODUCED — screen CRC {result_crc} after '2 ENTER' is NOT a known "
            f"CalcEngine screen. Emulator may have crashed to TI-OS home."
        )

    return "INCONCLUSIVE — result CRC not captured (expression timed out?)"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Attempt to reproduce the 2026-04-14 hardware crash in emulator"
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Show detailed output")
    parser.add_argument(
        "--bundle-dir", default="", help="Path to CalcEngine .8xp bundle directory"
    )
    parser.add_argument(
        "--backend", choices=["auto", "autotester", "headless"], default="auto",
        help="Emulator backend to use"
    )
    parser.add_argument(
        "--two-expressions", action="store_true",
        help=(
            "Also evaluate a second expression after the first result. "
            "Hardware never reaches this point (crashes on iteration 2 re-entry). "
            "On the emulator this confirms CEmu survives iteration 3 as well."
        )
    )
    args = parser.parse_args()

    # --- Resolve resources ---
    rom = _find_first(_ROM_CANDIDATES)
    autotester = _find_first(_AUTOTESTER_CANDIDATES)
    headless = _find_first(_HEADLESS_CANDIDATES)

    bundle_override = Path(args.bundle_dir) if args.bundle_dir else None
    bundle = bundle_override or _find_first_dir(_BUNDLE_CANDIDATES)

    print("=" * 70)
    print("  CalcEngine hardware crash repro (iter-2 stack overflow)")
    print("=" * 70)
    print(f"  ROM       : {rom or '<NOT FOUND>'}")
    print(f"  bundle    : {bundle or '<NOT FOUND>'}")
    print(f"  autotester: {autotester or '<NOT FOUND>'}")
    print(f"  headless  : {headless or '<NOT FOUND>'}")
    print(f"  mode      : {'two-expressions' if args.two_expressions else 'single-expression'}")
    print()
    if bundle:
        bundle_is_debug = "CalcEngine-Debug" in str(bundle)
        print(f"  bundle type : {'DIAGNOSTIC (preferred)' if bundle_is_debug else 'PRODUCTION'}")
        print()

    if not rom:
        print("ERROR: Rom.rom not found.")
        print("       Set CALCENGINE_ROM_PATH or place it at rom/Rom.rom")
        return 2

    if not bundle:
        print("ERROR: Bundle directory not found.")
        print("       Set CALCENGINE_BUNDLE_DIR or pass --bundle-dir")
        return 2

    # --- Choose backend ---
    use_autotester = False
    use_headless = False

    if args.backend == "autotester":
        if not autotester:
            print("ERROR: autotester.exe not found (--backend=autotester specified)")
            return 2
        use_autotester = True
    elif args.backend == "headless":
        if not headless:
            print("ERROR: cemu-headless.exe not found (--backend=headless specified)")
            return 2
        use_headless = True
    else:  # auto
        if autotester:
            use_autotester = True
        elif headless:
            use_headless = True
        else:
            print("ERROR: Neither autotester.exe nor cemu-headless.exe found.")
            print("       Set CEMU_AUTOTESTER_PATH to your built autotester.exe.")
            print("       or set CEMU_AUTOTESTER_PATH / CEMU_HEADLESS_PATH")
            return 2

    # --- Run repro ---
    result: dict
    if use_autotester:
        print(f"Backend: autotester.exe ({autotester})")
        print("Running: boot -> jailbreak -> CALCENG -> '2' ENTER (iter-2 crash site) ...")
        print()
        assert autotester is not None
        result = run_autotester_repro(autotester, rom, bundle, verbose=args.verbose)
    else:
        print(f"Backend: cemu-headless ({headless})")
        print("Running: boot -> jailbreak -> CALCENG -> '2' ENTER (iter-2 crash site) ...")
        if args.two_expressions:
            print("         -> '3' ENTER (iter-3, beyond hardware crash point) ...")
        print()
        assert headless is not None
        result = run_headless_repro(headless, rom, bundle,
                                    verbose=args.verbose,
                                    two_expressions=args.two_expressions)

    # --- Report ---
    print("-" * 70)
    print(f"  Tool       : {result['tool']}")
    print(f"  Exit code  : {result['exit_code']}")
    print(f"  Elapsed    : {result['elapsed_s']}s")
    print()

    observed = result["observed_crcs"]
    prompt_crc = observed.get("p00_prompt", "<none>")
    result_crc = observed.get("result_after_2", "<none>")
    result3_crc = observed.get("result_after_3", "<none>")

    print(f"  p00_prompt   CRC : {prompt_crc}")
    print(f"  result_after_2   : {result_crc}  <- iter-2 re-entry (CRASH SITE on hw)")
    if args.two_expressions:
        print(f"  result_after_3   : {result3_crc}  <- iter-3 (hw never reaches this)")
    print()

    # Cross-reference result_crc against known CRC table
    if result_crc != "<none>":
        if result_crc in CALCENGINE_KNOWN_CRCS:
            print(f"  result_after_2 is a KNOWN CalcEngine CRC -> emulator survived iter-2")
        else:
            print(f"  result_after_2 is UNKNOWN -> may indicate crash to TI-OS home")
            print(f"  (compare against comprehensive autotest CRCs to identify screen)")

    print()
    verdict = _verdict(observed)
    if verdict.startswith("NOT_REPRODUCED"):
        marker = "[NOT REPRODUCED]"
    elif verdict.startswith("REPRODUCED"):
        marker = "[REPRODUCED]"
    else:
        marker = "[INCONCLUSIVE]"

    print(f"  VERDICT: {marker}")
    print(f"  {verdict}")
    print()

    # --- Emulator divergence note ---
    if marker == "[NOT REPRODUCED]":
        print("  EXPECTED RESULT: This crash is hardware-specific.")
        print()
        print("  ROOT CAUSE (confirmed 2026-04-14 via diagnostic stage markers):")
        print("  On iteration 2, ce_module_runtime_recover_child_failure allocates")
        print("  ce_response_t probe (4510 B) as a local and calls ce_response_read.")
        print("  Combined with ce_request_t pending_req (4273 B) in main(), the stack")
        print("  depth reaches ~9080 bytes, overflowing the TI-84 CE stack.")
        print("  See src/split/module_runtime.c:179 and src/core/launcher/main.c:562.")
        print()
        print("  WHY EMULATOR DIVERGES:")
        print("  CEmu uses a flat host memory model.  The eZ80 stack grows into memory")
        print("  that the emulator does not guard with OS-level watchdog semantics.  The")
        print("  overflow write completes without corrupting critical OS structures in the")
        print("  emulator, so the program continues.  On real hardware the OS detects")
        print("  the corrupted RAM and issues a 'validating OS' reset.")
        print()

    print("  --- STDOUT TAIL ---")
    for line in result.get("stdout_tail", [])[-10:]:
        print(f"  {line}")
    if result.get("stderr_tail"):
        print("  --- STDERR TAIL ---")
        for line in result.get("stderr_tail", [])[-5:]:
            print(f"  {line}")
    print("=" * 70)

    return 0 if marker != "[REPRODUCED]" else 1


if __name__ == "__main__":
    sys.exit(main())
