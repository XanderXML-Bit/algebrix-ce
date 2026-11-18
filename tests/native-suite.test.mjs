import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import assert from "node:assert/strict";

import { expect, test, vi } from "vitest";
import setup, { clearNativeBuildCache } from "./helpers/native-suite-global-setup.mjs";
import {
  isNativeArtifactAvailable,
  isSkippableNativeBuildFailure,
  resolveNativeArtifactPath,
  resolveNativeSuite,
  resolveNativeToolchain,
  resolveNpmRunInvocation,
} from "./helpers/native-suite.mjs";

test("native-suite resolves npm through cmd.exe on Windows", () => {
  const invocation = resolveNpmRunInvocation("build:desktop");

  if (process.platform === "win32") {
    expect(invocation).toEqual({
      command: "cmd.exe",
      args: ["/d", "/s", "/c", "npm run --silent build:desktop"],
    });
    return;
  }

  expect(invocation).toEqual({
    command: "npm",
    args: ["run", "--silent", "build:desktop"],
  });
});

test("native-suite treats missing PowerShell as a skippable native build failure", () => {
  expect(isSkippableNativeBuildFailure("sh: 1: powershell: not found")).toBe(true);
  expect(isSkippableNativeBuildFailure("'pwsh' is not recognized as an internal or external command")).toBe(true);
  expect(isSkippableNativeBuildFailure("gcc: fatal error: no input files")).toBe(false);
});

test("native-suite marks suites unavailable when required repo fixtures are missing", () => {
  const repoRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-native-suite-"));

  try {
    const suite = resolveNativeSuite({
      key: "fixture-gated-suite",
      label: "Fixture gated suite",
      repoRoot,
      buildScript: "build:fixture-gated-suite",
      artifactPath: path.join("bin", "fixture.exe"),
      requiredPaths: [path.join("rom", "Rom.rom")],
    });

    expect(suite.available).toBe(false);
    expect(suite.skipReason).toContain(path.join("rom", "Rom.rom"));
  } finally {
    fs.rmSync(repoRoot, { recursive: true, force: true });
  }
});

test("native-suite can skip before build when a host capability is unavailable", () => {
  const repoRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-native-suite-"));

  try {
    const suite = resolveNativeSuite({
      key: "preflight-gated-suite",
      label: "Preflight gated suite",
      repoRoot,
      buildScript: "build:missing-tooling",
      artifactPath: path.join("bin", "fixture.exe"),
      preflightSkipReason: () => "Windows-host PowerShell is unavailable.",
    });

    expect(suite.available).toBe(false);
    expect(suite.skipReason).toContain("Windows-host PowerShell is unavailable");
  } finally {
    fs.rmSync(repoRoot, { recursive: true, force: true });
  }
});

test("native-suite selects extensionless siblings only outside Windows", () => {
  for (const artifact of ["bin/calcengine.exe", "tests/test_runner.exe", "tools/emulator/ez80/ez80_cli.exe"]) {
    expect(resolveNativeArtifactPath(artifact, "win32")).toBe(artifact);
    expect(resolveNativeArtifactPath(artifact, "linux")).toBe(artifact.slice(0, -4));
    expect(resolveNativeArtifactPath(artifact, "darwin")).toBe(artifact.slice(0, -4));
  }
  expect(resolveNativeArtifactPath("bin/calcengine", "linux")).toBe("bin/calcengine");
});

test("native-suite requires executable access outside Windows", () => {
  const fileSystem = {
    existsSync: vi.fn(() => true),
    accessSync: vi.fn(() => { throw new Error("EACCES"); }),
    statSync: vi.fn(() => ({ isFile: () => true })),
  };
  expect(isNativeArtifactAvailable("runner.exe", "win32", fileSystem)).toBe(true);
  expect(fileSystem.accessSync).not.toHaveBeenCalled();
  expect(isNativeArtifactAvailable("runner", "linux", fileSystem)).toBe(false);
  expect(fileSystem.accessSync).toHaveBeenCalledWith("runner", fs.constants.X_OK);
  fileSystem.accessSync.mockImplementation(() => {});
  expect(isNativeArtifactAvailable("runner", "linux", fileSystem)).toBe(true);
  fileSystem.existsSync.mockReturnValue(false);
  expect(isNativeArtifactAvailable("runner.exe", "win32", fileSystem)).toBe(false);
  expect(isNativeArtifactAvailable("runner", "linux", fileSystem)).toBe(false);
});

