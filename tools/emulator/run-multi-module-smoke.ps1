[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "C:\Users\user\Downloads\CalcEngine",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeProgramPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$BootstrapArtificeAsset,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeBootstrapScriptPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeSmokeRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$EnvironmentRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Invoke-PowerShellFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $false)]
        [string[]]$Arguments = @()
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $outputLines = New-Object System.Collections.Generic.List[string]
    $exitCode = 1
    $invocationError = ""
    $startTime = Get-Date

    try {
        $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $resolvedScriptPath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
        foreach ($entry in @($captured)) {
            if ($null -ne $entry) {
                $outputLines.Add([string]$entry)
            }
        }
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    return [ordered]@{
        success = ([int]$exitCode -eq 0)
        exit_code = [int]$exitCode
        duration_ms = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\multi-module-smoke.summary.json"
}
else {
    if ([System.IO.Path]::IsPathRooted($OutputPath)) {
        [System.IO.Path]::GetFullPath($OutputPath)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputPath))
    }
}

$artificeSmokeRunnerPath = if ([string]::IsNullOrWhiteSpace($ArtificeSmokeRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-artifice-smoke.ps1"
}
else {
    if ([System.IO.Path]::IsPathRooted($ArtificeSmokeRunnerPath)) {
        [System.IO.Path]::GetFullPath($ArtificeSmokeRunnerPath)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $ArtificeSmokeRunnerPath))
    }
}
$environmentRunnerPath = if ([string]::IsNullOrWhiteSpace($EnvironmentRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-comprehensive-environment-verification.ps1"
}
else {
    if ([System.IO.Path]::IsPathRooted($EnvironmentRunnerPath)) {
        [System.IO.Path]::GetFullPath($EnvironmentRunnerPath)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $EnvironmentRunnerPath))
    }
}
$artificeSmokeSummaryPath = Join-Path $repoRoot "tests\emulator\outputs\multi-module-smoke.artifice.summary.json"
$environmentSummaryPath = Join-Path $repoRoot "tests\emulator\outputs\multi-module-smoke.environment.summary.json"

$summary = [ordered]@{
    schema_version = 1
    runner = "run-multi-module-smoke.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    options = [ordered]@{
        rom_path = $RomPath
        autotester_path = $AutotesterPath
        downloads_dir = $DownloadsDir
        artifice_program_path = $ArtificeProgramPath
        bootstrap_artifice_asset = [bool]$BootstrapArtificeAsset
        artifice_bootstrap_script_path = $ArtificeBootstrapScriptPath
        artifice_smoke_runner_path = $artificeSmokeRunnerPath
        environment_runner_path = $environmentRunnerPath
    }
    artifice_smoke = $null
    environment_verification = $null
    results = [ordered]@{
        staged_transfer_count = 0
        manifest_module_count = 0
        runtime_dependency_count = 0
        dispatch_program_count = 0
        smoke_hash_count = 0
        smoke_pass_count = 0
    }
    blockers = @()
    limitations = @(
        "The real-ROM smoke stage proves the full staged package plus arTIfiCE can boot and exercise multiple CalcEngine segments, but it does not deterministically launch every one of the 35 modules through the UI in a single CEmu session.",
        "The per-module 35-program proof uses the existing eZ80 scaffold dispatch sweep because CEmu AutoTester still lacks a robust all-program launcher API for the real ROM."
    )
}

