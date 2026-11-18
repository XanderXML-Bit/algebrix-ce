import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

export interface TransferManifestEntry {
  name: string;
  sizeBytes: number;
  crc32: string;
  kind: "module" | "runtime" | "readme";
}

export interface TransferPackage {
  manifestPath: string;
  moduleEntries: TransferManifestEntry[];
  runtimeEntries: TransferManifestEntry[];
  readmeEntry: TransferManifestEntry | null;
  resolvedPaths: Record<string, string>;
  candidateRoots: string[];
  missingEntries: string[];
  moduleNames: string[];
  runtimeNames: string[];
  transferPaths: string[];
  allArtifactPaths: string[];
  moduleCount: number;
  runtimeDependencyCount: number;
  transferCount: number;
  ready: boolean;
}

export interface ResolvedCEmuEnvironment {
  repoRoot: string;
  romPath: string | null;
  guiPath: string | null;
  autotesterPath: string | null;
  headlessPath: string | null;
  artificePath: string | null;
  transferPackage: TransferPackage | null;
  blockers: string[];
  ready: boolean;
  artificeReady: boolean;
}

export interface CEmuPipelineContext {
  rootDir: string;
  directTransferDir: string;
  artificeTransferDir: string | null;
  cleanup: () => Promise<void>;
}

export interface StagedTransferValidationResult {
  expectedNames: string[];
  missingEntries: string[];
  mismatchedEntries: string[];
  unexpectedEntries: string[];
  success: boolean;
}

export interface ResolveCEmuEnvironmentOptions {
  repoRoot?: string;
  manifestPath?: string;
  romPath?: string;
  guiPath?: string;
  autotesterPath?: string;
  headlessPath?: string;
  transferDir?: string;
  downloadsDir?: string;
  artificePath?: string;
}

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export const REPO_ROOT = path.resolve(__dirname, "..", "..");
export const DEFAULT_MANIFEST_PATH = path.join(REPO_ROOT, "tests", "emulator", "manifest.json");
export const DEFAULT_ROM_PATH = path.join(REPO_ROOT, "rom", "Rom.rom");
export const DEFAULT_DOWNLOADS_DIR = path.join(os.homedir(), "Downloads", "CalcEngine");
export const DEFAULT_ARTIFICE_PATH = path.join(REPO_ROOT, "tools", "emulator", "assets", "artifice-v2.8xp");
export const DEFAULT_CEMU_TIMEOUT_MULTIPLIER_ENV = "CEMU_TIMEOUT_MULTIPLIER";
export const DEFAULT_CEMU_TIMEOUT_MULTIPLIER = 1;
export const DEFAULT_REAL_CEMU_CI_OPT_IN_ENV = "CALCENGINE_ALLOW_REAL_CEMU_IN_CI";
export const STAGING_TEMP_SUFFIX = ".calcengine-stage";

const WINDOWS_GUI_CANDIDATES = [
  path.join(os.homedir(), "Tools", "CEmu", "v2.0", "CEmu.exe"),
  path.join(os.homedir(), "Tools", "CEmu", "CEmu.exe"),
  "C:\\Tools\\CEmu\\CEmu.exe",
  "C:\\tools\\cemu\\CEmu.exe",
  path.join(process.env.ProgramFiles ?? "", "CEmu", "CEmu.exe"),
  path.join(process.env["ProgramFiles(x86)"] ?? "", "CEmu", "CEmu.exe"),
  path.join(process.env.LOCALAPPDATA ?? "", "Programs", "CEmu", "CEmu.exe"),
];

const WINDOWS_AUTOTESTER_CANDIDATES = [
  path.join(os.homedir(), "Tools", "CEmu", "v2.0", "autotester.exe"),
  path.join(os.homedir(), "Tools", "CEmu", "autotester.exe"),
  "C:\\Tools\\CEmu\\autotester.exe",
  "C:\\tools\\cemu\\autotester.exe",
  path.join(process.env.ProgramFiles ?? "", "CEmu", "autotester.exe"),
  path.join(process.env["ProgramFiles(x86)"] ?? "", "CEmu", "autotester.exe"),
  path.join(process.env.LOCALAPPDATA ?? "", "Programs", "CEmu", "autotester.exe"),
];

