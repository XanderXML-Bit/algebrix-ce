import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { resolveNativeSuite, runCommand } from "./tests/helpers/native-suite.mjs";
import { createWindowsAccessibleTempDir, normalizePathForWindowsHostProcess } from "./tests/helpers/windows-host-paths.mjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

let ez80Built = false;
const ez80Suite = resolveNativeSuite({
  key: "ez80-cli",
  label: "eZ80 CLI suite",
  repoRoot: __dirname,
  buildScript: "build:ez80",
  artifactPath: path.join("tools", "emulator", "ez80", "ez80_cli.exe"),
  requiredPaths: [path.join("rom", "Rom.rom")],
});
const ez80Test = ez80Suite.available ? test : test.skip;
const archivedEz80Test = ez80Suite.available &&
  fs.existsSync(path.join(__dirname, "bin", "core", "CALCENG.8xp")) &&
  fs.existsSync(path.join(__dirname, "bin", "calculus", "CECALC.8xp"))
  ? test
  : test.skip;

function ensureEz80Built() {
  if (ez80Built) {
    return;
  }

  expect(ez80Suite.available).toBe(true);
  ez80Built = true;
}

function runEz80(args, options = {}) {
  ensureEz80Built();
  return runCommand(
    ez80Suite.artifactPath,
    args,
    __dirname,
    options,
  );
}

function runEz80InteractiveRaw(lines) {
  const input = [...lines, ""].join("\n");
  const result = runEz80(["--interactive"], { input });
  const outputLines = result.stdout
    .trim()
    .split(/\r?\n/)
    .filter(Boolean);

  return { result, lines: outputLines };
}

function runEz80Interactive(commands) {
  const { result, lines } = runEz80InteractiveRaw(commands.map((command) => JSON.stringify(command)));

  return { result, lines: lines.map((line) => JSON.parse(line)) };
}

function responseById(lines, id) {
  return lines.find((line) => line.id === id);
}

ez80Test("ez80 CLI help exposes interactive, screen, and program listing modes", () => {
  const result = runEz80(["--help"]);

  expect(result.status).toBe(0);
  expect(result.stdout).toContain("--interactive");
  expect(result.stdout).toContain("--screen-state");
  expect(result.stdout).toContain("--list-programs");
  expect(result.stdout).toContain("--screenshot-png");
});

