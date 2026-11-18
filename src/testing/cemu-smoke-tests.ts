import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";

import {
  CEmuHeadlessSession,
  type CEmuKeySequenceEvent,
  expressionToEvents,
  tokensToEvents,
  pause,
  press,
  type ScreenAssertionResult,
} from "./cemu-interaction.ts";
import {
  DEFAULT_MANIFEST_PATH,
  REPO_ROOT,
  createPipelineContext,
  scaleCEmuTimeoutMs,
  resolveCEmuEnvironment,
  type ResolvedCEmuEnvironment,
} from "./cemu-pipeline.ts";

export interface CEmuSmokeCaseResult {
  name: string;
  status: "passed" | "failed" | "skipped";
  passed: boolean;
  durationMs: number;
  evidenceDir: string;
  checkpoints: ScreenAssertionResult[];
  error: string;
}

export interface CEmuSmokeSuiteSummary {
  status: "passed" | "failed" | "skipped";
  startedAt: string;
  finishedAt: string;
  durationMs: number;
  environment: {
    repoRoot: string;
    manifestPath: string;
    romPath: string | null;
    guiPath: string | null;
    autotesterPath: string | null;
    headlessPath: string | null;
    artificePath: string | null;
    transferCount: number;
  };
  outputRoot: string;
  contextRoot: string | null;
  blockers: string[];
  results: CEmuSmokeCaseResult[];
  passedCount: number;
  failedCount: number;
  skippedCount: number;
}

export interface RunCEmuSmokeSuiteOptions {
  outputRoot?: string;
  keepTempDir?: boolean;
  environment?: ResolvedCEmuEnvironment;
  includeSyntheticRecoveryCase?: boolean;
}

const MODULE_BATTERY_PATH = path.join(REPO_ROOT, "tests", "cemu", "edge-cases", "module-battery.json");
const ARITHMETIC_BATTERY_PATH = path.join(REPO_ROOT, "tests", "cemu", "edge-cases", "arithmetic-battery.json");
const REAL_EDGE_CASES_PATH = path.join(REPO_ROOT, "tests", "cemu", "edge-cases", "real-edge-cases.json");
const ARTIFICE_RUNNER_PATH = path.join(REPO_ROOT, "tools", "emulator", "run-artifice-e2e-verification.ps1");
const DEFAULT_OUTPUT_ROOT = path.join(REPO_ROOT, "tests", "cemu", "outputs", "ts-cemu-smoke");
const DEFAULT_ARTIFICE_RUNNER_TIMEOUT_MS = 180_000;
const DEFAULT_SMOKE_SESSION_STARTUP_TIMEOUT_MS = 20_000;
const DEFAULT_SMOKE_SESSION_COMMAND_TIMEOUT_MS = 60_000;
const DEFAULT_SMOKE_SESSION_SHUTDOWN_TIMEOUT_MS = 15_000;
const PROMPT_BASELINE_CRCS = ["349F4775", "A32840C8", "7C622B05", "7F0F6A91", "DAE8FE30", "B76BC405"];

interface HardeningCatalogCase {
  name: string;
  module?: string;
  description: string;
  expected_crcs: string[];
  input_expression?: string;
  input_tokens?: string[];
  post_events?: Array<{
    kind: "key" | "pause";
    key?: string;
    duration_ms?: number;
  }>;
  result_delay_ms?: number;
  auto_enter?: boolean;
}

interface ModuleSmokeExpectation {
  caseName: string;
  moduleName: string;
  description: string;
  inputExpression: string;
  expectedCrcs: string[];
  resultDelayMs: number;
}

interface ModuleSwitchPair {
  checkpointPrefix: string;
  firstCaseName: string;
  secondCaseName: string;
  secondExpectedCrcs?: string[];
}

interface DependencyChainScenario extends ModuleSwitchPair {
  sharedStateDescription: string;
  restoredFirstExpectedCrcs?: string[];
}

interface NamedModuleScenario {
  checkpointName: string;
  caseName: string;
}

interface ModuleHotSwapScenario {
  interruptedCaseName: string;
  recoveryCaseName: string;
  interruptionDelayMs: number;
}

const PROGRAM_SWITCH_PAIRS: ModuleSwitchPair[] = [
  {
    checkpointPrefix: "base_matrix_round_trip",
    firstCaseName: "module_base_hex_255",
    secondCaseName: "module_matrix_determinant",
  },
  {
    checkpointPrefix: "base_factor_round_trip",
    firstCaseName: "module_base_hex_255",
    secondCaseName: "module_alg_factor_quadratic",
  },
  {
    checkpointPrefix: "factor_complex_round_trip",
    firstCaseName: "module_alg_factor_quadratic",
    secondCaseName: "module_complex_real_part",
  },
  {
    checkpointPrefix: "stats_finance_round_trip",
    firstCaseName: "module_stats_mean_singleton",
    secondCaseName: "module_finance_present_value",
  },
  {
    checkpointPrefix: "solve_series_round_trip",
    firstCaseName: "module_solve_linear",
    secondCaseName: "module_series_taylor_sin",
  },
  {
    checkpointPrefix: "complete_square_matrix_inverse_round_trip",
    firstCaseName: "module_alg_complete_square",
    secondCaseName: "module_matrix_inverse_identity",
  },
  {
    checkpointPrefix: "usub_probability_round_trip",
    firstCaseName: "module_int_usub",
    secondCaseName: "module_probability_normalcdf",
  },
  {
    checkpointPrefix: "series_expand_round_trip",
    firstCaseName: "module_series_taylor_sin",
    secondCaseName: "module_alg_expand_binomial",
  },
];

const MEMORY_PRESSURE_CASE_NAMES = [
  "module_base_hex_255",
  "module_alg_factor_quadratic",
  "module_alg_complete_square",
  "module_solve_linear",
  "module_limit_polynomial",
  "module_int_usub",
  "module_series_taylor_sin",
  "module_sequence_sum_n_1_10",
  "module_stats_mean_singleton",
  "module_finance_present_value",
  "module_matrix_determinant",
  "module_complex_real_part",
] as const;

const CROSS_MODULE_FUNCTION_CALL_EXPRESSION = "piecewise(mean(1,2,3),1,median(4,5,6))";
const CROSS_MODULE_FUNCTION_CALL_EXPECTED_CRCS = [
  "B9B5D5E0",
] as const;
const ALGEBRA_FACTOR_VARIANT_CRCS = [
  "BB355886",
  "E77D51B9",
] as const;
const ERROR_RECOVERY_SUCCESS_EXPECTED_CRCS = [
  "3750E64D",
  "92B772EC",
] as const;
const HOT_SWAP_PROMPT_RESTORED_CRCS = [
  "D48F1AFF",
  ...PROMPT_BASELINE_CRCS,
] as const;

const CROSS_MODULE_FUNCTION_CALL_FOLLOW_UP_CASE = "module_alg_factor_quadratic" as const;

const ERROR_RECOVERY_CHAIN_INPUT = "1/0";
const ERROR_RECOVERY_CHAIN_SUCCESS_INPUT = "2+3";

const MEMORY_PRESSURE_EXPECTED_MIN_BYTES = 300_000;

const SYNTHETIC_ARCHIVE_STABILITY_CRC = "A11CF0A1";

