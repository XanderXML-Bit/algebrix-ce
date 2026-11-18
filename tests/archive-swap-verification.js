import fs from "node:fs";
import path from "node:path";
import { expect, test } from "vitest";
import { normalizePathFromWindowsHostProcess } from "./helpers/windows-host-paths.mjs";
import {
  REPO_ROOT,
  relativeRepoPath,
  resolveManifestModuleArtifacts,
  runPowerShellScript,
} from "./helpers/split-suite-artifacts.mjs";

const harnessScriptPath = path.join(REPO_ROOT, "tools", "emulator", "run-terminal-harness.ps1");
const manifestSuite = resolveManifestModuleArtifacts(REPO_ROOT);
const manifestArtifactsUnavailable = manifestSuite.errors.length > 0 &&
  manifestSuite.errors.every((error) => error.includes("Could not resolve manifest module"));
const manifestTest = manifestArtifactsUnavailable ? test.skip : test;

if (manifestArtifactsUnavailable) {
  console.warn(
    `[SKIP] archive-swap verification skipped because packaged split-suite artifacts are absent: ${manifestSuite.errors.join(" | ")}`,
  );
}

function runArchiveSwapHarness(extraArgs) {
  const result = runPowerShellScript(harnessScriptPath, [
    "-Keys",
    "2,+,3,enter",
    "-Format",
    "json",
    "-ForceMock",
    ...extraArgs,
  ]);

  if (!(result.stdout ?? "").trim()) {
    throw new Error(`Archive swap harness returned empty stdout: status=${result.status}, signal=${result.signal}, stderr=${result.stderr ?? ""}`);
  }
  const payload = JSON.parse(result.stdout);
  return { result, payload };
}

function expectSuccessfulTimeline(payload) {
  const timeline = [
    "archive",
    payload.archive_ram_swap.dispatch.loaded_to_ram ? "ram" : "dispatch_failed",
    payload.screen.status === "ok" && payload.screen.result === "5" ? "execute" : "execute_failed",
    payload.archive_ram_swap.modules.corruption_detected ? "dirty" : "clean",
    payload.archive_ram_swap.return_to_archive.returned_to_archive ? "archive" : "ram",
  ];

  expect(timeline).toEqual(["archive", "ram", "execute", "clean", "archive"]);
}

function normalizeArtifactPath(artifactPath) {
  if (typeof artifactPath !== "string") {
    return artifactPath;
  }

  const slashNormalizedPath = artifactPath.split('\\').join('/');
  // On Windows native Node.js, keep the Windows-style path (slash-normalized only).
  // The /mnt/<drive>/ conversion is only correct in a WSL/Linux environment.
  if (process.platform === 'win32') {
    return slashNormalizedPath;
  }

  return normalizePathFromWindowsHostProcess(slashNormalizedPath);
}

function countOccurrences(values) {
  const counts = new Map();
  for (const value of values) {
    counts.set(value, (counts.get(value) ?? 0) + 1);
  }
  return counts;
}

function buildCrc32Table() {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value & 1) === 1 ? (0xEDB88320 ^ (value >>> 1)) : (value >>> 1);
    }
    table[index] = value >>> 0;
  }
  return table;
}

const CRC32_TABLE = buildCrc32Table();

function computeCrc32Hex(buffer) {
  let crc = 0xFFFFFFFF;
  for (const byte of buffer) {
    crc = CRC32_TABLE[(crc ^ byte) & 0xFF] ^ (crc >>> 8);
  }
  return ((crc ^ 0xFFFFFFFF) >>> 0).toString(16).toUpperCase().padStart(8, "0");
}

manifestTest("archive -> RAM -> execute -> clean -> archive completes for the largest official module", () => {
  const largestModule = [...manifestSuite.supportArtifacts].sort((left, right) => right.sizeBytes - left.sizeBytes)[0];
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    largestModule.artifactPath,
  ]);

  expect(result.status).toBe(0);
  expect(payload.success).toBe(true);
  expect(payload.archive_ram_swap.simulated).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(true);
  expect(payload.archive_ram_swap.failure_phase).toBe("none");
  expect(payload.archive_ram_swap.dispatch.loaded_to_ram).toBe(true);
  expect(payload.archive_ram_swap.return_to_archive.attempted).toBe(true);
  expect(payload.archive_ram_swap.return_to_archive.returned_to_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);
  expect(payload.archive_ram_swap.modules.requested_count).toBe(1);
  expect(payload.archive_ram_swap.modules.items[0].checksum_before).toBe(
    payload.archive_ram_swap.modules.items[0].checksum_after,
  );
  expect(payload.archive_ram_swap.diagnostics.join(" | ")).toContain("archive->RAM->archive succeeded");

  expectSuccessfulTimeline(payload);
}, 30_000);

