from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from cemu_runner import CEmuRunner
from accuracy_audit import (
    DEFAULT_ACCURACY_BASELINE_PATH,
    load_accuracy_cases,
    run_accuracy_audit,
    summarize_accuracy_audit,
)
from pipeline_support import (
    DEFAULT_BOOT_SEQUENCE,
    DEFAULT_DOWNLOADS_DIR,
    DEFAULT_MANIFEST_PATH,
    build_runner_config,
    get_real_cemu_status,
    resolve_real_cemu_environment,
    run_smoke_checklist,
    stage_transfer_package,
    summarize_checkpoints,
)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the CalcEngine CEmu end-to-end smoke pipeline against the real TI-84 CE ROM lane "
            "through the repo's cemu-headless wrapper."
        )
    )
    parser.add_argument(
        "--rom",
        type=Path,
        default=REPO_ROOT / "rom" / "Rom.rom",
        help="Path to the TI-84 CE ROM file.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST_PATH,
        help="Path to the CalcEngine transfer manifest JSON.",
    )
    parser.add_argument(
        "--transfer-dir",
        type=Path,
        default=None,
        help="Optional directory containing the 35 .8xp files and runtime .8xv dependencies.",
    )
    parser.add_argument(
        "--downloads-dir",
        type=Path,
        default=DEFAULT_DOWNLOADS_DIR,
        help="Fallback staged transfer directory to search before scanning repo build outputs.",
    )
    parser.add_argument(
        "--headless",
        type=Path,
        default=None,
        help="Optional explicit path to cemu-headless.exe.",
    )
    parser.add_argument(
        "--autotester",
        type=Path,
        default=None,
        help="Optional explicit path to autotester.exe.",
    )
    parser.add_argument(
        "--cemu-gui",
        type=Path,
        default=None,
        help="Optional explicit path to CEmu.exe for documentation/reporting purposes.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / "tests" / "cemu" / "outputs" / "cemu-pipeline-summary.json",
        help="Path to write the JSON pipeline summary.",
    )
    parser.add_argument(
        "--snapshot",
        type=Path,
        default=REPO_ROOT / "tests" / "cemu" / "outputs" / "cemu-pipeline-snapshot.json",
        help="Path to write the final emulator snapshot artifact.",
    )
    parser.add_argument(
        "--keep-staging",
        action="store_true",
        help="Preserve the temporary staged transfer directory after the run completes.",
    )
    return parser.parse_args()


def _ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def _write_json(path: Path, payload: dict[str, object]) -> None:
    _ensure_parent(path)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _build_manual_steps() -> list[str]:
    return [
        "Install the official Windows CEmu binary with `winget install --id CE-Programming.CEmu --exact --source winget` or download it from https://github.com/CE-Programming/CEmu/releases.",
        "Provide or build `autotester.exe` from the matching upstream CEmu tag. Releases currently ship `CEmu.exe`, but not a standalone AutoTester binary.",
        (
            'Build the repo wrapper with Visual Studio CMake support: '
            '"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe" '
            f'-S "{REPO_ROOT / "tools" / "emulator" / "cemu-headless"}" '
            f'-B "{REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build"}" '
            '-G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release'
        ),
        (
            'Then build it with: '
            '"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe" '
            f'--build "{REPO_ROOT / "tools" / "emulator" / "cemu-headless" / "build"}" --config Release'
        ),
        "Ensure the final staged transfer package contains all 35 .8xp modules, the 4 required .8xv runtime dependencies, and README.md.",
    ]


