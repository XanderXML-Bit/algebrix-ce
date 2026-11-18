import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { afterEach, expect, test, vi } from "vitest";

const spawnSyncMock = vi.fn();
const sessionOptionsLog: Array<Record<string, unknown>> = [];

vi.mock("node:child_process", () => ({
  spawnSync: (...args: unknown[]) => spawnSyncMock(...args),
}));

vi.mock("../src/testing/cemu-interaction.ts", () => {
  function countTransferArtifacts(transferPath: string): number {
    if (!transferPath || !fs.existsSync(transferPath)) {
      return 0;
    }
    return fs.readdirSync(transferPath).filter((name) => /\.(8xp|8xv)$/i.test(name)).length;
  }

  class MockCEmuHeadlessSession {
    currentTransferPath = "";

    constructor(options: Record<string, unknown>) {
      sessionOptionsLog.push(options);
    }

    async boot(transferPath: string): Promise<void> {
      this.currentTransferPath = transferPath;
    }

    async close(): Promise<void> {}

    async reset(): Promise<void> {}

    async getState(): Promise<{ ok: boolean; cmd: string; app_count: number }> {
      return {
        ok: true,
        cmd: "get_state",
        app_count: countTransferArtifacts(this.currentTransferPath),
      };
    }

    async sendSequence(): Promise<void> {
      if (this.currentTransferPath.includes("corrupted-transfer")) {
        throw new Error("Synthetic corrupted module load rejected.");
      }
    }

    async captureScreenAssertion(options: {
      name: string;
      description: string;
      expectedCrcs: string[];
      evidenceDir: string;
    }) {
      const stateSnapshotPath = path.join(options.evidenceDir, `${options.name}.state.json`);
      fs.mkdirSync(options.evidenceDir, { recursive: true });
      fs.writeFileSync(stateSnapshotPath, JSON.stringify({ name: options.name }, null, 2), "utf8");
      return {
        name: options.name,
        description: options.description,
        passed: true,
        expectedCrcs: options.expectedCrcs,
        observedCrc: options.expectedCrcs[0] ?? "DEADBEEF",
        expectedOutputExcerptContains: [],
        observedOutputExcerpt: "mocked",
        screenshotMode: "crc",
        stateSnapshotPath,
        autotesterExit: 0,
      };
    }
  }

  return {
    CEmuHeadlessSession: MockCEmuHeadlessSession,
    expressionToEvents: (expression: string) => [{ kind: "expression", expression }],
    tokensToEvents: (tokens: string[]) => tokens.map((token) => ({ kind: "key", key: token })),
    pause: (durationMs: number, note: string) => ({ kind: "pause", durationMs, note }),
    press: (key: string, note?: string) => ({ kind: "key", key, note: note ?? "" }),
  };
});

const { runCEmuSmokeSuite } = await import("../src/testing/cemu-smoke-tests.ts");
const {
  getCEmuTimeoutMultiplier,
  resolveCEmuEnvironment,
  scaleCEmuTimeoutMs,
  stageTransferPackage,
  validateStagedTransferDirectory,
} = await import("../src/testing/cemu-pipeline.ts");

const tempRoots: string[] = [];

function buildSizedPayload(label: string, sizeBytes: number): Buffer {
  const source = Buffer.from(`${label}|`, "utf8");
  const payload = Buffer.alloc(sizeBytes);
  for (let offset = 0; offset < sizeBytes; offset += source.length) {
    source.copy(payload, offset, 0, Math.min(source.length, sizeBytes - offset));
  }
  return payload;
}

afterEach(() => {
  spawnSyncMock.mockReset();
  sessionOptionsLog.length = 0;
  delete process.env.CEMU_TIMEOUT_MULTIPLIER;
  delete process.env.CI;
  delete process.env.GITHUB_ACTIONS;
  delete process.env.CALCENGINE_ALLOW_REAL_CEMU_IN_CI;
  while (tempRoots.length > 0) {
    fs.rmSync(tempRoots.pop()!, { recursive: true, force: true });
  }
});