ez80Test("ez80 CLI JSON mode exports screen state, PNGs, and empty program inventory", () => {
  const tmpDir = createWindowsAccessibleTempDir("calcengine-ez80-", __dirname);
  const pngPath = path.join(tmpDir, "lcd.png");
  const screenshotArgumentPath = /\.exe$/i.test(ez80Suite.artifactPath)
    ? normalizePathForWindowsHostProcess(pngPath)
    : pngPath;

  try {
    const result = runEz80([
      "--rom",
      path.join("rom", "Rom.rom"),
      "--run",
      "64",
      "--json",
      "--screen-state",
      "--dump-state",
      "--list-programs",
      "--screenshot-png",
      screenshotArgumentPath,
      "--quiet",
    ]);
    const payload = JSON.parse(result.stdout);
    const signature = fs.readFileSync(pngPath).subarray(0, 4);

    expect(result.status).toBe(0);
    expect(payload.status).toBe("ok");
    expect(payload.cpu.pc).toMatch(/^0x[0-9A-F]{6}$/);
    expect(payload.screen).toBeDefined();
    expect(payload.screen.visible_lines).toHaveLength(10);
    expect(payload.program_count).toBe(0);
    expect(payload.programs).toEqual([]);
    expect(payload.lcd_png_path).toBe(screenshotArgumentPath);
    expect(Array.from(signature)).toEqual([0x89, 0x50, 0x4e, 0x47]);
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
});

ez80Test("ez80 CLI accepts --cycles as an alias for --run", () => {
  const result = runEz80([
    "--rom",
    path.join("rom", "Rom.rom"),
    "--cycles",
    "64",
    "--json",
    "--quiet",
  ]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(0);
  expect(payload.status).toBe("ok");
  expect(payload.cycles_executed).toBeGreaterThanOrEqual(64);
});

ez80Test("ez80 interactive mode reports correct step PCs and updated agent help", () => {
  const romPath = path.join("rom", "Rom.rom").replaceAll("\\", "/");
  const input = [
    JSON.stringify({ cmd: "load_rom", id: 1, path: romPath }),
    JSON.stringify({ cmd: "step", id: 2 }),
    JSON.stringify({ cmd: "help", id: 3 }),
    JSON.stringify({ cmd: "quit", id: 4 }),
    "",
  ].join("\n");
  const result = runEz80(["--interactive"], { input });
  const lines = result.stdout.trim().split(/\r?\n/).map((line) => JSON.parse(line));

  expect(result.status).toBe(0);
  expect(lines[0].status).toBe("ready");
  expect(lines[1].status).toBe("ok");
  expect(lines[2].pc).toBe("0x000000");
  expect(lines[2].next_pc).toBe("0x000001");
  expect(lines[2].mnemonic).toBe("DI");
  expect(lines[3].commands).toContain("list_programs");
  expect(lines.at(-1).goodbye).toBe(true);
});

ez80Test("ez80 interactive mode reports structured agent errors for invalid commands", () => {
  const romPath = path.join("rom", "Rom.rom").replaceAll("\\", "/");
  const { result, lines } = runEz80InteractiveRaw([
    JSON.stringify({ cmd: "load_rom", id: 1, path: romPath }),
    JSON.stringify({ cmd: "launch_program", id: 2 }),
    JSON.stringify({ cmd: "bogus_command", id: 3 }),
    "{\"cmd\":\"load_rom\",\"id\":4,\"path\":",
    JSON.stringify({ cmd: "quit", id: 5 }),
  ]);
  const parsed = lines.map((line) => JSON.parse(line));
  const malformed = parsed.filter((line) => line.id === 0 && line.message === "invalid JSON command");

  expect(result.status).toBe(0);
  expect(parsed[0].status).toBe("ready");
  expect(responseById(parsed, 2).status).toBe("error");
  expect(responseById(parsed, 2).message).toBe("missing 'name' argument");
  expect(responseById(parsed, 3).status).toBe("error");
  expect(responseById(parsed, 3).message).toBe("unknown command");
  expect(malformed).toHaveLength(1);
  expect(lines.at(-1)).toContain("\"goodbye\":true");
});

ez80Test("ez80 interactive mode exposes RTC alarm standby wake telemetry", () => {
  const romPath = path.join("rom", "Rom.rom").replaceAll("\\", "/");
  const { result, lines } = runEz80Interactive([
    { cmd: "load_rom", id: 1, path: romPath },
    { cmd: "write_port", id: 2, port: "0x00EE", value: 1 },
    { cmd: "write_port", id: 3, port: "0x00EF", value: 0 },
    { cmd: "write_port", id: 4, port: "0x00F0", value: 12 },
    { cmd: "write_port", id: 5, port: "0x00F1", value: 3 },
    { cmd: "write_memory", id: 6, addr: "0xE30270", value: 2 },
    { cmd: "run", id: 7, cycles: 32768 },
    { cmd: "read_memory", id: 8, addr: "0xE30270", length: 5 },
    { cmd: "read_memory", id: 9, addr: "0xE30254", length: 1 },
    { cmd: "read_port", id: 10, port: "0x00EE" },
    { cmd: "read_port", id: 11, port: "0x00F1" },
    { cmd: "quit", id: 12 },
  ]);

  expect(result.status).toBe(0);
  expect(lines[0].status).toBe("ready");
  expect(responseById(lines, 7).status).toBe("ok");
  expect(responseById(lines, 8).hex).toBe("0008010080");
  expect(responseById(lines, 9).hex).toBe("01");
  expect(responseById(lines, 10).value).toBe("0x01");
  expect(responseById(lines, 11).value).toBe("0x03");
  expect(lines.at(-1).goodbye).toBe(true);
});

ez80Test("ez80 interactive mode supports full 16-bit high-port access", () => {
  const romPath = path.join("rom", "Rom.rom").replaceAll("\\", "/");
  const { result, lines } = runEz80Interactive([
    { cmd: "load_rom", id: 1, path: romPath },
    { cmd: "write_port", id: 2, port: "0x5005", value: 0x4a },
    { cmd: "read_port", id: 3, port: "0x5005" },
    { cmd: "quit", id: 4 },
  ]);

  expect(result.status).toBe(0);
  expect(lines[0].status).toBe("ready");
  expect(responseById(lines, 2).status).toBe("ok");
  expect(responseById(lines, 2).port).toBe("0x5005");
  expect(responseById(lines, 3).status).toBe("ok");
  expect(responseById(lines, 3).port).toBe("0x5005");
  expect(responseById(lines, 3).value).toBe("0x4A");
  expect(lines.at(-1).goodbye).toBe(true);
});

ez80Test("ez80 interactive mode exposes watchdog sleep wake telemetry", () => {
  const romPath = path.join("rom", "Rom.rom").replaceAll("\\", "/");
  const { result, lines } = runEz80Interactive([
    { cmd: "load_rom", id: 1, path: romPath },
    { cmd: "write_port", id: 2, port: "0x00F4", value: 2 },
    { cmd: "write_port", id: 3, port: "0x00F5", value: 0 },
    { cmd: "write_port", id: 4, port: "0x00F2", value: 5 },
    { cmd: "write_memory", id: 5, addr: "0xE30270", value: 3 },
    { cmd: "run", id: 6, cycles: 8 },
    { cmd: "read_memory", id: 7, addr: "0xE30270", length: 5 },
    { cmd: "read_memory", id: 8, addr: "0xE30265", length: 1 },
    { cmd: "get_state", id: 9 },
    { cmd: "quit", id: 10 },
  ]);

  expect(result.status).toBe(0);
  expect(lines[0].status).toBe("ready");
  expect(responseById(lines, 6).status).toBe("ok");
  expect(responseById(lines, 7).hex).toBe("0020010800");
  expect(responseById(lines, 8).hex).toBe("01");
  expect(responseById(lines, 9).cpu.pc).toBe("0x000000");
  expect(responseById(lines, 9).cpu.halted).toBe(false);
  expect(lines.at(-1).goodbye).toBe(true);
});

archivedEz80Test("ez80 CLI lists loaded programs after archived launch flows", () => {
  const result = runEz80([
    "--rom",
    path.join("rom", "Rom.rom"),
    "--archived",
    "--load",
    path.join("bin", "core", "CALCENG.8xp"),
    "--launch",
    "CALCENG",
    "--run",
    "1",
    "--json",
    "--list-programs",
    "--quiet",
  ]);
  const payload = JSON.parse(result.stdout);
  const calceng = payload.programs.find((program) => program.name === "CALCENG");

  expect(result.status).toBe(0);
  expect(payload.program_count).toBeGreaterThanOrEqual(1);
  expect(calceng).toBeDefined();
  expect(calceng.archived).toBe(true);
  expect(calceng.loaded_in_ram).toBe(true);
  expect(calceng.active).toBe(true);
});

archivedEz80Test("ez80 CLI screen-state snapshots preserve sequential launch history", () => {
  const result = runEz80([
    "--rom",
    path.join("rom", "Rom.rom"),
    "--archived",
    "--load",
    path.join("bin", "core", "CALCENG.8xp"),
    "--archived",
    "--load",
    path.join("bin", "calculus", "CECALC.8xp"),
    "--launch",
    "CALCENG",
    "--launch",
    "CECALC",
    "--run",
    "1",
    "--json",
    "--screen-state",
    "--quiet",
  ]);
  const payload = JSON.parse(result.stdout);

  expect(result.status).toBe(0);
  expect(payload.status).toBe("ok");
  expect(payload.screen.step_lines.some((line) => line.includes("RUN: CALCENG"))).toBe(true);
  expect(payload.screen.input_line).toContain("RUN: CECALC");
  expect(payload.screen.active_program).toBe("CECALC");
  expect(payload.screen.visible_lines[payload.screen.cursor_row].trimEnd()).toBe(payload.screen.input_line.trimEnd());
});

ez80Test("ez80 CLI requires --rom outside agent mode", () => {
  const result = runEz80([]);
  const combined = `${result.stdout}\n${result.stderr}`;

  expect(result.status).toBe(1);
  expect(combined).toContain("--rom is required");
});

ez80Test("ez80 CLI rejects invalid key sequences", () => {
  const result = runEz80([
    "--rom",
    path.join("rom", "Rom.rom"),
    "--keys",
    "zz",
  ]);
  const combined = `${result.stdout}\n${result.stderr}`;

  expect(result.status).toBe(1);
  expect(combined).toContain("invalid key code");
});

ez80Test("ez80 CLI rejects invalid breakpoint arguments", () => {
  const result = runEz80([
    "--rom",
    path.join("rom", "Rom.rom"),
    "--breakpoint",
    "not_hex",
  ]);
  const combined = `${result.stdout}\n${result.stderr}`;

  expect(result.status).toBe(1);
  expect(combined).toContain("invalid breakpoint address");
});

archivedEz80Test("ez80 CLI reports sequential launch handoff in program inventory", () => {
  const result = runEz80([
    "--rom",
    path.join("rom", "Rom.rom"),
    "--archived",
    "--load",
    path.join("bin", "core", "CALCENG.8xp"),
    "--archived",
    "--load",
    path.join("bin", "calculus", "CECALC.8xp"),
    "--launch",
    "CALCENG",
    "--launch",
    "CECALC",
    "--run",
    "1",
    "--json",
    "--list-programs",
    "--quiet",
  ]);
  const payload = JSON.parse(result.stdout);
  const calceng = payload.programs.find((program) => program.name === "CALCENG");
  const cecalc = payload.programs.find((program) => program.name === "CECALC");

  expect(result.status).toBe(0);
  expect(payload.program_count).toBeGreaterThanOrEqual(2);
  expect(calceng).toBeDefined();
  expect(cecalc).toBeDefined();
  expect(calceng.active).toBe(false);
  expect(calceng.archived).toBe(true);
  expect(calceng.loaded_in_ram).toBe(false);
  expect(cecalc.active).toBe(true);
  expect(cecalc.archived).toBe(true);
  expect(cecalc.loaded_in_ram).toBe(true);
});
