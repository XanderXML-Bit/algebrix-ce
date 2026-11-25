#!/usr/bin/env python3
"""
capture_lcd_text_baseline.py - Populate lcd_text_emulator_baseline.json.

Companion to test_lcd_text_emulator.py. Runs the same boot + 59-expression
pipeline inside the pinned tools/emulator/cemu-headless/ fork, captures one
CRC per expression, and merges those CRCs into each expression's
`expected_crcs` list in tests/emulator/lcd_text_emulator_baseline.json.
Existing CRCs are preserved (dedup on insert) so multiple capture runs can
accumulate renders for timing flap, matching how the autotest.comprehensive
suite already builds its baselines.

Usage (Windows, where the headless fork runs):
    python tests/emulator/capture_lcd_text_baseline.py

Optional flags:
    --only <id>[,<id>...]     Capture only those expression ids.
    --dry-run                 Run the session but do not write the baseline.
    --print-paths             Print resolved paths and exit.

Environment variables honored:
    CALCENGINE_ROM_PATH              Absolute path to Rom.rom.
    CALCENGINE_BUNDLE_DIR            Absolute path to the .8xp bundle dir.
    CEMU_HEADLESS_AUTOTESTER_PATH    Absolute path to autotester.exe.
    CEMU_AUTOTESTER_PATH             Alternate name for the same thing.

Hard rules (user-enforced 2026-04-13) - identical to the verify suite:
    * Binary resolution is pinned to tools/emulator/cemu-headless/.
    * No PATH lookup, no GUI CEmu fallback.
    * Hard-fail (exit 2) if the pinned binary is missing.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import sys
from pathlib import Path
from typing import Any

# Local imports - this script lives alongside the shared module.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from _lcd_text_emulator_shared import (  # type: ignore[import-not-found]
    BASELINE_PATH,
    HeadlessSession,
    HeadlessSessionError,
    explain_resolved_paths,
    load_baseline,
    resolve_autotester,
    resolve_bundle,
    resolve_headless_binary,
    resolve_rom,
    save_baseline_atomic,
    sha256_file,
)


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture LCD text emulator CRC baselines for CalcEngine."
    )
    parser.add_argument(
        "--only",
        default="",
        help="Comma-separated list of expression ids to capture (default: all).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Run the session, print the captured CRCs, but do not write the baseline.",
    )
    parser.add_argument(
        "--print-paths",
        action="store_true",
        help="Print resolved paths and exit without running the emulator.",
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help="Replace each affected expression's expected_crcs with ONLY the freshly captured CRC "
             "(default merges). Use with care - existing baselines are overwritten.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = _parse_args(argv)

    print("\nLCD text emulator baseline capture")
    print("==================================")
    print(explain_resolved_paths())
    if args.print_paths:
        return 0

    # Hard-fail if the pinned binary is absent. The user's rule is
    # non-negotiable: this script must never launch GUI CEmu or a PATH
    # binary.
    try:
        headless_binary = resolve_headless_binary()
    except FileNotFoundError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 2

    rom = resolve_rom()
    if rom is None:
        print(
            "\nERROR: Rom.rom not found. Set CALCENGINE_ROM_PATH to the "
            "absolute path of your dumped TI-84 Plus CE ROM.",
            file=sys.stderr,
        )
        return 2

    bundle = resolve_bundle()
    if bundle is None:
        print(
            "\nERROR: CalcEngine .8xp bundle directory not found. Build the "
            "TI target and stage outputs in deploy/hardware-ready, "
            "or set CALCENGINE_BUNDLE_DIR.",
            file=sys.stderr,
        )
        return 2

    autotester = resolve_autotester()

    # Load and validate the baseline JSON (we merge into it; do not clobber).
    payload: dict[str, Any] = load_baseline()
    entries: list[dict[str, Any]] = list(payload.get("expressions", []))
    if len(entries) != 59:
        print(
            f"\nERROR: baseline file {BASELINE_PATH} has {len(entries)} "
            "entries; expected 59 (the full PR #20 set).",
            file=sys.stderr,
        )
        return 2

    only_ids: set[str] = set(filter(None, (s.strip() for s in args.only.split(","))))
    if only_ids:
        unknown = only_ids.difference(e["id"] for e in entries)
        if unknown:
            print(
                f"\nERROR: --only specifies unknown expression ids: {sorted(unknown)}",
                file=sys.stderr,
            )
            return 2
        target_entries = [e for e in entries if e["id"] in only_ids]
    else:
        target_entries = entries

    print(
        f"\nCapturing {len(target_entries)} expression(s) from the "
        "hardware-equivalent path..."
    )

    session = HeadlessSession(
        headless_binary=headless_binary,
        rom=rom,
        bundle=bundle,
        autotester=autotester,
    )
    session.queue_boot_and_launch()
    for entry in target_entries:
        session.queue_expression(entry["expr"], entry["id"])

    try:
        tag_to_crc = session.run()
    except HeadlessSessionError as exc:
        print(f"\nERROR: cemu-headless session failed: {exc}", file=sys.stderr)
        return 3

    # Drop the boot marker; it is not per-expression.
    boot_crc = tag_to_crc.pop("__boot_calcengine_prompt__", None)

    print("\nCaptured CRCs:")
    print("--------------")
    missing: list[str] = []
    for entry in target_entries:
        expression_id = entry["id"]
        crc = tag_to_crc.get(expression_id)
        if crc is None:
            missing.append(expression_id)
            print(f"  [MISSING] {expression_id:<32} ({entry['label']})")
        else:
            print(f"  {crc}  {expression_id:<32} ({entry['label']})")

    if missing:
        print(
            f"\nERROR: {len(missing)} expression(s) did not yield a CRC. "
            "This usually means the emulator stalled or get_screen was "
            "rejected. Fix the root cause before proceeding; do not merge a "
            "partial baseline.",
            file=sys.stderr,
        )
        return 4

    if args.dry_run:
        print("\n--dry-run: not writing baseline. Boot-prompt CRC:", boot_crc)
        return 0

    # Merge captured CRCs into the baseline.
    updated = 0
    for entry in target_entries:
        captured = tag_to_crc[entry["id"]].upper()
        existing: list[str] = [c.upper() for c in entry.get("expected_crcs", []) if isinstance(c, str)]
        if args.reset:
            new_list = [captured]
        else:
            if captured in existing:
                continue
            new_list = existing + [captured]
        entry["expected_crcs"] = new_list
        updated += 1

    payload["expressions"] = entries
    payload["generated_utc"] = _dt.datetime.now(tz=_dt.timezone.utc).isoformat(timespec="seconds")
    payload["headless_binary_sha256"] = sha256_file(headless_binary)
    payload["rom_sha256"] = sha256_file(rom)
    # bundle is a directory; we hash a manifest of (name, size) pairs so
    # that swapping individual .8xp modules shows as drift.
    try:
        manifest_items = sorted(
            (p.name, p.stat().st_size)
            for p in Path(bundle).iterdir()
            if p.is_file()
        )
        import hashlib
        digest = hashlib.sha256(
            json.dumps(manifest_items).encode("utf-8")
        ).hexdigest()
        payload["bundle_sha256"] = digest
    except OSError as exc:
        print(f"Warning: could not hash bundle directory: {exc}")
        payload["bundle_sha256"] = None

    save_baseline_atomic(payload)

    print(
        f"\nBaseline updated. {updated} expression(s) received a new CRC; "
        f"the rest were already present. Wrote {BASELINE_PATH}."
    )
    print(
        "\nNext step: run `python -m pytest tests/emulator/test_lcd_text_emulator.py -v` "
        "to verify the baseline round-trips cleanly."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
