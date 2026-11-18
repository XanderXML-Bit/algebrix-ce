import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { expect, test } from "vitest";

import { resolveCEmuEnvironment, scaleCEmuTimeoutMs } from "../src/testing/cemu-pipeline.ts";
import { runCEmuSmokeSuite } from "../src/testing/cemu-smoke-tests.ts";

const environment = resolveCEmuEnvironment();
const cemuSmokeReady = environment.artificeReady && process.env.CALCENGINE_ALLOW_REAL_CEMU_IN_CI === "true";
const cemuSkipReasons = [...environment.blockers];
if (environment.ready && !environment.artificeReady) {
  cemuSkipReasons.push("arTIfiCE asset is unavailable");
}
if (environment.artificeReady && process.env.CALCENGINE_ALLOW_REAL_CEMU_IN_CI !== "true") {
  cemuSkipReasons.push("real CEmu Vitest e2e lane is opt-in only; set CALCENGINE_ALLOW_REAL_CEMU_IN_CI=true");
}
const cemuTest = cemuSmokeReady ? test : test.skip;

if (!cemuSmokeReady) {
  console.warn(`[SKIP] CEmu smoke suite skipped: ${cemuSkipReasons.join("; ")}`);
}

cemuTest("CEmu end-to-end smoke suite passes on the real ROM lane", async () => {
  const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-vitest-cemu-"));
  const summary = await runCEmuSmokeSuite({
    outputRoot,
  });

  expect(summary.status).toBe("passed");
  expect(summary.failedCount).toBe(0);
  expect(summary.results.length).toBeGreaterThanOrEqual(3);
  expect(summary.results.every((result) => result.passed)).toBe(true);

  const directChecklist = summary.results.find((result) => result.name === "direct_real_rom_smoke_checklist");
  expect(directChecklist).toBeDefined();
  expect(directChecklist?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "boot_home",
    "addition_result",
    "prompt_after_addition",
    "multiplication_result",
    "prompt_after_multiplication",
    "negative_result",
    "decimal_result",
    "parenthesized_result",
    "power_result",
    "divide_by_zero_error",
    "prompt_after_divide_by_zero",
    "graphing_mode_result",
    "prompt_after_graphing_mode",
    "program_menu_open",
    "prompt_after_menu_close",
    "memory_store_variable",
    "memory_recall_variable",
    "memory_clear_variable",
    "memory_recall_after_clear",
    "overflow_error",
    "syntax_error_screen",
    "syntax_error_prompt_restore",
    "archive_program_execution",
  ]);

  const programSwitching = summary.results.find((result) => result.name === "program_switching_between_modules");
  expect(programSwitching).toBeDefined();
  expect(programSwitching?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "base_matrix_round_trip_first_module",
    "base_matrix_round_trip_second_module",
    "base_matrix_round_trip_restored_first_module",
    "base_factor_round_trip_first_module",
    "base_factor_round_trip_second_module",
    "base_factor_round_trip_restored_first_module",
    "factor_complex_round_trip_first_module",
    "factor_complex_round_trip_second_module",
    "factor_complex_round_trip_restored_first_module",
    "stats_finance_round_trip_first_module",
    "stats_finance_round_trip_second_module",
    "stats_finance_round_trip_restored_first_module",
    "solve_series_round_trip_first_module",
    "solve_series_round_trip_second_module",
    "solve_series_round_trip_restored_first_module",
    "complete_square_matrix_inverse_round_trip_first_module",
    "complete_square_matrix_inverse_round_trip_second_module",
    "complete_square_matrix_inverse_round_trip_restored_first_module",
    "usub_probability_round_trip_first_module",
    "usub_probability_round_trip_second_module",
    "usub_probability_round_trip_restored_first_module",
    "series_expand_round_trip_first_module",
    "series_expand_round_trip_second_module",
    "series_expand_round_trip_restored_first_module",
  ]);

  const rapidSwitching = summary.results.find((result) => result.name === "rapid_sequential_module_switching");
  expect(rapidSwitching).toBeDefined();
  expect(rapidSwitching?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "rapid_switch_base_module",
    "rapid_switch_matrix_module",
    "rapid_switch_finance_module",
    "rapid_switch_complex_module",
    "rapid_switch_stats_module",
    "rapid_switch_series_module",
    "rapid_switch_limit_module",
    "rapid_switch_usub_module",
    "rapid_switch_factor_module",
    "rapid_switch_probability_module",
    "rapid_switch_archive_count_stable",
  ]);

  const edgeCaseMath = summary.results.find((result) => result.name === "edge_case_math_inputs");
  expect(edgeCaseMath).toBeDefined();
  expect(edgeCaseMath?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "edge_math_division_by_zero",
    "edge_math_prompt_after_division_by_zero",
    "edge_math_overflow",
    "edge_math_prompt_after_overflow",
    "edge_math_large_literal_scientific_notation",
    "edge_math_prompt_after_large_literal",
    "edge_math_large_number_near_display_limit",
    "edge_math_prompt_after_large_number",
    "edge_math_negative_exponent",
    "edge_math_prompt_after_negative_exponent",
    "edge_math_repeated_equals",
  ]);

  const moduleHotSwap = summary.results.find((result) => result.name === "module_hot_swap_during_calculation");
  expect(moduleHotSwap).toBeDefined();
  expect(moduleHotSwap?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "module_hot_swap_prompt_restored",
    "module_hot_swap_recovery_module",
    "module_hot_swap_original_module_restored",
    "module_hot_swap_archive_count_stable",
  ]);

  const launcherMenuStress = summary.results.find((result) => result.name === "launcher_menu_navigation_stress");
  expect(launcherMenuStress).toBeDefined();
  expect(launcherMenuStress?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "launcher_menu_cycle_1_open",
    "launcher_menu_cycle_1_close",
    "launcher_menu_cycle_2_open",
    "launcher_menu_cycle_2_close",
    "launcher_menu_cycle_3_open",
    "launcher_menu_cycle_3_close",
    "launcher_menu_cycle_4_open",
    "launcher_menu_cycle_4_close",
  ]);

  const dependencyChains = summary.results.find((result) => result.name === "cross_module_dependency_chains");
  expect(dependencyChains).toBeDefined();
  expect(dependencyChains?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "base_matrix_shared_scratch_primary_module",
    "base_matrix_shared_scratch_dependent_module",
    "base_matrix_shared_scratch_shared_state_prompt",
    "base_matrix_shared_scratch_primary_module_reloaded",
    "factor_complex_symbolic_cache_primary_module",
    "factor_complex_symbolic_cache_dependent_module",
    "factor_complex_symbolic_cache_shared_state_prompt",
    "factor_complex_symbolic_cache_primary_module_reloaded",
    "stats_finance_shared_lists_primary_module",
    "stats_finance_shared_lists_dependent_module",
    "stats_finance_shared_lists_shared_state_prompt",
    "stats_finance_shared_lists_primary_module_reloaded",
  ]);

  const crossModuleFunctionCalls = summary.results.find((result) => result.name === "cross_module_function_calls");
  expect(crossModuleFunctionCalls).toBeDefined();
  expect(crossModuleFunctionCalls?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "cross_module_stats_function_dispatch",
    "cross_module_algebra_follow_up",
  ]);

  const memoryPressure = summary.results.find((result) => result.name === "archive_swap_under_memory_pressure");
  expect(memoryPressure).toBeDefined();
  expect(memoryPressure?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "memory_pressure_stage_1",
    "memory_pressure_stage_2",
    "memory_pressure_stage_3",
    "memory_pressure_stage_4",
    "memory_pressure_stage_5",
    "memory_pressure_stage_6",
    "memory_pressure_stage_7",
    "memory_pressure_stage_8",
    "memory_pressure_stage_9",
    "memory_pressure_stage_10",
    "memory_pressure_stage_11",
    "memory_pressure_stage_12",
    "memory_pressure_primary_module_reloaded",
    "memory_pressure_archive_count_stable",
  ]);

  const errorRecovery = summary.results.find((result) => result.name === "error_recovery_chain");
  expect(errorRecovery).toBeDefined();
  expect(errorRecovery?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "error_recovery_error_screen",
    "error_recovery_prompt_restored",
    "error_recovery_successful_calculation",
    "error_recovery_prompt_after_success",
  ]);
}, scaleCEmuTimeoutMs(360_000));
