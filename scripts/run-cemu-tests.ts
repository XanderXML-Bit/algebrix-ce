#!/usr/bin/env node

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { runCEmuSmokeSuite, type CEmuSmokeSuiteSummary } from "../src/testing/cemu-smoke-tests.ts";

interface CliOptions {
  outputPath: string;
  outputRoot: string;
  keepTempDir: boolean;
  jsonOnly: boolean;
}

function buildDefaultArtifactPaths(): { outputPath: string; outputRoot: string } {
  const explicitOutputPath = process.env.CALCENGINE_CEMU_SUMMARY_PATH?.trim();
  const explicitOutputRoot = process.env.CALCENGINE_CEMU_OUTPUT_ROOT?.trim();

  if (explicitOutputPath || explicitOutputRoot) {
    const outputRoot = explicitOutputRoot
      ? path.resolve(explicitOutputRoot)
      : path.dirname(path.resolve(explicitOutputPath ?? "tests/cemu/outputs/ts-cemu-smoke-summary.json"));
    const outputPath = explicitOutputPath
      ? path.resolve(explicitOutputPath)
      : path.join(outputRoot, "summary.json");
    return { outputPath, outputRoot };
  }

  const useEphemeralArtifacts = process.env.CI === "true" || process.env.GITHUB_ACTIONS === "true" || process.env.VITEST === "true";
  if (useEphemeralArtifacts) {
    const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-cemu-smoke-"));
    return {
      outputPath: path.join(outputRoot, "summary.json"),
      outputRoot,
    };
  }

  return {
    outputPath: path.resolve("tests/cemu/outputs/ts-cemu-smoke-summary.json"),
    outputRoot: path.resolve("tests/cemu/outputs/ts-cemu-smoke"),
  };
}

function parseArgs(argv: string[]): CliOptions {
  const defaults = buildDefaultArtifactPaths();
  let outputPath = defaults.outputPath;
  let outputRoot = defaults.outputRoot;
  let keepTempDir = false;
  let jsonOnly = false;

  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index];
    if (value === "--output" && argv[index + 1]) {
      outputPath = path.resolve(argv[index + 1]);
      index += 1;
      continue;
    }
    if (value === "--evidence-dir" && argv[index + 1]) {
      outputRoot = path.resolve(argv[index + 1]);
      index += 1;
      continue;
    }
    if (value === "--keep-temp") {
      keepTempDir = true;
      continue;
    }
    if (value === "--json") {
      jsonOnly = true;
      continue;
    }
    if (value === "--help" || value === "-h") {
      console.log("Usage: node --experimental-strip-types scripts/run-cemu-tests.ts [--output <file>] [--evidence-dir <dir>] [--keep-temp] [--json]");
      process.exit(0);
    }
  }

  return {
    outputPath,
    outputRoot,
    keepTempDir,
    jsonOnly,
  };
}

function ensureParentDirectory(filePath: string): void {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
}

function writeSummary(summary: CEmuSmokeSuiteSummary, outputPath: string): void {
  ensureParentDirectory(outputPath);
  fs.writeFileSync(outputPath, `${JSON.stringify(summary, null, 2)}\n`, "utf8");
}

function printSummary(summary: CEmuSmokeSuiteSummary, outputPath: string): void {
  console.log("CalcEngine CEmu Smoke Suite");
  console.log(`Summary         : ${outputPath}`);
  console.log(`Evidence        : ${summary.outputRoot}`);
  console.log(`ROM             : ${summary.environment.romPath ?? "missing"}`);
  console.log(`Headless        : ${summary.environment.headlessPath ?? "missing"}`);
  console.log(`AutoTester      : ${summary.environment.autotesterPath ?? "missing"}`);
  console.log(`CEmu GUI        : ${summary.environment.guiPath ?? "not required"}`);
  console.log(`Transfer files  : ${summary.environment.transferCount}`);
  console.log(`Overall         : ${summary.status.toUpperCase()}`);
  console.log("");

  for (const result of summary.results) {
    console.log(`${result.name.padEnd(32, " ")} ${result.status.toUpperCase()}`);
    for (const checkpoint of result.checkpoints) {
      const stateLabel = path.basename(checkpoint.stateSnapshotPath);
      console.log(`  ${checkpoint.name.padEnd(28, " ")} ${checkpoint.passed ? "PASS" : "FAIL"}  ${checkpoint.observedCrc}  ${stateLabel}`);
    }
    if (result.error) {
      console.log(`  error: ${result.error}`);
    }
    console.log(`  evidence: ${result.evidenceDir}`);
  }

  if (summary.blockers.length > 0) {
    console.log("");
    console.log("Blockers:");
    for (const blocker of summary.blockers) {
      console.log(`  - ${blocker}`);
    }
  }
}

const options = parseArgs(process.argv.slice(2));
const summary = await runCEmuSmokeSuite({
  outputRoot: options.outputRoot,
  keepTempDir: options.keepTempDir,
});

writeSummary(summary, options.outputPath);

if (options.jsonOnly) {
  process.stdout.write(`${JSON.stringify(summary, null, 2)}\n`);
} else {
  printSummary(summary, options.outputPath);
}

if (summary.status === "failed") {
  process.exit(1);
}

process.exit(0);
