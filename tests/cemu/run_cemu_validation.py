from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Mapping


REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_CEMU_PATH = REPO_ROOT / "tests" / "cemu"
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(TESTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(TESTS_CEMU_PATH))
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from cemu_hardening import (
    DEFAULT_ARITHMETIC_BATTERY_PATH,
    DEFAULT_EDGE_CASE_BASELINE_PATH,
    DEFAULT_FAKE_CASE_BASELINE_PATH,
    DEFAULT_MODULE_BATTERY_PATH,
    DEFAULT_PIPELINE_CONFIG_PATH,
    DEFAULT_PIPELINE_SNAPSHOT_PATH,
    DEFAULT_PIPELINE_SUMMARY_PATH,
    load_pipeline_config,
    resolve_configured_path,
    resolve_pipeline_environment,
    run_fake_pipeline_sections,
    run_real_pipeline_sections,
)
from pipeline_support import stage_transfer_package
from visible_smoke import DEFAULT_VISIBLE_SMOKE_CASE_PATH


EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_ERROR = 2


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the hardened CalcEngine CEmu validation pipeline.")
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_PIPELINE_CONFIG_PATH,
        help="Path to the pipeline validation configuration JSON.",
    )
    parser.add_argument(
        "--mode",
        choices=("full", "real-only", "fake-only"),
        default="full",
        help="Select which validation lanes to run.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Path to write the machine-readable JSON summary.",
    )
    parser.add_argument(
        "--snapshot",
        type=Path,
        default=None,
        help="Path to write the final real-backend state snapshot.",
    )
    parser.add_argument(
        "--edge-cases",
        type=Path,
        default=None,
        help="Path to the real edge-case baseline JSON.",
    )
    parser.add_argument(
        "--arithmetic-cases",
        type=Path,
        default=None,
        help="Path to the real arithmetic battery JSON.",
    )
    parser.add_argument(
        "--visible-smoke-cases",
        type=Path,
        default=None,
        help="Path to the real visible-smoke transition JSON.",
    )
    parser.add_argument(
        "--module-cases",
        type=Path,
        default=None,
        help="Path to the real module battery JSON.",
    )
    parser.add_argument(
        "--fake-cases",
        type=Path,
        default=None,
        help="Path to the fake-backend hardening baseline JSON.",
    )
    parser.add_argument(
        "--keep-staging",
        action="store_true",
        help="Preserve the temporary real transfer staging directory.",
    )
    return parser.parse_args()


def _ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def _write_json(path: Path, payload: dict[str, object]) -> None:
    _ensure_parent(path)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _collect_failure_details(summary: Mapping[str, object], failed_sections: list[str]) -> list[dict[str, object]]:
    details: list[dict[str, object]] = []

    for section_name in failed_sections:
        if section_name == "module_verification":
            section = dict(summary.get("module_verification", {}))
            details.append(
                {
                    "section": section_name,
                    "reason": "Transferred archive payload did not satisfy manifest or archive integrity checks.",
                    "missing_modules": list(section.get("missing_modules", [])),
                    "missing_runtime_dependencies": list(section.get("missing_runtime_dependencies", [])),
                    "unexpected_entries": list(section.get("unexpected_entries", [])),
                    "load_order_matches_manifest": bool(section.get("load_order_matches_manifest", False)),
                    "load_order_mismatches": list(section.get("load_order_mismatches", [])),
                    "mismatched_manifest_entries": list(section.get("mismatched_manifest_entries", [])),
                    "diagnostics": list(section.get("diagnostics", [])),
                }
            )
            continue

        section = dict(summary.get(section_name, {}))
        details.append(
            {
                "section": section_name,
                "reason": "One or more cases in this validation section failed.",
                "failed_names": list(section.get("failed_names", [])),
                "timed_out_names": list(section.get("timed_out_names", [])),
                "failure_details": list(section.get("failure_details", [])),
                "timeout_count": int(section.get("timeout_count", 0)),
                "error_count": int(section.get("error_count", 0)),
            }
        )

    return details


