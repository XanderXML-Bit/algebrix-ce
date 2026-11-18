import path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { resolveNativeSuite, runCommand } from "./tests/helpers/native-suite.mjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const desktopSuite = resolveNativeSuite({
  key: "desktop-runner",
  label: "desktop C regression suite",
  repoRoot: __dirname,
  buildScript: "build:desktop",
  artifactPath: path.join("tests", "test_runner.exe"),
});
const desktopTest = desktopSuite.available ? test : test.skip;

function parseFinalResults(output) {
  const matches = [...output.matchAll(/Results:\s+(\d+)\s+passed,\s+(\d+)\s+failed,\s+(\d+)\s+total/g)];
  if (matches.length === 0) {
    return null;
  }

  const lastMatch = matches.at(-1);
  return {
    passed: Number(lastMatch[1]),
    failed: Number(lastMatch[2]),
    total: Number(lastMatch[3]),
  };
}

desktopTest("desktop C regression suite passes", () => {
  const runnerDir = path.join(__dirname, "tests");
  const result = runCommand(desktopSuite.artifactPath, [], runnerDir);
  const combinedOutput = `${result.stdout}\n${result.stderr}`;
  const summary = parseFinalResults(combinedOutput);

  expect(result.status).toBe(0);
  expect(summary).not.toBeNull();
  expect(summary.failed).toBe(0);
  expect(summary.passed).toBe(summary.total);
  expect(summary.total).toBeGreaterThanOrEqual(8244);
}, 120000);
