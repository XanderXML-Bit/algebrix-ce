import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";

const buildCache = new Map();
const warnedSuites = new Set();

export function resolveNpmRunInvocation(scriptName) {
  if (process.platform === "win32") {
    return {
      // Windows batch shims like npm.cmd are not valid spawn targets without cmd.exe.
      command: "cmd.exe",
      args: ["/d", "/s", "/c", `npm run --silent ${scriptName}`],
    };
  }

  return {
    command: "npm",
    args: ["run", "--silent", scriptName],
  };
}

export function runCommand(command, args, cwd, options = {}) {
  const result = spawnSync(command, args, {
    cwd,
    encoding: "utf8",
    input: options.input,
    shell: options.shell ?? false,
  });

  if (result.error) {
    throw result.error;
  }

  return result;
}

function trimOutput(output) {
  return output
    .split(/\r?\n/)
    .map((line) => line.trimEnd())
    .filter(Boolean)
    .join("\n");
}

export function isSkippableNativeBuildFailure(output) {
  return /(powershell|pwsh)(\.exe)?: not found|not recognized as an internal or external command/i.test(output);
}

function warnOnce(key, message) {
  if (warnedSuites.has(key)) {
    return;
  }

  warnedSuites.add(key);
  console.warn(message);
}

export function resolveMissingRequiredPaths(repoRoot, requiredPaths = []) {
  return requiredPaths
    .map((requiredPath) => ({
      displayPath: requiredPath,
      absolutePath: path.resolve(repoRoot, requiredPath),
    }))
    .filter(({ absolutePath }) => !fs.existsSync(absolutePath));
}

export function resolveNativeArtifactPath(artifactPath, platform = process.platform) {
  return platform === "win32" ? artifactPath : artifactPath.replace(/\.exe$/i, "");
}

export function isNativeArtifactAvailable(artifactPath, platform = process.platform, fileSystem = fs) {
  if (!fileSystem.existsSync(artifactPath)) {
    return false;
  }
  if (platform === "win32") {
    return true;
  }
  try {
    fileSystem.accessSync(artifactPath, fs.constants.X_OK);
    return fileSystem.statSync(artifactPath).isFile();
  } catch {
    return false;
  }
}

export function resolveNativeToolchain(env = process.env) {
  const onPath = (command) => {
    // CC is split on whitespace like make's shell does.
    const executable = command?.trim().split(/\s+/)[0];
    return executable && (env.PATH ?? "").split(path.delimiter)
      .some((directory) => isNativeArtifactAvailable(path.resolve(directory, executable), "linux"));
  };
  const compiler = env.CC?.trim() || ["gcc", "cc"].find(onPath);
  return onPath("make") && onPath(compiler) ? { compiler } : null;
}

function cachedBuild(key, cacheDir, build) {
  const memoryKey = `${cacheDir ?? ""}::${key}`;
  const resultPath = cacheDir
    ? path.join(cacheDir, `${createHash("sha256").update(key).digest("hex")}.json`)
    : null;
  if (resultPath || !buildCache.has(memoryKey)) {
    if (resultPath && fs.existsSync(resultPath)) {
      buildCache.set(memoryKey, JSON.parse(fs.readFileSync(resultPath, "utf8")));
    } else {
      let result;
      try {
        result = build();
      } catch (error) {
        result = { failed: true, error: String(error?.message ?? error) || "native build failed" };
      }
      // Vitest runs files sequentially. Publish failures too so every consumer throws.
      if (resultPath) {
        const temporaryPath = `${resultPath}.${process.pid}.tmp`;
        fs.writeFileSync(temporaryPath, JSON.stringify(result));
        fs.renameSync(temporaryPath, resultPath);
      }
      buildCache.set(memoryKey, result);
    }
  }
  const result = buildCache.get(memoryKey);
  if (result.failed) {
    throw new Error(result.error);
  }
  return result;
}

