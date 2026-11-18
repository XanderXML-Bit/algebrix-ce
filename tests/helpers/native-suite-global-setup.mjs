import fs from "node:fs";
import os from "node:os";
import path from "node:path";

export function clearNativeBuildCache(dir) {
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
}

// One cache dir per Vitest session: `vitest run` gets a fresh dir, and watch
// reruns / rerunTestSpecifications() clear it through onTestsRerun. Known limit:
// a long-lived programmatic Vitest instance that calls runTestSpecifications()
// more than once does not fire onTestsRerun, so it reuses the first build result.
// Call rerunTestSpecifications() (or start a new Vitest instance) to force a rebuild.
export default function setup(project) {
  const cacheDir = fs.mkdtempSync(path.join(os.tmpdir(), "algebrix-native-build-"));
  const previous = process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR;
  process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR = cacheDir;
  if (typeof project?.onTestsRerun === "function") {
    project.onTestsRerun(() => clearNativeBuildCache(cacheDir));
  }

  return () => {
    fs.rmSync(cacheDir, { recursive: true, force: true });
    if (previous === undefined) {
      delete process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR;
    } else {
      process.env.ALGEBRIX_NATIVE_BUILD_CACHE_DIR = previous;
    }
  };
}
