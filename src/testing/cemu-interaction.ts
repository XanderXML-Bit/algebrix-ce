import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";

import { scaleCEmuTimeoutMs } from "./cemu-pipeline.ts";

export interface CEmuCommandEnvelope {
  ok: boolean;
  cmd: string;
  error?: string;
  [key: string]: unknown;
}

export interface CEmuSessionOptions {
  executablePath: string;
  romPath: string;
  autotesterPath: string;
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  startupTimeoutMs?: number;
  commandTimeoutMs?: number;
  shutdownTimeoutMs?: number;
}

export interface CEmuLoadAppOptions {
  targetName?: string;
  targetIsAsm?: boolean;
}

export interface CEmuPauseEvent {
  kind: "pause";
  durationMs: number;
  note?: string;
}

export interface CEmuKeyEvent {
  kind: "key";
  key: string;
  durationMs?: number;
  note?: string;
}

export type CEmuKeySequenceEvent = CEmuPauseEvent | CEmuKeyEvent;

export interface ScreenAssertionOptions {
  name: string;
  description: string;
  // Optional: when omitted or empty, a nonempty `expectedOutputExcerptContains`
  // is required. No CRC and no text expectations fails closed (known failure).
  // This supports UI-flow tests that rely on text-excerpt matching only.
  expectedCrcs?: string[];
  evidenceDir: string;
  expectedOutputExcerptContains?: string[];
}

export interface ScreenAssertionResult {
  name: string;
  description: string;
  passed: boolean;
  expectedCrcs: string[];
  observedCrc: string;
  expectedOutputExcerptContains: string[];
  observedOutputExcerpt: string;
  screenshotMode: "crc";
  stateSnapshotPath: string;
  autotesterExit: number;
}

const CYCLES_PER_MILLISECOND = 48_000;
const DEFAULT_STARTUP_TIMEOUT_MS = 15_000;
const DEFAULT_COMMAND_TIMEOUT_MS = 45_000;
const DEFAULT_SHUTDOWN_TIMEOUT_MS = 10_000;

const TI84_KEY_ALIASES: Record<string, string> = {
  "0": "0",
  "1": "1",
  "2": "2",
  "3": "3",
  "4": "4",
  "5": "5",
  "6": "6",
  "7": "7",
  "8": "8",
  "9": "9",
  "+": "+",
  "add": "+",
  "plus": "+",
  "-": "-",
  "sub": "-",
  "subtract": "-",
  "minus": "-",
  "*": "*",
  "mul": "*",
  "multiply": "*",
  "times": "*",
  "/": "/",
  "div": "/",
  "divide": "/",
  "^": "^",
  "pow": "^",
  "power": "^",
  ".": ".",
  "decimal": ".",
  "dot": ".",
  "point": ".",
  "(": "(",
  "left_paren": "(",
  "leftparen": "(",
  "lparen": "(",
  ")": ")",
  "right_paren": ")",
  "rightparen": ")",
  "rparen": ")",
  ",": ",",
  "comma": ",",
  ";": "apps",
  "semicolon": "apps",
  "(-)": "(-)",
  "neg": "(-)",
  "negative": "(-)",
  "enter": "enter",
  "return": "enter",
  "clear": "clear",
  "clr": "clear",
  "mode": "mode",
  "y=": "yequ",
  "yequ": "yequ",
  "window": "window",
  "zoom": "zoom",
  "trace": "trace",
  "graph": "graph",
  "2nd": "2nd",
  "second": "2nd",
  "alpha": "alpha",
  "math": "math",
  "apps": "apps",
  "app": "apps",
  "prgm": "prgm",
  "sin": "sin",
  "cos": "cos",
  "tan": "tan",
  "ln": "ln",
  "log": "log",
  "^2": "^2",
  "square": "^2",
  "-1": "-1",
  "reciprocal": "-1",
  "recip": "-1",
  "xton": "xton",
  "x": "xton",
  "left": "left",
  "right": "right",
  "up": "up",
  "down": "down",
  "del": "del",
  "delete": "del",
  "sto": "sto",
  "sto>": "sto",
  "store": "sto",
  "storeto": "sto",
};

