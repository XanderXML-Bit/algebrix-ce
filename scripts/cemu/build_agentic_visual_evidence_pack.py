from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Mapping, Sequence


DEFAULT_CONFIDENCE_JSON = Path("tests/emulator/outputs/cemu-release-candidate-confidence.json")
DEFAULT_OUTPUT_JSON = Path("tests/emulator/outputs/agentic-visual-evidence-pack.json")
DEFAULT_OUTPUT_MD = Path("tests/emulator/outputs/agentic-visual-evidence-pack.md")


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _as_mapping(value: object) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _as_sequence(value: object) -> Sequence[object]:
    return value if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)) else ()


def _items_by_id(items: object) -> dict[str, Mapping[str, Any]]:
    by_id: dict[str, Mapping[str, Any]] = {}
    for item in _as_sequence(items):
        if isinstance(item, Mapping):
            item_id = str(item.get("id", "")).strip()
            if item_id:
                by_id[item_id] = item
    return by_id


def _status_from_bool(covered: bool, passed: bool) -> str:
    if not covered:
        return "missing"
    return "passed" if passed else "failed"


def _collect_first_try(payload: Mapping[str, Any]) -> dict[str, Any]:
    checklist = _as_mapping(payload.get("first_try_confirmation_checklist"))
    items = list(_as_sequence(checklist.get("items")))
    return {
        "available": bool(checklist.get("available")),
        "required_item_count": int(checklist.get("required_item_count", len(items)) or 0),
        "covered_item_count": int(checklist.get("covered_item_count", 0) or 0),
        "passed_item_count": int(checklist.get("passed_item_count", 0) or 0),
        "missing_items": [str(value) for value in _as_sequence(checklist.get("missing_items"))],
        "items": [dict(item) for item in items if isinstance(item, Mapping)],
    }


def _collect_capability_items(payload: Mapping[str, Any]) -> dict[str, Mapping[str, Any]]:
    capability = _as_mapping(payload.get("capability_depth"))
    return _items_by_id(capability.get("items"))


def _windows_path_to_local(path: str) -> Path:
    if len(path) >= 3 and path[1:3] == ":\\":
        drive = path[0].lower()
        return Path(f"/mnt/{drive}/" + path[3:].replace("\\", "/"))
    return Path(path)


def _repo_root_from_confidence_json(confidence_json: Path) -> Path:
    resolved = confidence_json.resolve()
    for parent in resolved.parents:
        if (parent / "tests").is_dir() and (parent / "scripts").is_dir():
            return parent
    return Path.cwd()


def _artifact_exists(path: str) -> bool:
    local = _windows_path_to_local(path)
    if local.exists():
        return True
    if not local.is_absolute():
        return (Path.cwd() / local).exists()
    return False


def _collect_visual_artifacts(payload: Mapping[str, Any], *, confidence_json: Path) -> dict[str, Any]:
    capability_items = _collect_capability_items(payload)
    artifacts: list[dict[str, object]] = []
    seen: set[tuple[str, str]] = set()

    def add(source: str, path_value: object, label: str = "") -> None:
        path = str(path_value or "").strip()
        if not path:
            return
        key = (source, path)
        if key in seen:
            return
        seen.add(key)
        exists = _artifact_exists(path)
        artifacts.append({"source": source, "path": path, "label": label, "exists": exists})

    for item_id in ("launch_visual_surfaces", "matrix_literal_dispatch"):
        item = capability_items.get(item_id, {})
        for path in _as_sequence(item.get("snapshot_artifact_paths")):
            add(item_id, path, str(item.get("label", item_id)))

    visible = _as_mapping(payload.get("visible_smoke_summary"))
    for result in _as_sequence(visible.get("results")):
        if not isinstance(result, Mapping):
            continue
        result_name = str(result.get("name", ""))
        for assertion in _as_sequence(result.get("assertions")):
            if isinstance(assertion, Mapping):
                add("visible_smoke", assertion.get("snapshot_path"), f"{result_name}:{assertion.get('name', '')}")

    for smoke in _as_sequence(payload.get("smoke_tests")):
        if not isinstance(smoke, Mapping):
            continue
        smoke_name = str(smoke.get("name", ""))
        for checkpoint in _as_sequence(smoke.get("checkpoints")):
            if isinstance(checkpoint, Mapping):
                add("smoke_tests", checkpoint.get("snapshot_path"), f"{smoke_name}:{checkpoint.get('step', '')}")

    repo_root = _repo_root_from_confidence_json(confidence_json)
    durable_roots = [
        confidence_json.parent,
        repo_root / "tests" / "cemu" / "outputs",
    ]
    durable_needles = (
        "calcengine_launch",
        "calcengine_menu",
        "calcengine_result_2plus3",
        "addition_result",
        "multiplication_result",
        "prompt_after_menu_close",
        "switch_to_base_module",
        "switch_to_matrix_module",
        "boot_home",
    )
    for root in durable_roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*.state.json")):
            rel = path.relative_to(repo_root) if path.is_relative_to(repo_root) else path
            haystack = str(rel).lower()
            if any(needle in haystack for needle in durable_needles):
                add("durable_repo_artifact", str(rel), path.stem)

    existing_count = sum(1 for artifact in artifacts if artifact["exists"])
    return {
        "reference_count": len(artifacts),
        "existing_artifact_count": existing_count,
        "missing_artifact_count": len(artifacts) - existing_count,
        "artifacts": artifacts,
    }


