#!/usr/bin/env python3
"""
capture_phase_crcs.py

CRC baseline capture helper for Phases 2-4 emulator tests.

Runs a set of CalcEngine sessions against the cemu-headless native binary
and records the observed screen CRCs into a JSON baseline file.  The
baseline file is used by test_phases_2_4.py to update expected CRCs after
deliberate rendering changes.

Usage:
    python tests/cemu/capture_phase_crcs.py [--output PATH] [--rom PATH]
                                             [--binary PATH] [--modules-dir PATH]

    Defaults:
      --output     tests/cemu/phase_crcs_baseline.json
      --rom        rom/Rom.rom
      --binary     tools/emulator/cemu-headless/build/Release/cemu-headless.exe
      --modules-dir deploy/hardware-ready/

Exit codes:
    0  Baseline written (or updated) successfully.
    1  Required resource missing (binary, ROM, arTIfiCE, CALCENG).
    2  Argument error.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Capture CalcEngine phase CRC baselines via cemu-headless."
    )
    p.add_argument(
        "--output", type=Path,
        default=REPO_ROOT / "tests" / "cemu" / "phase_crcs_baseline.json",
        help="Path to write (or update) the JSON baseline file.",
    )
    p.add_argument(
        "--rom", type=Path,
        default=None,
        help="Path to Rom.rom (default: auto-search).",
    )
    p.add_argument(
        "--binary", type=Path,
        default=None,
        help="Path to cemu-headless.exe (default: auto-search).",
    )
    p.add_argument(
        "--modules-dir", type=Path,
        default=None,
        help="Directory containing .8xp modules (default: deploy/hardware-ready/).",
    )
    p.add_argument(
        "--artifice", type=Path,
        default=None,
        help="Path to arTIfiCE.8xp (default: auto-search).",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Resource resolution
# ---------------------------------------------------------------------------

def _find(candidates: list[Path]) -> Path | None:
    for p in candidates:
        try:
            if p.exists():
                return p
        except OSError:
            pass
    return None


def _resolve_resources(args: argparse.Namespace) -> dict[str, Path]:
    binary = args.binary or _find([
        REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build" / "Release" / "cemu-headless.exe",
        REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build_native" / "cemu-headless.exe",
    ])
    rom = args.rom or _find([
        REPO_ROOT / "rom" / "Rom.rom",
        Path("C:/Users/user/src/algebrix-ce/rom/Rom.rom"),
    ])
    artifice = args.artifice or _find([
        REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE.8xp",
        REPO_ROOT / "tools" / "emulator" / "assets" / "arTIfiCE" / "arTIfiCE.8xp",
        Path("C:/Users/user/Downloads/arTIfiCE.8xp"),
    ])
    modules_dir = args.modules_dir or _find([
        REPO_ROOT / "deploy" / "hardware-ready",
        REPO_ROOT / "bin",
    ])

    missing = []
    if not binary:   missing.append("cemu-headless binary")
    if not rom:      missing.append("Rom.rom")
    if not artifice: missing.append("arTIfiCE.8xp")
    if not modules_dir: missing.append("modules directory")

    if missing:
        print(f"ERROR: Missing required resources: {', '.join(missing)}", file=sys.stderr)
        sys.exit(1)

    return {
        "binary":      binary,
        "rom":         rom,
        "artifice":    artifice,
        "modules_dir": modules_dir,
    }


# ---------------------------------------------------------------------------
# Session runner
# ---------------------------------------------------------------------------

def _p(path: Path) -> str:
    return str(path).replace("\\", "/")


def _run_session(
    binary: Path,
    commands: list[dict[str, Any]],
    timeout: float = 240.0,
) -> list[dict[str, Any]]:
    if not any(c.get("cmd") == "exit" for c in commands):
        commands = list(commands) + [{"cmd": "exit"}]
    input_text = "\n".join(json.dumps(c) for c in commands) + "\n"
    result = subprocess.run(
        [str(binary)],
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


def _crcs(responses: list[dict[str, Any]]) -> list[str]:
    return [
        r.get("crc", "")
        for r in responses
        if r.get("cmd") == "get_screen"
        and r.get("format") == "crc"
        and r.get("ok")
    ]


# ---------------------------------------------------------------------------
# Shared boot commands
# ---------------------------------------------------------------------------

def _build_boot_cmds(
    rom: Path,
    artifice: Path,
    modules_dir: Path,
) -> list[dict[str, Any]]:
    cmds: list[dict[str, Any]] = [
        {"cmd": "load_rom", "path": _p(rom)},
        {"cmd": "load_app", "path": _p(artifice)},
    ]
    for p in sorted(modules_dir.glob("*.8xp")):
        cmds.append({"cmd": "load_app", "path": _p(p)})
    cmds += [
        {"cmd": "step", "cycles_ms": 4000},
        {"cmd": "inject_key", "key": "clear", "duration_ms": 250},
        {"cmd": "step", "cycles_ms": 500},
    ]
    return cmds


def _launch_cmds() -> list[dict[str, Any]]:
    return [
        {"cmd": "inject_key", "key": "prgm", "duration_ms": 250},
        {"cmd": "step", "cycles_ms": 1200},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 250},
        {"cmd": "step", "cycles_ms": 2000},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 250},
        {"cmd": "step", "cycles_ms": 1000},
        {"cmd": "inject_key", "key": "enter", "duration_ms": 250},
        {"cmd": "step", "cycles_ms": 3000},
    ]


def _key(name: str, ms: int = 250) -> dict[str, Any]:
    return {"cmd": "inject_key", "key": name, "duration_ms": ms}


def _step(ms: int) -> dict[str, Any]:
    return {"cmd": "step", "cycles_ms": ms}


def _crc_cmd() -> dict[str, Any]:
    return {"cmd": "get_screen", "format": "crc"}


# ---------------------------------------------------------------------------
# Capture sessions
# ---------------------------------------------------------------------------

def _capture_prompt_crc(binary: Path, boot_cmds: list[dict[str, Any]]) -> str | None:
    """Capture the CalcEngine prompt screen CRC."""
    cmds = boot_cmds + _launch_cmds() + [_crc_cmd()]
    responses = _run_session(binary, cmds, timeout=180)
    cs = _crcs(responses)
    return cs[0] if cs else None


def _capture_fraction_render_crc(binary: Path, boot_cmds: list[dict[str, Any]]) -> dict[str, str]:
    """Capture CRC of graphx fraction render screen after '6/2'."""
    cmds = (
        boot_cmds
        + _launch_cmds()
        + [
            _key("6"), _step(80), _key("div"), _step(80), _key("2"), _step(80),
            _key("enter", 400), _step(5000),
            _crc_cmd(),          # graphx render screen
            _key("enter", 300), _step(800),
            _crc_cmd(),          # back at prompt
        ]
    )
    responses = _run_session(binary, cmds, timeout=300)
    cs = _crcs(responses)
    return {
        "render":  cs[-2] if len(cs) >= 2 else "",
        "restored": cs[-1] if cs else "",
    }


def _capture_power_render_crc(binary: Path, boot_cmds: list[dict[str, Any]]) -> dict[str, str]:
    """Capture CRC of graphx power render screen after '2^8'."""
    cmds = (
        boot_cmds
        + _launch_cmds()
        + [
            _key("2"), _step(80), _key("^"), _step(80), _key("8"), _step(80),
            _key("enter", 400), _step(5000),
            _crc_cmd(),
            _key("enter", 300), _step(800),
            _crc_cmd(),
        ]
    )
    responses = _run_session(binary, cmds, timeout=300)
    cs = _crcs(responses)
    return {
        "render":   cs[-2] if len(cs) >= 2 else "",
        "restored": cs[-1] if cs else "",
    }


def _capture_sqrt_render_crc(binary: Path, boot_cmds: list[dict[str, Any]]) -> dict[str, str]:
    """Capture CRC of graphx sqrt render screen after 'sqrt(16)'."""
    # 2nd+x^2 = sqrt( shortcut on CE
    cmds = (
        boot_cmds
        + _launch_cmds()
        + [
            _key("2nd"), _step(80), _key("^2"), _step(80),
            _key("1"), _step(80), _key("6"), _step(80), _key(")"), _step(80),
            _key("enter", 400), _step(5000),
            _crc_cmd(),
            _key("enter", 300), _step(800),
            _crc_cmd(),
        ]
    )
    responses = _run_session(binary, cmds, timeout=300)
    cs = _crcs(responses)
    return {
        "render":   cs[-2] if len(cs) >= 2 else "",
        "restored": cs[-1] if cs else "",
    }


def _capture_mode_menu_crc(binary: Path, boot_cmds: list[dict[str, Any]]) -> str | None:
    """Capture the mode-menu screen CRC."""
    cmds = (
        boot_cmds
        + _launch_cmds()
        + [_key("mode"), _step(800), _crc_cmd()]
    )
    responses = _run_session(binary, cmds, timeout=180)
    cs = _crcs(responses)
    return cs[0] if cs else None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    args = _parse_args()
    res = _resolve_resources(args)

    binary      = res["binary"]
    rom         = res["rom"]
    artifice    = res["artifice"]
    modules_dir = res["modules_dir"]

    print(f"Binary:      {binary}")
    print(f"ROM:         {rom}")
    print(f"arTIfiCE:    {artifice}")
    print(f"Modules dir: {modules_dir}")
    print()

    boot_cmds = _build_boot_cmds(rom, artifice, modules_dir)

    baseline: dict[str, Any] = {}

    # Existing baseline (update rather than overwrite)
    if args.output.exists():
        try:
            baseline = json.loads(args.output.read_text(encoding="utf-8"))
        except Exception:
            baseline = {}

    print("Capturing CalcEngine prompt CRC …")
    prompt_crc = _capture_prompt_crc(binary, boot_cmds)
    if prompt_crc:
        print(f"  prompt:       {prompt_crc}")
        baseline["prompt_crc"] = prompt_crc
    else:
        print("  WARNING: no CRC received for prompt screen")

    print("Capturing fraction render CRC (6/2) …")
    frac = _capture_fraction_render_crc(binary, boot_cmds)
    print(f"  render:       {frac['render']}")
    print(f"  restored:     {frac['restored']}")
    baseline["fraction_render_crc"]   = frac["render"]
    baseline["fraction_restored_crc"] = frac["restored"]

    print("Capturing power render CRC (2^8) …")
    pw = _capture_power_render_crc(binary, boot_cmds)
    print(f"  render:       {pw['render']}")
    print(f"  restored:     {pw['restored']}")
    baseline["power_render_crc"]   = pw["render"]
    baseline["power_restored_crc"] = pw["restored"]

    print("Capturing sqrt render CRC (sqrt(16)) …")
    sq = _capture_sqrt_render_crc(binary, boot_cmds)
    print(f"  render:       {sq['render']}")
    print(f"  restored:     {sq['restored']}")
    baseline["sqrt_render_crc"]   = sq["render"]
    baseline["sqrt_restored_crc"] = sq["restored"]

    print("Capturing mode-menu CRC …")
    menu_crc = _capture_mode_menu_crc(binary, boot_cmds)
    if menu_crc:
        print(f"  mode_menu:    {menu_crc}")
        baseline["mode_menu_crc"] = menu_crc
    else:
        print("  WARNING: no CRC received for mode-menu screen")

    # Write baseline
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(baseline, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"\nBaseline written to: {args.output}")


if __name__ == "__main__":
    main()