manifestTest("module A calling module B leaves both modules archived again after the handoff", () => {
  const [moduleA, moduleB] = manifestSuite.supportArtifacts.slice(0, 2);
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    moduleA.artifactPath,
    "-ArchiveRamModulePaths",
    `${moduleA.artifactPath};${moduleB.artifactPath}`,
    "-ArchiveRamRapidSwapCycles",
    "2",
  ]);

  const itemPaths = payload.archive_ram_swap.modules.items.map((item) => relativeRepoPath(normalizeArtifactPath(item.resolved_path)));

  expect(result.status).toBe(0);
  expect(payload.success).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(true);
  expect(payload.archive_ram_swap.modules.requested_count).toBe(2);
  expect(payload.archive_ram_swap.modules.rapid_swap_cycles).toBe(2);
  expect(payload.archive_ram_swap.modules.dispatch_events).toBe(4);
  expect(payload.archive_ram_swap.modules.return_events).toBe(4);
  expect(payload.archive_ram_swap.modules.corruption_detected).toBe(false);
  expect(payload.archive_ram_swap.modules.partial_load_detected).toBe(false);
  expect(itemPaths).toEqual([
    relativeRepoPath(moduleA.artifactPath),
    relativeRepoPath(moduleB.artifactPath),
    relativeRepoPath(moduleA.artifactPath),
    relativeRepoPath(moduleB.artifactPath),
  ]);
  expect(payload.archive_ram_swap.modules.items.every((item) => item.loaded_to_ram && item.returned_to_archive)).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);
}, 30_000);

manifestTest("dispatch-load failure recovers without leaving the module resident in RAM", () => {
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    manifestSuite.supportArtifacts[0].artifactPath,
    "-ArchiveRamFailurePhase",
    "dispatch_load",
  ]);

  expect(result.status).toBe(1);
  expect(payload.success).toBe(false);
  expect(payload.archive_ram_swap.simulated).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(false);
  expect(payload.archive_ram_swap.failure_phase).toBe("dispatch_load");
  expect(payload.archive_ram_swap.dispatch.loaded_to_ram).toBe(false);
  expect(payload.archive_ram_swap.return_to_archive.attempted).toBe(false);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);
  expect(payload.archive_ram_swap.errors.join(" | ")).toContain("dispatch_load failure");
}, 30_000);

manifestTest("archive-full rearchive failure leaves the module stranded in RAM with an explicit diagnostic", () => {
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    manifestSuite.supportArtifacts[1].artifactPath,
    "-ArchiveRamFailurePhase",
    "archive_full",
  ]);

  expect(result.status).toBe(1);
  expect(payload.success).toBe(false);
  expect(payload.archive_ram_swap.simulated).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(false);
  expect(payload.archive_ram_swap.failure_phase).toBe("archive_full");
  expect(payload.archive_ram_swap.dispatch.loaded_to_ram).toBe(true);
  expect(payload.archive_ram_swap.return_to_archive.attempted).toBe(true);
  expect(payload.archive_ram_swap.return_to_archive.returned_to_archive).toBe(false);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(false);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(true);
  expect(payload.archive_ram_swap.errors.join(" | ")).toContain("archive is full");
}, 30_000);