def _first_try_item_passed(first_try: Mapping[str, Any], *ids: str) -> bool:
    items = _items_by_id(first_try.get("items"))
    return all(bool(items.get(item_id, {}).get("passed")) for item_id in ids)


def _visible_has_passing_case(payload: Mapping[str, Any], *needles: str) -> bool:
    visible = _as_mapping(payload.get("visible_smoke_summary"))
    if not bool(visible.get("success")):
        return False
    lowered_needles = tuple(needle.lower() for needle in needles)
    for result in _as_sequence(visible.get("results")):
        if not isinstance(result, Mapping):
            continue
        name = str(result.get("name", "")).lower()
        lane = str(result.get("lane", "")).lower()
        haystack = f"{name} {lane}"
        if all(needle in haystack for needle in lowered_needles):
            assertions = [row for row in _as_sequence(result.get("assertions")) if isinstance(row, Mapping)]
            if assertions and all(bool(row.get("passed")) for row in assertions):
                return True
    return False


def _load_fidelity_fallback(confidence_json: Path, *, summary_name: str, item_id: str, label: str) -> dict[str, Any]:
    summary_path = confidence_json.parent / summary_name
    if not summary_path.is_file():
        return {}
    try:
        summary = _read_json(summary_path)
    except (OSError, json.JSONDecodeError):
        return {}
    return {
        "id": item_id,
        "label": label,
        "covered": True,
        "passed": bool(summary.get("success")),
        "status": "passed" if bool(summary.get("success")) else "failed",
        "summary_path": str(summary_path),
        "runner": str(summary.get("runner", "")),
        "mode": str(summary.get("mode", "")),
        "exit_code": int(summary.get("exit_code", 0) or 0),
    }


def _load_summary_success(confidence_json: Path, summary_name: str) -> dict[str, Any]:
    summary_path = confidence_json.parent / summary_name
    if not summary_path.is_file():
        return {}
    try:
        summary = _read_json(summary_path)
    except (OSError, json.JSONDecodeError):
        return {}
    return {
        "summary_path": str(summary_path),
        "success": bool(summary.get("success")),
        "exit_code": int(summary.get("exit_code", 0) or 0),
        "mode": str(summary.get("mode", "")),
        "template": str(summary.get("template", "")),
    }