function buildFakeEnvironment(options: { artificeReady?: boolean } = {}) {
  const rootDir = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-cemu-ts-unit-"));
  tempRoots.push(rootDir);

  const transferDir = path.join(rootDir, "transfer");
  fs.mkdirSync(transferDir, { recursive: true });
  const moduleCatalog = [
    { name: "CALCENG.8xp", sizeBytes: 41386 },
    { name: "CEBASE.8xp", sizeBytes: 31026 },
    { name: "CEALGF.8xp", sizeBytes: 35101 },
    { name: "CEALGSY.8xp", sizeBytes: 45786 },
    { name: "CEASLV.8xp", sizeBytes: 36244 },
    { name: "CECLMB.8xp", sizeBytes: 33872 },
    { name: "CEIUSB.8xp", sizeBytes: 47942 },
    { name: "CECSER.8xp", sizeBytes: 36140 },
    { name: "CECSEQ.8xp", sizeBytes: 35832 },
    { name: "CESTPRB.8xp", sizeBytes: 46158 },
    { name: "CEFIN.8xp", sizeBytes: 33161 },
    { name: "CEMAT.8xp", sizeBytes: 34606 },
    { name: "CELSCX.8xp", sizeBytes: 41761 },
  ];
  const resolvedPaths: Record<string, string> = {};
  const manifestPath = path.join(rootDir, "manifest.json");
  const moduleEntries = moduleCatalog.map((moduleEntry) => {
    const modulePath = path.join(transferDir, moduleEntry.name);
    fs.writeFileSync(modulePath, buildSizedPayload(moduleEntry.name, moduleEntry.sizeBytes));
    resolvedPaths[moduleEntry.name] = modulePath;
    return {
      name: moduleEntry.name,
      sizeBytes: moduleEntry.sizeBytes,
      crc32: "",
      kind: "module" as const,
    };
  });
  const readmePath = path.join(transferDir, "README.md");
  const romPath = path.join(rootDir, "Rom.rom");
  const autotesterPath = path.join(rootDir, "autotester.exe");
  const headlessPath = path.join(rootDir, "cemu-headless.exe");
  const artificePath = path.join(rootDir, "artifice-v2.8xp");
  fs.writeFileSync(readmePath, "CalcEngine transfer package", "utf8");
  fs.writeFileSync(romPath, "ROM", "utf8");
  fs.writeFileSync(autotesterPath, "AUTOTESTER", "utf8");
  fs.writeFileSync(headlessPath, "HEADLESS", "utf8");
  if (options.artificeReady) {
    fs.writeFileSync(artificePath, "ARTIFICE", "utf8");
  }
  resolvedPaths["README.md"] = readmePath;
  fs.writeFileSync(manifestPath, JSON.stringify({
    modules: moduleEntries.map((entry) => ({
      name: entry.name,
      size_bytes: entry.sizeBytes,
      crc32: entry.crc32,
    })),
    runtime_dependencies: [],
    readme: {
      name: "README.md",
      crc32: "",
    },
  }, null, 2), "utf8");

  return {
    repoRoot: rootDir,
    romPath,
    guiPath: null,
    autotesterPath,
    headlessPath,
    artificePath: options.artificeReady ? artificePath : null,
    transferPackage: {
      manifestPath,
      moduleEntries,
      runtimeEntries: [],
      readmeEntry: {
        name: "README.md",
        sizeBytes: -1,
        crc32: "",
        kind: "readme" as const,
      },
      resolvedPaths,
      candidateRoots: [transferDir],
      missingEntries: [],
      moduleNames: moduleEntries.map((entry) => entry.name),
      runtimeNames: [],
      transferPaths: moduleEntries.map((entry) => resolvedPaths[entry.name]),
      allArtifactPaths: [...moduleEntries.map((entry) => resolvedPaths[entry.name]), readmePath],
      moduleCount: moduleEntries.length,
      runtimeDependencyCount: 0,
      transferCount: moduleEntries.length,
      ready: true,
    },
    blockers: [],
    ready: true,
    artificeReady: Boolean(options.artificeReady),
  };
}

function readJsonFile<T>(filePath: string): T {
  return JSON.parse(fs.readFileSync(filePath, "utf8")) as T;
}

function normalizeSmokeSummary(summary: Awaited<ReturnType<typeof runCEmuSmokeSuite>>) {
  return {
    status: summary.status,
    blockers: [...summary.blockers],
    environment: summary.environment,
    results: summary.results.map((result) => ({
      name: result.name,
      status: result.status,
      passed: result.passed,
      error: result.error,
      checkpoints: result.checkpoints.map((checkpoint) => ({
        name: checkpoint.name,
        description: checkpoint.description,
        passed: checkpoint.passed,
        expectedCrcs: [...checkpoint.expectedCrcs],
        observedCrc: checkpoint.observedCrc,
        observedOutputExcerpt: checkpoint.observedOutputExcerpt,
        screenshotMode: checkpoint.screenshotMode,
        autotesterExit: checkpoint.autotesterExit,
        snapshotPayload: readJsonFile<Record<string, unknown>>(checkpoint.stateSnapshotPath),
      })),
    })),
    passedCount: summary.passedCount,
    failedCount: summary.failedCount,
    skippedCount: summary.skippedCount,
  };
}