manifestTest("manifest-backed packaged support modules survive repeated archive churn without corruption", () => {
  const largestModule = [...manifestSuite.supportArtifacts].sort((left, right) => right.sizeBytes - left.sizeBytes)[0];
  const expectedCounts = countOccurrences(manifestSuite.supportArtifacts.map((artifact) => relativeRepoPath(artifact.artifactPath)));
  const { result, payload } = runArchiveSwapHarness([
    "-ModuleProgramPathSource",
    "manifest_transfer_suite",
    "-ArchiveRamDispatchModulePath",
    largestModule.artifactPath,
    "-ArchiveRamRapidSwapCycles",
    "2",
  ]);

  const itemPaths = payload.archive_ram_swap.modules.items.map((item) => relativeRepoPath(normalizeArtifactPath(item.resolved_path)));
  const observedCounts = countOccurrences(itemPaths);

  expect(result.status).toBe(0);
  expect(payload.success).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(true);
  expect(payload.archive_ram_swap.modules.requested_count).toBe(manifestSuite.supportArtifacts.length);
  expect(payload.archive_ram_swap.modules.rapid_swap_cycles).toBe(2);
  expect(payload.archive_ram_swap.modules.dispatch_events).toBe(manifestSuite.supportArtifacts.length * 2);
  expect(payload.archive_ram_swap.modules.return_events).toBe(manifestSuite.supportArtifacts.length * 2);
  expect(payload.archive_ram_swap.modules.items).toHaveLength(manifestSuite.supportArtifacts.length * 2);
  expect(payload.archive_ram_swap.modules.corruption_detected).toBe(false);
  expect(payload.archive_ram_swap.modules.partial_load_detected).toBe(false);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);
  expect(payload.archive_ram_swap.modules.items.every((item) => item.loaded_to_ram && item.returned_to_archive)).toBe(true);
  expect([...observedCounts.entries()].sort()).toEqual(
    [...expectedCounts.entries()].map(([artifactPath, count]) => [artifactPath, count * 2]).sort(),
  );
}, 30_000);

manifestTest("manifest-backed full archive churn covers all 35 official .8xp payloads including CALCENG in manifest order", () => {
  expect(manifestSuite.artifacts).toHaveLength(35);
  expect(manifestSuite.programArtifact).not.toBeNull();

  const requestedPaths = manifestSuite.artifacts.map((artifact) => artifact.artifactPath);
  const expectedItemPaths = manifestSuite.artifacts.map((artifact) => relativeRepoPath(artifact.artifactPath));
  const expectedCrcs = new Map(
    manifestSuite.artifacts.map((artifact) => [
      normalizeArtifactPath(artifact.artifactPath),
      computeCrc32Hex(fs.readFileSync(artifact.artifactPath)),
    ]),
  );
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    manifestSuite.programArtifact.artifactPath,
    "-ArchiveRamModulePaths",
    requestedPaths.join(";"),
  ]);

  const itemPaths = payload.archive_ram_swap.modules.items.map((item) => relativeRepoPath(normalizeArtifactPath(item.resolved_path)));

  expect(result.status).toBe(0);
  expect(payload.success).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(true);
  expect(payload.archive_ram_swap.modules.requested_count).toBe(35);
  expect(payload.archive_ram_swap.modules.dispatch_events).toBe(35);
  expect(payload.archive_ram_swap.modules.return_events).toBe(35);
  expect(payload.archive_ram_swap.modules.items).toHaveLength(35);
  expect(payload.archive_ram_swap.modules.items.every((item) => item.loaded_to_ram && item.returned_to_archive)).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);
  expect(itemPaths).toEqual(expectedItemPaths);
  expect(itemPaths[0]).toBe(relativeRepoPath(manifestSuite.programArtifact.artifactPath));
  expect(new Set(itemPaths).size).toBe(35);
  for (const item of payload.archive_ram_swap.modules.items) {
    expect(item.checksum_before).toBe(item.checksum_after);
    expect(computeCrc32Hex(fs.readFileSync(normalizeArtifactPath(item.resolved_path)))).toBe(expectedCrcs.get(normalizeArtifactPath(item.resolved_path)));
  }
}, 30_000);

