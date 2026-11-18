import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

import {
  AT_RISK_WINDOW_BYTES,
  OFFICIAL_MODULE_LIMIT_BYTES,
  REPO_ROOT,
  TI84_CE_ARCHIVE_BUDGET_BYTES,
  createWindowsAccessibleTempDir,
  formatBytes,
  relativeRepoPath,
  resolveManifestModuleArtifacts,
  runPowerShellScript,
} from "./helpers/split-suite-artifacts.mjs";

const PASS_LABEL = "Pass 92 module size audit";
const harnessScriptPath = path.join(REPO_ROOT, "tools", "emulator", "run-terminal-harness.ps1");
const manifestSuite = resolveManifestModuleArtifacts(REPO_ROOT);
const manifestArtifactsUnavailable = manifestSuite.errors.length > 0 &&
  manifestSuite.errors.every((error) => error.includes("Could not resolve manifest module"));
const TABLE_HEADERS = ["Artifact", "Size", "Limit", "Headroom", "Status"];

function padCell(value, width, alignment = "left") {
  const text = String(value);
  if (text.length >= width) {
    return text;
  }
  return alignment === "right"
    ? `${" ".repeat(width - text.length)}${text}`
    : `${text}${" ".repeat(width - text.length)}`;
}

function renderSizeTable(artifacts) {
  const rows = artifacts.map((artifact) => {
    const headroomBytes = OFFICIAL_MODULE_LIMIT_BYTES - artifact.sizeBytes;
    const riskBoundaryBytes = OFFICIAL_MODULE_LIMIT_BYTES - AT_RISK_WINDOW_BYTES;
    const status = artifact.sizeBytes > OFFICIAL_MODULE_LIMIT_BYTES
      ? "OVER LIMIT"
      : artifact.sizeBytes >= riskBoundaryBytes
        ? "AT RISK"
        : "OK";
    return [
      artifact.artifactName,
      formatBytes(artifact.sizeBytes),
      formatBytes(OFFICIAL_MODULE_LIMIT_BYTES),
      `${headroomBytes >= 0 ? "+" : ""}${formatBytes(headroomBytes)}`,
      status,
    ];
  });
  const widths = TABLE_HEADERS.map((header, columnIndex) => Math.max(
    header.length,
    ...rows.map((row) => String(row[columnIndex]).length),
  ));
  const alignments = ["left", "right", "right", "right", "left"];

  return [
    TABLE_HEADERS.map((header, index) => padCell(header, widths[index], alignments[index])).join(" | "),
    widths.map((width, index) => (
      alignments[index] === "right"
        ? `${"-".repeat(Math.max(width, 3) - 1)}:`
        : `:${"-".repeat(Math.max(width, 3) - 1)}`
    )).join("-|-"),
    ...rows.map((row) => row.map((value, index) => padCell(value, widths[index], alignments[index])).join(" | ")),
  ].join("\n");
}

function renderAuditReport(artifacts, atRiskArtifacts, totalArchiveFootprintBytes, launcherArtifact) {
  const lines = [
    PASS_LABEL,
    `Scanned staged hardware-ready .8xp outputs: ${artifacts.length}`,
    `Per-module limit: ${formatBytes(OFFICIAL_MODULE_LIMIT_BYTES)}`,
    `At-risk threshold: ${formatBytes(OFFICIAL_MODULE_LIMIT_BYTES - AT_RISK_WINDOW_BYTES)}`,
    `Total archive footprint: ${formatBytes(totalArchiveFootprintBytes)}`,
    `Archive budget: ${formatBytes(TI84_CE_ARCHIVE_BUDGET_BYTES)}`,
    `Remaining archive headroom: ${formatBytes(TI84_CE_ARCHIVE_BUDGET_BYTES - totalArchiveFootprintBytes)}`,
    "",
    "Largest-first size report:",
    renderSizeTable(artifacts),
    "",
    launcherArtifact
      ? `Resident launcher: ${launcherArtifact.artifactName} (${relativeRepoPath(launcherArtifact.artifactPath)}) = ${formatBytes(launcherArtifact.sizeBytes)} / ${formatBytes(OFFICIAL_MODULE_LIMIT_BYTES)} (${formatBytes(OFFICIAL_MODULE_LIMIT_BYTES - launcherArtifact.sizeBytes)} headroom)`
      : "Resident launcher: unresolved",
    atRiskArtifacts.length > 0
      ? `At-risk modules: ${atRiskArtifacts.map((artifact) => artifact.artifactName).join(", ")}`
      : "At-risk modules: none",
  ];

  return lines.join("\n");
}

