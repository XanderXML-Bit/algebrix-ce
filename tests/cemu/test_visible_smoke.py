from __future__ import annotations

import json
from pathlib import Path

import pytest

from cemu_runner import CEmuRunner
from fake_backend import build_fake_cemu_runner, fake_crc_for_keys
from pipeline_support import get_real_cemu_status
from visible_smoke import (
    DEFAULT_VISIBLE_SMOKE_CASE_PATH,
    load_visible_smoke_cases,
    run_visible_smoke_cases,
    summarize_visible_smoke_results,
)


REAL_CEMU_READY, REAL_CEMU_SKIP_REASON = get_real_cemu_status()


def test_visible_smoke_catalog_covers_required_lanes() -> None:
    cases = load_visible_smoke_cases(DEFAULT_VISIBLE_SMOKE_CASE_PATH)
    names = {case.name for case in cases}
    lanes = {case.lane for case in cases}
    source_names = {case.source_case_name for case in cases if case.source_case_name}

    assert lanes == {
        "algebra",
        "arithmetic",
        "calculus",
        "complex",
        "finance",
        "geometry",
        "graphing",
        "integration",
        "launcher",
        "limits",
        "list_operations",
        "module_dispatch",
        "probability",
        "sequence",
        "series",
        "solver",
        "statistics",
        "table_generation",
    }
    assert len(cases) >= 26
    assert "launcher_program_menu_transition" in names
    assert "arith_subtraction_chain_visible_then_result" in names
    assert "graph_constant_plot_visible_then_dispatch" in names
    assert "sequence_odd_generator_visible_then_result" in names
    assert "sequence_table_probe_visible_then_result" in names
    assert "list_dimension_visible_then_result" in names
    assert "list_cumsum_visible_then_result" in names
    assert "algebra_factor_visible_then_result" in names
    assert "geometry_slope_visible_then_result" in names
    assert "calculus_nderiv_visible_then_result" in names
    assert "limits_polynomial_visible_then_result" in names
    assert "integration_definite_visible_then_result" in names
    assert "series_taylor_visible_then_result" in names
    assert "solver_linear_visible_then_result" in names
    assert "finance_present_value_visible_then_result" in names
    assert "statistics_mean_visible_then_result" in names
    assert "complex_real_part_visible_then_result" in names
    assert "probability_normalcdf_visible_then_result" in names
    assert "matrix_inverse_visible_then_result" in names
    assert "arith_add_2_plus_3" in source_names
    assert "arith_division_by_zero" in source_names
    assert "module_base_hex_255" in source_names
    assert "module_limit_advanced_cosine" in source_names
    assert "module_matrix_determinant" in source_names
    assert "module_matrix_inverse_identity" in source_names
    assert "module_alg_factor_quadratic" in source_names
    assert "module_geo_slope_two_point" in source_names
    assert "module_calc_nderiv_x2_at_1" in source_names
    assert "module_limit_polynomial" in source_names
    assert "module_int_definite_polynomial" in source_names
    assert "module_series_taylor_sin" in source_names
    assert "module_solve_linear" in source_names
    assert "module_finance_present_value" in source_names
    assert "module_stats_mean_singleton" in source_names
    assert "module_complex_real_part" in source_names
    assert "module_probability_normalcdf" in source_names


def test_matrix_inverse_prompt_return_tracks_observed_real_backend_crc_variants() -> None:
    cases = load_visible_smoke_cases(DEFAULT_VISIBLE_SMOKE_CASE_PATH)
    case = next(case for case in cases if case.name == "matrix_inverse_visible_then_result")

    assert set(case.followup_expected_crcs) >= {"2CAF4B8A", "22CEF46C"}