function withNativeToolchain(check) {
  const repoRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-toolchain-"));
  try {
    for (const name of ["make", "gcc", "cc", "custom-cc"]) {
      fs.writeFileSync(path.join(repoRoot, name), "", { mode: 0o755 });
    }
    check(repoRoot, { PATH: repoRoot });
  } finally {
    fs.rmSync(repoRoot, { recursive: true, force: true });
  }
}

test("native-suite discovers make and the configured or fallback C compiler", () => {
  withNativeToolchain((repoRoot, env) => {
    expect(resolveNativeToolchain(env)).toEqual({ compiler: "gcc" });
    expect(resolveNativeToolchain({ ...env, CC: "custom-cc" })).toEqual({ compiler: "custom-cc" });
    fs.rmSync(path.join(repoRoot, "gcc"));
    expect(resolveNativeToolchain(env)).toEqual({ compiler: "cc" });
    fs.rmSync(path.join(repoRoot, "make"));
    expect(resolveNativeToolchain(env)).toBeNull();
  });
});

test("native-suite builds both desktop artifacts once per repo root", () => {
  withNativeToolchain((repoRoot, env) => {
    const run = vi.fn(() => ({ status: 0, stdout: "native build\n", stderr: "" }));
    for (const artifactPath of ["calcengine.exe", "test_runner.exe"]) {
      fs.writeFileSync(path.join(repoRoot, artifactPath), "Windows artifact");
      fs.writeFileSync(path.join(repoRoot, artifactPath.slice(0, -4)), "", { mode: 0o755 });
      const suite = resolveNativeSuite({
        key: artifactPath, label: "Desktop", repoRoot, buildScript: "build:desktop", artifactPath,
      }, { platform: "linux", env, run });
      expect(suite.available).toBe(true);
      expect(suite.artifactPath).toBe(path.join(repoRoot, artifactPath.slice(0, -4)));
      expect(suite.buildOutput).toBe("native build");
    }
    expect(run).toHaveBeenCalledExactlyOnceWith("make", ["-C", "tests", "all", "CC=gcc"], repoRoot);
  });
});