const DEFAULT_HEADLESS_CANDIDATES = [
  path.join(REPO_ROOT, "tools", "emulator", "cemu-headless", "build", "Release", "cemu-headless.exe"),
  path.join(REPO_ROOT, "tools", "emulator", "cemu-headless", "build", "cemu-headless.exe"),
  path.join(REPO_ROOT, "tools", "emulator", "cemu-headless", "cmake-build-release", "cemu-headless.exe"),
  path.join(REPO_ROOT, "tools", "emulator", "cemu-headless", "out", "Release", "cemu-headless.exe"),
];
const TRANSFER_ARTIFACT_SUFFIXES = new Set([".8xp", ".8xv"]);

const CRC32_TABLE = buildCrc32Table();

export function getCEmuTimeoutMultiplier(env: NodeJS.ProcessEnv = process.env): number {
  const rawValue = `${env[DEFAULT_CEMU_TIMEOUT_MULTIPLIER_ENV] ?? ""}`.trim();
  if (rawValue.length === 0) {
    return DEFAULT_CEMU_TIMEOUT_MULTIPLIER;
  }

  const parsedMultiplier = Number(rawValue);
  return Number.isFinite(parsedMultiplier) && parsedMultiplier > 0
    ? parsedMultiplier
    : DEFAULT_CEMU_TIMEOUT_MULTIPLIER;
}

export function scaleCEmuTimeoutMs(
  timeoutMs: number,
  options: {
    env?: NodeJS.ProcessEnv;
    minimumMs?: number;
  } = {},
): number {
  const scaledTimeoutMs = Math.round(timeoutMs * getCEmuTimeoutMultiplier(options.env));
  return Math.max(options.minimumMs ?? 0, scaledTimeoutMs);
}

function buildCrc32Table(): Uint32Array {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value & 1) === 1 ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
    }
    table[index] = value >>> 0;
  }
  return table;
}

function computeCrc32Hex(filePath: string): string {
  const buffer = fs.readFileSync(filePath);
  let crc = 0xffffffff;
  for (const byte of buffer) {
    crc = CRC32_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return ((crc ^ 0xffffffff) >>> 0).toString(16).toUpperCase().padStart(8, "0");
}

function isNonEmptyString(value: string | null | undefined): value is string {
  return typeof value === "string" && value.trim().length > 0;
}

function envFlagEnabled(value: string | null | undefined): boolean {
  const normalized = `${value ?? ""}`.trim().toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on";
}

function hostedCiBlocksRealCEmu(env: NodeJS.ProcessEnv = process.env): boolean {
  const runningInCi = envFlagEnabled(env.CI) || envFlagEnabled(env.GITHUB_ACTIONS);
  const ciOptInEnabled = envFlagEnabled(env[DEFAULT_REAL_CEMU_CI_OPT_IN_ENV]);
  return runningInCi && !ciOptInEnabled;
}

function hostedCiRealCEmuBlocker(): string {
  return "Real CEmu and arTIfiCE hardware-dependent lanes are disabled on hosted CI. "
    + `Set ${DEFAULT_REAL_CEMU_CI_OPT_IN_ENV}=true to opt into a provisioned runner.`;
}

function normalizeExistingPath(candidate: string | null | undefined): string | null {
  if (!isNonEmptyString(candidate)) {
    return null;
  }
  const resolved = path.resolve(candidate);
  return fs.existsSync(resolved) ? resolved : null;
}

function uniquePaths(candidates: Array<string | null | undefined>): string[] {
  const seen = new Set<string>();
  const resolved: string[] = [];
  for (const candidate of candidates) {
    const existing = normalizeExistingPath(candidate);
    if (!existing) {
      continue;
    }
    const key = existing.toLowerCase();
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    resolved.push(existing);
  }
  return resolved;
}

function resolveExecutableFromPath(names: string[]): string | null {
  const pathValue = process.env.PATH ?? "";
  const segments = pathValue.split(path.delimiter).filter(Boolean);
  const suffixes = process.platform === "win32" ? ["", ".exe", ".cmd", ".bat"] : [""];
  for (const segment of segments) {
    for (const name of names) {
      for (const suffix of suffixes) {
        const candidate = path.join(segment, `${name}${suffix}`);
        if (fs.existsSync(candidate)) {
          return candidate;
        }
      }
    }
  }
  return null;
}

function resolveWindowsWhere(names: string[]): string | null {
  if (process.platform !== "win32") {
    return null;
  }
  for (const name of names) {
    const result = spawnSync("where.exe", [name], { encoding: "utf8" });
    if (result.status !== 0) {
      continue;
    }
    const firstLine = `${result.stdout ?? ""}`.split(/\r?\n/).find((line) => line.trim().length > 0);
    if (firstLine && fs.existsSync(firstLine.trim())) {
      return path.resolve(firstLine.trim());
    }
  }
  return null;
}

function resolveGuiPath(explicitPath?: string): string | null {
  return uniquePaths([
    explicitPath,
    process.env.CEMU_GUI_PATH,
    process.env.CEMU_PATH,
    resolveWindowsWhere(["CEmu.exe", "cemu.exe"]),
    resolveExecutableFromPath(["CEmu", "cemu"]),
    ...WINDOWS_GUI_CANDIDATES,
  ])[0] ?? null;
}

function resolveAutotesterPath(explicitPath?: string): string | null {
  return uniquePaths([
    explicitPath,
    process.env.CEMU_AUTOTESTER_PATH,
    resolveWindowsWhere(["autotester.exe", "autotester"]),
    resolveExecutableFromPath(["autotester"]),
    ...WINDOWS_AUTOTESTER_CANDIDATES,
  ])[0] ?? null;
}

function resolveHeadlessPath(explicitPath?: string): string | null {
  return uniquePaths([
    explicitPath,
    process.env.CEMU_HEADLESS_PATH,
    process.env.CEMU_BINARY_PATH,
    resolveWindowsWhere(["cemu-headless.exe", "cemu-headless"]),
    resolveExecutableFromPath(["cemu-headless"]),
    ...DEFAULT_HEADLESS_CANDIDATES,
  ])[0] ?? null;
}

function resolveArtificePath(explicitPath?: string): string | null {
  return uniquePaths([
    explicitPath,
    process.env.CALCENGINE_ARTIFICE_PATH,
    DEFAULT_ARTIFICE_PATH,
  ])[0] ?? null;
}

function resolveRomPath(explicitPath?: string): string | null {
  return uniquePaths([
    explicitPath,
    process.env.CALCENGINE_ROM_PATH,
    DEFAULT_ROM_PATH,
  ])[0] ?? null;
}

function readJsonFile<T>(filePath: string): T {
  return JSON.parse(fs.readFileSync(filePath, "utf8")) as T;
}

function artifactMatches(entry: TransferManifestEntry, filePath: string): boolean {
  if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
    return false;
  }
  const stats = fs.statSync(filePath);
  if (entry.sizeBytes >= 0 && stats.size !== entry.sizeBytes) {
    return false;
  }
  if (entry.crc32) {
    return computeCrc32Hex(filePath) === entry.crc32;
  }
  return true;
}