def test_visible_smoke_catalog_resolves_source_case_expectations(tmp_path: Path) -> None:
    catalog_path = tmp_path / "visible-smoke-cases.json"
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "suite": "visible_smoke",
                "cases": [
                    {
                        "name": "resolved_case",
                        "lane": "arithmetic",
                        "source_case": "arith_add_2_plus_3",
                        "followup_name": "prompt_after_clear",
                        "followup_description": "Prompt recovers after the addition result.",
                        "followup_events": [
                            {"kind": "key", "key": "clear"},
                            {"kind": "pause", "duration_ms": 500}
                        ],
                        "followup_expected_crcs": ["349F4775"]
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    case = load_visible_smoke_cases(catalog_path)[0]

    assert case.primary_case.input_expression == "2+3"
    assert case.primary_case.expected_display == "5"
    assert case.primary_case.expected_crcs == ("3750E64D",)
    assert case.followup_expected_crcs == ("349F4775",)


def test_fake_visible_smoke_suite_passes(
    tmp_path: Path,
    staged_transfer_package_dir: Path,
    temp_rom_path: Path,
) -> None:
    catalog_path = tmp_path / "fake-visible-smoke.json"
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "suite": "visible_smoke",
                "cases": [
                    {
                        "name": "fake_launcher_to_menu",
                        "lane": "launcher",
                        "category": "launcher",
                        "description": "Fake prompt remains stable before opening the program menu.",
                        "expected_crcs": [fake_crc_for_keys("clear", "clear")],
                        "auto_enter": False,
                        "prompt_reset": True,
                        "followup_name": "program_menu_open",
                        "followup_description": "Fake PRGM menu capture stays deterministic.",
                        "followup_events": [
                            {"kind": "key", "key": "prgm"},
                            {"kind": "pause", "duration_ms": 1200}
                        ],
                        "followup_expected_crcs": [fake_crc_for_keys("clear", "clear", "prgm")]
                    },
                    {
                        "name": "fake_addition_prompt_return",
                        "lane": "arithmetic",
                        "category": "arithmetic",
                        "description": "Fake arithmetic result remains deterministic before clear returns to prompt.",
                        "input_expression": "2+3",
                        "expected_crcs": [fake_crc_for_keys("clear", "clear", "2", "+", "3", "enter")],
                        "followup_name": "prompt_after_clear",
                        "followup_description": "Fake prompt returns after clear.",
                        "followup_events": [
                            {"kind": "key", "key": "clear"},
                            {"kind": "pause", "duration_ms": 500}
                        ],
                        "followup_expected_crcs": [fake_crc_for_keys("clear", "clear", "2", "+", "3", "enter", "clear")]
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    cases = load_visible_smoke_cases(catalog_path)

    def make_runner() -> CEmuRunner:
        return build_fake_cemu_runner(tmp_path, temp_rom_path)

    results = run_visible_smoke_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=cases,
        default_timeout_s=5.0,
    )
    summary = summarize_visible_smoke_results(cases, results)

    assert summary["success"] is True
    assert summary["assertion_count"] == 4
    assert summary["failed_count"] == 0
    assert summary["lane_summary"] == {
        "arithmetic": {
            "case_count": 1,
            "assertion_count": 2,
            "passed_count": 1,
            "failed_count": 0,
            "failed_names": [],
            "failed_assertions": [],
            "status": "passed",
        },
        "launcher": {
            "case_count": 1,
            "assertion_count": 2,
            "passed_count": 1,
            "failed_count": 0,
            "failed_names": [],
            "failed_assertions": [],
            "status": "passed",
        },
    }
    assert results[0]["assertions"][1]["name"] == "program_menu_open"
    assert results[1]["assertions"][0]["name"] == "primary"


@pytest.mark.skipif(not REAL_CEMU_READY, reason=REAL_CEMU_SKIP_REASON)
def test_visible_smoke_real_backend(real_cemu_config, staged_transfer_package_dir: Path) -> None:
    assert real_cemu_config is not None
    cases = load_visible_smoke_cases(DEFAULT_VISIBLE_SMOKE_CASE_PATH)

    def make_runner() -> CEmuRunner:
        return CEmuRunner(real_cemu_config)

    results = run_visible_smoke_cases(
        make_runner=make_runner,
        program_paths=[staged_transfer_package_dir],
        cases=cases,
        default_timeout_s=30.0,
    )
    summary = summarize_visible_smoke_results(cases, results)

    assert summary["success"] is True
    assert summary["failed_count"] == 0