function createSizedArtifact(directoryPath, fileName, sizeBytes) {
  const artifactPath = path.join(directoryPath, fileName);
  fs.writeFileSync(artifactPath, Buffer.alloc(sizeBytes, 0x43));
  return artifactPath;
}

function runModuleSizeValidation(modulePaths, limitBytes = OFFICIAL_MODULE_LIMIT_BYTES) {
  const result = runPowerShellScript(harnessScriptPath, [
    "-Keys",
    "2,+,3,enter",
    "-Format",
    "json",
    "-ForceMock",
    "-ModuleProgramPaths",
    modulePaths.join(";"),
    "-ModuleLinkedBinaryLimitBytes",
    String(limitBytes),
  ]);

  if (!(result.stdout ?? "").trim()) {
    throw new Error(`Module size harness returned empty stdout: status=${result.status}, signal=${result.signal}, stderr=${result.stderr ?? ""}`);
  }
  return { result, payload: JSON.parse(result.stdout) };
}

function collectModuleSizeAudit(suite = manifestSuite) {
  const sortedArtifacts = [...suite.artifacts].sort((left, right) => {
    if (right.sizeBytes !== left.sizeBytes) {
      return right.sizeBytes - left.sizeBytes;
    }

    return left.artifactName.localeCompare(right.artifactName);
  });
  const totalArchiveFootprintBytes = sortedArtifacts.reduce((sum, artifact) => sum + artifact.sizeBytes, 0);
  const launcherArtifact = sortedArtifacts.find((artifact) => artifact.launcher) ?? null;
  const oversizeArtifacts = sortedArtifacts.filter((artifact) => artifact.sizeBytes > OFFICIAL_MODULE_LIMIT_BYTES);
  const atRiskArtifacts = sortedArtifacts.filter(
    (artifact) => artifact.sizeBytes >= OFFICIAL_MODULE_LIMIT_BYTES - AT_RISK_WINDOW_BYTES,
  );

  return {
    suite,
    sortedArtifacts,
    totalArchiveFootprintBytes,
    launcherArtifact,
    oversizeArtifacts,
    atRiskArtifacts,
    report: renderAuditReport(sortedArtifacts, atRiskArtifacts, totalArchiveFootprintBytes, launcherArtifact),
  };
}

function collectAuditFailures(audit) {
  const failures = [];

  if (audit.suite.errors.length > 0) {
    failures.push(...audit.suite.errors);
  }
  if (audit.sortedArtifacts.length !== audit.suite.manifest.module_count) {
    failures.push(
      `Resolved ${audit.sortedArtifacts.length} manifest modules but expected ${audit.suite.manifest.module_count}.`,
    );
  }
  if (audit.oversizeArtifacts.length > 0) {
    failures.push(
      `Oversize modules detected: ${audit.oversizeArtifacts.map((artifact) => `${artifact.artifactName}=${artifact.sizeBytes}`).join(", ")}.`,
    );
  }
  if (audit.totalArchiveFootprintBytes > TI84_CE_ARCHIVE_BUDGET_BYTES) {
    failures.push(
      `Archive footprint ${audit.totalArchiveFootprintBytes} exceeds budget ${TI84_CE_ARCHIVE_BUDGET_BYTES}.`,
    );
  }
  if (!audit.launcherArtifact) {
    failures.push("Resident launcher CALCENG.8xp could not be resolved from the manifest-backed artifact set.");
  } else if (audit.launcherArtifact.sizeBytes > OFFICIAL_MODULE_LIMIT_BYTES) {
    failures.push(
      `Resident launcher ${audit.launcherArtifact.artifactName} is ${audit.launcherArtifact.sizeBytes} bytes, exceeding the ${OFFICIAL_MODULE_LIMIT_BYTES}-byte ceiling by ${audit.launcherArtifact.sizeBytes - OFFICIAL_MODULE_LIMIT_BYTES} bytes.`,
    );
  }

  return failures;
}