test("native-suite skips absent tooling and rejects genuine native build failures", () => {
  withNativeToolchain((repoRoot, env) => {
    const options = {
      key: "native-no-tools", label: "Desktop", repoRoot,
      buildScript: "build:desktop", artifactPath: "calcengine.exe",
    };
    const run = vi.fn();
    fs.writeFileSync(path.join(repoRoot, "calcengine.exe"), "Windows artifact");
    const suite = resolveNativeSuite(options, { platform: "linux", env: { PATH: "" }, run });
    expect(suite.available).toBe(false);
    expect(suite.skipReason).toBe(`Desktop skipped: required native artifact is unavailable at ${path.join(repoRoot, "calcengine")}.`);
    expect(run).not.toHaveBeenCalled();
    run.mockReturnValue({ status: 2, stdout: "", stderr: "compiler error" });
    expect(() => resolveNativeSuite({ ...options, repoRoot: path.join(repoRoot, "failed-build") }, {
      platform: "linux", env, run,
    })).toThrow(/build failed via "make .*exit 2.*compiler error/s);
  });
});

test("native-suite skips Windows-only builds off win32, even with an executable sibling", () => {
  withNativeToolchain((repoRoot, env) => {
    const run = vi.fn(() => { throw new Error("Windows-only build must not run"); });
    fs.writeFileSync(path.join(repoRoot, "ez80_cli"), "", { mode: 0o755 });
    const suite = resolveNativeSuite({
      key: "ez80-linux", label: "eZ80", repoRoot,
      buildScript: "build:ez80", artifactPath: "ez80_cli.exe",
    }, { platform: "linux", env, run });
    assert.equal(run.mock.calls.length, 0);
    assert.equal(suite.available, false);
    assert.equal(suite.skipReason, `eZ80 skipped: required native artifact is unavailable at ${path.join(repoRoot, "ez80_cli")}.`);
    assert.equal(suite.buildOutput, "[WARN] eZ80 build skipped: build:ez80 produces Windows-only artifacts; not runnable on linux.");
  });
});

test("native-suite still runs Windows-only builds on win32", () => {
  withNativeToolchain((repoRoot, env) => {
    const run = vi.fn(() => ({ status: 0, stdout: "ez80 build", stderr: "" }));
    fs.writeFileSync(path.join(repoRoot, "ez80_cli.exe"), "Windows artifact");
    const suite = resolveNativeSuite({
      key: "ez80-win32", label: "eZ80", repoRoot,
      buildScript: "build:ez80", artifactPath: "ez80_cli.exe",
    }, { platform: "win32", env, run });
    assert.deepEqual(run.mock.calls, [[
      resolveNpmRunInvocation("build:ez80").command,
      resolveNpmRunInvocation("build:ez80").args,
      repoRoot,
    ]]);
    assert.equal(suite.available, true);
    assert.equal(suite.buildOutput, "ez80 build");
  });
});

test("native-suite checks fixtures before preflight and platform skips", () => {
  withNativeToolchain((repoRoot, env) => {
    const run = vi.fn();
    const preflightSkipReason = vi.fn(() => "host capability unavailable");
    const options = {
      key: "ez80-order", label: "eZ80", repoRoot,
      buildScript: "build:ez80", artifactPath: "ez80_cli.exe",
      requiredPaths: ["fixture"], preflightSkipReason,
    };
    const missingFixture = resolveNativeSuite(options, { platform: "linux", env, run });
    assert.match(missingFixture.skipReason, /required repo fixture/);
    assert.equal(preflightSkipReason.mock.calls.length, 0);
    fs.writeFileSync(path.join(repoRoot, "fixture"), "");
    const missingCapability = resolveNativeSuite(options, { platform: "linux", env, run });
    assert.equal(missingCapability.skipReason, "eZ80 skipped: host capability unavailable");
    assert.equal(preflightSkipReason.mock.calls.length, 1);
    assert.equal(run.mock.calls.length, 0);
  });
});

test("native-suite checks the compiler token and passes all CC arguments to make", () => {
  withNativeToolchain((repoRoot, env) => {
    assert.deepEqual(resolveNativeToolchain({ ...env, CC: "gcc -m64" }), { compiler: "gcc -m64" });
    assert.deepEqual(resolveNativeToolchain({ ...env, CC: "" }), { compiler: "gcc" });
    const run = vi.fn(() => ({ status: 0, stdout: "", stderr: "" }));
    resolveNativeSuite({
      key: "desktop-cc-args", label: "Desktop", repoRoot,
      buildScript: "build:desktop", artifactPath: "calcengine.exe",
    }, { platform: "linux", env: { ...env, CC: "gcc -m64" }, run });
    assert.deepEqual(run.mock.calls, [["make", ["-C", "tests", "all", "CC=gcc -m64"], repoRoot]]);
  });
});

test("native-suite accepts an absolute compiler path with or without arguments", () => {
  const exists = vi.spyOn(fs, "existsSync").mockReturnValue(true);
  const access = vi.spyOn(fs, "accessSync").mockImplementation(() => {});
  const stat = vi.spyOn(fs, "statSync").mockReturnValue({ isFile: () => true });
  try {
    for (const compiler of ["/usr/bin/gcc", "/usr/bin/gcc -m64"]) {
      access.mockClear();
      assert.deepEqual(resolveNativeToolchain({ PATH: "/usr/bin", CC: compiler }), { compiler });
      assert.ok(access.mock.calls.some(([target]) => target === path.resolve("/usr/bin/gcc")));
    }
  } finally {
    exists.mockRestore();
    access.mockRestore();
    stat.mockRestore();
  }
});

test("native-suite shares desktop results across files, but rebuilds in a new run", async () => {
  const repoRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-build-cache-"));
  try {
    for (const name of ["make", "gcc", "calcengine", "test_runner"]) {
      fs.writeFileSync(path.join(repoRoot, name), "", { mode: 0o755 });
    }
    const firstRun = fs.mkdtempSync(path.join(repoRoot, "run-"));
    const nextRun = fs.mkdtempSync(path.join(repoRoot, "run-"));
    const run = vi.fn(() => ({ status: 0, stdout: "shared build", stderr: "" }));
    const resolveInNewFile = async (cacheDir, artifactPath = "calcengine.exe", root = repoRoot) => {
      vi.resetModules();
      const { resolveNativeSuite: resolveInFile } = await import("./helpers/native-suite.mjs");
      return resolveInFile({
        key: "desktop-cache", label: "Desktop", repoRoot: root,
        buildScript: "build:desktop", artifactPath,
      }, { platform: "linux", env: { PATH: repoRoot, ALGEBRIX_NATIVE_BUILD_CACHE_DIR: cacheDir }, run });
    };
    assert.equal((await resolveInNewFile(firstRun)).available, true);
    const reused = await resolveInNewFile(firstRun, "test_runner.exe");
    assert.equal(reused.available, true);
    assert.equal(reused.buildOutput, "shared build");
    assert.equal(run.mock.calls.length, 1);
    await resolveInNewFile(nextRun);
    assert.equal(run.mock.calls.length, 2);
    // A second checkout never consumes the first checkout's result.
    await resolveInNewFile(firstRun, "calcengine.exe", path.join(repoRoot, "other-repo"));
    assert.equal(run.mock.calls.length, 3);

    run.mockReturnValue({ status: 2, stdout: "", stderr: "compiler error" });
    const failedRun = fs.mkdtempSync(path.join(repoRoot, "run-"));
    await assert.rejects(resolveInNewFile(failedRun), /build failed.*exit 2.*compiler error/s);
    await assert.rejects(resolveInNewFile(failedRun, "test_runner.exe"), /build failed.*exit 2.*compiler error/s);
    assert.equal(run.mock.calls.length, 4);
    run.mockImplementation(() => { throw new Error("spawn failed"); });
    const spawnFailureRun = fs.mkdtempSync(path.join(repoRoot, "run-"));
    await assert.rejects(resolveInNewFile(spawnFailureRun), /spawn failed/);
    await assert.rejects(resolveInNewFile(spawnFailureRun, "test_runner.exe"), /spawn failed/);
    assert.equal(run.mock.calls.length, 5);
  } finally {
    fs.rmSync(repoRoot, { recursive: true, force: true });
  }
});

test("native-suite clears cached results and nested files without removing the cache directory", () => {
  const cacheDir = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-cache-clear-"));
  try {
    fs.mkdirSync(path.join(cacheDir, "nested"));
    fs.writeFileSync(path.join(cacheDir, "nested", "result.json"), "{}");
    clearNativeBuildCache(cacheDir);
    assert.deepEqual(fs.readdirSync(cacheDir), []);
    clearNativeBuildCache(cacheDir);
    assert.deepEqual(fs.readdirSync(cacheDir), []);
  } finally {
    fs.rmSync(cacheDir, { recursive: true, force: true });
  }
});

test("native-suite setup registers a rerun hook that invalidates successes and failures", () => {
  const previous = process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR;
  const project = { onTestsRerun: vi.fn() };
  const teardown = setup(project);
  const cacheDir = process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR;
  try {
    assert.equal(project.onTestsRerun.mock.calls.length, 1);
    const [rerun] = project.onTestsRerun.mock.calls[0];
    withNativeToolchain((repoRoot, env) => {
      fs.writeFileSync(path.join(repoRoot, "calcengine"), "", { mode: 0o755 });
      const run = vi.fn(() => ({ status: 0, stdout: "", stderr: "" }));
      const resolve = () => resolveNativeSuite({
        key: "desktop-rerun", label: "Desktop", repoRoot,
        buildScript: "build:desktop", artifactPath: "calcengine.exe",
      }, { platform: "linux", env: { ...env, ALGEBRIX_NATIVE_BUILD_CACHE_DIR: cacheDir }, run });
      assert.equal(resolve().available, true);
      assert.equal(resolve().available, true);
      assert.equal(run.mock.calls.length, 1);

      rerun();
      assert.deepEqual(fs.readdirSync(cacheDir), []);
      run.mockImplementation(() => { throw new Error("rerun failed"); });
      assert.throws(resolve, /rerun failed/);
      assert.throws(resolve, /rerun failed/);
      assert.equal(run.mock.calls.length, 2);

      rerun();
      run.mockReturnValue({ status: 0, stdout: "rebuilt", stderr: "" });
      assert.equal(resolve().buildOutput, "rebuilt");
      assert.equal(resolve().buildOutput, "rebuilt");
      assert.equal(run.mock.calls.length, 3);
    });
  } finally {
    teardown();
  }
  assert.equal(fs.existsSync(cacheDir), false);
  assert.equal(process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR, previous);
});

test("native-suite setup supports projects without the rerun API", () => {
  const previous = process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR;
  for (const project of [undefined, {}]) {
    const teardown = setup(project);
    const cacheDir = process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR;
    try {
      assert.equal(fs.statSync(cacheDir).isDirectory(), true);
    } finally {
      teardown();
    }
    assert.equal(fs.existsSync(cacheDir), false);
    assert.equal(process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR, previous);
  }
});

test("native-suite caches an empty-message throw as a failure across files", async () => {
  const repoRoot = fs.mkdtempSync(path.join(os.tmpdir(), "calcengine-empty-error-"));
  try {
    const cacheDir = fs.mkdtempSync(path.join(repoRoot, "run-"));
    const run = vi.fn(() => { throw new Error(""); });
    const options = {
      key: "empty-error", label: "Desktop", repoRoot,
      buildScript: "build:desktop", artifactPath: "calcengine.exe",
    };
    const dependencies = {
      platform: "linux", env: { PATH: process.env.PATH, ALGEBRIX_NATIVE_BUILD_CACHE_DIR: cacheDir }, run,
    };
    // Supply a compiler fixture so the failure check is independent of host tooling.
    for (const name of ["make", "gcc"]) {
      fs.writeFileSync(path.join(repoRoot, name), "", { mode: 0o755 });
    }
    dependencies.env.PATH = repoRoot;
    assert.throws(() => resolveNativeSuite(options, dependencies), /^Error: native build failed$/);
    const [resultFile] = fs.readdirSync(cacheDir);
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(cacheDir, resultFile), "utf8")), {
      failed: true, error: "native build failed",
    });
    vi.resetModules();
    const { resolveNativeSuite: resolveInNewFile } = await import("./helpers/native-suite.mjs");
    assert.throws(() => resolveInNewFile(options, dependencies), /^Error: native build failed$/);
    assert.equal(run.mock.calls.length, 1);
  } finally {
    fs.rmSync(repoRoot, { recursive: true, force: true });
  }
});