test("stageTransferPackage cleans temporary staging files after a partial copy failure", async () => {
  const environment = buildFakeEnvironment();
  const destinationDir = path.join(environment.repoRoot, "staged-transfer");
  const originalCopyFileSync = fs.copyFileSync.bind(fs);
  const copyFileSyncSpy = vi.spyOn(fs, "copyFileSync");

  copyFileSyncSpy.mockImplementation(((source: fs.PathLike, destination: fs.PathLike, mode?: number) => {
    if (path.basename(String(source)) === "CEBASE.8xp") {
      fs.writeFileSync(String(destination), "PARTIAL-STAGE", "utf8");
      throw new Error("synthetic partial write");
    }
    return originalCopyFileSync(source, destination, mode);
  }) as typeof fs.copyFileSync);

  try {
    expect(() => stageTransferPackage(environment.transferPackage, destinationDir)).toThrow(
      "Staged artifact 'CEBASE.8xp' could not be copied cleanly: synthetic partial write",
    );
  } finally {
    copyFileSyncSpy.mockRestore();
  }

  expect(fs.existsSync(path.join(destinationDir, ".CEBASE.8xp.calcengine-stage"))).toBe(false);
  expect(fs.existsSync(path.join(destinationDir, "CEBASE.8xp"))).toBe(false);
});

test("validateStagedTransferDirectory reports truncated staged modules cleanly", async () => {
  const environment = buildFakeEnvironment();
  const destinationDir = path.join(environment.repoRoot, "validated-transfer");

  stageTransferPackage(environment.transferPackage, destinationDir);
  const truncatedModulePath = path.join(destinationDir, "CEBASE.8xp");
  const originalPayload = fs.readFileSync(truncatedModulePath);
  fs.writeFileSync(truncatedModulePath, originalPayload.subarray(0, originalPayload.length - 17));

  const validation = validateStagedTransferDirectory(environment.transferPackage, destinationDir);

  expect(validation.success).toBe(false);
  expect(validation.missingEntries).toEqual([]);
  expect(validation.mismatchedEntries).toEqual(["CEBASE.8xp"]);
  expect(validation.unexpectedEntries).toEqual([]);
});

test("resolveCEmuEnvironment blocks hosted CI by default and re-enables the real lane only with opt-in", async () => {
  const environment = buildFakeEnvironment({ artificeReady: true });
  process.env.CI = "true";
  process.env.GITHUB_ACTIONS = "true";
  spawnSyncMock.mockReturnValue({
    status: 1,
    stdout: "",
    stderr: "",
  });

  const hostedCiResolution = resolveCEmuEnvironment({
    repoRoot: environment.repoRoot,
    manifestPath: environment.transferPackage.manifestPath,
    transferDir: environment.transferPackage.candidateRoots[0],
    romPath: environment.romPath ?? undefined,
    autotesterPath: environment.autotesterPath ?? undefined,
    headlessPath: environment.headlessPath ?? undefined,
    artificePath: environment.artificePath ?? undefined,
  });

  expect(hostedCiResolution.ready).toBe(false);
  expect(hostedCiResolution.artificeReady).toBe(false);
  expect(hostedCiResolution.blockers.some((blocker) => blocker.includes("disabled on hosted CI"))).toBe(true);

  process.env.CALCENGINE_ALLOW_REAL_CEMU_IN_CI = "true";
  const optedInResolution = resolveCEmuEnvironment({
    repoRoot: environment.repoRoot,
    manifestPath: environment.transferPackage.manifestPath,
    transferDir: environment.transferPackage.candidateRoots[0],
    romPath: environment.romPath ?? undefined,
    autotesterPath: environment.autotesterPath ?? undefined,
    headlessPath: environment.headlessPath ?? undefined,
    artificePath: environment.artificePath ?? undefined,
  });

  expect(optedInResolution.ready).toBe(true);
  expect(optedInResolution.artificeReady).toBe(true);
  expect(optedInResolution.blockers).toEqual([]);
});

