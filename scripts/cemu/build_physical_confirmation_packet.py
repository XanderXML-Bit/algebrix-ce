#!/usr/bin/env python3
"""Generate an honest package/evidence handoff, never hardware verification.

Unlike the historical evidence packs this packet consumes only current staged
bytes and the inspected module-battery catalog. Known failures are blockers,
not successful checks. Does not touch a device or create a Downloads bundle.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parents[2]
# arTIfiCE.8xp is bundled outside the manifest. Pin its identity: this is the
# committed v2.1 file (unchanged since 003c132), which the v2.0.0 release notes
# record as identical to upstream arTIfiCE_v2.1.8xp.
ARTIFICE_SHA256 = "46e2cd27a93bad402de8811d01d77f52b0419fb09a9162957703ffe0ef756caa"


def build_packet() -> dict:
    pkg = ROOT / "deploy/hardware-ready"
    manifest = json.loads((ROOT / "tests/emulator/manifest.json").read_text())
    battery = json.loads((ROOT / "tests/cemu/edge-cases/module-battery.json").read_text())
    files = []
    blockers = []
    sums: dict[str, str] = {}
    for line in (pkg / "SHA256SUMS.txt").read_text().splitlines():
        if not line:
            continue
        digest, name = line.split("  ", 1)
        if name in sums:
            blockers.append(f"sha256sums_duplicate:{name}")
        sums[name] = digest
    rows = manifest["modules"] + manifest["runtime_dependencies"] + [{"name": "arTIfiCE.8xp"}]
    for row in rows:
        p = pkg / row["name"]
        b = p.read_bytes()
        crc = f"{zlib.crc32(b) & 0xFFFFFFFF:08X}"
        sha = hashlib.sha256(b).hexdigest()
        if "size_bytes" in row:
            matched = len(b) == row["size_bytes"] and crc == row["crc32"]
        else:  # arTIfiCE: pinned digest, and SHA256SUMS must agree
            matched = sha == ARTIFICE_SHA256 and sums.get(p.name) == sha
        files.append({"name": p.name, "bytes": len(b), "crc32": crc,
                      "sha256": sha, "manifest_matches": matched,
                      "identity_check": "manifest size+crc32" if "size_bytes" in row
                      else "pinned sha256 + SHA256SUMS"})
        if not matched:
            blockers.append(f"package_manifest_mismatch:{p.name}")
    known = [{"name": c["name"], **c["known_failure"]} for c in battery["cases"] if c.get("known_failure")]
    blockers += [f"known_failure:{c['name']}:{c['card']}" for c in known]
    blockers += ["other_crc_catalogs_not_rederived", "physical_calculator_confirmation_pending"]
    return {"schema_version": 1, "version": manifest["version"],
            "status": "source_transfer_prep_only", "ready_for_physical_confirmation": False,
            "hardware_verified": False, "physical_device_action": False,
            "source": (pkg / "VERSION.txt").read_text().split("Source:    ", 1)[1].splitlines()[0],
            "files": files, "battery_capture": battery["capture"],
            "baselined_cases": [c["name"] for c in battery["cases"] if c["expected_crcs"]],
            "known_failures": known, "blockers": blockers,
            "limits": ["Emulator evidence only, not hardware verification.",
                       "No ROM is distributed.", "No physical transfer or Downloads bundle was created.",
                       "Historical wrapper-lane CRC passes are not CalcEngine evidence."]}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "tests/emulator/outputs/physical-confirmation-packet.json")
    args = ap.parse_args()
    p = build_packet()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(p, indent=2) + "\n")
    text = ["# Algebrix physical-confirmation packet", "",
            f"Version: {p['version']}", f"Source: {p['source']}", "",
            "**source_transfer_prep_only — NOT hardware-verified.**", "",
            f"{len(p['baselined_cases'])} inspected emulator screens baselined; "
            f"{len(p['known_failures'])} carded known failures. Other CRC catalogs need re-derivation.", "",
            "## Blockers", *[f"- {b}" for b in p['blockers']], "", "## Limits",
            *[f"- {b}" for b in p['limits']], "", "## Package identity", "",
            "| File | Bytes | CRC32 | SHA256 |", "|---|---:|---|---|",
            *[f"| {f['name']} | {f['bytes']} | {f['crc32']} | {f['sha256']} |" for f in p['files']]]
    args.out.with_suffix(".md").write_text("\n".join(text) + "\n")
    print(f"{args.out}: {p['status']}, {len(p['blockers'])} blockers; hardware_verified=false")


if __name__ == "__main__":
    main()
