#!/usr/bin/env python3
"""Refresh (or --check) deploy/hardware-ready/ from a fresh `make all`.

What it syncs, all derived from the actual files:
  * the 35 manifest modules: bin/<dir>/<NAME>.8xp -> deploy/hardware-ready/<NAME>.8xp
  * tests/emulator/manifest.json: per-module size_bytes / crc32 (CRC32-IEEE), and the
    README.md entry crc32
  * tests/emulator/*.crc-catalog.json + .md: the module_inventory rows
  * deploy/hardware-ready/SHA256SUMS.txt: every package file except itself, sorted

It never touches screen-CRC baselines, arTIfiCE.8xp, the runtime .8xv AppVars, or
anything outside the repo (no Downloads bundle). VERSION.txt / README prose are
edited by hand; run this afterwards so the README crc and SHA256SUMS follow.

  python3 scripts/cemu/refresh_hardware_ready.py --check   # exit 1 on any drift
  python3 scripts/cemu/refresh_hardware_ready.py           # write
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / "deploy" / "hardware-ready"
MANIFEST = ROOT / "tests" / "emulator" / "manifest.json"
CATALOG_GLOB = "tests/emulator/*.crc-catalog.json"
HEADER_COMMENT_END = 53  # 8-byte signature + 3 bytes + 42-byte comment


def crc32_ieee(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"


# Package files that are not manifest modules or runtime AppVars.
PACKAGE_EXTRAS = {"arTIfiCE.8xp", "README.md", "README.txt", "VERSION.txt"}


def check_version(version: str) -> None:
    """The manifest version (release.yml compares it with the tag) must match
    VERSION.txt, the Makefile and split/program.mk defaults and src/version.h."""
    seen = {"tests/emulator/manifest.json": version}
    m = re.search(r"^Version:\s+(\S+)", (PKG / "VERSION.txt").read_text(encoding="utf-8"), re.M)
    seen["deploy/hardware-ready/VERSION.txt"] = m.group(1) if m else None
    mk = (ROOT / "Makefile").read_text(encoding="utf-8")
    parts = [re.search(rf"^V_{k} \?= (\d+)", mk, re.M) for k in ("MAJOR", "MINOR", "PATCH")]
    seen["Makefile V_*"] = ".".join(x.group(1) for x in parts) if all(parts) else None
    mk = (ROOT / "split/program.mk").read_text(encoding="utf-8")
    parts = [re.search(rf"^V_{k} \?= (\d+)", mk, re.M) for k in ("MAJOR", "MINOR", "PATCH")]
    seen["split/program.mk V_*"] = ".".join(x.group(1) for x in parts) if all(parts) else None
    hdr = (ROOT / "src/version.h").read_text(encoding="utf-8")
    parts = [re.search(rf"#define CE_VERSION_{k}\s+(\d+)", hdr) for k in ("MAJOR", "MINOR", "PATCH")]
    seen["src/version.h"] = ".".join(x.group(1) for x in parts) if all(parts) else None
    if len(set(seen.values())) != 1:
        sys.exit(f"version mismatch: {seen}")


def built_modules(names: list[str]) -> dict[str, Path]:
    found: dict[str, list[Path]] = {}
    for path in sorted((ROOT / "bin").glob("*/*.8xp")):
        found.setdefault(path.name, []).append(path)
    dupes = {n: p for n, p in found.items() if len(p) > 1}
    if dupes:
        sys.exit(f"duplicate built programs (run `make clean && make all`): {dupes}")
    if set(found) != set(names):
        sys.exit(f"bin/*/*.8xp differs from the manifest: extra={sorted(set(found) - set(names))} "
                 f"missing={sorted(set(names) - set(found))}")
    return {n: found[n][0] for n in names}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="report drift and exit 1 instead of writing")
    args = ap.parse_args()
    drift: list[str] = []

    manifest_text = MANIFEST.read_text(encoding="utf-8")
    manifest = json.loads(manifest_text)
    names = [row["name"] for row in manifest["modules"]]
    if manifest["module_count"] != len(names):
        sys.exit("manifest module_count does not match its module list")
    if len(set(names)) != len(names):
        sys.exit("manifest module list has duplicate names")
    check_version(manifest["version"])
    sources = built_modules(names)
    # Validate the package contents before any write. Modules may be missing
    # (this run restores them); anything else unexpected or missing is fatal.
    expected = set(names) | {row["name"] for row in manifest["runtime_dependencies"]} | PACKAGE_EXTRAS
    present = {p.name for p in PKG.iterdir() if p.is_file() and p.name != "SHA256SUMS.txt"}
    if present - expected or (expected - set(names)) - present:
        sys.exit(f"deploy/hardware-ready holds unexpected files {sorted(present - expected)} "
                 f"or lacks {sorted((expected - set(names)) - present)}; nothing was written")

    # 1. program bytes. The .8xp header comment (bytes 11-52) carries the build
    # timestamp, so a rebuild of unchanged sources is not drift: compare from byte 53.
    blob: dict[str, bytes] = {}
    for name, src in sources.items():
        dst = PKG / name
        built = src.read_bytes()
        staged = dst.read_bytes() if dst.exists() else b""
        if (staged[:11] == built[:11] and
                staged[HEADER_COMMENT_END:] == built[HEADER_COMMENT_END:] and staged):
            blob[name] = staged
            continue
        drift.append(f"package {name} payload differs from {src.relative_to(ROOT)}")
        blob[name] = built
        if not args.check:
            shutil.copyfile(src, dst)

    # 2. manifest
    for row in manifest["modules"]:
        size, crc = len(blob[row["name"]]), crc32_ieee(blob[row["name"]])
        if (row["size_bytes"], row["crc32"]) != (size, crc):
            drift.append(f"manifest {row['name']}: {row['size_bytes']}/{row['crc32']} -> {size}/{crc}")
            row["size_bytes"], row["crc32"] = size, crc
    readme_crc = crc32_ieee((PKG / manifest["readme"]["name"]).read_bytes())
    if manifest["readme"]["crc32"] != readme_crc:
        drift.append(f"manifest readme crc32 {manifest['readme']['crc32']} -> {readme_crc}")
        manifest["readme"]["crc32"] = readme_crc
    new_manifest = json.dumps(manifest, indent=2) + "\n"
    if not args.check and new_manifest != manifest_text:
        MANIFEST.write_text(new_manifest, encoding="utf-8")

    # 3. CRC catalog module inventories (json + md)
    for cat_path in sorted(ROOT.glob(CATALOG_GLOB)):
        text = cat_path.read_text(encoding="utf-8")
        cat = json.loads(text)
        inv = cat.get("module_inventory")
        if not isinstance(inv, dict):
            continue
        inv_names = [row["name"] for row in inv["modules"]]
        if sorted(inv_names) != sorted(names):
            sys.exit(f"{cat_path.name}: module_inventory rows differ from the manifest "
                     f"(missing={sorted(set(names) - set(inv_names))}, "
                     f"extra={sorted(set(inv_names) - set(names))}, rows={len(inv_names)})")
        for row in inv["modules"]:
            size, crc = len(blob[row["name"]]), crc32_ieee(blob[row["name"]])
            want = {"manifest_size_bytes": size, "manifest_crc32": crc,
                    "resolved_size_bytes": size, "resolved_crc32": crc}
            if any(row.get(k) != v for k, v in want.items()):
                drift.append(f"{cat_path.name} {row['name']}")
                row.update(want)
        new = json.dumps(cat, indent=2) + "\n"
        if not args.check and new != text:
            cat_path.write_text(new, encoding="utf-8")
        md = cat_path.with_name(cat_path.name[: -len(".json")] + ".md")
        if md.exists():
            md_text = md.read_text(encoding="utf-8")

            heading = "## Official Module Inventory"
            if heading not in md_text:
                sys.exit(f"{md}: missing module inventory heading")
            table = [heading, "", "Manifest: `tests/emulator/manifest.json`",
                     f"Verified Official Modules: {len(names)}/{len(names)}", "",
                     "| # | Module | Size (Bytes) | Manifest CRC32 | Resolved CRC32 | Status |",
                     "| --- | --- | --- | --- | --- | --- |"]
            for order, name in enumerate(names, 1):
                crc = crc32_ieee(blob[name])
                table.append(f"| {order} | `{name}` | {len(blob[name])} | `{crc}` | `{crc}` | `matched` |")
            new_md = md_text.split(heading, 1)[0] + "\n".join(table) + "\n"
            if new_md != md_text:
                drift.append(f"{md.name} module table")
                if not args.check:
                    md.write_text(new_md, encoding="utf-8")

    # 4. SHA256SUMS.txt (programs as they will be after the copy)
    sums_path = PKG / "SHA256SUMS.txt"
    files = sorted(p.name for p in PKG.iterdir() if p.is_file() and p.name != "SHA256SUMS.txt")
    lines = []
    for n in files:
        payload = blob[n] if n in blob else (PKG / n).read_bytes()
        lines.append(f"{hashlib.sha256(payload).hexdigest()}  {n}")
    new_sums = "\n".join(lines) + "\n"
    if sums_path.read_text(encoding="utf-8") != new_sums:
        drift.append("SHA256SUMS.txt")
        if not args.check:
            sums_path.write_text(new_sums, encoding="utf-8")

    for d in drift:
        print(("DRIFT " if args.check else "updated ") + d)
    calceng = blob["CALCENG.8xp"]
    print(json.dumps({"check": args.check, "changes": len(drift), "modules": len(names),
                      "package_files": len(files) + 1,
                      "CALCENG.8xp": {"bytes": len(calceng), "crc32": crc32_ieee(calceng),
                                      "sha256": hashlib.sha256(calceng).hexdigest()}}))
    return 1 if (args.check and drift) else 0


if __name__ == "__main__":
    sys.exit(main())
