from __future__ import annotations

import json
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Iterable, Mapping, Sequence

from cemu_runner import CEmuRunner
from keypress_sequences import KeyAction, pause, press


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ACCURACY_BASELINE_PATH = REPO_ROOT / "tests" / "cemu" / "cemu_accuracy_baseline.json"
DEFAULT_CASE_RESET_SEQUENCE: tuple[KeyAction, ...] = (
    press("clear"),
    pause(500, note="return to prompt"),
    press("clear"),
    pause(500, note="blank prompt"),
)


@dataclass(frozen=True)
class AccuracyCase:
    name: str
    category: str
    description: str
    expected_display: str
    display_kind: str
    expected_crcs: tuple[str, ...]
    input_tokens: tuple[str, ...]
    result_delay_ms: int
    timeout_ms: int
    reference_value: str
    exact_value: str
    source_hash_name: str
    notes: str

    def to_summary(self) -> dict[str, object]:
        return {
            "name": self.name,
            "category": self.category,
            "description": self.description,
            "expected_display": self.expected_display,
            "display_kind": self.display_kind,
            "expected_crcs": list(self.expected_crcs),
            "input_tokens": list(self.input_tokens),
            "result_delay_ms": self.result_delay_ms,
            "timeout_ms": self.timeout_ms,
            "reference_value": self.reference_value,
            "exact_value": self.exact_value,
            "source_hash_name": self.source_hash_name,
            "notes": self.notes,
        }


def load_accuracy_cases(path: Path = DEFAULT_ACCURACY_BASELINE_PATH) -> tuple[AccuracyCase, ...]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return tuple(
        AccuracyCase(
            name=str(row["name"]),
            category=str(row["category"]),
            description=str(row["description"]),
            expected_display=str(row["expected_display"]),
            display_kind=str(row.get("display_kind", "numeric")),
            expected_crcs=tuple(str(value).upper() for value in row["expected_crcs"]),
            input_tokens=tuple(str(value) for value in row["input_tokens"]),
            result_delay_ms=int(row.get("result_delay_ms", 3000)),
            timeout_ms=int(row.get("timeout_ms", 8000)),
            reference_value=str(row.get("reference_value", "")),
            exact_value=str(row.get("exact_value", "")),
            source_hash_name=str(row.get("source_hash_name", "")),
            notes=str(row.get("notes", "")),
        )
        for row in payload["cases"]
    )


def _count_decimal_places(value: str) -> int:
    if "." not in value:
        return 0
    return len(value.split(".", 1)[1].rstrip("0"))


def _normalize_decimal(value: Decimal) -> str:
    normalized = format(value.normalize(), "f")
    if "." in normalized:
        normalized = normalized.rstrip("0").rstrip(".")
    return normalized or "0"


def _precision_summary(case: AccuracyCase) -> dict[str, object]:
    if case.display_kind != "numeric" or not case.expected_display or not case.reference_value:
        return {
            "available": False,
            "decimal_places": 0,
            "absolute_error": "",
            "signed_error": "",
            "comparison_note": "Precision analysis is only available for numeric display baselines.",
        }

    try:
        display_value = Decimal(case.expected_display)
        reference_value = Decimal(case.reference_value)
    except InvalidOperation:
        return {
            "available": False,
            "decimal_places": 0,
            "absolute_error": "",
            "signed_error": "",
            "comparison_note": "Precision analysis could not parse the numeric display baseline.",
        }

    signed_error = display_value - reference_value
    absolute_error = abs(signed_error)
    return {
        "available": True,
        "decimal_places": _count_decimal_places(case.expected_display),
        "absolute_error": _normalize_decimal(absolute_error),
        "signed_error": _normalize_decimal(signed_error),
        "comparison_note": "Calculated from the expected TI display value versus the exact/reference value.",
    }


def _crc_frequency(cases: Sequence[AccuracyCase]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for case in cases:
        for crc in case.expected_crcs:
            counts[crc] = counts.get(crc, 0) + 1
    return counts


def _build_case_sequence(case: AccuracyCase, key_delay_ms: int = 250) -> list[KeyAction]:
    sequence: list[KeyAction] = []
    for token in case.input_tokens:
        sequence.append(press(token))
        sequence.append(pause(key_delay_ms, note="settle after key"))
    sequence.append(pause(200, note="settle before enter"))
    sequence.append(press("enter"))
    sequence.append(pause(case.result_delay_ms, note="wait for result"))
    return sequence


def run_accuracy_audit(
    runner: CEmuRunner,
    cases: Sequence[AccuracyCase],
) -> list[dict[str, object]]:
    crc_frequency = _crc_frequency(cases)
    results: list[dict[str, object]] = []

    for case in cases:
        runner.reset(timeout_s=max(5.0, case.timeout_ms / 1000))
        runner.send_sequence(DEFAULT_CASE_RESET_SEQUENCE, timeout_s=max(5.0, case.timeout_ms / 1000))
        runner.send_sequence(_build_case_sequence(case), timeout_s=max(5.0, case.timeout_ms / 1000))
        response = runner.capture_screen(timeout_s=max(5.0, case.timeout_ms / 1000))
        observed_crc = str(response.payload["crc"]).upper()
        crc_specificity = "unique" if max(crc_frequency.get(crc, 0) for crc in case.expected_crcs) == 1 else "shared"

        results.append(
            {
                "name": case.name,
                "category": case.category,
                "description": case.description,
                "display_kind": case.display_kind,
                "expected_display": case.expected_display,
                "expected_crcs": list(case.expected_crcs),
                "observed_crc": observed_crc,
                "passed": observed_crc in case.expected_crcs,
                "crc_specificity": crc_specificity,
                "reference_value": case.reference_value,
                "exact_value": case.exact_value,
                "precision": _precision_summary(case),
                "source_hash_name": case.source_hash_name,
                "notes": case.notes,
                "comparison_mode": "screen_crc",
                "comparison_limitations": (
                    "The current autotester-wrapper backend exposes only screen CRCs, not decoded LCD text. "
                    "Display-value precision analysis is therefore computed from the locked TI baseline, while "
                    "emulator correctness is asserted via real-ROM CRC matching."
                ),
                "payload": dict(response.payload),
            }
        )

    return results


def summarize_accuracy_audit(
    cases: Sequence[AccuracyCase],
    results: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    passed = [result for result in results if bool(result.get("passed"))]
    failed = [result for result in results if not bool(result.get("passed"))]
    shared_crc_cases = [result for result in results if result.get("crc_specificity") == "shared"]
    numeric_cases = [case for case in cases if case.display_kind == "numeric"]
    return {
        "case_count": len(results),
        "passed_count": len(passed),
        "failed_count": len(failed),
        "failed_names": [str(result["name"]) for result in failed],
        "categories": sorted({case.category for case in cases}),
        "numeric_case_count": len(numeric_cases),
        "shared_crc_case_count": len(shared_crc_cases),
        "success": not failed,
        "comparison_mode": "screen_crc",
        "limitations": [
            "The cemu-headless autotester wrapper currently returns CRC-only screen captures.",
            "Precision and rounding analysis are computed against the locked TI display baselines, not OCR from emulator frames.",
            "Cases with shared CRC baselines are weaker regressions than unique-CRC cases and are flagged in the report.",
        ],
    }