function installArtificeSummary(summaryFactory: (summaryPath: string) => unknown) {
  spawnSyncMock.mockImplementation((_command: string, args: string[]) => {
    const outputFlagIndex = args.indexOf("-OutputPath");
    if (outputFlagIndex < 0 || !args[outputFlagIndex + 1]) {
      throw new Error("Expected -OutputPath when invoking the arTIfiCE runner.");
    }

    const summaryPath = args[outputFlagIndex + 1];
    fs.mkdirSync(path.dirname(summaryPath), { recursive: true });
    fs.writeFileSync(summaryPath, `${JSON.stringify(summaryFactory(summaryPath), null, 2)}\n`, "utf8");
    return {
      status: 0,
      stdout: "",
      stderr: "",
    };
  });
}

test("runCEmuSmokeSuite skips the arTIfiCE lane when the asset is unavailable", async () => {
  const environment = buildFakeEnvironment();
  const outputRoot = path.join(environment.repoRoot, "outputs");

  const summary = await runCEmuSmokeSuite({
    environment,
    outputRoot,
  });

  expect(summary.status).toBe("skipped");
  expect(summary.failedCount).toBe(0);
  expect(summary.skippedCount).toBe(1);
  expect(summary.results[0]).toMatchObject({
    name: "artifice_os58_launch_path",
    status: "skipped",
    error: "arTIfiCE asset is missing; OS 5.8 jailbreak path was skipped.",
  });
  const directChecklist = summary.results.find((result) => result.name === "direct_real_rom_smoke_checklist");
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
  expect(moduleHotSwap?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "module_hot_swap_prompt_restored",
    "module_hot_swap_recovery_module",
    "module_hot_swap_original_module_restored",
    "module_hot_swap_archive_count_stable",
  ]);
  const launcherMenuStress = summary.results.find((result) => result.name === "launcher_menu_navigation_stress");
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
  expect(crossModuleFunctionCalls?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "cross_module_stats_function_dispatch",
    "cross_module_algebra_follow_up",
  ]);
  const memoryPressure = summary.results.find((result) => result.name === "archive_swap_under_memory_pressure");
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
  expect(errorRecovery?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "error_recovery_error_screen",
    "error_recovery_prompt_restored",
    "error_recovery_successful_calculation",
    "error_recovery_prompt_after_success",
  ]);
  expect(summary.results.slice(1).every((result) => result.status === "passed")).toBe(true);
  expect(summary.results.slice(1).every((result) => result.checkpoints.length > 0)).toBe(true);
  for (const result of summary.results.slice(1)) {
    for (const checkpoint of result.checkpoints) {
      expect(checkpoint.screenshotMode).toBe("crc");
      expect(fs.existsSync(checkpoint.stateSnapshotPath)).toBe(true);
    }
  }
});

test("runCEmuSmokeSuite returns a skipped summary when the CEmu binary is unavailable", async () => {
  const environment = buildFakeEnvironment();
  const outputRoot = path.join(environment.repoRoot, "missing-headless-output");

  const summary = await runCEmuSmokeSuite({
    environment: {
      ...environment,
      headlessPath: null,
      ready: false,
      artificeReady: false,
      blockers: ["cemu-headless executable is unavailable"],
    },
    outputRoot,
  });

  expect(summary.status).toBe("skipped");
  expect(summary.results).toEqual([]);
  expect(summary.failedCount).toBe(0);
  expect(summary.passedCount).toBe(0);
  expect(summary.skippedCount).toBe(1);
  expect(summary.environment.headlessPath).toBeNull();
  expect(summary.blockers).toContain("cemu-headless executable is unavailable");
});

test("runCEmuSmokeSuite validates and records the arTIfiCE lane when the jailbreak asset is available", async () => {
  installArtificeSummary(() => ({
    comparison: {
      results: [
        {
          step: "artifice_shell_loaded",
          description: "arTIfiCE shell renders after the launch handoff.",
          status: "PASS",
          expected_hash: "ABCDEF01",
          actual_hash: "ABCDEF01",
        },
        {
          step: "calcengine_launch_menu",
          description: "CalcEngine launcher menu appears from arTIfiCE.",
          status: "PASS",
          expected_hash: "1234ABCD",
          actual_hash: "1234ABCD",
        },
      ],
    },
  }));

  const environment = buildFakeEnvironment({ artificeReady: true });
  const outputRoot = path.join(environment.repoRoot, "artifice-output");
  const summary = await runCEmuSmokeSuite({
    environment,
    outputRoot,
  });

  expect(summary.status).toBe("passed");
  expect(summary.failedCount).toBe(0);
  expect(spawnSyncMock).toHaveBeenCalledTimes(1);
  const artificeResult = summary.results.find((result) => result.name === "artifice_os58_launch_path");
  expect(artificeResult?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "artifice_shell_loaded",
    "calcengine_launch_menu",
  ]);
  expect(artificeResult?.checkpoints.every((checkpoint) => checkpoint.passed)).toBe(true);
});