test("native-suite publishes complete cache JSON by renaming a process-specific temporary file", () => {
  withNativeToolchain((repoRoot, env) => {
    const cacheDir = fs.mkdtempSync(path.join(repoRoot, "run-"));
    const renameSync = fs.renameSync;
    const rename = vi.spyOn(fs, "renameSync").mockImplementation((temporaryPath, resultPath) => {
      assert.equal(temporaryPath, `${resultPath}.${process.pid}.tmp`);
      assert.equal(fs.existsSync(resultPath), false);
      assert.deepEqual(JSON.parse(fs.readFileSync(temporaryPath, "utf8")), { buildOutput: "complete" });
      renameSync(temporaryPath, resultPath);
    });
    try {
      resolveNativeSuite({
        key: "atomic-cache", label: "Desktop", repoRoot,
        buildScript: "build:desktop", artifactPath: "calcengine.exe",
      }, {
        platform: "linux", env: { ...env, ALGEBRIX_NATIVE_BUILD_CACHE_DIR: cacheDir },
        run: () => ({ status: 0, stdout: "complete", stderr: "" }),
      });
      assert.equal(rename.mock.calls.length, 1);
      assert.equal(fs.readdirSync(cacheDir).length, 1);
      assert.equal(fs.readdirSync(cacheDir)[0].endsWith(".json"), true);
    } finally {
      rename.mockRestore();
    }
  });
});

test("native-suite falls back to gcc or cc when CC contains only whitespace", () => {
  withNativeToolchain((repoRoot, env) => {
    assert.deepEqual(resolveNativeToolchain({ ...env, CC: " \t\n " }), { compiler: "gcc" });
    fs.rmSync(path.join(repoRoot, "gcc"));
    assert.deepEqual(resolveNativeToolchain({ ...env, CC: " \t\n " }), { compiler: "cc" });
  });
});