// TI-84 Plus CE ALPHA layer exactly as printed on the keycaps (row order:
// math..÷ = A..M, log..2 = N..Z). Must match src/core/launcher/main.c.
const ALPHA_KEY_MAP: Record<string, string> = {
  a: "math",
  b: "apps",
  c: "prgm",
  d: "-1",
  e: "sin",
  f: "cos",
  g: "tan",
  h: "^",
  i: "^2",
  j: ",",
  k: "(",
  l: ")",
  m: "/",
  n: "log",
  o: "7",
  p: "8",
  q: "9",
  r: "*",
  s: "ln",
  t: "4",
  u: "5",
  v: "6",
  w: "-",
  x: "sto",
  y: "1",
  z: "2",
};

const EXPRESSION_KEY_MAP: Record<string, string> = {
  "+": "+",
  "-": "-",
  "*": "*",
  "/": "/",
  "^": "^",
  ".": ".",
  "(": "(",
  ")": ")",
  ",": ",",
  ";": "apps",
};

function createTimeoutError(message: string): Error {
  const error = new Error(message);
  error.name = "CEmuTimeoutError";
  return error;
}

function normalizeEnvelope(raw: string): CEmuCommandEnvelope | null {
  try {
    const parsed = JSON.parse(raw) as CEmuCommandEnvelope;
    if (!parsed || typeof parsed !== "object" || typeof parsed.cmd !== "string") {
      return null;
    }
    return parsed;
  } catch {
    return null;
  }
}

function waitFor(predicate: () => boolean, timeoutMs: number, intervalMs = 25): Promise<void> {
  const startedAt = Date.now();
  return new Promise((resolve, reject) => {
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer);
        resolve();
        return;
      }
      if (Date.now() - startedAt >= timeoutMs) {
        clearInterval(timer);
        reject(createTimeoutError(`Timed out after ${timeoutMs}ms.`));
      }
    }, intervalMs);
  });
}

export function normalizeKey(key: string): string {
  const normalized = TI84_KEY_ALIASES[key.toLowerCase()];
  if (!normalized) {
    throw new Error(`Unsupported TI-84 CE key alias: ${key}`);
  }
  return normalized;
}

export function press(key: string, durationMs = 0, note = ""): CEmuKeyEvent {
  return {
    kind: "key",
    key: normalizeKey(key),
    durationMs,
    note,
  };
}

export function pause(durationMs: number, note = ""): CEmuPauseEvent {
  if (durationMs <= 0) {
    throw new Error("Pause duration must be positive.");
  }
  return {
    kind: "pause",
    durationMs,
    note,
  };
}

function looksLikeScientificNotation(expressionText: string, index: number): boolean {
  if (expressionText[index]?.toLowerCase() !== "e") {
    return false;
  }
  const previous = expressionText[index - 1] ?? "";
  const following = expressionText[index + 1] ?? "";
  return (/\d|\./).test(previous) && (/\d|[+-]/).test(following);
}

export function expressionToTokens(expressionText: string): string[] {
  const tokens: string[] = [];
  for (let index = 0; index < expressionText.length; index += 1) {
    const character = expressionText[index];
    if (/\s/.test(character)) {
      continue;
    }
    if (/\d/.test(character)) {
      tokens.push(character);
      continue;
    }
    const mapped = EXPRESSION_KEY_MAP[character];
    if (mapped) {
      tokens.push(mapped);
      continue;
    }
    if (/[a-z]/i.test(character)) {
      if (looksLikeScientificNotation(expressionText, index)) {
        tokens.push("2nd", "(-)");
        continue;
      }
      const alphaKey = ALPHA_KEY_MAP[character.toLowerCase()];
      if (!alphaKey) {
        throw new Error(`Unsupported expression character: ${character}`);
      }
      tokens.push("alpha", alphaKey);
      continue;
    }
    throw new Error(`Unsupported expression character: ${character}`);
  }
  return tokens;
}

