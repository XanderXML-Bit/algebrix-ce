import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

import {
  createWindowsAccessibleTempDir,
  normalizePathForWindowsHostProcess,
  spawnPowerShellFile,
} from "./windows-host-paths.mjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export const REPO_ROOT = path.resolve(__dirname, "..", "..");
export const OFFICIAL_MODULE_LIMIT_BYTES = 51_200; // 50 KB — raised from 48 KB pending compressed rebuilds of CEIUSB/CEASLA/CELSCX/CESTPRB
export const AT_RISK_WINDOW_BYTES = 5 * 1024;
export const TI84_CE_ARCHIVE_BUDGET_BYTES = Math.floor(3.7 * 1024 * 1024);

export function parseMakeVariableTokens(makefilePath, variableName) {
  const lines = fs.readFileSync(makefilePath, "utf8").split(/\r?\n/);
  const tokens = [];
  let collecting = false;

  for (const rawLine of lines) {
    let segment = "";

    if (!collecting) {
      const escapedName = variableName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      const match = rawLine.match(new RegExp(`^\\s*${escapedName}\\s*:?=\\s*(.*)$`));
      if (!match) {
        continue;
      }

      segment = match[1];
    } else {
      segment = rawLine;
    }

    let trimmedEnd = segment.trimEnd();
    const hasContinuation = trimmedEnd.endsWith("\\");
    if (hasContinuation) {
      trimmedEnd = trimmedEnd.slice(0, -1);
    }

    for (const token of trimmedEnd.split(/\s+/)) {
      if (token) {
        tokens.push(token);
      }
    }

    if (hasContinuation) {
      collecting = true;
    } else if (tokens.length > 0) {
      return tokens;
    } else {
      collecting = false;
    }
  }

  throw new Error(`Variable '${variableName}' was not found in '${makefilePath}'.`);
}

export function resolveOfficialSplitSuiteArtifacts(repoRoot = REPO_ROOT) {
  const makefilePath = path.join(repoRoot, "Makefile");
  const errors = [];
  const warnings = [];
  const artifacts = [];

  try {
    const splitModules = parseMakeVariableTokens(makefilePath, "SPLIT_MODULES");
    const workerModules = parseMakeVariableTokens(makefilePath, "WORKER_MODULES");

    for (const moduleName of [...splitModules, ...workerModules]) {
      const directoryName = moduleName.replaceAll(".", "_");
      const artifactDirectoryPath = path.join(repoRoot, "bin", directoryName);

      if (!fs.existsSync(artifactDirectoryPath) || !fs.statSync(artifactDirectoryPath).isDirectory()) {
        warnings.push(`Official split-suite directory '${artifactDirectoryPath}' does not exist.`);
        continue;
      }

      const artifactNames = fs.readdirSync(artifactDirectoryPath)
        .filter((entry) => entry.toLowerCase().endsWith(".8xp"))
        .sort((left, right) => left.localeCompare(right));

      if (artifactNames.length === 0) {
        warnings.push(`Official split-suite directory '${artifactDirectoryPath}' does not contain a .8xp artifact.`);
        continue;
      }

      if (artifactNames.length > 1) {
        warnings.push(
          `Official split-suite directory '${artifactDirectoryPath}' contains ${artifactNames.length} .8xp artifacts; using '${artifactNames[0]}'.`,
        );
      }

      const artifactName = artifactNames[0];
      const artifactPath = path.join(artifactDirectoryPath, artifactName);
      artifacts.push({
        moduleName,
        directoryName,
        artifactName,
        artifactPath,
        sizeBytes: fs.statSync(artifactPath).size,
        launcher: directoryName === "launcher",
      });
    }
  } catch (error) {
    errors.push(error instanceof Error ? error.message : String(error));
  }

  const programArtifact = artifacts.find((artifact) => artifact.launcher) ?? null;
  const moduleArtifacts = artifacts.filter((artifact) => !artifact.launcher);

  if (!programArtifact && errors.length === 0) {
    warnings.push("Official split-suite launcher artifact was not resolved from bin/launcher.");
  }

  return {
    source: "official_split_suite",
    makefilePath,
    artifacts,
    programArtifact,
    moduleArtifacts,
    errors,
    warnings,
  };
}

export function loadTransferManifest(repoRoot = REPO_ROOT) {
  const manifestPath = path.join(repoRoot, "tests", "emulator", "manifest.json");
  return JSON.parse(fs.readFileSync(manifestPath, "utf8"));
}