function expectedStagedEntries(
  transferPackage: TransferPackage,
  includeReadme: boolean,
): TransferManifestEntry[] {
  return [
    ...transferPackage.moduleEntries,
    ...transferPackage.runtimeEntries,
    ...(includeReadme && transferPackage.readmeEntry ? [transferPackage.readmeEntry] : []),
  ];
}

function formatValidationFailures(validation: StagedTransferValidationResult): string {
  const failures: string[] = [];
  if (validation.missingEntries.length > 0) {
    failures.push(`missing: ${validation.missingEntries.join(", ")}`);
  }
  if (validation.mismatchedEntries.length > 0) {
    failures.push(`mismatched: ${validation.mismatchedEntries.join(", ")}`);
  }
  if (validation.unexpectedEntries.length > 0) {
    failures.push(`unexpected: ${validation.unexpectedEntries.join(", ")}`);
  }
  return failures.join("; ");
}

function copyArtifactToStage(
  sourcePath: string,
  destinationDir: string,
  destinationName: string,
  entry: TransferManifestEntry,
): void {
  const stagingPath = path.join(destinationDir, `.${destinationName}${STAGING_TEMP_SUFFIX}`);
  const destinationPath = path.join(destinationDir, destinationName);
  if (fs.existsSync(stagingPath)) {
    fs.rmSync(stagingPath, { force: true });
  }
  try {
    fs.copyFileSync(sourcePath, stagingPath);
  } catch (error) {
    if (fs.existsSync(stagingPath)) {
      fs.rmSync(stagingPath, { force: true });
    }
    throw new Error(
      `Staged artifact '${destinationName}' could not be copied cleanly: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
  if (!artifactMatches(entry, stagingPath)) {
    if (fs.existsSync(stagingPath)) {
      fs.rmSync(stagingPath, { force: true });
    }
    throw new Error(`Staged artifact '${destinationName}' failed size/CRC verification immediately after copy.`);
  }
  try {
    fs.renameSync(stagingPath, destinationPath);
  } catch (error) {
    if (fs.existsSync(stagingPath)) {
      fs.rmSync(stagingPath, { force: true });
    }
    throw new Error(
      `Staged artifact '${destinationName}' could not be finalized cleanly: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
}

export function validateStagedTransferDirectory(
  transferPackage: TransferPackage,
  destinationDir: string,
  options: {
    includeReadme?: boolean;
    allowedExtraNames?: string[];
  } = {},
): StagedTransferValidationResult {
  const includeReadme = options.includeReadme ?? true;
  const expectedEntries = expectedStagedEntries(transferPackage, includeReadme);
  const expectedNames = expectedEntries.map((entry) => entry.name);
  const expectedNameSet = new Set(expectedNames.map((name) => name.toLowerCase()));
  const allowedExtraNames = new Set((options.allowedExtraNames ?? []).map((name) => name.toLowerCase()));
  const unexpectedEntries: string[] = [];

  if (fs.existsSync(destinationDir) && fs.statSync(destinationDir).isDirectory()) {
    for (const entry of fs.readdirSync(destinationDir, { withFileTypes: true })) {
      if (!entry.isFile()) {
        continue;
      }
      const extension = path.extname(entry.name).toLowerCase();
      if (
        !TRANSFER_ARTIFACT_SUFFIXES.has(extension)
        && entry.name.toLowerCase() !== "readme.md"
        && !entry.name.endsWith(STAGING_TEMP_SUFFIX)
      ) {
        continue;
      }
      const normalizedName = entry.name.toLowerCase();
      if (!expectedNameSet.has(normalizedName) && !allowedExtraNames.has(normalizedName)) {
        unexpectedEntries.push(entry.name);
      }
    }
  }

  const missingEntries: string[] = [];
  const mismatchedEntries: string[] = [];
  for (const entry of expectedEntries) {
    const stagedPath = path.join(destinationDir, entry.name);
    if (!fs.existsSync(stagedPath) || !fs.statSync(stagedPath).isFile()) {
      missingEntries.push(entry.name);
      continue;
    }
    if (!artifactMatches(entry, stagedPath)) {
      mismatchedEntries.push(entry.name);
    }
  }

  return {
    expectedNames,
    missingEntries,
    mismatchedEntries,
    unexpectedEntries: unexpectedEntries.sort((left, right) => left.localeCompare(right)),
    success: missingEntries.length === 0 && mismatchedEntries.length === 0 && unexpectedEntries.length === 0,
  };
}

function loadTransferManifest(manifestPath: string): {
  moduleEntries: TransferManifestEntry[];
  runtimeEntries: TransferManifestEntry[];
  readmeEntry: TransferManifestEntry | null;
} {
  const payload = readJsonFile<{
    modules: Array<{ name: string; size_bytes: number; crc32: string }>;
    runtime_dependencies: Array<{ name: string; size_bytes: number; crc32: string }>;
    readme?: { name: string; crc32?: string };
  }>(manifestPath);

  const moduleEntries = payload.modules.map((entry) => ({
    name: entry.name,
    sizeBytes: entry.size_bytes,
    crc32: entry.crc32.toUpperCase(),
    kind: "module" as const,
  }));

  const runtimeEntries = payload.runtime_dependencies.map((entry) => ({
    name: entry.name,
    sizeBytes: entry.size_bytes,
    crc32: entry.crc32.toUpperCase(),
    kind: "runtime" as const,
  }));

  const readmeEntry = payload.readme
    ? {
        name: payload.readme.name,
        sizeBytes: -1,
        crc32: (payload.readme.crc32 ?? "").toUpperCase(),
        kind: "readme" as const,
      }
    : null;

  return { moduleEntries, runtimeEntries, readmeEntry };
}

function candidateRootsForTransferPackage(
  repoRoot: string,
  transferDir?: string,
  downloadsDir?: string,
): string[] {
  return uniquePaths([
    transferDir,
    process.env.CALCENGINE_TRANSFER_DIR,
    downloadsDir,
    DEFAULT_DOWNLOADS_DIR,
    path.join(repoRoot, "bin"),
    path.join(repoRoot, "_research_tmp", "toolchain", "src"),
    repoRoot,
  ]);
}

function walkForFilename(root: string, filename: string, recursive: boolean): string[] {
  const matches: string[] = [];
  if (!fs.existsSync(root)) {
    return matches;
  }

  const directCandidate = path.join(root, filename);
  if (fs.existsSync(directCandidate) && fs.statSync(directCandidate).isFile()) {
    matches.push(path.resolve(directCandidate));
  }

  if (!recursive || !fs.statSync(root).isDirectory()) {
    return matches;
  }

  const stack = [root];
  while (stack.length > 0) {
    const current = stack.pop()!;
    const entries = fs.readdirSync(current, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = path.join(current, entry.name);
      if (entry.isDirectory()) {
        stack.push(fullPath);
        continue;
      }
      if (!entry.isFile()) {
        continue;
      }
      if (entry.name.toLowerCase() === filename.toLowerCase()) {
        matches.push(path.resolve(fullPath));
      }
    }
  }

  return matches;
}

function resolveManifestEntry(entry: TransferManifestEntry, candidateRoots: string[], repoRoot: string): string | null {
  for (const root of candidateRoots) {
    const recursive = path.resolve(root) !== path.resolve(repoRoot);
    const candidates = walkForFilename(root, entry.name, recursive).sort((left, right) => {
      const leftDepth = left.split(path.sep).length;
      const rightDepth = right.split(path.sep).length;
      if (leftDepth !== rightDepth) {
        return leftDepth - rightDepth;
      }
      if (left.length !== right.length) {
        return left.length - right.length;
      }
      return left.localeCompare(right);
    });
    for (const candidate of candidates) {
      if (artifactMatches(entry, candidate)) {
        return candidate;
      }
    }
  }
  return null;
}

export function resolveTransferPackage(options: {
  repoRoot?: string;
  manifestPath?: string;
  transferDir?: string;
  downloadsDir?: string;
} = {}): TransferPackage {
  const repoRoot = path.resolve(options.repoRoot ?? REPO_ROOT);
  const manifestPath = path.resolve(options.manifestPath ?? DEFAULT_MANIFEST_PATH);
  const { moduleEntries, runtimeEntries, readmeEntry } = loadTransferManifest(manifestPath);
  const candidateRoots = candidateRootsForTransferPackage(repoRoot, options.transferDir, options.downloadsDir);
  const resolvedPaths: Record<string, string> = {};
  const missingEntries: string[] = [];

  for (const entry of [...moduleEntries, ...runtimeEntries]) {
    const resolvedPath = resolveManifestEntry(entry, candidateRoots, repoRoot);
    if (!resolvedPath) {
      missingEntries.push(entry.name);
      continue;
    }
    resolvedPaths[entry.name] = resolvedPath;
  }

  if (readmeEntry) {
    const resolvedReadmePath = resolveManifestEntry(readmeEntry, candidateRoots, repoRoot);
    if (!resolvedReadmePath) {
      missingEntries.push(readmeEntry.name);
    } else {
      resolvedPaths[readmeEntry.name] = resolvedReadmePath;
    }
  }

  const moduleNames = moduleEntries.map((entry) => entry.name);
  const runtimeNames = runtimeEntries.map((entry) => entry.name);
  const transferPaths = [...moduleNames, ...runtimeNames]
    .map((name) => resolvedPaths[name])
    .filter((value): value is string => isNonEmptyString(value));
  const allArtifactPaths = [...moduleNames, ...runtimeNames, ...(readmeEntry ? [readmeEntry.name] : [])]
    .map((name) => resolvedPaths[name])
    .filter((value): value is string => isNonEmptyString(value));

  return {
    manifestPath,
    moduleEntries,
    runtimeEntries,
    readmeEntry,
    resolvedPaths,
    candidateRoots,
    missingEntries,
    moduleNames,
    runtimeNames,
    transferPaths,
    allArtifactPaths,
    moduleCount: moduleEntries.length,
    runtimeDependencyCount: runtimeEntries.length,
    transferCount: transferPaths.length,
    ready: missingEntries.length === 0,
  };
}

export function resolveCEmuEnvironment(options: ResolveCEmuEnvironmentOptions = {}): ResolvedCEmuEnvironment {
  const repoRoot = path.resolve(options.repoRoot ?? REPO_ROOT);
  const romPath = resolveRomPath(options.romPath);
  const guiPath = resolveGuiPath(options.guiPath);
  const autotesterPath = resolveAutotesterPath(options.autotesterPath);
  const headlessPath = resolveHeadlessPath(options.headlessPath);
  const artificePath = resolveArtificePath(options.artificePath);
  const blockers: string[] = [];

  if (hostedCiBlocksRealCEmu()) {
    blockers.push(hostedCiRealCEmuBlocker());
  }

  if (!romPath) {
    blockers.push(`ROM file is missing at ${path.resolve(options.romPath ?? DEFAULT_ROM_PATH)}`);
  }
  if (!headlessPath) {
    blockers.push("cemu-headless executable is unavailable");
  }
  if (!autotesterPath) {
    blockers.push("autotester.exe is unavailable");
  }

  let transferPackage: TransferPackage | null = null;
  try {
    transferPackage = resolveTransferPackage({
      repoRoot,
      manifestPath: options.manifestPath,
      transferDir: options.transferDir,
      downloadsDir: options.downloadsDir,
    });
    if (!transferPackage.ready) {
      blockers.push(`CalcEngine transfer package is incomplete: ${transferPackage.missingEntries.join(", ")}`);
    }
  } catch (error) {
    blockers.push(error instanceof Error ? error.message : String(error));
  }

  const ready = Boolean(romPath && headlessPath && autotesterPath && transferPackage?.ready && blockers.length === 0);
  const artificeReady = ready && Boolean(artificePath);

  return {
    repoRoot,
    romPath,
    guiPath,
    autotesterPath,
    headlessPath,
    artificePath,
    transferPackage,
    blockers,
    ready,
    artificeReady,
  };
}

export function stageTransferPackage(
  transferPackage: TransferPackage,
  destinationDir: string,
  options: { includeReadme?: boolean } = {},
): string {
  if (!transferPackage.ready) {
    throw new Error(`CalcEngine transfer package is incomplete: ${transferPackage.missingEntries.join(", ")}`);
  }

  fs.mkdirSync(destinationDir, { recursive: true });
  for (const entry of fs.readdirSync(destinationDir, { withFileTypes: true })) {
    if (!entry.isFile()) {
      continue;
    }
    const extension = path.extname(entry.name).toLowerCase();
    if (
      TRANSFER_ARTIFACT_SUFFIXES.has(extension)
      || entry.name.toLowerCase() === "readme.md"
      || entry.name.endsWith(STAGING_TEMP_SUFFIX)
    ) {
      fs.rmSync(path.join(destinationDir, entry.name), { force: true });
    }
  }
  const sourcePaths = options.includeReadme === false ? transferPackage.transferPaths : transferPackage.allArtifactPaths;
  const entryByName = new Map(expectedStagedEntries(transferPackage, options.includeReadme !== false).map((entry) => [entry.name, entry]));
  for (const sourcePath of sourcePaths) {
    const destinationName = path.basename(sourcePath);
    const entry = entryByName.get(destinationName);
    if (!entry) {
      throw new Error(`Transfer package metadata is missing staged entry '${destinationName}'.`);
    }
    copyArtifactToStage(sourcePath, destinationDir, destinationName, entry);
  }
  const validation = validateStagedTransferDirectory(transferPackage, destinationDir, {
    includeReadme: options.includeReadme,
  });
  if (!validation.success) {
    throw new Error(`Staged transfer directory failed validation after copy: ${formatValidationFailures(validation)}`);
  }
  return destinationDir;
}

export function createPipelineContext(
  environment: ResolvedCEmuEnvironment,
  options: { keepTempDir?: boolean } = {},
): CEmuPipelineContext {
  if (!environment.transferPackage) {
    throw new Error("Transfer package is unavailable.");
  }

  const rootDir = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-cemu-ts-"));
  const directTransferDir = stageTransferPackage(environment.transferPackage, path.join(rootDir, "direct-transfer"));
  let artificeTransferDir: string | null = null;

  if (environment.artificePath) {
    artificeTransferDir = stageTransferPackage(environment.transferPackage, path.join(rootDir, "artifice-transfer"));
    const artificeStats = fs.statSync(environment.artificePath);
    if (!artificeStats.isFile() || artificeStats.size <= 0) {
      throw new Error(`arTIfiCE asset is unreadable or empty: ${environment.artificePath}`);
    }
    copyArtifactToStage(
      environment.artificePath,
      artificeTransferDir,
      "A.8xp",
      {
        name: "A.8xp",
        sizeBytes: artificeStats.size,
        crc32: "",
        kind: "module",
      },
    );
    const validation = validateStagedTransferDirectory(environment.transferPackage, artificeTransferDir, {
      allowedExtraNames: ["A.8xp"],
    });
    if (!validation.success) {
      throw new Error(`arTIfiCE transfer directory failed validation after staging: ${formatValidationFailures(validation)}`);
    }
  }

  return {
    rootDir,
    directTransferDir,
    artificeTransferDir,
    cleanup: async () => {
      if (options.keepTempDir) {
        return;
      }
      await fs.promises.rm(rootDir, { recursive: true, force: true });
    },
  };
}