try {
    if (-not (Test-Path -LiteralPath $artificeSmokeRunnerPath -PathType Leaf)) {
        throw "arTIfiCE smoke runner was not found at '$artificeSmokeRunnerPath'."
    }
    if (-not (Test-Path -LiteralPath $environmentRunnerPath -PathType Leaf)) {
        throw "Environment verification runner was not found at '$environmentRunnerPath'."
    }

    $artificeArguments = New-Object System.Collections.Generic.List[string]
    foreach ($argumentPart in @("-RomPath", $RomPath, "-AutotesterPath", $AutotesterPath, "-DownloadsDir", $DownloadsDir)) {
        $artificeArguments.Add([string]$argumentPart)
    }
    if (-not [string]::IsNullOrWhiteSpace($ArtificeProgramPath)) {
        $artificeArguments.Add("-ArtificeProgramPath")
        $artificeArguments.Add($ArtificeProgramPath)
    }
    if ($BootstrapArtificeAsset) {
        $artificeArguments.Add("-BootstrapArtificeAsset")
    }
    if (-not [string]::IsNullOrWhiteSpace($ArtificeBootstrapScriptPath)) {
        $artificeArguments.Add("-ArtificeBootstrapScriptPath")
        $artificeArguments.Add($ArtificeBootstrapScriptPath)
    }
    foreach ($argumentPart in @("-Mode", "Verify", "-OutputPath", $artificeSmokeSummaryPath)) {
        $artificeArguments.Add([string]$argumentPart)
    }
    $artificeInvocation = Invoke-PowerShellFile -ScriptPath $artificeSmokeRunnerPath -Arguments $artificeArguments.ToArray()
    if (Test-Path -LiteralPath $artificeSmokeSummaryPath -PathType Leaf) {
        $artificeInvocation["summary_json"] = Get-Content -LiteralPath $artificeSmokeSummaryPath -Raw | ConvertFrom-Json
    }
    $summary.artifice_smoke = $artificeInvocation
    if (-not $artificeInvocation.success) {
        throw "arTIfiCE smoke stage failed."
    }

    $environmentArguments = @(
        "-RomPath", $RomPath,
        "-AutotesterPath", $AutotesterPath,
        "-DownloadsDir", $DownloadsDir,
        "-SkipCEmuVerify",
        "-OutputPath", $environmentSummaryPath
    )
    $environmentInvocation = Invoke-PowerShellFile -ScriptPath $environmentRunnerPath -Arguments $environmentArguments
    if (Test-Path -LiteralPath $environmentSummaryPath -PathType Leaf) {
        $environmentInvocation["summary_json"] = Get-Content -LiteralPath $environmentSummaryPath -Raw | ConvertFrom-Json
    }
    $summary.environment_verification = $environmentInvocation
    if (-not $environmentInvocation.success) {
        throw "Environment verification dispatch stage failed."
    }

    if ($summary.artifice_smoke -and $summary.artifice_smoke.Contains("summary_json") -and $summary.artifice_smoke["summary_json"]) {
        $artificeSummaryJson = $summary.artifice_smoke["summary_json"]
        $summary.results["staged_transfer_count"] = [int]$artificeSummaryJson.staged_downloads.transfer_count
        if ($artificeSummaryJson.delegated_runner.summary_json.verify_stage) {
            $summary.results["smoke_hash_count"] = [int]$artificeSummaryJson.delegated_runner.summary_json.verify_stage.crc_catalog_hash_count
            $summary.results["smoke_pass_count"] = [int]$artificeSummaryJson.delegated_runner.summary_json.verify_stage.pass_count
        }
    }

    if ($summary.environment_verification -and $summary.environment_verification.Contains("summary_json") -and $summary.environment_verification["summary_json"]) {
        $environmentSummaryJson = $summary.environment_verification["summary_json"]
        $summary.results["manifest_module_count"] = [int]$environmentSummaryJson.package_audit.expected_program_count
        $summary.results["runtime_dependency_count"] = [int]$environmentSummaryJson.package_audit.expected_appvar_count
        $summary.results["dispatch_program_count"] = [int]$environmentSummaryJson.ez80_dispatch.program_count
    }

    $summary.success = $true
    $summary.exit_code = 0
}
catch {
    $summary.success = $false
    $summary.exit_code = 1
    if ($summary.artifice_smoke -and $summary.artifice_smoke.Contains("summary_json") -and
        $summary.artifice_smoke["summary_json"]) {
        $artificeSummaryJson = $summary.artifice_smoke["summary_json"]
        if ($artificeSummaryJson.PSObject.Properties.Name -contains "blockers" -and
            $artificeSummaryJson.blockers) {
            $summary.blockers += @($artificeSummaryJson.blockers)
        }
    }
    if ($summary.environment_verification -and $summary.environment_verification.Contains("summary_json") -and
        $summary.environment_verification["summary_json"]) {
        $environmentSummaryJson = $summary.environment_verification["summary_json"]
        if ($environmentSummaryJson.PSObject.Properties.Name -contains "error" -and
            $environmentSummaryJson.error) {
            $summary.blockers += @([string]$environmentSummaryJson.error)
        }
    }
    if (@($summary.blockers).Count -eq 0) {
        $summary.blockers = @($_.Exception.Message)
    }
}
finally {
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
}

Write-Host "CalcEngine Multi-Module Smoke"
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
if ($summary.artifice_smoke) {
    Write-Host ("arTIfiCE smoke  : exit {0}" -f [int]$summary.artifice_smoke["exit_code"])
}
if ($summary.environment_verification) {
    Write-Host ("Dispatch sweep  : exit {0}" -f [int]$summary.environment_verification["exit_code"])
}
Write-Host ("Overall         : {0}" -f $(if ($summary.success) { "PASS" } else { "FAIL" }))

if (@($summary.blockers).Count -gt 0) {
    Write-Host ""
    Write-Host "Blockers:"
    foreach ($blocker in @($summary.blockers)) {
        Write-Host ("  - {0}" -f [string]$blocker)
    }
}

exit ([int]$summary.exit_code)