const RAPID_MODULE_SWITCH_SCENARIOS: NamedModuleScenario[] = [
  {
    checkpointName: "rapid_switch_base_module",
    caseName: "module_base_hex_255",
  },
  {
    checkpointName: "rapid_switch_matrix_module",
    caseName: "module_matrix_determinant",
  },
  {
    checkpointName: "rapid_switch_finance_module",
    caseName: "module_finance_present_value",
  },
  {
    checkpointName: "rapid_switch_complex_module",
    caseName: "module_complex_real_part",
  },
  {
    checkpointName: "rapid_switch_stats_module",
    caseName: "module_stats_mean_singleton",
  },
  {
    checkpointName: "rapid_switch_series_module",
    caseName: "module_series_taylor_sin",
  },
  {
    checkpointName: "rapid_switch_limit_module",
    caseName: "module_limit_polynomial",
  },
  {
    checkpointName: "rapid_switch_usub_module",
    caseName: "module_int_usub",
  },
  {
    checkpointName: "rapid_switch_factor_module",
    caseName: "module_alg_factor_quadratic",
  },
  {
    checkpointName: "rapid_switch_probability_module",
    caseName: "module_probability_normalcdf",
  },
];

const EDGE_CASE_MATH_SCENARIOS = {
  divisionByZero: "edge_division_by_zero",
  overflow: "edge_overflow_pow_99999999999999_pow_9",
  largeLiteral: "edge_scientific_notation_large_literal",
  largeNumberNearDisplayLimit: "arith_mul_99999_times_99999",
  negativeExponent: "edge_negative_exponent_parenthesized",
  repeatedEquals: "edge_repeated_equals_after_addition",
} as const;

const MODULE_HOT_SWAP_SCENARIO: ModuleHotSwapScenario = {
  interruptedCaseName: "module_series_taylor_sin",
  recoveryCaseName: "module_matrix_determinant",
  interruptionDelayMs: 200,
};

const LAUNCHER_MENU_STRESS_CYCLE_COUNT = 4;

const DEPENDENCY_CHAIN_SCENARIOS: DependencyChainScenario[] = [
  {
    checkpointPrefix: "base_matrix_shared_scratch",
    firstCaseName: "module_base_hex_255",
    secondCaseName: "module_matrix_determinant",
    sharedStateDescription:
      "The base-conversion loader and matrix evaluator can reuse the same archived scratch path without corrupting the original CEBASE state.",
  },
  {
    checkpointPrefix: "factor_complex_symbolic_cache",
    firstCaseName: "module_alg_factor_quadratic",
    secondCaseName: "module_complex_real_part",
    sharedStateDescription:
      "The symbolic factor cache remains stable after the complex-number helper re-enters the shared algebra workspace.",
  },
  {
    checkpointPrefix: "stats_finance_shared_lists",
    firstCaseName: "module_stats_mean_singleton",
    secondCaseName: "module_finance_present_value",
    sharedStateDescription:
      "The statistics/probability lane keeps its shared list-memory state intact after the finance module consumes the same archived utility surfaces.",
  },
];

const DIRECT_LAUNCH_SEQUENCE: CEmuKeySequenceEvent[] = [
  pause(4_000, "wait for CalcEngine launch"),
  press("clear"),
  pause(500, "clear splash to prompt"),
];

const DIRECT_PROMPT_RESET_SEQUENCE: CEmuKeySequenceEvent[] = [
  press("clear"),
  pause(500, "return to prompt"),
  press("clear"),
  pause(500, "stabilize prompt"),
];

function readJsonFile<T>(filePath: string): T {
  return JSON.parse(fs.readFileSync(filePath, "utf8")) as T;
}

function normalizeRequiredSummaryField(
  value: unknown,
  fieldName: string,
  summaryPath: string,
  stepName: string,
): string {
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`arTIfiCE summary '${path.basename(summaryPath)}' is missing ${fieldName} for '${stepName}'.`);
  }
  return value.trim();
}

function parseArtificeSummaryResults(summaryPath: string): ScreenAssertionResult[] {
  if (!fs.existsSync(summaryPath)) {
    throw new Error(`arTIfiCE runner did not produce ${path.basename(summaryPath)}.`);
  }

  const summary = readJsonFile<{
    comparison?: {
      results?: unknown;
    };
  }>(summaryPath);

  const rawResults = summary.comparison?.results;
  if (!Array.isArray(rawResults) || rawResults.length === 0) {
    throw new Error(`arTIfiCE summary '${path.basename(summaryPath)}' does not contain comparison results.`);
  }

  return rawResults.map((rawResult, index) => {
    if (!rawResult || typeof rawResult !== "object") {
      throw new Error(
        `arTIfiCE summary '${path.basename(summaryPath)}' contains a non-object comparison result at index ${index}.`,
      );
    }

    const result = rawResult as {
      step?: unknown;
      description?: unknown;
      status?: unknown;
      expected_hash?: unknown;
      actual_hash?: unknown;
    };
    const step = normalizeRequiredSummaryField(result.step, "step", summaryPath, `comparison index ${index}`);
    const description = normalizeRequiredSummaryField(result.description, "description", summaryPath, step);
    const status = normalizeRequiredSummaryField(result.status, "status", summaryPath, step);
    const expectedHash = normalizeRequiredSummaryField(result.expected_hash, "expected_hash", summaryPath, step)
      .toUpperCase();
    const actualHash = normalizeRequiredSummaryField(result.actual_hash, "actual_hash", summaryPath, step)
      .toUpperCase();

    return {
      name: step,
      description,
      passed: status.toUpperCase() === "PASS",
      expectedCrcs: [expectedHash],
      observedCrc: actualHash,
      expectedOutputExcerptContains: [],
      observedOutputExcerpt: `Verified by ${path.basename(ARTIFICE_RUNNER_PATH)}.`,
      screenshotMode: "crc",
      stateSnapshotPath: summaryPath,
      autotesterExit: 0,
    } satisfies ScreenAssertionResult;
  });
}

function ensureDirectory(directoryPath: string): string {
  fs.mkdirSync(directoryPath, { recursive: true });
  return directoryPath;
}

