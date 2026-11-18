import path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { resolveNativeSuite } from "./tests/helpers/native-suite.mjs";
import { isWindowsHostPowerShellExecutable, resolvePowerShellExecutable, spawnPowerShellFile } from "./tests/helpers/windows-host-paths.mjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const hasWindowsHostPowerShell = (() => {
  try {
    return isWindowsHostPowerShellExecutable(resolvePowerShellExecutable());
  } catch {
    return false;
  }
})();

const ez80Suite = resolveNativeSuite({
  key: "ez80-runtime",
  label: "eZ80 scaffold runtime suite",
  repoRoot: __dirname,
  buildScript: "build:ez80",
  artifactPath: path.join("tools", "emulator", "ez80", "tests", "ez80_test_runner.exe"),
  preflightSkipReason: () => hasWindowsHostPowerShell
    ? ""
    : "Windows-host PowerShell is unavailable.",
});
const ez80Test = ez80Suite.available ? test : test.skip;

ez80Test("ez80 scaffold runtime suite passes", () => {
  const result = spawnPowerShellFile(
    path.join(__dirname, "tools", "emulator", "tests", "run-ez80-scaffold-tests.ps1"),
    [],
    { cwd: __dirname },
  );
  const combinedOutput = `${result.stdout}\n${result.stderr}`;
  const runtimeSummary = combinedOutput.match(/PASS: all runtime scaffold tests passed\. \((\d+) assertions\)/);

  expect(result.status).toBe(0);
  expect(combinedOutput).toContain("PASS: eZ80 scaffold contract satisfied.");
  expect(runtimeSummary).not.toBeNull();
  expect(Number(runtimeSummary[1])).toBeGreaterThan(0);
  expect(combinedOutput).toContain("PASS: eZ80 scaffold runtime tests passed.");
}, 120000);
