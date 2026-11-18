import os from "node:os";
import { afterEach, beforeEach, expect, test, vi } from "vitest";

const windowsPwsh = "/mnt/c/Program Files/PowerShell/7/pwsh.exe";
const windowsPowerShell = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
const launchFailure = {
  status: 1,
  stdout: Buffer.from(""),
  stderr: Buffer.from("<3>WSL (1234 - ) ERROR: UtilAcceptVsock:281: accept4 failed 110\n"),
};
const version = { status: 0, stdout: "7.4.0\n", stderr: "" };
const missing = { error: { code: "ENOENT" }, status: null, stdout: null, stderr: null };
const originalPlatform = Object.getOwnPropertyDescriptor(process, "platform");
let spawn;
let wait;

beforeEach(() => {
  vi.resetModules();
  spawn = vi.fn();
  vi.doMock("node:child_process", () => ({ spawnSync: spawn }));
  Object.defineProperty(process, "platform", { value: "linux" });
  vi.stubEnv("WSL_DISTRO_NAME", "Ubuntu");
  vi.spyOn(os, "release").mockReturnValue("6.1.0-generic");
  wait = vi.spyOn(Atomics, "wait").mockReturnValue("timed-out");
});

afterEach(() => {
  vi.doUnmock("node:child_process");
  vi.restoreAllMocks();
  vi.unstubAllEnvs();
  Object.defineProperty(process, "platform", originalPlatform);
});

test("spawnPowerShellFile retries Windows PowerShell launch failures on WSL", async () => {
  const { resolvePowerShellExecutable, spawnPowerShellFile } = await import("./helpers/windows-host-paths.mjs");
  spawn.mockReturnValueOnce(version);
  expect(resolvePowerShellExecutable()).toBe(windowsPwsh);
  spawn.mockClear();
  const success = { status: 0, stdout: Buffer.from('{"success":true}'), stderr: Buffer.from("") };
  spawn.mockReturnValueOnce(launchFailure).mockReturnValueOnce(launchFailure).mockReturnValueOnce(success);

  expect(spawnPowerShellFile("/mnt/c/fixtures/test.ps1", ["/mnt/d/input.json"], { encoding: "buffer" })).toBe(success);
  expect(spawn).toHaveBeenCalledTimes(3);
  for (const call of spawn.mock.calls) {
    expect(call).toEqual([
      windowsPwsh,
      ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", String.raw`C:\fixtures\test.ps1`, String.raw`D:\input.json`],
      { encoding: "buffer" },
    ]);
  }
  expect(wait.mock.calls.map((call) => call[3])).toEqual([250, 1000]);
});

test.each([
  { platform: "linux", wsl: "", executable: windowsPwsh },
  { platform: "linux", wsl: "Ubuntu", executable: "pwsh" },
  { platform: "win32", wsl: "Ubuntu", executable: "pwsh.exe" },
])("spawnPowerShellFile launches once on $platform with WSL=$wsl and $executable", async ({ platform, wsl, executable }) => {
  Object.defineProperty(process, "platform", { value: platform });
  vi.stubEnv("WSL_DISTRO_NAME", wsl);
  const { resolvePowerShellExecutable, spawnPowerShellFile } = await import("./helpers/windows-host-paths.mjs");
  if (executable === "pwsh") {
    spawn.mockReturnValueOnce(missing).mockReturnValueOnce(missing);
  }
  spawn.mockReturnValueOnce(version);
  expect(resolvePowerShellExecutable()).toBe(executable);
  spawn.mockClear();
  spawn.mockReturnValueOnce(launchFailure);

  expect(spawnPowerShellFile("relative/test.ps1", [], { encoding: "buffer" })).toBe(launchFailure);
  expect(spawn).toHaveBeenCalledExactlyOnceWith(
    executable,
    ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", "relative/test.ps1"],
    { encoding: "buffer" },
  );
  expect(wait).not.toHaveBeenCalled();
});

test("resolvePowerShellExecutable retries the version probe on the same Windows candidate", async () => {
  const { resolvePowerShellExecutable } = await import("./helpers/windows-host-paths.mjs");
  spawn.mockReturnValueOnce(launchFailure).mockReturnValueOnce(launchFailure).mockReturnValueOnce(version);

  expect(resolvePowerShellExecutable()).toBe(windowsPwsh);
  expect(spawn).toHaveBeenCalledTimes(3);
  for (const call of spawn.mock.calls) {
    expect(call).toEqual([
      windowsPwsh,
      ["-NoProfile", "-NonInteractive", "-Command", "$PSVersionTable.PSVersion.ToString()"],
      { encoding: "utf8" },
    ]);
  }
  expect(wait.mock.calls.map((call) => call[3])).toEqual([250, 1000]);
  expect(resolvePowerShellExecutable()).toBe(windowsPwsh);
  expect(spawn).toHaveBeenCalledTimes(3);
});

test.each([
  { platform: "linux", wsl: "", candidates: [windowsPwsh, windowsPowerShell] },
  { platform: "win32", wsl: "Ubuntu", candidates: ["pwsh.exe", "powershell.exe"] },
  { platform: "linux", wsl: "Ubuntu", candidates: [windowsPwsh, windowsPowerShell, "pwsh", "powershell"] },
])("resolvePowerShellExecutable skips retry outside WSL Windows probes: $platform WSL=$wsl", async ({ platform, wsl, candidates }) => {
  Object.defineProperty(process, "platform", { value: platform });
  vi.stubEnv("WSL_DISTRO_NAME", wsl);
  const { resolvePowerShellExecutable } = await import("./helpers/windows-host-paths.mjs");
  if (candidates.includes("pwsh")) {
    spawn.mockReturnValueOnce(missing).mockReturnValueOnce(missing);
  }
  spawn.mockReturnValueOnce(launchFailure).mockReturnValueOnce(version);

  expect(resolvePowerShellExecutable()).toBe(candidates.at(-1));
  expect(spawn.mock.calls.map((call) => call[0])).toEqual(candidates);
  expect(wait).not.toHaveBeenCalled();
});