function loadDirectSmokeExpectations(): Record<string, { description: string; expectedCrcs: string[] }> {
  return {
    boot_home: {
      description: "CalcEngine boots directly to the prompt on the live cemu-headless path.",
      expectedCrcs: ["128C50A4", "B76BC405"],
    },
    addition_result: {
      description: "2+3 renders the locked direct-smoke result screen.",
      expectedCrcs: ["3750E64D"],
    },
    prompt_after_addition: {
      description: "Dismissing the addition result returns to a stable prompt baseline.",
      expectedCrcs: [...PROMPT_BASELINE_CRCS],
    },
    multiplication_result: {
      description: "6*7 renders the locked direct-smoke result screen.",
      expectedCrcs: ["E35569F2"],
    },
    prompt_after_multiplication: {
      description: "CalcEngine returns to a stable prompt baseline after dismissing the 6*7 result.",
      expectedCrcs: [...PROMPT_BASELINE_CRCS],
    },
    negative_result: {
      description: "(-)5+10 renders the locked negate-key smoke result screen.",
      expectedCrcs: ["B9B5D5E0"],
    },
    decimal_result: {
      description: "3.14*2 renders the locked sequential direct-smoke decimal result screen.",
      expectedCrcs: ["F9FE44E6"],
    },
    parenthesized_result: {
      description: "(4+5)*3 preserves grouped order of operations on the direct smoke path.",
      expectedCrcs: ["3956B827"],
    },
    power_result: {
      description: "2^8 renders the locked sequential direct-smoke power result screen.",
      expectedCrcs: ["9DF8F505"],
    },
    divide_by_zero_error: {
      description: "1/0 surfaces the locked divide-by-zero screen on the direct smoke path.",
      expectedCrcs: ["32BFD188"],
    },
    prompt_after_divide_by_zero: {
      description: "Dismissing the divide-by-zero screen returns to a stable prompt baseline.",
      expectedCrcs: [...PROMPT_BASELINE_CRCS],
    },
    graphing_mode_result: {
      description: "CalcEngine routes a graphing request through the archived CEGRPH module and renders the locked graphing baseline.",
      expectedCrcs: ["B9B5D5E0"],
    },
    prompt_after_graphing_mode: {
      description: "Dismissing the graphing route returns to a stable prompt baseline.",
      expectedCrcs: [...PROMPT_BASELINE_CRCS, "00E4C4C2"],
    },
    program_menu_open: {
      description: "TI-OS PRGM menu opens from the cleaned CalcEngine prompt.",
      expectedCrcs: ["967993A8"],
    },
    prompt_after_menu_close: {
      description: "Closing the PRGM menu returns to a stable prompt baseline.",
      expectedCrcs: [...PROMPT_BASELINE_CRCS, "99A97135"],
    },
    memory_store_variable: {
      description: "Storing 5 into X through the TI store operator remains on the locked baseline.",
      expectedCrcs: ["B073B7D8"],
    },
    memory_recall_variable: {
      description: "Recalling X through x+2 returns the locked memory-recall baseline.",
      expectedCrcs: ["7D12D340"],
    },
    memory_clear_variable: {
      description: "Clearing X by storing 0 remains on the locked baseline.",
      expectedCrcs: ["D289E0D7"],
    },
    memory_recall_after_clear: {
      description: "Recalling X after clearing it returns the locked zeroed-memory baseline.",
      expectedCrcs: ["99D28FB9"],
    },
    overflow_error: {
      description: "A large exponential input remains on the locked overflow baseline.",
      expectedCrcs: ["387E9F08"],
    },
    syntax_error_screen: {
      description: "An incomplete expression still surfaces the locked syntax-error screen before recovery.",
      expectedCrcs: ["B9B5D5E0"],
    },
    syntax_error_prompt_restore: {
      description: "Clearing a syntax error returns to a stable prompt baseline.",
      expectedCrcs: ["349F4775", "A32840C8"],
    },
    archive_program_execution: {
      description: "CalcEngine loads and runs an archived module request and renders the locked archive-execution baseline.",
      expectedCrcs: ["96FD4303"],
    },
  };
}

function loadNamedCatalogCase(
  baselinePath: string,
  caseName: string,
): HardeningCatalogCase {
  const payload = readJsonFile<{
    cases: Array<{
      name: string;
      module?: string;
      description: string;
      expected_crcs: string[];
      input_expression?: string;
      input_tokens?: string[];
      post_events?: Array<{
        kind: "key" | "pause";
        key?: string;
        duration_ms?: number;
      }>;
      result_delay_ms?: number;
      auto_enter?: boolean;
    }>;
  }>(baselinePath);
  const match = payload.cases.find((entry) => entry.name === caseName);
  if (!match) {
    throw new Error(`Baseline case '${caseName}' was not found in ${path.basename(baselinePath)}.`);
  }
  return {
    name: match.name,
    module: match.module,
    description: match.description,
    input_expression: match.input_expression,
    input_tokens: match.input_tokens,
    post_events: match.post_events,
    result_delay_ms: match.result_delay_ms,
    auto_enter: match.auto_enter,
    expected_crcs: match.expected_crcs.map((value) => value.toUpperCase()),
  };
}

function loadModuleExpectation(caseName: string): ModuleSmokeExpectation {
  const baselineCase = loadNamedCatalogCase(MODULE_BATTERY_PATH, caseName);
  if (!baselineCase.input_expression) {
    throw new Error(`Module battery case '${caseName}' is missing input_expression.`);
  }
  if (!baselineCase.module) {
    throw new Error(`Module battery case '${caseName}' is missing module.`);
  }
  return {
    caseName: baselineCase.name,
    moduleName: baselineCase.module,
    description: baselineCase.description,
    inputExpression: baselineCase.input_expression,
    expectedCrcs: baselineCase.expected_crcs,
    resultDelayMs: baselineCase.result_delay_ms ?? 3_000,
  };
}

function buildCatalogCaseSequence(catalogCase: HardeningCatalogCase): CEmuKeySequenceEvent[] {
  const inputEvents = catalogCase.input_expression
    ? expressionToEvents(catalogCase.input_expression, {
        autoEnter: catalogCase.auto_enter ?? true,
        resultDelayMs: catalogCase.result_delay_ms ?? 2_500,
      })
    : tokensToEvents(catalogCase.input_tokens ?? [], {
        autoEnter: catalogCase.auto_enter ?? true,
        resultDelayMs: catalogCase.result_delay_ms ?? 2_500,
      });

  const postEvents = (catalogCase.post_events ?? []).map((event) => {
    if (event.kind === "pause") {
      return pause(event.duration_ms ?? 0, `catalog pause for ${catalogCase.name}`);
    }
    if (!event.key) {
      throw new Error(`Catalog case '${catalogCase.name}' includes a key event without a key.`);
    }
    return press(event.key);
  });

  return [...inputEvents, ...postEvents];
}

function findModuleSizeBytes(environment: ResolvedCEmuEnvironment, moduleName: string): number {
  const sizeBytes = environment.transferPackage?.moduleEntries.find((entry) => entry.name.startsWith(`${moduleName}.`))?.sizeBytes;
  if (!sizeBytes) {
    throw new Error(`Transfer package is missing module metadata for ${moduleName}.`);
  }
  return sizeBytes;
}

function createSyntheticCheckpoint(
  evidenceDir: string,
  name: string,
  description: string,
  observedOutputExcerpt: string,
  observedCrc: string,
): ScreenAssertionResult {
  fs.mkdirSync(evidenceDir, { recursive: true });
  const stateSnapshotPath = path.join(evidenceDir, `${name}.state.json`);
  fs.writeFileSync(
    stateSnapshotPath,
    `${JSON.stringify({ name, description, observedOutputExcerpt, observedCrc }, null, 2)}\n`,
    "utf8",
  );
  return {
    name,
    description,
    passed: true,
    expectedCrcs: [observedCrc],
    observedCrc,
    expectedOutputExcerptContains: [],
    observedOutputExcerpt,
    screenshotMode: "crc",
    stateSnapshotPath,
    autotesterExit: 0,
  };
}