def _build_prior_hardware_failure_replay(
    payload: Mapping[str, Any],
    first_try: Mapping[str, Any],
    *,
    confidence_json: Path,
) -> dict[str, Any]:
    capability_items = _collect_capability_items(payload)
    keypad = capability_items.get("keypad_control_fidelity", {}) or _load_fidelity_fallback(
        confidence_json,
        summary_name="keypad-fidelity.verify.summary.json",
        item_id="keypad_control_fidelity",
        label="Keypad control fidelity suite",
    )
    lcd = capability_items.get("lcd_visual_fidelity", {}) or _load_fidelity_fallback(
        confidence_json,
        summary_name="lcd-fidelity.verify.summary.json",
        item_id="lcd_visual_fidelity",
        label="LCD visual fidelity suite",
    )

    clean_exit_visible = _visible_has_passing_case(payload, "clean", "exit")
    clean_exit_summary = _load_summary_success(confidence_json, "clean-exit-verify.summary.json")
    if clean_exit_visible:
        unexpected_exit_status = "passed"
        unexpected_exit_evidence = ["visible_smoke_summary clean-exit case"]
        unexpected_exit_notes = ["Explicit clean-exit visible smoke case passed."]
    elif clean_exit_summary.get("success"):
        unexpected_exit_status = "partial"
        unexpected_exit_evidence = [str(clean_exit_summary.get("summary_path", ""))]
        unexpected_exit_notes = [
            "Clean-exit verify summary passed, but a richer visual no-crash replay around Enter, menu, history, and error recovery is still needed."
        ]
    else:
        unexpected_exit_status = "missing"
        unexpected_exit_evidence = []
        unexpected_exit_notes = ["Needs an explicit no-crash/no-exit visual replay around Enter, menu, history, and error recovery."]

    replay_items = [
        {
            "id": "wrong_or_base_ti_key_leakage",
            "label": "wrong/base-TI key leakage",
            "status": _status_from_bool(bool(keypad.get("covered")), bool(keypad.get("passed"))),
            "evidence": [str(keypad.get("summary_path", ""))] if keypad.get("summary_path") else [],
            "notes": ["Uses the keypad-control fidelity suite as the current automated guard against shifted-key/base-TI leakage."],
        },
        {
            "id": "menu_button_nonfunctional",
            "label": "menu button nonfunctional",
            "status": "passed" if _first_try_item_passed(first_try, "menu_open_close") else "missing",
            "evidence": ["first_try_confirmation_checklist.menu_open_close"],
            "notes": ["Requires both menu-open and menu-close checkpoints to pass."],
        },
        {
            "id": "history_scroll_retrieval",
            "label": "history-scroll retrieval",
            "status": "passed" if _visible_has_passing_case(payload, "history") else "missing",
            "evidence": ["visible_smoke_summary history case"] if _visible_has_passing_case(payload, "history") else [],
            "notes": ["Needs an explicit visual walk through previous input/output retrieval, not just internal history code coverage."],
        },
        {
            "id": "bad_enter_result_formatting_2_plus_2_style",
            "label": "bad Enter/result formatting (`2+2` / `2 XP`-style failure)",
            "status": "passed"
            if _first_try_item_passed(first_try, "addition_2_plus_2")
            else "partial"
            if _first_try_item_passed(first_try, "addition_2_plus_3", "return_to_prompt_after_result")
            else "missing",
            "evidence": ["first_try_confirmation_checklist.addition_2_plus_3", "first_try_confirmation_checklist.return_to_prompt_after_result"],
            "notes": ["Current proxy evidence proves the Enter/result path for 2+3; a literal 2+2 replay remains preferable because that was the reported hardware symptom."],
        },
        {
            "id": "unexpected_exits_or_crashes",
            "label": "unexpected exits/crashes",
            "status": unexpected_exit_status,
            "evidence": unexpected_exit_evidence,
            "notes": unexpected_exit_notes,
        },
        {
            "id": "visual_rendering_regression",
            "label": "visual rendering regression",
            "status": _status_from_bool(bool(lcd.get("covered")), bool(lcd.get("passed"))),
            "evidence": [str(lcd.get("summary_path", ""))] if lcd.get("summary_path") else [],
            "notes": ["LCD fidelity suite is the current automated visual-rendering guard; richer screenshot review still belongs in the evidence pack."],
        },
    ]
    passed = sum(1 for item in replay_items if item["status"] == "passed")
    partial = sum(1 for item in replay_items if item["status"] == "partial")
    missing = sum(1 for item in replay_items if item["status"] == "missing")
    failed = sum(1 for item in replay_items if item["status"] == "failed")
    return {
        "required_item_count": len(replay_items),
        "passed_item_count": passed,
        "partial_item_count": partial,
        "missing_item_count": missing,
        "failed_item_count": failed,
        "status": "passed" if passed == len(replay_items) else "failed" if failed else "partial",
        "items": replay_items,
    }