export function tokensToEvents(
  tokens: string[],
  options: {
    keyDelayMs?: number;
    autoEnter?: boolean;
    enterSettleDelayMs?: number;
    resultDelayMs?: number;
  } = {},
): CEmuKeySequenceEvent[] {
  const keyDelayMs = options.keyDelayMs ?? 250;
  const autoEnter = options.autoEnter ?? true;
  const enterSettleDelayMs = options.enterSettleDelayMs ?? 200;
  const resultDelayMs = options.resultDelayMs ?? 2_500;
  const events: CEmuKeySequenceEvent[] = [];
  for (const token of tokens) {
    events.push(press(token));
    events.push(pause(keyDelayMs, "settle after key"));
  }
  if (autoEnter) {
    events.push(pause(enterSettleDelayMs, "settle before enter"));
    events.push(press("enter"));
    events.push(pause(resultDelayMs, "wait for result"));
  }
  return events;
}

export function expressionToEvents(
  expressionText: string,
  options: {
    keyDelayMs?: number;
    autoEnter?: boolean;
    enterSettleDelayMs?: number;
    resultDelayMs?: number;
  } = {},
): CEmuKeySequenceEvent[] {
  return tokensToEvents(expressionToTokens(expressionText), options);
}

export function buildAlphaProgramLaunchSequence(programName: string): CEmuKeySequenceEvent[] {
  const events: CEmuKeySequenceEvent[] = [];
  for (const character of programName.toLowerCase()) {
    const alphaKey = ALPHA_KEY_MAP[character];
    if (!alphaKey) {
      throw new Error(`Program name '${programName}' contains unsupported character '${character}'.`);
    }
    events.push(press("alpha"));
    events.push(press(alphaKey));
  }
  return events;
}

export class CEmuHeadlessSession {
  readonly executablePath: string;
  readonly romPath: string;
  readonly autotesterPath: string;
  readonly cwd?: string;
  readonly env?: NodeJS.ProcessEnv;
  readonly startupTimeoutMs: number;
  readonly commandTimeoutMs: number;
  readonly shutdownTimeoutMs: number;

  private process: ChildProcessWithoutNullStreams | null = null;
  private stdoutMessages: CEmuCommandEnvelope[] = [];
  private stderrLines: string[] = [];
  private ready = false;
  private commandChain: Promise<unknown> = Promise.resolve();
  private stdoutWaiter: ((message: CEmuCommandEnvelope) => void) | null = null;
  private readyWaiter: (() => void) | null = null;

  constructor(options: CEmuSessionOptions) {
    this.executablePath = path.resolve(options.executablePath);
    this.romPath = path.resolve(options.romPath);
    this.autotesterPath = path.resolve(options.autotesterPath);
    this.cwd = options.cwd ? path.resolve(options.cwd) : undefined;
    this.env = options.env;
    this.startupTimeoutMs = options.startupTimeoutMs ?? scaleCEmuTimeoutMs(DEFAULT_STARTUP_TIMEOUT_MS);
    this.commandTimeoutMs = options.commandTimeoutMs ?? scaleCEmuTimeoutMs(DEFAULT_COMMAND_TIMEOUT_MS);
    this.shutdownTimeoutMs = options.shutdownTimeoutMs ?? scaleCEmuTimeoutMs(DEFAULT_SHUTDOWN_TIMEOUT_MS);
  }