manifestTest("archive byte and CRC integrity remain identical across load execute archive cycles for all 35 manifest modules", () => {
  const requestedArtifacts = manifestSuite.artifacts;
  const requestedPaths = requestedArtifacts.map((artifact) => artifact.artifactPath);
  const originalPayloads = new Map(
    requestedArtifacts.map((artifact) => [normalizeArtifactPath(artifact.artifactPath), Buffer.from(fs.readFileSync(artifact.artifactPath))]),
  );
  const originalCrcs = new Map(
    requestedArtifacts.map((artifact) => [normalizeArtifactPath(artifact.artifactPath), computeCrc32Hex(originalPayloads.get(normalizeArtifactPath(artifact.artifactPath)))]),
  );
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    requestedArtifacts[0].artifactPath,
    "-ArchiveRamModulePaths",
    requestedPaths.join(";"),
  ]);

  expect(requestedArtifacts).toHaveLength(35);
  expect(result.status).toBe(0);
  expect(payload.success).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(true);
  expect(payload.archive_ram_swap.failure_phase).toBe("none");
  expect(payload.archive_ram_swap.modules.requested_count).toBe(35);
  expect(payload.archive_ram_swap.modules.dispatch_events).toBe(35);
  expect(payload.archive_ram_swap.modules.return_events).toBe(35);
  expect(payload.archive_ram_swap.modules.items).toHaveLength(35);
  expect(payload.archive_ram_swap.modules.corruption_detected).toBe(false);
  expect(payload.archive_ram_swap.modules.partial_load_detected).toBe(false);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);

  for (const item of payload.archive_ram_swap.modules.items) {
    const normalizedResolvedPath = normalizeArtifactPath(item.resolved_path);
    const beforePayload = originalPayloads.get(normalizedResolvedPath);
    const afterPayload = fs.readFileSync(normalizedResolvedPath);
    expect(beforePayload).toBeDefined();
    expect(item.loaded_to_ram).toBe(true);
    expect(item.returned_to_archive).toBe(true);
    expect(item.checksum_before).toBe(item.checksum_after);
    expect(Buffer.compare(beforePayload, afterPayload)).toBe(0);
    expect(computeCrc32Hex(beforePayload)).toBe(originalCrcs.get(normalizedResolvedPath));
    expect(computeCrc32Hex(afterPayload)).toBe(originalCrcs.get(normalizedResolvedPath));
  }
}, 30_000);

manifestTest("archive corruption is surfaced and a clean retry restores archive residency without checksum drift", () => {
  const largestArtifact = [...manifestSuite.artifacts].sort((left, right) => right.sizeBytes - left.sizeBytes)[0];
  const failedAttempt = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    largestArtifact.artifactPath,
    "-ArchiveRamFailurePhase",
    "partial_load",
  ]);

  expect(failedAttempt.result.status).toBe(1);
  expect(failedAttempt.payload.success).toBe(false);
  expect(failedAttempt.payload.archive_ram_swap.pass).toBe(false);
  expect(failedAttempt.payload.archive_ram_swap.failure_phase).toBe("partial_load");
  expect(failedAttempt.payload.archive_ram_swap.modules.corruption_detected).toBe(true);
  expect(failedAttempt.payload.archive_ram_swap.modules.partial_load_detected).toBe(true);
  expect(failedAttempt.payload.archive_ram_swap.state.module_in_archive).toBe(false);
  expect(failedAttempt.payload.archive_ram_swap.state.module_in_ram).toBe(true);
  expect(failedAttempt.payload.archive_ram_swap.modules.items[0].checksum_before).not.toBe(
    failedAttempt.payload.archive_ram_swap.modules.items[0].checksum_after,
  );

  const recoveredAttempt = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    largestArtifact.artifactPath,
  ]);

  expect(recoveredAttempt.result.status).toBe(0);
  expect(recoveredAttempt.payload.success).toBe(true);
  expect(recoveredAttempt.payload.archive_ram_swap.pass).toBe(true);
  expect(recoveredAttempt.payload.archive_ram_swap.failure_phase).toBe("none");
  expect(recoveredAttempt.payload.archive_ram_swap.modules.corruption_detected).toBe(false);
  expect(recoveredAttempt.payload.archive_ram_swap.modules.partial_load_detected).toBe(false);
  expect(recoveredAttempt.payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(recoveredAttempt.payload.archive_ram_swap.state.module_in_ram).toBe(false);
  expect(recoveredAttempt.payload.archive_ram_swap.modules.items[0].checksum_before).toBe(
    recoveredAttempt.payload.archive_ram_swap.modules.items[0].checksum_after,
  );
  expect(recoveredAttempt.payload.archive_ram_swap.diagnostics.join(" | ")).toContain(
    "archive->RAM->archive succeeded",
  );
}, 30_000);

