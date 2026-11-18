import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { resolveNativeSuite, runCommand } from "./tests/helpers/native-suite.mjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

let desktopBuilt = false;
const desktopSuite = resolveNativeSuite({
  key: "desktop-cli",
  label: "desktop CLI suite",
  repoRoot: __dirname,
  buildScript: "build:desktop",
  artifactPath: path.join("bin", "calcengine.exe"),
});
const desktopTest = desktopSuite.available ? test : test.skip;

function ensureDesktopBuilt() {
  if (desktopBuilt) {
    return;
  }

  expect(desktopSuite.available).toBe(true);
  desktopBuilt = true;
}

function runCli(args) {
  ensureDesktopBuilt();
  return runCommand(desktopSuite.artifactPath, args, __dirname);
}

desktopTest("one-shot solve prints direct result", () => {
  const result = runCli(["--solve", "2+3"]);
  const stdout = result.stdout.trim();

  expect(result.status).toBe(0);
  expect(stdout).toContain("= 5");
  expect(stdout).toContain("Parsed: 2+3");
  expect(stdout).not.toContain("Steps:");
}, 15_000);

desktopTest("show-steps exposes derivative reasoning", () => {
  const result = runCli(["--solve", "d/dx(x^2)", "--show-steps"]);
  const stdout = result.stdout;

  expect(result.status).toBe(0);
  expect(stdout).toContain("d/dx = 2x");
  expect(stdout).toContain("Steps:");
  expect(stdout).toContain("Power rule");
  expect(stdout).toContain("= (2*x)");
});

desktopTest("json mode includes routing metadata and structured steps", () => {
  const result = runCli([
    "--solve",
    "int(1/(x*(x+1)),x)",
    "--method=partial-frac",
    "--show-steps",
    "--json",
  ]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(0);
  expect(payload.ok).toBe(true);
  expect(payload.domain).toBe("integrals");
  expect(payload.program).toBe("CECALC");
  expect(payload.result).toContain("ln(abs(x))");
  expect(payload.step_count).toBeGreaterThanOrEqual(4);
  expect(payload.steps.some((step) => step.includes("Partial fractions"))).toBe(true);
});

desktopTest("json mode suppresses steps unless show-steps is requested", () => {
  const result = runCli([
    "--solve",
    "int(1/(x*(x+1)),x)",
    "--method=partial-frac",
    "--json",
  ]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(0);
  expect(payload.ok).toBe(true);
  expect(payload.show_steps).toBe(false);
  expect(payload.step_count).toBe(0);
  expect(payload.steps).toEqual([]);
  expect(payload.checkpoints).toEqual([]);
});

desktopTest("one-shot CLI accepts equals-form solve and split method args", () => {
  const result = runCli([
    "--solve=int(x*sin(x),x)",
    "--method",
    "parts",
    "--show-steps",
    "--json",
  ]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(0);
  expect(payload.ok).toBe(true);
  expect(payload.show_steps).toBe(true);
  expect(payload.result).toContain("sin(x)");
  expect(payload.steps.some((step) => step.toLowerCase().includes("by parts"))).toBe(true);
});

desktopTest("json mode solves edge-case algebra routing with structured output", () => {
  const result = runCli([
    "--solve",
    "x^2-5*x+6=0",
    "--json",
  ]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(0);
  expect(payload.ok).toBe(true);
  expect(payload.domain).toBe("algebra");
  expect(payload.result).toMatch(/2/);
  expect(payload.result).toMatch(/3/);
});

desktopTest("json mode preserves by-parts step ordering for definite integrals", () => {
  const result = runCli([
    "--solve",
    "int(x*sin(x),x,0,pi)",
    "--method=parts",
    "--show-steps",
    "--json",
  ]);
  const payload = JSON.parse(result.stdout);
  const byPartsIndex = payload.steps.findIndex((step) => step.includes("By parts"));
  const upperBoundIndex = payload.steps.findIndex((step) => step.includes("Evaluate at upper bound"));
  const lowerBoundIndex = payload.steps.findIndex((step) => step.includes("Evaluate at lower bound"));
  const subtractIndex = payload.steps.findIndex((step) => step.startsWith("Subtract:"));

  expect(result.status).toBe(0);
  expect(payload.ok).toBe(true);
  expect(payload.domain).toBe("integrals");
  expect(payload.show_steps).toBe(true);
  expect(payload.result).toContain("3.14159");
  expect(byPartsIndex).toBeGreaterThanOrEqual(0);
  expect(upperBoundIndex).toBeGreaterThan(byPartsIndex);
  expect(lowerBoundIndex).toBeGreaterThan(upperBoundIndex);
  expect(subtractIndex).toBeGreaterThan(lowerBoundIndex);
});

desktopTest("json mode preserves bounded-radical step ordering for definite integrals", () => {
  const result = runCli([
    "--solve",
    "int(1/sqrt(1-x^2),x,0,0.5)",
    "--method=trigsub",
    "--show-steps",
    "--json",
  ]);
  const payload = JSON.parse(result.stdout);
  const antiderivativeIndex = payload.steps.findIndex((step) => step.includes("asin(x)"));
  const upperBoundIndex = payload.steps.findIndex((step) => step.includes("Evaluate at upper bound"));
  const lowerBoundIndex = payload.steps.findIndex((step) => step.includes("Evaluate at lower bound"));
  const subtractIndex = payload.steps.findIndex((step) => step.startsWith("Subtract:"));

  expect(result.status).toBe(0);
  expect(payload.ok).toBe(true);
  expect(payload.domain).toBe("integrals");
  expect(payload.result).toContain("0.523599");
  expect(antiderivativeIndex).toBeGreaterThanOrEqual(0);
  expect(upperBoundIndex).toBeGreaterThan(antiderivativeIndex);
  expect(lowerBoundIndex).toBeGreaterThan(upperBoundIndex);
  expect(subtractIndex).toBeGreaterThan(lowerBoundIndex);
});

desktopTest("json mode reports parse failures cleanly", () => {
  const result = runCli(["--solve", "(", "--json"]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(1);
  expect(payload.ok).toBe(false);
  expect(payload.error.length).toBeGreaterThan(0);
});

desktopTest("unsupported method exits with an error", () => {
  const result = runCli(["--solve", "int(x,x)", "--method=bogus"]);
  const stderr = `${result.stdout}\n${result.stderr}`;

  expect(result.status).toBe(1);
  expect(stderr).toContain("unsupported method 'bogus'");
});

desktopTest("help exits successfully", () => {
  const result = runCli(["--help"]);

  expect(result.status).toBe(0);
  expect(result.stdout).toContain("calcengine -- one-shot desktop solver");
  expect(result.stdout).toContain("--solve <expression>");
});
