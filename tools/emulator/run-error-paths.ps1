<#
.SYNOPSIS
    Run the CalcEngine error-path autotest suite against the headless CEmu
    autotester.

.DESCRIPTION
    Thin wrapper around tools/emulator/run-cemu-autotest.ps1 that targets the
    error-path regression template (tests/emulator/autotest.error-paths.json).
    The suite exercises seven CalcEngine error classes against the real
    TI-OS + eZ80 environment and, after each class, proves recovery by
    re-running 2+3 and asserting the baselined LCD CRC from
    autotest.functional.json.

    Error classes covered:
      1. Divide-by-zero          (1/0 ENTER)
      2. Domain error            (sqrt(-1), ln(-1), asin(2))
      3. Syntax error            (unbalanced paren, trailing operator)
      4. Undefined symbol        (bare x, g(3) via alpha)
      5. Non-convergent          (int(1/x,x,0,1), lim(1/x,x,0))
      6. Oversize                (taylor(sin(x),x,0,100))
      7. Escape long computation (taylor(e^x,x,0,50) + ON break mid-compute)

    Every class runs the CalcEngine .8xp through autotester.exe, meaning the
    TI-OS/module-dispatch/RAM path is exercised exactly as on hardware. If any
    class crashes to "RAM Cleared" or leaves the emulator hung, that is a real
    hardware-impacting bug and the PR should not land until the src/ handler
    is fixed.

.PARAMETER RomPath
    Path to the TI-84 Plus CE OS 5.8 ROM. Required.

.PARAMETER AutotesterPath
    Optional override for autotester.exe. Defaults to $env:CEMU_AUTOTESTER_PATH
    or the path auto-discovered under tools/emulator/cemu-headless/.

.PARAMETER ProgramPath
    Optional override for the CalcEngine base .8xp. Defaults to the wrapper
    artifact auto-discovered under bin/ or deploy/.

.PARAMETER OutputConfigPath
    Optional override for where the resolved autotester JSON is written.
    Defaults to tests/emulator/autotest.error-paths.generated.json next to the
    template.

.PARAMETER Record
    When set, copy the autotester output back into the template so that the
    temporary `00000000` CRC sentinels in tests/emulator/autotest.error-paths.json
    become committed baselines. Use this on the first run, after which the
    script runs in verify mode.

.EXAMPLE
    pwsh .\tools\emulator\run-error-paths.ps1 -RomPath C:\roms\ti84pce_os58.rom

.EXAMPLE
    # First time: record baselines
    pwsh .\tools\emulator\run-error-paths.ps1 `
        -RomPath C:\roms\ti84pce_os58.rom -Record

.NOTES
    This script intentionally does not generate or substitute CRCs itself; the
    baselining step is a cooperative action between autotester.exe (which
    prints the observed CRC on a mismatch) and the human reviewer who copies
    the observed value into the template after verifying the LCD visually
    matches the expected error screen.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$RomPath,

    [Parameter(Mandatory = $false)]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$ProgramPath = "",

    [Parameter(Mandatory = $false)]
    [string]$OutputConfigPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$Record
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$templatePath = Join-Path $repoRoot "tests\emulator\autotest.error-paths.json"

if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw "Error-paths template '$templatePath' does not exist."
}

if ([string]::IsNullOrWhiteSpace($OutputConfigPath)) {
    $OutputConfigPath = Join-Path $repoRoot "tests\emulator\autotest.error-paths.generated.json"
}

$runnerPath = Join-Path $PSScriptRoot "run-cemu-autotest.ps1"
if (-not (Test-Path -LiteralPath $runnerPath -PathType Leaf)) {
    throw "Core runner '$runnerPath' does not exist."
}

Write-Host "=== CalcEngine error-path autotest suite ==="
Write-Host "Template: $templatePath"
Write-Host "Generated config: $OutputConfigPath"
if ($Record) {
    Write-Host "Mode: RECORD (baselines will need to be copied back to template on first-run failures)"
}
else {
    Write-Host "Mode: VERIFY"
}

$runnerArgs = @{
    RomPath          = $RomPath
    AutotesterPath   = $AutotesterPath
    ProgramPath      = $ProgramPath
    TemplatePath     = $templatePath
    OutputConfigPath = $OutputConfigPath
}

& $runnerPath @runnerArgs
$exitCode = $LASTEXITCODE

if ($Record -and $exitCode -ne 0) {
    Write-Host ""
    Write-Host "=== RECORD mode hint ==="
    Write-Host "Autotester reported mismatches. Open '$OutputConfigPath',"
    Write-Host "copy each observed CRC into the matching hash entry's expected_CRCs"
    Write-Host "array in '$templatePath', and re-run without -Record to verify."
}

exit $exitCode