def main() -> int:
    args = _parse_args()
    started_at = time.time()
    summary: dict[str, object] = {
        "schema_version": 2,
        "runner": "scripts/cemu_pipeline.py",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "success": False,
        "blockers": [],
        "manual_steps": _build_manual_steps(),
        "paths": {
            "repo_root": str(REPO_ROOT),
            "manifest": str(args.manifest),
            "rom": str(args.rom),
            "transfer_dir": str(args.transfer_dir) if args.transfer_dir else "",
            "downloads_dir": str(args.downloads_dir),
            "headless": str(args.headless) if args.headless else "",
            "autotester": str(args.autotester) if args.autotester else "",
            "cemu_gui": str(args.cemu_gui) if args.cemu_gui else "",
            "output": str(args.output),
            "snapshot": str(args.snapshot),
        },
        "rom": {},
        "availability": {},
        "transfer_package": {},
        "checkpoints": [],
        "checkpoint_summary": {},
        "accuracy_audit": [],
        "accuracy_summary": {},
        "snapshot_written": "",
        "staging_dir": "",
    }

    status_ready, status_reason = get_real_cemu_status(repo_root=REPO_ROOT, manifest_path=args.manifest)
    summary["availability"]["status_ready"] = status_ready
    summary["availability"]["status_reason"] = status_reason

    environment = resolve_real_cemu_environment(
        repo_root=REPO_ROOT,
        manifest_path=args.manifest,
        transfer_dir=args.transfer_dir,
        downloads_dir=args.downloads_dir,
        headless_path=args.headless,
        autotester_path=args.autotester,
        gui_path=args.cemu_gui,
        rom_path=args.rom,
    )

    summary["availability"].update(
        {
            "rom_path": str(environment.rom_path) if environment.rom_path else "",
            "headless_path": str(environment.headless_path) if environment.headless_path else "",
            "autotester_path": str(environment.autotester_path) if environment.autotester_path else "",
            "gui_path": str(environment.gui_path) if environment.gui_path else "",
            "ready": environment.ready,
            "blockers": list(environment.blockers),
        }
    )
    if environment.rom_validation is not None:
        summary["rom"] = environment.rom_validation.to_summary()

    if environment.transfer_package is not None:
        package = environment.transfer_package
        summary["transfer_package"] = {
            "manifest_path": str(package.manifest_path),
            "module_count": package.module_count,
            "runtime_dependency_count": package.runtime_dependency_count,
            "transfer_count": package.transfer_count,
            "ready": package.ready,
            "missing_entries": list(package.missing_entries),
            "module_names": list(package.module_names),
            "runtime_names": list(package.runtime_names),
            "candidate_roots": [str(path) for path in package.candidate_roots],
        }

    if not environment.ready:
        blockers = list(environment.blockers)
        summary["blockers"] = blockers
        summary["success"] = False
        summary["duration_seconds"] = round(time.time() - started_at, 2)
        _write_json(args.output, summary)
        return 1

    assert environment.transfer_package is not None
    staging_root = Path(tempfile.mkdtemp(prefix="calcengine-cemu-pipeline-"))
    staged_transfer_dir = staging_root / "transfer"
    summary["staging_dir"] = str(staged_transfer_dir)

    try:
        stage_transfer_package(environment.transfer_package, staged_transfer_dir)
        config = build_runner_config(environment)
        checkpoints: list[dict[str, object]]
        audit_results: list[dict[str, object]] = []
        audit_summary: dict[str, object] = {}
        accuracy_cases = load_accuracy_cases(DEFAULT_ACCURACY_BASELINE_PATH)

        with CEmuRunner(config) as runner:
            runner.boot(program_paths=[staged_transfer_dir], ready_sequence=DEFAULT_BOOT_SEQUENCE)
            checkpoints = run_smoke_checklist(runner)
            audit_results = run_accuracy_audit(runner, accuracy_cases)
            audit_summary = summarize_accuracy_audit(accuracy_cases, audit_results)
            _ensure_parent(args.snapshot)
            runner.snapshot(args.snapshot)

        checkpoint_summary = summarize_checkpoints(checkpoints)
        summary["checkpoints"] = checkpoints
        summary["checkpoint_summary"] = checkpoint_summary
        summary["accuracy_audit"] = audit_results
        summary["accuracy_summary"] = {
            **audit_summary,
            "baseline_path": str(DEFAULT_ACCURACY_BASELINE_PATH),
        }
        summary["snapshot_written"] = str(args.snapshot)
        summary["success"] = bool(checkpoint_summary["success"]) and bool(audit_summary.get("success", False))
        if not summary["success"]:
            blockers = [
                f"Checkpoint failed: {name}"
                for name in checkpoint_summary["failed_names"]
            ]
            blockers.extend(
                f"Accuracy audit failed: {name}"
                for name in audit_summary.get("failed_names", [])
            )
            summary["blockers"] = blockers
    except Exception as exc:  # pragma: no cover - exercised by runtime failures
        summary["success"] = False
        summary["blockers"] = [str(exc)]
    finally:
        summary["duration_seconds"] = round(time.time() - started_at, 2)
        if not args.keep_staging:
            shutil.rmtree(staging_root, ignore_errors=True)
        _write_json(args.output, summary)

    return 0 if summary["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