test("runCEmuSmokeSuite fails cleanly when the arTIfiCE runner summary is malformed", async () => {
  installArtificeSummary(() => ({
    comparison: {
      results: [
        {
          step: "artifice_shell_loaded",
          description: "arTIfiCE shell renders after the launch handoff.",
          status: "PASS",
          expected_hash: "ABCDEF01",
          actual_hash: "",
        },
      ],
    },
  }));

  const environment = buildFakeEnvironment({ artificeReady: true });
  const outputRoot = path.join(environment.repoRoot, "artifice-malformed-output");
  const summary = await runCEmuSmokeSuite({
    environment,
    outputRoot,
  });

  expect(summary.status).toBe("failed");
  expect(summary.failedCount).toBe(1);
  expect(summary.results[0]).toMatchObject({
    name: "artifice_os58_launch_path",
    status: "failed",
  });
  expect(summary.results[0]?.error).toContain("missing actual_hash");
  expect(summary.blockers.some((blocker) => (
    blocker.includes("artifice_os58_launch_path: arTIfiCE summary 'artifice-summary.json' is missing actual_hash")
  ))).toBe(true);
  expect(summary.results.slice(1).every((result) => result.status === "passed")).toBe(true);
});

test("runCEmuSmokeSuite recovers cleanly on the next boot after a synthetic arTIfiCE power-cycle reset", async () => {
  let invocationCount = 0;
  spawnSyncMock.mockImplementation((_command: string, args: string[]) => {
    invocationCount += 1;
    const outputFlagIndex = args.indexOf("-OutputPath");
    if (outputFlagIndex < 0 || !args[outputFlagIndex + 1]) {
      throw new Error("Expected -OutputPath when invoking the arTIfiCE runner.");
    }
    const summaryPath = args[outputFlagIndex + 1];
    if (invocationCount === 1) {
      return {
        status: 1,
        stdout: "",
        stderr: "mock power cycle during shell_exit",
      };
    }

    fs.mkdirSync(path.dirname(summaryPath), { recursive: true });
    fs.writeFileSync(summaryPath, `${JSON.stringify({
      comparison: {
        results: [
          {
            step: "artifice_shell_loaded",
            description: "arTIfiCE shell renders after the launch handoff.",
            status: "PASS",
            expected_hash: "ABCDEF01",
            actual_hash: "ABCDEF01",
          },
          {
            step: "calcengine_launch_menu",
            description: "CalcEngine launcher menu appears from arTIfiCE.",
            status: "PASS",
            expected_hash: "1234ABCD",
            actual_hash: "1234ABCD",
          },
        ],
      },
    }, null, 2)}\n`, "utf8");
    return {
      status: 0,
      stdout: "",
      stderr: "",
    };
  });

  const environment = buildFakeEnvironment({ artificeReady: true });
  const firstSummary = await runCEmuSmokeSuite({
    environment,
    outputRoot: path.join(environment.repoRoot, "power-cycle-first"),
  });
  const secondSummary = await runCEmuSmokeSuite({
    environment,
    outputRoot: path.join(environment.repoRoot, "power-cycle-second"),
  });

  expect(firstSummary.status).toBe("failed");
  expect(firstSummary.results[0]).toMatchObject({
    name: "artifice_os58_launch_path",
    status: "failed",
  });
  expect(firstSummary.results[0]?.error).toContain("mock power cycle during shell_exit");
  expect(firstSummary.results.slice(1).every((result) => result.status === "passed")).toBe(true);

  expect(secondSummary.status).toBe("passed");
  expect(secondSummary.failedCount).toBe(0);
  expect(secondSummary.results[0]?.name).toBe("artifice_os58_launch_path");
  expect(secondSummary.results[0]?.status).toBe("passed");
  expect(secondSummary.results.every((result) => result.status === "passed")).toBe(true);
});