def build_evidence_pack(confidence_json: Path) -> dict[str, Any]:
    confidence = _read_json(confidence_json)
    first_try = _collect_first_try(confidence)
    visual = _collect_visual_artifacts(confidence, confidence_json=confidence_json)
    replay = _build_prior_hardware_failure_replay(confidence, first_try, confidence_json=confidence_json)
    status = "passed" if replay["status"] == "passed" and visual["existing_artifact_count"] > 0 else "partial"
    return {
        "schema_version": 1,
        "runner": "scripts/cemu/build_agentic_visual_evidence_pack.py",
        "source_confidence_json": str(confidence_json),
        "status": status,
        "release_candidate_ready_from_source": bool(confidence.get("release_candidate_ready")),
        "first_try_confirmation": first_try,
        "prior_hardware_failure_replay": replay,
        "visual_evidence": visual,
        "next_gaps": [
            item["label"]
            for item in replay["items"]
            if item["status"] in {"missing", "partial", "failed"}
        ],
    }


def render_markdown(payload: Mapping[str, Any]) -> str:
    lines = [
        "# Agentic Visual Evidence Pack",
        "",
        f"Source confidence JSON: `{payload.get('source_confidence_json', '')}`",
        f"Overall status: **{payload.get('status', '')}**",
        "",
        "## First-try confirmation",
    ]
    first_try = _as_mapping(payload.get("first_try_confirmation"))
    lines.append(
        f"- Passed {first_try.get('passed_item_count', 0)}/{first_try.get('required_item_count', 0)} directed first-try items."
    )
    lines.extend(["", "## Prior hardware-failure replay", ""])
    replay = _as_mapping(payload.get("prior_hardware_failure_replay"))
    for item in _as_sequence(replay.get("items")):
        if isinstance(item, Mapping):
            lines.append(f"- **{item.get('label', '')}**: `{item.get('status', '')}`")
            for note in _as_sequence(item.get("notes")):
                lines.append(f"  - {note}")
    lines.extend(["", "## Agentic visual evidence artifacts", ""])
    visual = _as_mapping(payload.get("visual_evidence"))
    lines.append(f"Artifact references: `{visual.get('reference_count', 0)}`")
    lines.append(f"Existing artifacts: `{visual.get('existing_artifact_count', 0)}`")
    lines.append(f"Missing/stale artifact references: `{visual.get('missing_artifact_count', 0)}`")
    for artifact in _as_sequence(visual.get("artifacts")):
        if isinstance(artifact, Mapping):
            label = str(artifact.get("label", "")).strip()
            label_part = f" — {label}" if label else ""
            lines.append(f"- `{artifact.get('source', '')}`{label_part}: `{artifact.get('path', '')}` (exists: `{artifact.get('exists', False)}`)")
    lines.extend(["", "## Remaining gaps", ""])
    for gap in _as_sequence(payload.get("next_gaps")):
        lines.append(f"- {gap}")
    return "\n".join(lines).rstrip() + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build an agent-reviewable visual evidence pack from release-gate confidence JSON.")
    parser.add_argument("--confidence-json", type=Path, default=DEFAULT_CONFIDENCE_JSON)
    parser.add_argument("--output-json", type=Path, default=DEFAULT_OUTPUT_JSON)
    parser.add_argument("--output-md", type=Path, default=DEFAULT_OUTPUT_MD)
    args = parser.parse_args(argv)

    payload = build_evidence_pack(args.confidence_json)
    _write_json(args.output_json, payload)
    args.output_md.parent.mkdir(parents=True, exist_ok=True)
    args.output_md.write_text(render_markdown(payload), encoding="utf-8")
    print(json.dumps({"ok": True, "status": payload["status"], "output_json": str(args.output_json), "output_md": str(args.output_md)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