manifestTest("archive churn preserves explicit multi-program ordering across repeated cycles", () => {
  expect(manifestSuite.programArtifact).not.toBeNull();

  const requestedArtifacts = [
    manifestSuite.programArtifact,
    manifestSuite.supportArtifacts[0],
    manifestSuite.supportArtifacts[1],
  ];
  const expectedItemPaths = requestedArtifacts.map((artifact) => relativeRepoPath(artifact.artifactPath));
  const { result, payload } = runArchiveSwapHarness([
    "-ArchiveRamDispatchModulePath",
    requestedArtifacts[0].artifactPath,
    "-ArchiveRamModulePaths",
    requestedArtifacts.map((artifact) => artifact.artifactPath).join(";"),
    "-ArchiveRamRapidSwapCycles",
    "2",
  ]);

  const itemPaths = payload.archive_ram_swap.modules.items.map((item) => relativeRepoPath(normalizeArtifactPath(item.resolved_path)));

  expect(result.status).toBe(0);
  expect(payload.success).toBe(true);
  expect(payload.archive_ram_swap.pass).toBe(true);
  expect(payload.archive_ram_swap.modules.requested_count).toBe(3);
  expect(payload.archive_ram_swap.modules.rapid_swap_cycles).toBe(2);
  expect(payload.archive_ram_swap.modules.dispatch_events).toBe(6);
  expect(payload.archive_ram_swap.modules.return_events).toBe(6);
  expect(itemPaths).toEqual([...expectedItemPaths, ...expectedItemPaths]);
}, 30_000);

manifestTest("manifest-backed constrained archive pressure only blocks modules that exceed the effective swap budget", () => {
  const effectiveRamCapacityBytes = 40_000;
  const largestModule = [...manifestSuite.supportArtifacts].sort((left, right) => right.sizeBytes - left.sizeBytes)[0];
  const fittingArtifacts = manifestSuite.supportArtifacts.filter((artifact) => artifact.sizeBytes <= effectiveRamCapacityBytes);
  const overflowArtifacts = manifestSuite.supportArtifacts.filter((artifact) => artifact.sizeBytes > effectiveRamCapacityBytes);
  const { result, payload } = runArchiveSwapHarness([
    "-ModuleProgramPathSource",
    "manifest_transfer_suite",
    "-ArchiveRamDispatchModulePath",
    largestModule.artifactPath,
    "-ArchiveRamSwapRamCapacityBytes",
    "45000",
    "-ArchiveRamReservedRamBytes",
    "5000",
  ]);

  const loadedItems = payload.archive_ram_swap.modules.items.filter((item) => item.loaded_to_ram);
  const blockedItems = payload.archive_ram_swap.modules.items.filter((item) => !item.loaded_to_ram);

  expect(fittingArtifacts.length).toBeGreaterThan(0);
  expect(overflowArtifacts.length).toBeGreaterThan(0);
  expect(result.status).toBe(1);
  expect(payload.success).toBe(false);
  expect(payload.archive_ram_swap.pass).toBe(false);
  expect(payload.archive_ram_swap.failure_phase).toBe("dispatch_load");
  expect(payload.archive_ram_swap.modules.requested_count).toBe(manifestSuite.supportArtifacts.length);
  expect(payload.archive_ram_swap.modules.rapid_swap_cycles).toBe(1);
  expect(payload.archive_ram_swap.modules.reserved_ram_bytes).toBe(5_000);
  expect(payload.archive_ram_swap.modules.effective_ram_capacity_bytes).toBe(effectiveRamCapacityBytes);
  expect(payload.archive_ram_swap.modules.dispatch_events).toBe(fittingArtifacts.length);
  expect(payload.archive_ram_swap.modules.return_events).toBe(fittingArtifacts.length);
  expect(payload.archive_ram_swap.modules.corruption_detected).toBe(false);
  expect(payload.archive_ram_swap.modules.partial_load_detected).toBe(false);
  expect(payload.archive_ram_swap.state.module_in_archive).toBe(true);
  expect(payload.archive_ram_swap.state.module_in_ram).toBe(false);
  expect(loadedItems).toHaveLength(fittingArtifacts.length);
  expect(blockedItems).toHaveLength(overflowArtifacts.length);
  expect(loadedItems.every((item) => item.returned_to_archive && item.checksum_before === item.checksum_after)).toBe(true);
  expect(blockedItems.every((item) => item.returned_to_archive === false)).toBe(true);
  expect(payload.archive_ram_swap.errors.join(" | ")).toContain("effective swap RAM capacity is 40000 bytes");
}, 30_000);
