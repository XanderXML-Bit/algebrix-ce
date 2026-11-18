import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";

let cachedPowerShellExecutable = null;
const windowsPathCache = new Map();

export function isWslHost() {
  return process.platform === "linux" && (
    Boolean(process.env.WSL_DISTRO_NAME) ||
    os.release().toLowerCase().includes("microsoft")
  );
}

function translateWslPath(targetPath) {
  if (!windowsPathCache.has(targetPath)) {
    const result = spawnSync("wslpath", ["-w", targetPath], { encoding: "utf8" });
    if (result.error) {
      throw result.error;
    }
    if (result.status !== 0 || !result.stdout.trim()) {
      throw new Error(`wslpath failed for ${targetPath}: ${result.stderr ?? ""}`);
    }
    windowsPathCache.set(targetPath, result.stdout.trim());
  }
  return windowsPathCache.get(targetPath);
}

export function normalizePathForWindowsHostProcess(targetPath, {
  isWsl = isWslHost(),
  translate = translateWslPath,
} = {}) {
  if (typeof targetPath !== "string") {
    return targetPath;
  }

  const mountedDriveMatch = targetPath.match(/^\/mnt\/([a-z])\/(.*)$/i);
  if (!mountedDriveMatch) {
    return isWsl && targetPath.startsWith("/") ? translate(targetPath) : targetPath;
  }

  const [, driveLetter, remainder] = mountedDriveMatch;
  return path.win32.normalize(`${driveLetter.toUpperCase()}:/${remainder}`);
}

export function normalizePathFromWindowsHostProcess(targetPath) {
  if (typeof targetPath !== "string") {
    return targetPath;
  }

  const uncMatch = targetPath.match(/^[\\/]{2}wsl(?:\.localhost|\$)[\\/][^\\/]+(?:[\\/](.*))?$/i);
  if (uncMatch) {
    return `/${(uncMatch[1] ?? "").replace(/[\\/]+/g, "/")}`;
  }

  const driveLetterMatch = targetPath.match(/^([a-z]):[\\/](.*)$/i);
  if (!driveLetterMatch) {
    return targetPath;
  }

  const [, driveLetter, remainder] = driveLetterMatch;
  const linuxRemainder = remainder.replace(/[\\/]+/g, "/");
  return `/mnt/${driveLetter.toLowerCase()}/${linuxRemainder}`;
}

export function normalizeWindowsHostArgument(value, options = {}) {
  if (!(options.isWsl ?? isWslHost()) || typeof value !== "string") {
    return value;
  }

  if (value.includes(";")) {
    return value
      .split(";")
      .map((segment) => normalizeWindowsHostArgument(segment, options))
      .join(";");
  }

  return normalizePathForWindowsHostProcess(value, options);
}

export function isWindowsHostPowerShellExecutable(command) {
  if (typeof command !== "string") {
    return false;
  }

  if (/^\/mnt\/[a-z]\//i.test(command)) {
    return /(?:^|[\\/])(pwsh|powershell)(?:\.exe)?$/i.test(command);
  }

  return /(?:^|[\\/])(pwsh|powershell)\.exe$/i.test(command);
}

function spawnOutputText(output) {
  return Buffer.isBuffer(output) ? output.toString("utf8") : String(output ?? "");
}

export function isWslInteropLaunchFailure(result) {
  return !result.error &&
    !spawnOutputText(result.stdout).trim() &&
    /UtilAcceptVsock|accept4 failed/.test(spawnOutputText(result.stderr));
}

export function spawnWithInteropRetry(spawnFn, args, {
  attempts = 3,
  sleep = (milliseconds) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, milliseconds),
} = {}) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    const result = spawnFn(...args);
    if (attempt === attempts - 1 || !isWslInteropLaunchFailure(result)) {
      return result;
    }
    sleep(attempt === 0 ? 250 : 1000);
  }
}

export function formatPowerShellProbeFailure(candidate, probe) {
  if (probe.error) {
    return `${candidate}: ${probe.error.code ?? probe.error.message}`;
  }

  if (probe.status !== null) {
    const stderr = typeof probe.stderr === "string" ? probe.stderr.trim() : "";
    const firstLine = stderr
      .split(/\r?\n/)[0]
      ?.replace(/[^\x20-\x7E]/g, "?")
      .slice(0, 160);
    const detail = firstLine ? ` (${firstLine})` : "";
    return `${candidate}: exit ${probe.status}${detail}`;
  }

  return `${candidate}: no exit status`;
}

export function buildPowerShellResolutionErrorMessage(candidates, probeFailures, { isWsl = isWslHost() } = {}) {
  const interopHint = isWsl && probeFailures.some((failure) => failure.includes("ENOEXEC") || /MZ/.test(failure))
    ? " WSL Windows interop appears unavailable in this process (Windows .exe launch failed as a non-Linux binary); restart/enable WSLInterop or install a Linux pwsh before running PowerShell-backed hardware lanes."
    : "";
  return `PowerShell executable was not runnable. Tried: ${candidates.join(", ")}.${interopHint} Probe failures: ${probeFailures.join("; ")}`;
}

export function resolvePowerShellExecutable() {
  if (cachedPowerShellExecutable !== null) {
    return cachedPowerShellExecutable;
  }

  const candidates = process.platform === "win32"
    ? ["pwsh.exe", "powershell.exe", "pwsh", "powershell"]
    : [
        "/mnt/c/Program Files/PowerShell/7/pwsh.exe",
        "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
        "pwsh",
        "powershell",
        "pwsh.exe",
        "powershell.exe",
      ];

  const probeFailures = [];
  for (const candidate of candidates) {
    const probe = spawnWithInteropRetry(spawnSync, [
      candidate,
      ["-NoProfile", "-NonInteractive", "-Command", "$PSVersionTable.PSVersion.ToString()"],
      {
        encoding: "utf8",
      },
    ], { attempts: isWindowsHostPowerShellExecutable(candidate) && isWslHost() ? 3 : 1 });
    if (!probe.error && probe.status === 0) {
      cachedPowerShellExecutable = candidate;
      return candidate;
    }

    probeFailures.push(formatPowerShellProbeFailure(candidate, probe));
  }

  throw new Error(buildPowerShellResolutionErrorMessage(candidates, probeFailures));
}

export function spawnPowerShellFile(scriptPath, scriptArgs = [], options = {}) {
  const powershell = resolvePowerShellExecutable();
  const useWindowsPathArguments = isWindowsHostPowerShellExecutable(powershell) && isWslHost();
  const normalizedScriptPath = useWindowsPathArguments
    ? normalizeWindowsHostArgument(scriptPath)
    : scriptPath;
  const normalizedArgs = useWindowsPathArguments
    ? scriptArgs.map((arg) => normalizeWindowsHostArgument(arg))
    : scriptArgs;

  return spawnWithInteropRetry(spawnSync, [
    powershell,
    [
      "-NoProfile",
      "-NonInteractive",
      "-ExecutionPolicy",
      "Bypass",
      "-File",
      normalizedScriptPath,
      ...normalizedArgs,
    ],
    {
      encoding: options.encoding ?? "utf8",
      cwd: options.cwd,
      input: options.input,
      stdio: options.stdio,
      env: options.env,
      timeout: options.timeout,
    },
  ], { attempts: useWindowsPathArguments ? 3 : 1 });
}

export function createWindowsAccessibleTempDir(prefix, repoRoot) {
  const tempRoot = path.join(repoRoot, "tests", "tmp");
  fs.mkdirSync(tempRoot, { recursive: true });
  return fs.mkdtempSync(path.join(tempRoot, prefix));
}