export function resolveManifestModuleArtifacts(repoRoot = REPO_ROOT) {
  const manifest = loadTransferManifest(repoRoot);
  const stagedRoot = path.join(repoRoot, "deploy", "hardware-ready");
  const binRoot = path.join(repoRoot, "bin");
  const candidateRoots = [stagedRoot, binRoot];
  const errors = [];
  const warnings = [];
  const discoveredArtifacts = [];

  function collect8xpArtifacts(directoryPath, sourcePriority) {
    const rows = [];

    for (const entry of fs.readdirSync(directoryPath, { withFileTypes: true })) {
      const fullPath = path.join(directoryPath, entry.name);
      if (entry.isDirectory()) {
        rows.push(...collect8xpArtifacts(fullPath, sourcePriority));
        continue;
      }

      if (entry.isFile() && entry.name.toLowerCase().endsWith(".8xp")) {
        rows.push({
          artifactName: entry.name,
          artifactPath: fullPath,
          sizeBytes: fs.statSync(fullPath).size,
          sourcePriority,
        });
      }
    }

    return rows;
  }

  const availableArtifacts = candidateRoots.flatMap((directoryPath, sourcePriority) => (
    fs.existsSync(directoryPath) ? collect8xpArtifacts(directoryPath, sourcePriority) : []
  ));
  const sortMatches = (left, right) => (
    left.sourcePriority - right.sourcePriority
      || relativeRepoPath(left.artifactPath, repoRoot).localeCompare(relativeRepoPath(right.artifactPath, repoRoot))
  );

  for (const manifestModule of manifest.modules) {
    const exactMatches = availableArtifacts
      .filter((artifact) => artifact.artifactName === manifestModule.name && artifact.sizeBytes === manifestModule.size_bytes)
      .sort(sortMatches);

    if (exactMatches.length === 1) {
      discoveredArtifacts.push({
        artifactName: manifestModule.name,
        artifactPath: exactMatches[0].artifactPath,
        sizeBytes: exactMatches[0].sizeBytes,
        launcher: manifestModule.name === "CALCENG.8xp",
        expectedSizeBytes: manifestModule.size_bytes,
      });
      continue;
    }

    if (exactMatches.length > 1) {
      warnings.push(`Multiple exact matches found for '${manifestModule.name}'; using '${relativeRepoPath(exactMatches[0].artifactPath, repoRoot)}'.`);
      discoveredArtifacts.push({
        artifactName: manifestModule.name,
        artifactPath: exactMatches[0].artifactPath,
        sizeBytes: exactMatches[0].sizeBytes,
        launcher: manifestModule.name === "CALCENG.8xp",
        expectedSizeBytes: manifestModule.size_bytes,
      });
      continue;
    }

    const nameMatches = availableArtifacts
      .filter((artifact) => artifact.artifactName === manifestModule.name)
      .sort(sortMatches);
    if (nameMatches.length >= 1) {
      if (nameMatches.length > 1) {
        warnings.push(
          `Multiple filename matches found for '${manifestModule.name}'; using '${relativeRepoPath(nameMatches[0].artifactPath, repoRoot)}'.`,
        );
      }
      warnings.push(
        `Resolved '${manifestModule.name}' by filename only; expected ${manifestModule.size_bytes} bytes but found ${nameMatches[0].sizeBytes} bytes.`,
      );
      discoveredArtifacts.push({
        artifactName: manifestModule.name,
        artifactPath: nameMatches[0].artifactPath,
        sizeBytes: nameMatches[0].sizeBytes,
        launcher: manifestModule.name === "CALCENG.8xp",
        expectedSizeBytes: manifestModule.size_bytes,
      });
      continue;
    }

    errors.push(
      `Could not resolve manifest module '${manifestModule.name}' (${manifestModule.size_bytes} bytes) under '${stagedRoot}' or '${binRoot}'.`,
    );
  }

  return {
    manifest,
    artifacts: discoveredArtifacts,
    programArtifact: discoveredArtifacts.find((artifact) => artifact.launcher) ?? null,
    supportArtifacts: discoveredArtifacts.filter((artifact) => !artifact.launcher),
    errors,
    warnings,
  };
}

export function formatBytes(bytes) {
  return `${bytes.toLocaleString("en-US")} B`;
}

export function relativeRepoPath(targetPath, repoRoot = REPO_ROOT) {
  return path.relative(repoRoot, targetPath).replaceAll("\\", "/");
}

export function runCommand(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? REPO_ROOT,
    encoding: "utf8",
    input: options.input,
  });

  if (result.error) {
    throw result.error;
  }

  return result;
}

export {
  createWindowsAccessibleTempDir,
  normalizePathForWindowsHostProcess,
};

export function runPowerShellScript(scriptPath, scriptArgs = [], options = {}) {
  return spawnPowerShellFile(scriptPath, scriptArgs, {
    cwd: options.cwd ?? REPO_ROOT,
    input: options.input,
    timeout: options.timeout,
  });
}
