Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-PathExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Message Path: '$Path'"
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$buildScript = Join-Path $repoRoot "tools\emulator\ez80\build_tests.ps1"
$runnerPath = Join-Path $repoRoot "tools\emulator\ez80\tests\ez80_test_runner.exe"

Assert-PathExists -Path $buildScript -Message "Native eZ80 build script was not found."

Write-Host "=== eZ80 Scaffold Runtime Tests ==="

& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $buildScript
if ($LASTEXITCODE -ne 0) {
    throw "eZ80 runtime build failed with exit code $LASTEXITCODE."
}

Assert-PathExists -Path $runnerPath -Message "Built eZ80 runtime test runner was not found."

& $runnerPath
if ($LASTEXITCODE -ne 0) {
    throw "Runtime tests failed with exit code $LASTEXITCODE."
}

Write-Host "PASS: eZ80 scaffold runtime tests passed."
exit 0
