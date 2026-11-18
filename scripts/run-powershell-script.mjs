import path from "node:path";

import { spawnPowerShellFile } from "../tests/helpers/windows-host-paths.mjs";

const [scriptPath, ...scriptArgs] = process.argv.slice(2);

if (!scriptPath) {
  console.error("Usage: node ./scripts/run-powershell-script.mjs <script.ps1> [args...]");
  process.exit(1);
}

const resolvedScriptPath = path.resolve(scriptPath);
const result = spawnPowerShellFile(resolvedScriptPath, scriptArgs, {
  cwd: process.cwd(),
  stdio: "inherit",
});

if (result.error) {
  throw result.error;
}

process.exit(result.status ?? 1);
