import { expect, test, vi } from "vitest";

import {
  buildPowerShellResolutionErrorMessage,
  formatPowerShellProbeFailure,
  isWindowsHostPowerShellExecutable,
  isWslInteropLaunchFailure,
  normalizePathForWindowsHostProcess,
  normalizePathFromWindowsHostProcess,
  normalizeWindowsHostArgument,
  spawnWithInteropRetry,
} from "./helpers/windows-host-paths.mjs";

const interopLaunchFailure = {
  status: 1,
  stdout: "",
  stderr: "<3>WSL (1234 - ) ERROR: UtilAcceptVsock:281: accept4 failed 110\n",
};

test("windows-host-paths recognizes only empty-output WSL interop launch failures", () => {
  expect(isWslInteropLaunchFailure(interopLaunchFailure)).toBe(true);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stdout: " \r\n\t" })).toBe(true);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stdout: undefined })).toBe(true);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stderr: "accept4 failed 110\n" })).toBe(true);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stdout: '{"success":false}' })).toBe(false);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stderr: "Write-Error: script failed" })).toBe(false);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stderr: undefined })).toBe(false);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, error: new Error("spawn failed") })).toBe(false);
});

test.each(["", " \r\n\t"])("windows-host-paths recognizes Buffer launch output: %j", (stdout) => {
  expect(isWslInteropLaunchFailure({
    ...interopLaunchFailure,
    stdout: Buffer.from(stdout),
    stderr: Buffer.from(interopLaunchFailure.stderr),
  })).toBe(true);
});

test("windows-host-paths preserves Buffer JSON output as a script result", () => {
  expect(isWslInteropLaunchFailure({
    ...interopLaunchFailure,
    stdout: Buffer.from('{"success":false}'),
    stderr: Buffer.from(interopLaunchFailure.stderr),
  })).toBe(false);
});

test("windows-host-paths safely coerces other non-string output", () => {
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stdout: 0 })).toBe(false);
  expect(isWslInteropLaunchFailure({ ...interopLaunchFailure, stdout: null, stderr: null })).toBe(false);
  expect(isWslInteropLaunchFailure({
    ...interopLaunchFailure,
    stdout: [],
    stderr: { toString: () => interopLaunchFailure.stderr },
  })).toBe(true);
});

test.each([1, 3])("windows-host-paths leaves the terminal result untouched after %i attempts", (attempts) => {
  const result = {
    get stdout() {
      throw new Error("The retry predicate must not inspect the terminal result");
    },
  };
  const spawn = vi.fn(() => interopLaunchFailure);
  spawn.mockImplementationOnce(() => attempts === 1 ? result : interopLaunchFailure);
  if (attempts === 3) {
    spawn.mockReturnValueOnce(interopLaunchFailure).mockReturnValueOnce(result);
  }
  const sleep = vi.fn();

  expect(spawnWithInteropRetry(spawn, [], { attempts, sleep })).toBe(result);
  expect(spawn).toHaveBeenCalledTimes(attempts);
  expect(sleep).toHaveBeenCalledTimes(attempts - 1);
});

test("windows-host-paths retries launch failures with backoff and stops on success", () => {
  const success = { status: 0, stdout: '{"success":true}', stderr: "" };
  const spawn = vi.fn()
    .mockReturnValueOnce(interopLaunchFailure)
    .mockReturnValueOnce(interopLaunchFailure)
    .mockReturnValue(success);
  const sleep = vi.fn();
  const args = ["powershell.exe", ["-File", "test.ps1"], { encoding: "utf8" }];

  expect(spawnWithInteropRetry(spawn, args, { sleep })).toBe(success);
  expect(spawn).toHaveBeenCalledTimes(3);
  expect(spawn.mock.calls).toEqual([args, args, args]);
  expect(sleep.mock.calls).toEqual([[250], [1000]]);
});

test.each([
  { status: 0, stdout: '{"success":true}', stderr: "" },
  { ...interopLaunchFailure, stdout: '{"success":false}' },
  { status: 1, stdout: "", stderr: "Write-Error: script failed" },
  { ...interopLaunchFailure, error: new Error("spawn failed") },
])("windows-host-paths returns non-launch results without retry: %j", (result) => {
  const spawn = vi.fn(() => result);
  const sleep = vi.fn();

  expect(spawnWithInteropRetry(spawn, [], { sleep })).toBe(result);
  expect(spawn).toHaveBeenCalledTimes(1);
  expect(sleep).not.toHaveBeenCalled();
});

test("windows-host-paths returns the last launch failure after three attempts", () => {
  const lastFailure = { ...interopLaunchFailure, signal: null };
  const spawn = vi.fn()
    .mockReturnValueOnce(interopLaunchFailure)
    .mockReturnValueOnce(interopLaunchFailure)
    .mockReturnValue(lastFailure);
  const sleep = vi.fn();

  expect(spawnWithInteropRetry(spawn, [], { sleep })).toBe(lastFailure);
  expect(spawn).toHaveBeenCalledTimes(3);
  expect(sleep.mock.calls).toEqual([[250], [1000]]);
});