export function resolveNativeSuite({
  key,
  label,
  repoRoot,
  buildScript,
  artifactPath,
  requiredPaths = [],
  preflightSkipReason = null,
}, { platform = process.platform, env = process.env, run = runCommand } = {}) {
  const useMake = platform !== "win32" && buildScript === "build:desktop";
  const buildCacheKey = `${repoRoot}::${useMake ? "make-native" : buildScript}`;
  const artifactAbsolutePath = path.resolve(repoRoot, resolveNativeArtifactPath(artifactPath, platform));
  const missingRequiredPaths = resolveMissingRequiredPaths(repoRoot, requiredPaths);

  if (missingRequiredPaths.length > 0) {
    const missingSummary = missingRequiredPaths
      .map(({ displayPath }) => displayPath)
      .join(", ");
    const skipReason = `${label} skipped: required repo fixture(s) are unavailable: ${missingSummary}.`;
    warnOnce(key, `[SKIP] ${skipReason}`);
    return {
      available: false,
      artifactPath: artifactAbsolutePath,
      buildOutput: "",
      skipReason,
    };
  }

  const preflightReason = typeof preflightSkipReason === "function"
    ? preflightSkipReason()
    : preflightSkipReason;
  if (typeof preflightReason === "string" && preflightReason.trim().length > 0) {
    const skipReason = `${label} skipped: ${preflightReason.trim()}`;
    warnOnce(key, `[SKIP] ${skipReason}`);
    return {
      available: false,
      artifactPath: artifactAbsolutePath,
      buildOutput: "",
      skipReason,
    };
  }

  if (platform !== "win32" && !useMake) {
    const skipReason = `${label} skipped: required native artifact is unavailable at ${artifactAbsolutePath}.`;
    const buildOutput = `[WARN] ${label} build skipped: ${buildScript} produces Windows-only artifacts; not runnable on ${platform}.`;
    warnOnce(key, `${buildOutput}\n[SKIP] ${skipReason}`);
    return { available: false, artifactPath: artifactAbsolutePath, buildOutput, skipReason };
  }

  const build = cachedBuild(buildCacheKey, useMake ? env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR : null, () => {
    const toolchain = useMake ? resolveNativeToolchain(env) : null;
    if (useMake && !toolchain) {
      return {
        buildOutput: "[WARN] Native build skipped because make or a C compiler is unavailable on PATH.",
        unavailable: true,
      };
    } else {
      const invocation = useMake
        ? { command: "make", args: ["-C", "tests", "all", `CC=${toolchain.compiler}`] }
        : resolveNpmRunInvocation(buildScript);
      const buildResult = run(invocation.command, invocation.args, repoRoot);
      const combinedOutput = trimOutput(`${buildResult.stdout ?? ""}\n${buildResult.stderr ?? ""}`);

      if (buildResult.status !== 0) {
        if (!useMake && isSkippableNativeBuildFailure(combinedOutput)) {
          return {
            buildOutput: `[WARN] ${label} build was skipped because PowerShell is unavailable on ${process.platform}.\n${combinedOutput}`,
          };
        } else {
          const details = combinedOutput.length > 0 ? `\n${combinedOutput}` : "";
          throw new Error(
            `${label} build failed via "${invocation.command} ${invocation.args.join(" ")}" (exit ${buildResult.status}).${details}`,
          );
        }
      } else {
        return {
          buildOutput: combinedOutput,
        };
      }
    }
  });
  const combinedOutput = build.buildOutput;

  const available = !build.unavailable &&
    isNativeArtifactAvailable(artifactAbsolutePath, platform);
  const suite = {
    available,
    artifactPath: artifactAbsolutePath,
    buildOutput: combinedOutput,
    skipReason: "",
  };

  if (!available) {
    suite.skipReason = `${label} skipped: required native artifact is unavailable at ${artifactAbsolutePath}.`;
    if (combinedOutput.length > 0) {
      warnOnce(key, `${combinedOutput}\n[SKIP] ${suite.skipReason}`);
    } else {
      warnOnce(key, `[SKIP] ${suite.skipReason}`);
    }
  } else if (combinedOutput.includes("[WARN]")) {
    warnOnce(key, combinedOutput);
  }

  return suite;
}
