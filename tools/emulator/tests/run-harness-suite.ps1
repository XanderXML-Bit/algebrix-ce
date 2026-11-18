$ErrorActionPreference = "Stop"

$scripts = @(
  "tools/emulator/tests/run-terminal-harness-tests.ps1",
  "tools/emulator/tests/run-terminal-regression-tests.ps1",
  "tools/emulator/tests/run-cemu-autotest-tests.ps1",
  "tools/emulator/tests/run-comprehensive-autotest-tests.ps1",
  "tools/emulator/tests/run-prehardware-validation-gate-tests.ps1",
  "tools/emulator/tests/run-cemu-foundation-tests.ps1"
)

foreach ($script in $scripts) {
  Write-Host ("[PS-TEST] " + $script)
  & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
}