def main() -> int:
    args = _parse_args()
    config = load_pipeline_config(args.config)
    default_timeout_s = float(config.get("default_timeout_seconds", 30))
    output_path = (
        args.output.expanduser().resolve()
        if args.output is not None
        else resolve_configured_path(
            config.get("summary_path"),
            config_path=args.config,
            default_path=DEFAULT_PIPELINE_SUMMARY_PATH,
        )
    )
    snapshot_path = (
        args.snapshot.expanduser().resolve()
        if args.snapshot is not None
        else resolve_configured_path(
            config.get("snapshot_path"),
            config_path=args.config,
            default_path=DEFAULT_PIPELINE_SNAPSHOT_PATH,
        )
    )
    edge_case_path = (
        args.edge_cases.expanduser().resolve()
        if args.edge_cases is not None
        else resolve_configured_path(
            config.get("edge_case_baseline_path"),
            config_path=args.config,
            default_path=DEFAULT_EDGE_CASE_BASELINE_PATH,
        )
    )
    visible_smoke_case_path = (
        args.visible_smoke_cases.expanduser().resolve()
        if args.visible_smoke_cases is not None
        else resolve_configured_path(
            config.get("visible_smoke_baseline_path"),
            config_path=args.config,
            default_path=DEFAULT_VISIBLE_SMOKE_CASE_PATH,
        )
    )
    arithmetic_case_path = (
        args.arithmetic_cases.expanduser().resolve()
        if args.arithmetic_cases is not None
        else resolve_configured_path(
            config.get("arithmetic_battery_path"),
            config_path=args.config,
            default_path=DEFAULT_ARITHMETIC_BATTERY_PATH,
        )
    )
    module_case_path = (
        args.module_cases.expanduser().resolve()
        if args.module_cases is not None
        else resolve_configured_path(
            config.get("module_battery_path"),
            config_path=args.config,
            default_path=DEFAULT_MODULE_BATTERY_PATH,
        )
    )
    fake_case_path = (
        args.fake_cases.expanduser().resolve()
        if args.fake_cases is not None
        else resolve_configured_path(
            config.get("fake_case_baseline_path"),
            config_path=args.config,
            default_path=DEFAULT_FAKE_CASE_BASELINE_PATH,
        )
    )
    started_at = time.time()
    summary: dict[str, object] = {
        "schema_version": 5,
        "runner": "tests/cemu/run_cemu_validation.py",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "requested_mode": args.mode,
        "success": False,
        "exit_code": EXIT_ERROR,
        "blockers": [],
        "failed_sections": [],
        "failure_details": [],
        "config_path": str(args.config),
        "default_timeout_seconds": default_timeout_s,
        "paths": {
            "repo_root": str(REPO_ROOT),
            "summary": str(output_path),
            "snapshot": str(snapshot_path),
            "edge_cases": str(edge_case_path),
            "visible_smoke_cases": str(visible_smoke_case_path),
            "arithmetic_cases": str(arithmetic_case_path),
            "module_cases": str(module_case_path),
            "fake_cases": str(fake_case_path),
        },
        "availability": {},
        "transfer_package": {},
        "module_verification": {},
        "checkpoints": [],
        "checkpoint_summary": {},
        "visible_smoke": [],
        "visible_smoke_summary": {},
        "accuracy_audit": [],
        "accuracy_summary": {},
        "arithmetic_battery": [],
        "arithmetic_battery_summary": {},
        "module_battery": [],
        "module_battery_summary": {},
        "edge_cases": [],
        "edge_case_summary": {},
        "recovery_cases": [],
        "recovery_summary": {},
        "fake_hardening": [],
        "fake_hardening_summary": {},
        "snapshot_written": "",
    }

    staging_root = Path(tempfile.mkdtemp(prefix="calcengine-cemu-validation-"))
    staged_transfer_dir = staging_root / "transfer"

    try:
        environment, transfer_package = resolve_pipeline_environment(config, config_path=args.config)
        summary["availability"] = {
            "ready": environment.ready,
            "blockers": list(environment.blockers),
            "rom_path": str(environment.rom_path) if environment.rom_path else "",
            "headless_path": str(environment.headless_path) if environment.headless_path else "",
            "autotester_path": str(environment.autotester_path) if environment.autotester_path else "",
            "gui_path": str(environment.gui_path) if environment.gui_path else "",
        }
        summary["transfer_package"] = {
            "manifest_path": str(transfer_package.manifest_path),
            "module_count": transfer_package.module_count,
            "runtime_dependency_count": transfer_package.runtime_dependency_count,
            "transfer_count": transfer_package.transfer_count,
            "ready": transfer_package.ready,
            "missing_entries": list(transfer_package.missing_entries),
        }

        if args.mode != "fake-only":
            if not environment.ready:
                summary["blockers"] = list(environment.blockers)
                summary["failed_sections"] = ["availability"]
                summary["failure_details"] = [
                    {
                        "section": "availability",
                        "reason": "Real-ROM CEmu environment was not ready.",
                        "blockers": list(environment.blockers),
                    }
                ]
                summary["exit_code"] = EXIT_ERROR
                return EXIT_ERROR
            stage_transfer_package(environment.transfer_package, staged_transfer_dir)
            real_sections = run_real_pipeline_sections(
                environment=environment,
                staged_transfer_dir=staged_transfer_dir,
                edge_case_path=edge_case_path,
                visible_smoke_case_path=visible_smoke_case_path,
                arithmetic_case_path=arithmetic_case_path,
                module_case_path=module_case_path,
                snapshot_path=snapshot_path,
                default_timeout_s=default_timeout_s,
            )
            summary.update(real_sections)

        if args.mode != "real-only":
            fake_sections = run_fake_pipeline_sections(
                fake_case_path=fake_case_path,
                transfer_package=transfer_package,
                default_timeout_s=default_timeout_s,
            )
            summary.update(fake_sections)

        failures: list[str] = []
        if summary["module_verification"] and not bool(summary["module_verification"].get("success", False)):
            failures.append("module_verification")
        if summary["checkpoint_summary"] and not bool(summary["checkpoint_summary"].get("success", False)):
            failures.append("checkpoint_summary")
        if summary["visible_smoke_summary"] and not bool(summary["visible_smoke_summary"].get("success", False)):
            failures.append("visible_smoke_summary")
        if summary["accuracy_summary"] and not bool(summary["accuracy_summary"].get("success", False)):
            failures.append("accuracy_summary")
        if summary["arithmetic_battery_summary"] and not bool(summary["arithmetic_battery_summary"].get("success", False)):
            failures.append("arithmetic_battery_summary")
        if summary["module_battery_summary"] and not bool(summary["module_battery_summary"].get("success", False)):
            failures.append("module_battery_summary")
        if summary["edge_case_summary"] and not bool(summary["edge_case_summary"].get("success", False)):
            failures.append("edge_case_summary")
        if summary["recovery_summary"] and not bool(summary["recovery_summary"].get("success", False)):
            failures.append("recovery_summary")
        if summary["fake_hardening_summary"] and not bool(summary["fake_hardening_summary"].get("success", False)):
            failures.append("fake_hardening_summary")

        summary["success"] = not failures
        summary["blockers"] = failures
        summary["failed_sections"] = failures
        summary["failure_details"] = _collect_failure_details(summary, failures)
        summary["exit_code"] = EXIT_PASS if not failures else EXIT_FAIL
        return int(summary["exit_code"])
    except Exception as exc:  # pragma: no cover - runtime guard
        summary["success"] = False
        summary["blockers"] = [str(exc)]
        summary["failed_sections"] = ["runtime"]
        summary["failure_details"] = [
            {
                "section": "runtime",
                "reason": "The validation driver raised an unexpected exception.",
                "blockers": [str(exc)],
            }
        ]
        summary["exit_code"] = EXIT_ERROR
        return EXIT_ERROR
    finally:
        summary["duration_seconds"] = round(time.time() - started_at, 2)
        _write_json(output_path, summary)
        if not args.keep_staging:
            shutil.rmtree(staging_root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