  async start(): Promise<void> {
    if (this.process) {
      return;
    }

    if (!fs.existsSync(this.executablePath)) {
      throw new Error(`CEmu headless binary does not exist: ${this.executablePath}`);
    }
    if (!fs.existsSync(this.romPath)) {
      throw new Error(`ROM path does not exist: ${this.romPath}`);
    }
    if (!fs.existsSync(this.autotesterPath)) {
      throw new Error(`AutoTester path does not exist: ${this.autotesterPath}`);
    }

    this.process = spawn(this.executablePath, [], {
      cwd: this.cwd,
      env: {
        ...process.env,
        ...this.env,
      },
      stdio: "pipe",
      windowsHide: true,
    });

    this.process.on("exit", () => {
      this.process = null;
    });

    const stdoutReader = readline.createInterface({ input: this.process.stdout });
    stdoutReader.on("line", (line) => {
      const parsed = normalizeEnvelope(line);
      if (!parsed) {
        return;
      }
      if (this.stdoutWaiter) {
        const waiter = this.stdoutWaiter;
        this.stdoutWaiter = null;
        waiter(parsed);
        return;
      }
      this.stdoutMessages.push(parsed);
    });

    const stderrReader = readline.createInterface({ input: this.process.stderr });
    stderrReader.on("line", (line) => {
      this.stderrLines.push(line);
      if (!this.ready && line.toLowerCase().includes("ready")) {
        this.ready = true;
        if (this.readyWaiter) {
          const waiter = this.readyWaiter;
          this.readyWaiter = null;
          waiter();
        }
      }
    });

    if (this.ready) {
      return;
    }

    await Promise.race([
      waitFor(() => this.ready, this.startupTimeoutMs),
      new Promise<void>((resolve) => {
        this.readyWaiter = resolve;
      }),
    ]);
  }