test("windows-host-paths supports a single attempt without backoff", () => {
  const spawn = vi.fn(() => interopLaunchFailure);
  const sleep = vi.fn();

  expect(spawnWithInteropRetry(spawn, [], { attempts: 1, sleep })).toBe(interopLaunchFailure);
  expect(spawn).toHaveBeenCalledTimes(1);
  expect(sleep).not.toHaveBeenCalled();
});

test("windows-host-paths identifies Windows-host PowerShell launchers", () => {
  expect(isWindowsHostPowerShellExecutable("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")).toBe(true);
  expect(isWindowsHostPowerShellExecutable("/mnt/c/Program Files/PowerShell/7/pwsh.exe")).toBe(true);
  expect(isWindowsHostPowerShellExecutable("powershell.exe")).toBe(true);
  expect(isWindowsHostPowerShellExecutable("pwsh.exe")).toBe(true);
  expect(isWindowsHostPowerShellExecutable("pwsh")).toBe(false);
  expect(isWindowsHostPowerShellExecutable("/usr/bin/pwsh")).toBe(false);
  expect(isWindowsHostPowerShellExecutable("bash")).toBe(false);
  expect(isWindowsHostPowerShellExecutable("/usr/bin/node")).toBe(false);
});

test("windows-host-paths translates WSL-native paths and mounted drives", () => {
  const translate = vi.fn(() => String.raw`\\wsl.localhost\Ubuntu\home\user\repo with spaces`);
  const options = { isWsl: true, translate };
  expect(normalizePathForWindowsHostProcess("/mnt/c/repo with spaces/a.bin", options)).toBe(String.raw`C:\repo with spaces\a.bin`);
  expect(translate).not.toHaveBeenCalled();
  expect(normalizePathForWindowsHostProcess("/home/user/repo with spaces", options)).toBe(String.raw`\\wsl.localhost\Ubuntu\home\user\repo with spaces`);
  expect(translate).toHaveBeenCalledExactlyOnceWith("/home/user/repo with spaces");
  expect(normalizePathForWindowsHostProcess("/home/user/repo", { isWsl: false, translate })).toBe("/home/user/repo");
});

test("windows-host-paths leaves non-path arguments untouched and splits path lists", () => {
  const translate = vi.fn((value) => `translated:${value}`);
  const options = { isWsl: true, translate };
  for (const value of ["2,+,3,enter", "json", "65536", 65536, null, "relative/file", String.raw`C:\repo\file`]) {
    expect(normalizeWindowsHostArgument(value, options)).toBe(value);
  }
  expect(translate).not.toHaveBeenCalled();
  expect(normalizeWindowsHostArgument("/home/user/a;/mnt/d/b;json;;/tmp/c", options)).toBe(
    String.raw`translated:/home/user/a;D:\b;json;;translated:/tmp/c`,
  );
  expect(translate.mock.calls).toEqual([["/home/user/a"], ["/tmp/c"]]);
  expect(normalizeWindowsHostArgument("/home/user/a;/tmp/b", { isWsl: false, translate })).toBe("/home/user/a;/tmp/b");
});

test("windows-host-paths maps both WSL UNC forms and drive paths back to POSIX", () => {
  expect(normalizePathFromWindowsHostProcess(String.raw`\\wsl.localhost\Ubuntu\home\user\repo with spaces`)).toBe("/home/user/repo with spaces");
  expect(normalizePathFromWindowsHostProcess(String.raw`\\wsl$\Ubuntu-24.04\tmp\artifact.bin`)).toBe("/tmp/artifact.bin");
  expect(normalizePathFromWindowsHostProcess("//wsl.localhost/Ubuntu/home/user/repo")).toBe("/home/user/repo");
  expect(normalizePathFromWindowsHostProcess(String.raw`\\wsl$\Ubuntu`)).toBe("/");
  expect(normalizePathFromWindowsHostProcess(String.raw`C:\repo\artifact.bin`)).toBe("/mnt/c/repo/artifact.bin");
  expect(normalizePathFromWindowsHostProcess("D:/repo/artifact.bin")).toBe("/mnt/d/repo/artifact.bin");
  expect(normalizePathFromWindowsHostProcess(String.raw`\\server\share\artifact.bin`)).toBe(String.raw`\\server\share\artifact.bin`);
  expect(normalizePathFromWindowsHostProcess("/home/user/repo")).toBe("/home/user/repo");
  expect(normalizePathFromWindowsHostProcess(null)).toBeNull();
});

test("windows-host-paths reports probe failures without control characters", () => {
  expect(formatPowerShellProbeFailure("powershell.exe", { error: { code: "ENOEXEC" }, status: null })).toBe(
    "powershell.exe: ENOEXEC",
  );
  expect(formatPowerShellProbeFailure("pwsh", { error: null, status: 2, stderr: "MZ\u0000bad\nsecond line" })).toBe(
    "pwsh: exit 2 (MZ?bad)",
  );
});

test("windows-host-paths explains unavailable WSL Windows interop separately from missing PowerShell", () => {
  const message = buildPowerShellResolutionErrorMessage(
    ["/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", "pwsh"],
    ["/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe: ENOEXEC", "pwsh: ENOENT"],
    { isWsl: true },
  );

  expect(message).toContain("PowerShell executable was not runnable");
  expect(message).toContain("WSL Windows interop appears unavailable");
  expect(message).toContain("install a Linux pwsh");
  expect(message).toContain("Probe failures:");
});