test("CEmu timeout helpers scale smoke-session and arTIfiCE runner timeouts for slower CI lanes", async () => {
  process.env.CEMU_TIMEOUT_MULTIPLIER = "1.75";
  installArtificeSummary(() => ({
    comparison: {
      results: [
        {
          step: "artifice_shell_loaded",
          description: "arTIfiCE shell renders after the launch handoff.",
          status: "PASS",
          expected_hash: "ABCDEF01",
          actual_hash: "ABCDEF01",
        },
      ],
    },
  }));

  const environment = buildFakeEnvironment({ artificeReady: true });
  await runCEmuSmokeSuite({
    environment,
    outputRoot: path.join(environment.repoRoot, "scaled-timeout-output"),
  });

  expect(getCEmuTimeoutMultiplier()).toBe(1.75);
  expect(scaleCEmuTimeoutMs(60_000)).toBe(105_000);
  expect(sessionOptionsLog[0]).toMatchObject({
    startupTimeoutMs: scaleCEmuTimeoutMs(20_000),
    commandTimeoutMs: scaleCEmuTimeoutMs(60_000),
    shutdownTimeoutMs: scaleCEmuTimeoutMs(15_000),
  });
  expect(spawnSyncMock.mock.calls[0]?.[2]).toMatchObject({
    timeout: scaleCEmuTimeoutMs(180_000),
  });
});

test("runCEmuSmokeSuite records stable archive counts during rapid multi-module switching without stale artifacts", async () => {
  const environment = buildFakeEnvironment();
  const outputRoot = path.join(environment.repoRoot, "rapid-switch-output");

  const summary = await runCEmuSmokeSuite({
    environment,
    outputRoot,
  });

  const rapidSwitching = summary.results.find((result) => result.name === "rapid_sequential_module_switching");
  expect(rapidSwitching?.status).toBe("passed");
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

  const archiveStabilityCheckpoint = rapidSwitching?.checkpoints.find((checkpoint) => (
    checkpoint.name === "rapid_switch_archive_count_stable"
  ));
  expect(archiveStabilityCheckpoint).toMatchObject({
    passed: true,
    expectedCrcs: ["A11CF0A1"],
    observedCrc: "A11CF0A1",
    observedOutputExcerpt: `Observed archive counts remained stable at ${environment.transferPackage.transferCount} across 10 multi-module checks.`,
  });

  const stabilityPayload = readJsonFile<{
    name: string;
    description: string;
    observedOutputExcerpt: string;
    observedCrc: string;
  }>(archiveStabilityCheckpoint!.stateSnapshotPath);

  expect(stabilityPayload).toEqual({
    name: "rapid_switch_archive_count_stable",
    description: `Archive app_count remains fixed at ${environment.transferPackage.transferCount} throughout the no-cooldown rapid module launch sequence.`,
    observedOutputExcerpt: `Observed archive counts remained stable at ${environment.transferPackage.transferCount} across 10 multi-module checks.`,
    observedCrc: "A11CF0A1",
  });
});

test("runCEmuSmokeSuite produces deterministic output and state artifacts across repeated identical runs", async () => {
  const environment = buildFakeEnvironment();

  const firstSummary = await runCEmuSmokeSuite({
    environment,
    outputRoot: path.join(environment.repoRoot, "deterministic-run-one"),
  });
  const secondSummary = await runCEmuSmokeSuite({
    environment,
    outputRoot: path.join(environment.repoRoot, "deterministic-run-two"),
  });

  expect(normalizeSmokeSummary(firstSummary)).toEqual(normalizeSmokeSummary(secondSummary));
});

test("runCEmuSmokeSuite exercises the synthetic corrupted-module recovery lane when requested", async () => {
  const environment = buildFakeEnvironment();
  const outputRoot = path.join(environment.repoRoot, "synthetic-recovery-output");

  const summary = await runCEmuSmokeSuite({
    environment,
    outputRoot,
    includeSyntheticRecoveryCase: true,
  });

  expect(summary.status).toBe("skipped");
  expect(summary.failedCount).toBe(0);
  expect(summary.skippedCount).toBe(1);
  const syntheticRecovery = summary.results.find((result) => result.name === "synthetic_corrupted_module_recovery");
  expect(syntheticRecovery?.status).toBe("passed");
  expect(syntheticRecovery?.checkpoints.map((checkpoint) => checkpoint.name)).toEqual([
    "corrupted_module_load_rejected",
    "corrupted_module_recovery_restored",
  ]);
  expect(syntheticRecovery?.checkpoints.every((checkpoint) => fs.existsSync(checkpoint.stateSnapshotPath))).toBe(true);
});