  private async readMessage(timeoutMs: number): Promise<CEmuCommandEnvelope> {
    if (this.stdoutMessages.length > 0) {
      return this.stdoutMessages.shift()!;
    }

    return await new Promise<CEmuCommandEnvelope>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.stdoutWaiter = null;
        reject(createTimeoutError(`CEmu did not respond within ${timeoutMs}ms.`));
      }, timeoutMs);

      this.stdoutWaiter = (message) => {
        clearTimeout(timeout);
        resolve(message);
      };
    });
  }

  private async sendCommand<T extends CEmuCommandEnvelope>(
    command: string,
    payload: Record<string, unknown>,
    timeoutMs = this.commandTimeoutMs,
  ): Promise<T> {
    await this.start();
    if (!this.process || !this.process.stdin.writable) {
      throw new Error("CEmu process is not running.");
    }

    const run = async (): Promise<T> => {
      this.process!.stdin.write(`${JSON.stringify({ cmd: command, ...payload })}\n`);
      const message = await this.readMessage(timeoutMs);
      if (!message.ok) {
        throw new Error(typeof message.error === "string" ? message.error : `${command} failed`);
      }
      if (message.cmd !== command) {
        throw new Error(`Expected '${command}' response but received '${message.cmd}'.`);
      }
      return message as T;
    };

    const next = this.commandChain.then(run, run);
    this.commandChain = next.catch(() => undefined);
    return await next;
  }

  async loadRom(): Promise<CEmuCommandEnvelope> {
    return await this.sendCommand("load_rom", {
      path: this.romPath,
      autotester_path: this.autotesterPath,
    });
  }

  async loadApp(appPath: string, options: CEmuLoadAppOptions = {}): Promise<CEmuCommandEnvelope> {
    return await this.sendCommand("load_app", {
      path: path.resolve(appPath),
      ...(options.targetName ? { target_name: options.targetName } : {}),
      ...(typeof options.targetIsAsm === "boolean" ? { target_is_asm: options.targetIsAsm } : {}),
    });
  }

  async stepDelayMs(delayMs: number): Promise<CEmuCommandEnvelope> {
    if (delayMs <= 0) {
      throw new Error("delayMs must be positive.");
    }
    return await this.sendCommand("step", {
      cycles: delayMs * CYCLES_PER_MILLISECOND,
    });
  }

  async injectKey(key: string, durationMs = 0): Promise<CEmuCommandEnvelope> {
    return await this.sendCommand("inject_key", {
      key: normalizeKey(key),
      ...(durationMs > 0 ? { duration_ms: durationMs } : {}),
    });
  }

  async sendSequence(events: CEmuKeySequenceEvent[]): Promise<void> {
    for (const event of events) {
      if (event.kind === "pause") {
        await this.stepDelayMs(event.durationMs);
        continue;
      }
      await this.injectKey(event.key, event.durationMs ?? 0);
    }
  }

  async getState(): Promise<CEmuCommandEnvelope> {
    return await this.sendCommand("get_state", {});
  }

  async reset(): Promise<CEmuCommandEnvelope> {
    return await this.sendCommand("reset", {});
  }

  async getScreen(): Promise<CEmuCommandEnvelope> {
    return await this.sendCommand("get_screen", { format: "crc" });
  }

  async snapshot(snapshotPath: string): Promise<CEmuCommandEnvelope> {
    const resolvedPath = path.resolve(snapshotPath);
    fs.mkdirSync(path.dirname(resolvedPath), { recursive: true });
    return await this.sendCommand("snapshot", { path: resolvedPath });
  }

  async boot(transferPath: string, options: {
    targetName: string;
    targetIsAsm: boolean;
    launchSequence?: CEmuKeySequenceEvent[];
  }): Promise<void> {
    await this.loadRom();
    await this.loadApp(transferPath, {
      targetName: options.targetName,
      targetIsAsm: options.targetIsAsm,
    });
    if (options.launchSequence && options.launchSequence.length > 0) {
      await this.sendSequence(options.launchSequence);
    }
  }

  async captureScreenAssertion(options: ScreenAssertionOptions): Promise<ScreenAssertionResult> {
    const evidenceDir = path.resolve(options.evidenceDir);
    fs.mkdirSync(evidenceDir, { recursive: true });

    const screen = await this.getScreen();
    const stateSnapshotPath = path.join(evidenceDir, `${options.name}.state.json`);
    const snapshot = await this.snapshot(stateSnapshotPath);
    const requestedCrcs = (options.expectedCrcs ?? []).map((value) => value.toUpperCase());
    const expectedOutputExcerptContains = (options.expectedOutputExcerptContains ?? []).map((value) => value.toLowerCase());
    const observedOutputExcerpt = String(screen.output_excerpt ?? "");
    const observedCrc = String(screen.crc ?? "").toUpperCase();
    const crcCheckEnabled = requestedCrcs.length > 0;
    const crcPassed = crcCheckEnabled ? requestedCrcs.includes(observedCrc) : expectedOutputExcerptContains.length > 0;
    const textPassed = expectedOutputExcerptContains.every((fragment) =>
      observedOutputExcerpt.toLowerCase().includes(fragment),
    );
    const passed = crcPassed && textPassed;

    return {
      name: options.name,
      description: options.description,
      passed,
      expectedCrcs: requestedCrcs,
      observedCrc,
      expectedOutputExcerptContains: options.expectedOutputExcerptContains ?? [],
      observedOutputExcerpt,
      screenshotMode: "crc",
      stateSnapshotPath,
      autotesterExit: Number(snapshot.autotester_exit ?? screen.autotester_exit ?? 0),
    };
  }

  async close(): Promise<void> {
    if (!this.process) {
      return;
    }

    try {
      await this.sendCommand("exit", {}, Math.min(2_000, this.shutdownTimeoutMs));
    } catch {
      // Ignore shutdown errors and force the child closed below.
    }

    const processRef = this.process;
    await Promise.race([
      new Promise<void>((resolve) => {
        processRef.once("exit", () => resolve());
      }),
      new Promise<void>((resolve) => {
        setTimeout(() => {
          processRef.kill();
          resolve();
        }, this.shutdownTimeoutMs);
      }),
    ]);
  }
}