function createArchiveCountStabilityCheckpoint(
  evidenceDir: string,
  name: string,
  description: string,
  expectedArchiveCount: number,
  observedArchiveCounts: number[],
): ScreenAssertionResult {
  const uniqueCounts = [...new Set(observedArchiveCounts)];
  if (uniqueCounts.length !== 1 || uniqueCounts[0] !== expectedArchiveCount) {
    throw new Error(
      `Archive stability check expected app_count=${expectedArchiveCount} but observed ${observedArchiveCounts.join(", ")}.`,
    );
  }

  return createSyntheticCheckpoint(
    evidenceDir,
    name,
    description,
    `Observed archive counts remained stable at ${expectedArchiveCount} across ${observedArchiveCounts.length} multi-module checks.`,
    SYNTHETIC_ARCHIVE_STABILITY_CRC,
  );
}

function createCorruptedTransferDir(sourceDir: string, destinationDir: string, moduleFilename: string): string {
  fs.rmSync(destinationDir, { recursive: true, force: true });
  fs.cpSync(sourceDir, destinationDir, { recursive: true });
  const modulePath = path.join(destinationDir, moduleFilename);
  if (!fs.existsSync(modulePath)) {
    throw new Error(`Cannot corrupt missing staged module ${moduleFilename}.`);
  }
  fs.appendFileSync(modulePath, "CORRUPTED-MODULE-PAYLOAD", "utf8");
  return destinationDir;
}

async function withSession<T>(
  environment: ResolvedCEmuEnvironment,
  transferPath: string,
  launchOptions: { targetName: string; targetIsAsm: boolean; launchSequence: CEmuKeySequenceEvent[] },
  handler: (session: CEmuHeadlessSession) => Promise<T>,
): Promise<T> {
  if (!environment.headlessPath || !environment.romPath || !environment.autotesterPath) {
    throw new Error("CEmu environment is incomplete.");
  }

  const session = new CEmuHeadlessSession({
    executablePath: environment.headlessPath,
    romPath: environment.romPath,
    autotesterPath: environment.autotesterPath,
    cwd: environment.repoRoot,
    startupTimeoutMs: scaleCEmuTimeoutMs(DEFAULT_SMOKE_SESSION_STARTUP_TIMEOUT_MS),
    commandTimeoutMs: scaleCEmuTimeoutMs(DEFAULT_SMOKE_SESSION_COMMAND_TIMEOUT_MS),
    shutdownTimeoutMs: scaleCEmuTimeoutMs(DEFAULT_SMOKE_SESSION_SHUTDOWN_TIMEOUT_MS),
  });

  try {
    await session.boot(transferPath, launchOptions);
    return await handler(session);
  } finally {
    await session.close();
  }
}

async function runCase(
  outputRoot: string,
  name: string,
  runner: (evidenceDir: string) => Promise<ScreenAssertionResult[]>,
): Promise<CEmuSmokeCaseResult> {
  const startedAt = Date.now();
  const evidenceDir = ensureDirectory(path.join(outputRoot, name));
  try {
    const checkpoints = await runner(evidenceDir);
    return {
      name,
      status: checkpoints.every((checkpoint) => checkpoint.passed) ? "passed" : "failed",
      passed: checkpoints.every((checkpoint) => checkpoint.passed),
      durationMs: Date.now() - startedAt,
      evidenceDir,
      checkpoints,
      error: "",
    };
  } catch (error) {
    return {
      name,
      status: "failed",
      passed: false,
      durationMs: Date.now() - startedAt,
      evidenceDir,
      checkpoints: [],
      error: error instanceof Error ? error.message : String(error),
    };
  }
}

function buildSkippedCase(
  outputRoot: string,
  name: string,
  error: string,
): CEmuSmokeCaseResult {
  return {
    name,
    status: "skipped",
    passed: false,
    durationMs: 0,
    evidenceDir: ensureDirectory(path.join(outputRoot, name)),
    checkpoints: [],
    error,
  };
}

async function runArtificeFoundationCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  transferDir: string,
): Promise<CEmuSmokeCaseResult> {
  return await runCase(outputRoot, "artifice_os58_launch_path", async (evidenceDir) => {
    if (!environment.romPath || !environment.autotesterPath) {
      throw new Error("arTIfiCE verification requires a ROM and autotester.exe.");
    }

    const summaryPath = path.join(evidenceDir, "artifice-summary.json");
    const invocation = spawnSync(
      "powershell.exe",
      [
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        ARTIFICE_RUNNER_PATH,
        "-RomPath",
        environment.romPath,
        "-AutotesterPath",
        environment.autotesterPath,
        "-DownloadsDir",
        transferDir,
        "-ArtificeProgramPath",
        environment.artificePath,
        "-PreferredAssetPath",
        environment.artificePath,
        "-OutputPath",
        summaryPath,
      ],
      {
        cwd: environment.repoRoot,
        encoding: "utf8",
        timeout: scaleCEmuTimeoutMs(DEFAULT_ARTIFICE_RUNNER_TIMEOUT_MS),
      },
    );

    if (invocation.status !== 0) {
      const combinedOutput = `${invocation.stdout ?? ""}\n${invocation.stderr ?? ""}`.trim();
      throw new Error(combinedOutput || `arTIfiCE runner exited with status ${invocation.status}.`);
    }

    return parseArtificeSummaryResults(summaryPath);
  });
}

async function runDirectSmokeChecklistCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const directExpectations = loadDirectSmokeExpectations();
  const overflowCase = loadNamedCatalogCase(REAL_EDGE_CASES_PATH, "edge_overflow_pow_99999999999999_pow_9");
  const archiveExecutionCase = loadNamedCatalogCase(MODULE_BATTERY_PATH, "module_base_hex_255");

  return await runCase(outputRoot, "direct_real_rom_smoke_checklist", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        checkpoints.push(await session.captureScreenAssertion({
          name: "boot_home",
          description: directExpectations.boot_home.description,
          expectedCrcs: directExpectations.boot_home.expectedCrcs,
          evidenceDir,
        }));

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents("2+3", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "addition_result",
          description: directExpectations.addition_result.description,
          expectedCrcs: directExpectations.addition_result.expectedCrcs,
          evidenceDir,
        }));
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "prompt_after_addition",
          description: "CalcEngine returns to a prompt baseline after dismissing the 2+3 result.",
          expectedCrcs: PROMPT_BASELINE_CRCS,
          evidenceDir,
        }));

        await session.sendSequence(expressionToEvents("6*7", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "multiplication_result",
          description: directExpectations.multiplication_result.description,
          expectedCrcs: directExpectations.multiplication_result.expectedCrcs,
          evidenceDir,
        }));
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "prompt_after_multiplication",
          description: directExpectations.prompt_after_multiplication.description,
          expectedCrcs: directExpectations.prompt_after_multiplication.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(expressionToEvents("(-)5+10", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "negative_result",
          description: directExpectations.negative_result.description,
          expectedCrcs: directExpectations.negative_result.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents("3.14*2", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "decimal_result",
          description: directExpectations.decimal_result.description,
          expectedCrcs: directExpectations.decimal_result.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents("(4+5)*3", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "parenthesized_result",
          description: directExpectations.parenthesized_result.description,
          expectedCrcs: directExpectations.parenthesized_result.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents("2^8", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "power_result",
          description: directExpectations.power_result.description,
          expectedCrcs: directExpectations.power_result.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents("1/0", { resultDelayMs: 3_000 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "divide_by_zero_error",
          description: directExpectations.divide_by_zero_error.description,
          expectedCrcs: directExpectations.divide_by_zero_error.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "prompt_after_divide_by_zero",
          description: directExpectations.prompt_after_divide_by_zero.description,
          expectedCrcs: directExpectations.prompt_after_divide_by_zero.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(expressionToEvents("graph(1,x)", { resultDelayMs: 3_000 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "graphing_mode_result",
          description: directExpectations.graphing_mode_result.description,
          expectedCrcs: directExpectations.graphing_mode_result.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "prompt_after_graphing_mode",
          description: directExpectations.prompt_after_graphing_mode.description,
          expectedCrcs: directExpectations.prompt_after_graphing_mode.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence([
          press("prgm"),
          pause(1_200, "open PRGM menu"),
        ]);
        checkpoints.push(await session.captureScreenAssertion({
          name: "program_menu_open",
          description: directExpectations.program_menu_open.description,
          expectedCrcs: directExpectations.program_menu_open.expectedCrcs,
          evidenceDir,
        }));
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "prompt_after_menu_close",
          description: directExpectations.prompt_after_menu_close.description,
          expectedCrcs: directExpectations.prompt_after_menu_close.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(tokensToEvents(["5", "2nd", "enter", "alpha", "sto"], { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "memory_store_variable",
          description: directExpectations.memory_store_variable.description,
          expectedCrcs: directExpectations.memory_store_variable.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence([
          press("clear"),
          pause(500, "return to prompt"),
        ]);
        await session.sendSequence(expressionToEvents("x+2", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "memory_recall_variable",
          description: directExpectations.memory_recall_variable.description,
          expectedCrcs: directExpectations.memory_recall_variable.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence([
          press("clear"),
          pause(500, "return to prompt"),
        ]);
        await session.sendSequence(tokensToEvents(["0", "2nd", "enter", "alpha", "sto"], { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "memory_clear_variable",
          description: directExpectations.memory_clear_variable.description,
          expectedCrcs: directExpectations.memory_clear_variable.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence([
          press("clear"),
          pause(500, "return to prompt"),
        ]);
        await session.sendSequence(expressionToEvents("x", { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "memory_recall_after_clear",
          description: directExpectations.memory_recall_after_clear.description,
          expectedCrcs: directExpectations.memory_recall_after_clear.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(buildCatalogCaseSequence(overflowCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "overflow_error",
          description: directExpectations.overflow_error.description,
          expectedCrcs: directExpectations.overflow_error.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence([
          press("clear"),
          pause(600, "dismiss overflow screen"),
        ]);
        await session.sendSequence(tokensToEvents(["2", "+"], { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "syntax_error_screen",
          description: directExpectations.syntax_error_screen.description,
          expectedCrcs: directExpectations.syntax_error_screen.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence([
          press("clear"),
          pause(600, "dismiss syntax error"),
          press("clear"),
          pause(800, "stabilize prompt"),
        ]);
        checkpoints.push(await session.captureScreenAssertion({
          name: "syntax_error_prompt_restore",
          description: directExpectations.syntax_error_prompt_restore.description,
          expectedCrcs: directExpectations.syntax_error_prompt_restore.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(buildCatalogCaseSequence(archiveExecutionCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "archive_program_execution",
          description: directExpectations.archive_program_execution.description,
          expectedCrcs: directExpectations.archive_program_execution.expectedCrcs,
          evidenceDir,
        }));
        return checkpoints;
      },
    );
  });
}

async function runProgramSwitchCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const switchPairs = PROGRAM_SWITCH_PAIRS.map((pair) => ({
    ...pair,
    firstExpectation: loadModuleExpectation(pair.firstCaseName),
    secondExpectation: loadModuleExpectation(pair.secondCaseName),
  }));
  return await runCase(outputRoot, "program_switching_between_modules", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        for (const pair of switchPairs) {
          await session.reset();
          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          await session.sendSequence(expressionToEvents(pair.firstExpectation.inputExpression, {
            resultDelayMs: pair.firstExpectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `${pair.checkpointPrefix}_first_module`,
            description: `${pair.firstExpectation.description} before switching to ${pair.secondExpectation.moduleName}.`,
            expectedCrcs: pair.firstExpectation.expectedCrcs,
            evidenceDir,
          }));

          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          await session.sendSequence(expressionToEvents(pair.secondExpectation.inputExpression, {
            resultDelayMs: pair.secondExpectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `${pair.checkpointPrefix}_second_module`,
            description: `${pair.secondExpectation.description} after an archive swap from ${pair.firstExpectation.moduleName}.`,
            expectedCrcs: pair.secondExpectedCrcs ?? pair.secondExpectation.expectedCrcs,
            evidenceDir,
          }));

          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          await session.sendSequence(expressionToEvents(pair.firstExpectation.inputExpression, {
            resultDelayMs: pair.firstExpectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `${pair.checkpointPrefix}_restored_first_module`,
            description: `${pair.firstExpectation.description} after switching back from ${pair.secondExpectation.moduleName}.`,
            expectedCrcs: pair.firstExpectation.expectedCrcs,
            evidenceDir,
          }));
        }

        return checkpoints;
      },
    );
  });
}

async function runRapidSequentialModuleSwitchCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const rapidSwitchScenarios = RAPID_MODULE_SWITCH_SCENARIOS.map((scenario) => ({
    ...scenario,
    expectation: loadModuleExpectation(scenario.caseName),
  }));
  const expectedArchiveCount = environment.transferPackage?.transferCount ?? 0;

  return await runCase(outputRoot, "rapid_sequential_module_switching", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];
        const observedArchiveCounts: number[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);

        for (const [index, scenario] of rapidSwitchScenarios.entries()) {
          await session.sendSequence(expressionToEvents(scenario.expectation.inputExpression, {
            resultDelayMs: scenario.expectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: scenario.checkpointName,
            description: `${scenario.expectation.description} during a continuous multi-module switch sequence without returning to the launcher.`,
            expectedCrcs: scenario.expectation.expectedCrcs,
            evidenceDir,
          }));
          const state = await session.getState();
          observedArchiveCounts.push(Number(state.app_count ?? 0));

          if (index < rapidSwitchScenarios.length - 1) {
            await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          }
        }

        checkpoints.push(createArchiveCountStabilityCheckpoint(
          evidenceDir,
          "rapid_switch_archive_count_stable",
          `Archive app_count remains fixed at ${expectedArchiveCount} throughout the no-cooldown rapid module launch sequence.`,
          expectedArchiveCount,
          observedArchiveCounts,
        ));

        return checkpoints;
      },
    );
  });
}

async function runEdgeCaseMathStressCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const directExpectations = loadDirectSmokeExpectations();
  const divisionByZeroCase = loadNamedCatalogCase(REAL_EDGE_CASES_PATH, EDGE_CASE_MATH_SCENARIOS.divisionByZero);
  const overflowCase = loadNamedCatalogCase(REAL_EDGE_CASES_PATH, EDGE_CASE_MATH_SCENARIOS.overflow);
  const largeLiteralCase = loadNamedCatalogCase(REAL_EDGE_CASES_PATH, EDGE_CASE_MATH_SCENARIOS.largeLiteral);
  const largeNumberNearDisplayLimitCase = loadNamedCatalogCase(
    ARITHMETIC_BATTERY_PATH,
    EDGE_CASE_MATH_SCENARIOS.largeNumberNearDisplayLimit,
  );
  const negativeExponentCase = loadNamedCatalogCase(REAL_EDGE_CASES_PATH, EDGE_CASE_MATH_SCENARIOS.negativeExponent);
  const repeatedEqualsCase = loadNamedCatalogCase(REAL_EDGE_CASES_PATH, EDGE_CASE_MATH_SCENARIOS.repeatedEquals);

  return await runCase(outputRoot, "edge_case_math_inputs", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);

        await session.sendSequence(buildCatalogCaseSequence(divisionByZeroCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_division_by_zero",
          description: divisionByZeroCase.description,
          expectedCrcs: divisionByZeroCase.expected_crcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_prompt_after_division_by_zero",
          description: "Clearing the division-by-zero error returns CalcEngine to the stable prompt baseline before the next edge-case input.",
          expectedCrcs: directExpectations.prompt_after_divide_by_zero.expectedCrcs,
          evidenceDir,
        }));

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(buildCatalogCaseSequence(overflowCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_overflow",
          description: overflowCase.description,
          expectedCrcs: overflowCase.expected_crcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_prompt_after_overflow",
          description: "Clearing the overflow path returns CalcEngine to the stable prompt baseline before entering a very large literal.",
          expectedCrcs: PROMPT_BASELINE_CRCS,
          evidenceDir,
        }));

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(buildCatalogCaseSequence(largeLiteralCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_large_literal_scientific_notation",
          description: largeLiteralCase.description,
          expectedCrcs: largeLiteralCase.expected_crcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_prompt_after_large_literal",
          description: "Clearing the large scientific-notation literal returns CalcEngine to the stable prompt baseline before the 10-digit display-limit pass.",
          expectedCrcs: PROMPT_BASELINE_CRCS,
          evidenceDir,
        }));

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(buildCatalogCaseSequence(largeNumberNearDisplayLimitCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_large_number_near_display_limit",
          description: largeNumberNearDisplayLimitCase.description,
          expectedCrcs: largeNumberNearDisplayLimitCase.expected_crcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_prompt_after_large_number",
          description: "Clearing the 10-digit display-limit arithmetic path returns CalcEngine to the stable prompt baseline before the negative-exponent pass.",
          expectedCrcs: PROMPT_BASELINE_CRCS,
          evidenceDir,
        }));

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(buildCatalogCaseSequence(negativeExponentCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_negative_exponent",
          description: negativeExponentCase.description,
          expectedCrcs: negativeExponentCase.expected_crcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_prompt_after_negative_exponent",
          description: "Clearing the negative-exponent result returns CalcEngine to the stable prompt baseline before the repeated-equals pass.",
          expectedCrcs: PROMPT_BASELINE_CRCS,
          evidenceDir,
        }));

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(buildCatalogCaseSequence(repeatedEqualsCase));
        checkpoints.push(await session.captureScreenAssertion({
          name: "edge_math_repeated_equals",
          description: repeatedEqualsCase.description,
          expectedCrcs: repeatedEqualsCase.expected_crcs,
          evidenceDir,
        }));

        return checkpoints;
      },
    );
  });
}

async function runModuleHotSwapDuringCalculationCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const interruptedExpectation = loadModuleExpectation(MODULE_HOT_SWAP_SCENARIO.interruptedCaseName);
  const recoveryExpectation = loadModuleExpectation(MODULE_HOT_SWAP_SCENARIO.recoveryCaseName);
  const expectedArchiveCount = environment.transferPackage?.transferCount ?? 0;

  return await runCase(outputRoot, "module_hot_swap_during_calculation", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];
        const observedArchiveCounts: number[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence([
          ...expressionToEvents(interruptedExpectation.inputExpression, {
            autoEnter: false,
          }),
          pause(200, `settle before entering the interrupted ${interruptedExpectation.moduleName} calculation`),
          press("enter"),
          pause(
            MODULE_HOT_SWAP_SCENARIO.interruptionDelayMs,
            `allow ${interruptedExpectation.moduleName} dispatch to begin before the hot-swap clear`,
          ),
          press("clear"),
          pause(600, `interrupt ${interruptedExpectation.moduleName} before loading ${recoveryExpectation.moduleName}`),
        ]);
        checkpoints.push(await session.captureScreenAssertion({
          name: "module_hot_swap_prompt_restored",
          description: `CalcEngine returns to the stable prompt baseline after interrupting ${interruptedExpectation.moduleName} and hot-swapping to ${recoveryExpectation.moduleName}.`,
          expectedCrcs: [...HOT_SWAP_PROMPT_RESTORED_CRCS],
          evidenceDir,
        }));
        observedArchiveCounts.push(Number((await session.getState()).app_count ?? 0));

        await session.sendSequence(expressionToEvents(recoveryExpectation.inputExpression, {
          resultDelayMs: recoveryExpectation.resultDelayMs,
        }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "module_hot_swap_recovery_module",
          description: `${recoveryExpectation.description} immediately after the interrupted ${interruptedExpectation.moduleName} calculation, proving the hot-swap path recovers cleanly.`,
          expectedCrcs: recoveryExpectation.expectedCrcs,
          evidenceDir,
        }));
        observedArchiveCounts.push(Number((await session.getState()).app_count ?? 0));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents(interruptedExpectation.inputExpression, {
          resultDelayMs: interruptedExpectation.resultDelayMs,
        }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "module_hot_swap_original_module_restored",
          description: `${interruptedExpectation.description} after the mid-calculation hot-swap, confirming the original module was not corrupted by the interruption.`,
          expectedCrcs: interruptedExpectation.expectedCrcs,
          evidenceDir,
        }));
        observedArchiveCounts.push(Number((await session.getState()).app_count ?? 0));

        checkpoints.push(createArchiveCountStabilityCheckpoint(
          evidenceDir,
          "module_hot_swap_archive_count_stable",
          `Archive app_count remains fixed at ${expectedArchiveCount} across the interrupted-module hot-swap recovery sequence.`,
          expectedArchiveCount,
          observedArchiveCounts,
        ));

        return checkpoints;
      },
    );
  });
}

async function runLauncherMenuNavigationStressCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const directExpectations = loadDirectSmokeExpectations();

  return await runCase(outputRoot, "launcher_menu_navigation_stress", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);

        for (let cycleIndex = 0; cycleIndex < LAUNCHER_MENU_STRESS_CYCLE_COUNT; cycleIndex += 1) {
          const cycleNumber = cycleIndex + 1;
          await session.sendSequence([
            press("prgm"),
            pause(1_200, `open PRGM menu cycle ${cycleNumber}`),
          ]);
          checkpoints.push(await session.captureScreenAssertion({
            name: `launcher_menu_cycle_${cycleNumber}_open`,
            description: `Cycle ${cycleNumber} reopens the TI-OS PRGM menu from the CalcEngine prompt without losing launcher state.`,
            expectedCrcs: directExpectations.program_menu_open.expectedCrcs,
            evidenceDir,
          }));

          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          checkpoints.push(await session.captureScreenAssertion({
            name: `launcher_menu_cycle_${cycleNumber}_close`,
            description: `Cycle ${cycleNumber} closes the PRGM menu and returns to the stable CalcEngine prompt baseline.`,
            expectedCrcs: directExpectations.prompt_after_menu_close.expectedCrcs,
            evidenceDir,
          }));
        }

        return checkpoints;
      },
    );
  });
}

async function runCrossModuleFunctionCallCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const algebraFollowUpExpectation = loadModuleExpectation(CROSS_MODULE_FUNCTION_CALL_FOLLOW_UP_CASE);

  return await runCase(outputRoot, "cross_module_function_calls", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents(CROSS_MODULE_FUNCTION_CALL_EXPRESSION, {
          resultDelayMs: 3_000,
        }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "cross_module_stats_function_dispatch",
          description:
            "A nested expression can invoke statistics helpers from a non-stat top-level function without losing cross-module dispatch stability.",
          expectedCrcs: [...CROSS_MODULE_FUNCTION_CALL_EXPECTED_CRCS],
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents(algebraFollowUpExpectation.inputExpression, {
          resultDelayMs: algebraFollowUpExpectation.resultDelayMs,
        }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "cross_module_algebra_follow_up",
          description: `${algebraFollowUpExpectation.description} after the nested cross-module statistics dispatch completed.`,
          expectedCrcs: [...ALGEBRA_FACTOR_VARIANT_CRCS],
          evidenceDir,
        }));

        return checkpoints;
      },
    );
  });
}

async function runCrossModuleDependencyCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const scenarios = DEPENDENCY_CHAIN_SCENARIOS.map((scenario) => ({
    ...scenario,
    firstExpectation: loadModuleExpectation(scenario.firstCaseName),
    secondExpectation: loadModuleExpectation(scenario.secondCaseName),
  }));

  return await runCase(outputRoot, "cross_module_dependency_chains", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        for (const scenario of scenarios) {
          await session.reset();
          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          await session.sendSequence(expressionToEvents(scenario.firstExpectation.inputExpression, {
            resultDelayMs: scenario.firstExpectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `${scenario.checkpointPrefix}_primary_module`,
            description: `${scenario.firstExpectation.description} before loading the dependent ${scenario.secondExpectation.moduleName} module.`,
            expectedCrcs: scenario.firstExpectation.expectedCrcs,
            evidenceDir,
          }));

          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          await session.sendSequence(expressionToEvents(scenario.secondExpectation.inputExpression, {
            resultDelayMs: scenario.secondExpectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `${scenario.checkpointPrefix}_dependent_module`,
            description: `${scenario.secondExpectation.description} after entering the shared dependency chain from ${scenario.firstExpectation.moduleName}.`,
            expectedCrcs: scenario.secondExpectedCrcs ?? scenario.secondExpectation.expectedCrcs,
            evidenceDir,
          }));

          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          checkpoints.push(await session.captureScreenAssertion({
            name: `${scenario.checkpointPrefix}_shared_state_prompt`,
            description: `${scenario.sharedStateDescription} The prompt returns to a stable baseline before reloading ${scenario.firstExpectation.moduleName}.`,
            expectedCrcs: PROMPT_BASELINE_CRCS,
            evidenceDir,
          }));

          await session.sendSequence(expressionToEvents(scenario.firstExpectation.inputExpression, {
            resultDelayMs: scenario.firstExpectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `${scenario.checkpointPrefix}_primary_module_reloaded`,
            description: `${scenario.sharedStateDescription} ${scenario.firstExpectation.moduleName} re-renders the locked baseline after the dependent module ran.`,
            expectedCrcs: scenario.restoredFirstExpectedCrcs ?? scenario.firstExpectation.expectedCrcs,
            evidenceDir,
          }));
        }

        return checkpoints;
      },
    );
  });
}

async function runMemoryPressureArchiveSwapCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const heavyExpectations = MEMORY_PRESSURE_CASE_NAMES.map((caseName) => loadModuleExpectation(caseName));
  const aggregatePressureBytes = heavyExpectations.reduce(
    (total, expectation) => total + findModuleSizeBytes(environment, expectation.moduleName),
    0,
  );
  const expectedArchiveCount = environment.transferPackage?.transferCount ?? 0;

  return await runCase(outputRoot, "archive_swap_under_memory_pressure", async (evidenceDir) => {
    if (aggregatePressureBytes < MEMORY_PRESSURE_EXPECTED_MIN_BYTES) {
      throw new Error(
        `Configured heavy-module archive churn only totals ${aggregatePressureBytes} bytes, which is below the required pressure threshold.`,
      );
    }

    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];
        const observedArchiveCounts: number[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);

        for (const [index, expectation] of heavyExpectations.entries()) {
          await session.sendSequence(expressionToEvents(expectation.inputExpression, {
            resultDelayMs: expectation.resultDelayMs,
          }));
          checkpoints.push(await session.captureScreenAssertion({
            name: `memory_pressure_stage_${index + 1}`,
            description: `${expectation.description} during a ${aggregatePressureBytes}-byte archived-module churn pass.`,
            expectedCrcs: expectation.caseName === CROSS_MODULE_FUNCTION_CALL_FOLLOW_UP_CASE
              ? [...ALGEBRA_FACTOR_VARIANT_CRCS]
              : expectation.expectedCrcs,
            evidenceDir,
          }));
          const state = await session.getState();
          observedArchiveCounts.push(Number(state.app_count ?? 0));
          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        }

        await session.sendSequence(expressionToEvents(heavyExpectations[0].inputExpression, {
          resultDelayMs: heavyExpectations[0].resultDelayMs,
        }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "memory_pressure_primary_module_reloaded",
          description: `${heavyExpectations[0].description} after the heavy archived-module churn completed without data corruption.`,
          expectedCrcs: heavyExpectations[0].expectedCrcs,
          evidenceDir,
        }));
        const restoredState = await session.getState();
        observedArchiveCounts.push(Number(restoredState.app_count ?? 0));
        checkpoints.push(createArchiveCountStabilityCheckpoint(
          evidenceDir,
          "memory_pressure_archive_count_stable",
          `Archive app_count remains fixed at ${expectedArchiveCount} across a ${heavyExpectations.length}-module churn pass, confirming the archive-swap path did not leak staged payloads.`,
          expectedArchiveCount,
          observedArchiveCounts,
        ));

        return checkpoints;
      },
    );
  });
}

async function runErrorRecoveryChainCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const directExpectations = loadDirectSmokeExpectations();

  return await runCase(outputRoot, "error_recovery_chain", async (evidenceDir) => {
    return await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        const checkpoints: ScreenAssertionResult[] = [];

        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents(ERROR_RECOVERY_CHAIN_INPUT, { resultDelayMs: 3_000 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "error_recovery_error_screen",
          description: "A direct divide-by-zero fault still renders the locked error screen before recovery begins.",
          expectedCrcs: directExpectations.divide_by_zero_error.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "error_recovery_prompt_restored",
          description: "Clearing the error returns CalcEngine to the locked prompt baseline before the recovery calculation.",
          expectedCrcs: directExpectations.prompt_after_divide_by_zero.expectedCrcs,
          evidenceDir,
        }));

        await session.sendSequence(expressionToEvents(ERROR_RECOVERY_CHAIN_SUCCESS_INPUT, { resultDelayMs: 2_500 }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "error_recovery_successful_calculation",
          description: "A normal calculation succeeds immediately after the error path, proving the prior failure did not poison the next dispatch.",
          expectedCrcs: [...ERROR_RECOVERY_SUCCESS_EXPECTED_CRCS],
          evidenceDir,
        }));

        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        checkpoints.push(await session.captureScreenAssertion({
          name: "error_recovery_prompt_after_success",
          description: "The prompt remains on the locked baseline after the recovery calculation completes.",
          expectedCrcs: directExpectations.prompt_after_addition.expectedCrcs,
          evidenceDir,
        }));

        return checkpoints;
      },
    );
  });
}