function printAuditWarnings(warnings) {
  for (const warning of warnings) {
    console.warn(`[WARN] ${warning}`);
  }
}

function runStandaloneAudit() {
  if (manifestArtifactsUnavailable) {
    console.error(
      `[FAIL] module-size audit could not resolve packaged split-suite artifacts: ${manifestSuite.errors.join(" | ")}`,
    );
    return 1;
  }

  const audit = collectModuleSizeAudit(manifestSuite);
  printAuditWarnings(audit.suite.warnings);
  console.log(audit.report);

  const failures = collectAuditFailures(audit);
  if (failures.length > 0) {
    for (const failure of failures) {
      console.error(`[FAIL] ${failure}`);
    }
    return 1;
  }

  return 0;
}

const executedDirectly = process.argv[1]
  ? import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href
  : false;

if (process.env.VITEST) {
  const { expect, test } = await import("vitest");
  const manifestTest = manifestArtifactsUnavailable ? test.skip : test;

  if (manifestArtifactsUnavailable) {
    console.warn(
      `[SKIP] module-size audit skipped because packaged split-suite artifacts are absent: ${manifestSuite.errors.join(" | ")}`,
    );
  }

  manifestTest("staged hardware-ready .8xp outputs stay under the current 51,200-byte module limit and fit the TI-84 CE archive budget", () => {
    const audit = collectModuleSizeAudit(manifestSuite);

    console.log(audit.report);

    expect(audit.sortedArtifacts.length).toBe(manifestSuite.manifest.module_count);
    expect(
      audit.oversizeArtifacts,
      audit.oversizeArtifacts.map((artifact) => `${artifact.artifactName}=${artifact.sizeBytes}`).join(", "),
    ).toEqual([]);
    expect(audit.totalArchiveFootprintBytes).toBeLessThanOrEqual(TI84_CE_ARCHIVE_BUDGET_BYTES);
    expect(audit.sortedArtifacts[0].sizeBytes).toBeGreaterThanOrEqual(audit.sortedArtifacts.at(-1).sizeBytes);
    expect(audit.launcherArtifact).not.toBeNull();
    expect(audit.launcherArtifact.sizeBytes).toBeLessThanOrEqual(OFFICIAL_MODULE_LIMIT_BYTES);
  }, 30_000);

  manifestTest("resident launcher CALCENG.8xp stays under the current TI-84 CE resident-size ceiling with an exact byte-count guard", () => {
    const audit = collectModuleSizeAudit(manifestSuite);

    expect(audit.launcherArtifact).not.toBeNull();
    expect(
      audit.launcherArtifact.sizeBytes,
      audit.launcherArtifact
        ? `CALCENG.8xp is ${audit.launcherArtifact.sizeBytes} bytes; limit is ${OFFICIAL_MODULE_LIMIT_BYTES} bytes.`
        : "CALCENG.8xp could not be resolved from the manifest-backed artifact set.",
    ).toBeLessThanOrEqual(OFFICIAL_MODULE_LIMIT_BYTES);
  }, 30_000);

  test("manifest artifact resolution prefers the staged hardware-ready package over mismatched bin artifacts", () => {
    const tempRoot = createWindowsAccessibleTempDir("calcengine-staged-artifact-pass-", REPO_ROOT);

    try {
      const stagedDir = path.join(tempRoot, "deploy", "hardware-ready");
      const binDir = path.join(tempRoot, "bin", "algsy");
      const manifestDir = path.join(tempRoot, "tests", "emulator");
      fs.mkdirSync(stagedDir, { recursive: true });
      fs.mkdirSync(binDir, { recursive: true });
      fs.mkdirSync(manifestDir, { recursive: true });

      const stagedPath = path.join(stagedDir, "CEALGSY.8xp");
      const binPath = path.join(binDir, "CEALGSY.8xp");
      fs.writeFileSync(stagedPath, Buffer.alloc(100, 0x53));
      fs.writeFileSync(binPath, Buffer.alloc(120, 0x42));
      fs.writeFileSync(
        path.join(manifestDir, "manifest.json"),
        JSON.stringify(
          {
            module_count: 1,
            runtime_dependency_count: 0,
            modules: [{ name: "CEALGSY.8xp", size_bytes: 100, crc32: "" }],
            runtime_dependencies: [],
          },
          null,
          2,
        ),
        "utf8",
      );

      const suite = resolveManifestModuleArtifacts(tempRoot);

      expect(suite.errors).toEqual([]);
      expect(suite.artifacts).toHaveLength(1);
      expect(relativeRepoPath(suite.artifacts[0].artifactPath, tempRoot)).toBe("deploy/hardware-ready/CEALGSY.8xp");
      expect(suite.artifacts[0].sizeBytes).toBe(100);
    } finally {
      fs.rmSync(tempRoot, { recursive: true, force: true });
    }
  });

  test("module size validation accepts binaries exactly at the current official ceiling", () => {
    const tempRoot = createWindowsAccessibleTempDir("calcengine-module-limit-pass-", REPO_ROOT);

    try {
      const belowLimitPath = createSizedArtifact(tempRoot, "CEPASS1.8xp", OFFICIAL_MODULE_LIMIT_BYTES - 1);
      const atLimitPath = createSizedArtifact(tempRoot, "CEPASS2.8xp", OFFICIAL_MODULE_LIMIT_BYTES);
      const { result, payload } = runModuleSizeValidation([belowLimitPath, atLimitPath]);

      expect(result.status).toBe(0);
      expect(payload.success).toBe(true);
      expect(payload.hardware_constraints.validated).toBe(true);
      expect(payload.hardware_constraints.pass).toBe(true);
      expect(payload.hardware_constraints.module_size_exceeded).toBe(false);
      expect(payload.hardware_constraints.modules.checked_count).toBe(2);
      expect(payload.hardware_constraints.modules.over_limit_count).toBe(0);
      expect(payload.hardware_constraints.modules.items.map((item) => item.size_bytes)).toEqual([
        OFFICIAL_MODULE_LIMIT_BYTES - 1,
        OFFICIAL_MODULE_LIMIT_BYTES,
      ]);
      expect(payload.hardware_constraints.modules.items.every((item) => item.within_limit)).toBe(true);
    } finally {
      fs.rmSync(tempRoot, { recursive: true, force: true });
    }
  }, 30_000);

  test("module size validation rejects binaries that exceed the current official ceiling by a single byte", () => {
    const tempRoot = createWindowsAccessibleTempDir("calcengine-module-limit-fail-", REPO_ROOT);

    try {
      const oversizePath = createSizedArtifact(tempRoot, "CEFAIL1.8xp", OFFICIAL_MODULE_LIMIT_BYTES + 1);
      const { result, payload } = runModuleSizeValidation([oversizePath]);

      expect(result.status).toBe(1);
      expect(payload.success).toBe(false);
      expect(payload.hardware_constraints.validated).toBe(true);
      expect(payload.hardware_constraints.pass).toBe(false);
      expect(payload.hardware_constraints.module_size_exceeded).toBe(true);
      expect(payload.hardware_constraints.modules.checked_count).toBe(1);
      expect(payload.hardware_constraints.modules.over_limit_count).toBe(1);
      expect(payload.hardware_constraints.modules.items[0].within_limit).toBe(false);
      expect(payload.hardware_constraints.modules.items[0].size_bytes).toBe(OFFICIAL_MODULE_LIMIT_BYTES + 1);
      expect(payload.hardware_constraints.errors.join(" | ")).toContain(
        `must be <= ${OFFICIAL_MODULE_LIMIT_BYTES} bytes`,
      );
    } finally {
      fs.rmSync(tempRoot, { recursive: true, force: true });
    }
  }, 30_000);
}

if (executedDirectly) {
  process.exit(runStandaloneAudit());
}