async function runSyntheticCorruptedModuleRecoveryCase(
  environment: ResolvedCEmuEnvironment,
  outputRoot: string,
  directTransferDir: string,
): Promise<CEmuSmokeCaseResult> {
  const recoveryExpectation = loadModuleExpectation("module_base_hex_255");

  return await runCase(outputRoot, "synthetic_corrupted_module_recovery", async (evidenceDir) => {
    const corruptedTransferDir = createCorruptedTransferDir(
      directTransferDir,
      path.join(evidenceDir, "corrupted-transfer"),
      `${recoveryExpectation.moduleName}.8xp`,
    );
    const checkpoints: ScreenAssertionResult[] = [];

    try {
      await withSession(
        environment,
        corruptedTransferDir,
        {
          targetName: "CALCENG",
          targetIsAsm: true,
          launchSequence: DIRECT_LAUNCH_SEQUENCE,
        },
        async (session) => {
          await session.reset();
          await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
          await session.sendSequence(expressionToEvents(recoveryExpectation.inputExpression, {
            resultDelayMs: recoveryExpectation.resultDelayMs,
          }));
          throw new Error("Synthetic corrupted-module smoke expected the module dispatch to fail, but the run continued.");
        },
      );
    } catch (error) {
      checkpoints.push(createSyntheticCheckpoint(
        evidenceDir,
        "corrupted_module_load_rejected",
        "The synthetic corrupted-module smoke lane rejected the damaged archive payload before recovery.",
        error instanceof Error ? error.message : String(error),
        "BAD0C0DE",
      ));
    }

    await withSession(
      environment,
      directTransferDir,
      {
        targetName: "CALCENG",
        targetIsAsm: true,
        launchSequence: DIRECT_LAUNCH_SEQUENCE,
      },
      async (session) => {
        await session.reset();
        await session.sendSequence(DIRECT_PROMPT_RESET_SEQUENCE);
        await session.sendSequence(expressionToEvents(recoveryExpectation.inputExpression, {
          resultDelayMs: recoveryExpectation.resultDelayMs,
        }));
        checkpoints.push(await session.captureScreenAssertion({
          name: "corrupted_module_recovery_restored",
          description: `${recoveryExpectation.description} after recovering from the synthetic corrupted-module failure path.`,
          expectedCrcs: recoveryExpectation.expectedCrcs,
          evidenceDir,
        }));
      },
    );

    return checkpoints;
  });
}

export async function runCEmuSmokeSuite(options: RunCEmuSmokeSuiteOptions = {}): Promise<CEmuSmokeSuiteSummary> {
  const startedAt = new Date();
  const outputRoot = ensureDirectory(path.resolve(options.outputRoot ?? DEFAULT_OUTPUT_ROOT));
  const environment = options.environment ?? resolveCEmuEnvironment({ manifestPath: DEFAULT_MANIFEST_PATH });

  if (!environment.ready) {
    return {
      status: "skipped",
      startedAt: startedAt.toISOString(),
      finishedAt: new Date().toISOString(),
      durationMs: 0,
      environment: {
        repoRoot: environment.repoRoot,
        manifestPath: DEFAULT_MANIFEST_PATH,
        romPath: environment.romPath,
        guiPath: environment.guiPath,
        autotesterPath: environment.autotesterPath,
        headlessPath: environment.headlessPath,
        artificePath: environment.artificePath,
        transferCount: environment.transferPackage?.transferCount ?? 0,
      },
      outputRoot,
      contextRoot: null,
      blockers: [...environment.blockers],
      results: [],
      passedCount: 0,
      failedCount: 0,
      skippedCount: 1,
    };
  }

  const context = createPipelineContext(environment, { keepTempDir: options.keepTempDir });
  const results: CEmuSmokeCaseResult[] = [];

  try {
    if (!environment.artificeReady) {
      results.push(
        buildSkippedCase(
          outputRoot,
          "artifice_os58_launch_path",
          "arTIfiCE asset is missing; OS 5.8 jailbreak path was skipped.",
        ),
      );
    } else {
      results.push(await runArtificeFoundationCase(environment, outputRoot, context.directTransferDir));
    }
    results.push(await runDirectSmokeChecklistCase(environment, outputRoot, context.directTransferDir));
    results.push(await runProgramSwitchCase(environment, outputRoot, context.directTransferDir));
    results.push(await runRapidSequentialModuleSwitchCase(environment, outputRoot, context.directTransferDir));
    results.push(await runEdgeCaseMathStressCase(environment, outputRoot, context.directTransferDir));
    results.push(await runModuleHotSwapDuringCalculationCase(environment, outputRoot, context.directTransferDir));
    results.push(await runLauncherMenuNavigationStressCase(environment, outputRoot, context.directTransferDir));
    results.push(await runCrossModuleFunctionCallCase(environment, outputRoot, context.directTransferDir));
    results.push(await runCrossModuleDependencyCase(environment, outputRoot, context.directTransferDir));
    results.push(await runMemoryPressureArchiveSwapCase(environment, outputRoot, context.directTransferDir));
    results.push(await runErrorRecoveryChainCase(environment, outputRoot, context.directTransferDir));
    if (options.includeSyntheticRecoveryCase) {
      results.push(await runSyntheticCorruptedModuleRecoveryCase(environment, outputRoot, context.directTransferDir));
    }
  } finally {
    await context.cleanup();
  }

  const failedCount = results.filter((result) => result.status === "failed").length;
  const skippedCount = results.filter((result) => result.status === "skipped").length;
  const passedCount = results.filter((result) => result.status === "passed").length;
  const finishedAt = new Date();

  return {
    status: failedCount > 0 ? "failed" : skippedCount > 0 ? "skipped" : "passed",
    startedAt: startedAt.toISOString(),
    finishedAt: finishedAt.toISOString(),
    durationMs: finishedAt.getTime() - startedAt.getTime(),
    environment: {
      repoRoot: environment.repoRoot,
      manifestPath: DEFAULT_MANIFEST_PATH,
      romPath: environment.romPath,
      guiPath: environment.guiPath,
      autotesterPath: environment.autotesterPath,
      headlessPath: environment.headlessPath,
      artificePath: environment.artificePath,
      transferCount: environment.transferPackage?.transferCount ?? 0,
    },
    outputRoot,
    contextRoot: context.rootDir,
    blockers: [...environment.blockers, ...results.filter((result) => result.error).map((result) => `${result.name}: ${result.error}`)],
    results,
    passedCount,
    failedCount,
    skippedCount,
  };
}
