[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "deploy/hardware-ready",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeProgramPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PreferredAssetPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$BootstrapArtificeAsset,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeBootstrapScriptPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ManifestPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ComprehensiveRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BaselinePath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$artificeModulePath = Join-Path $PSScriptRoot "CalcEmuArtificePipeline.psm1"
if (-not (Test-Path -LiteralPath $artificeModulePath -PathType Leaf)) {
    throw "arTIfiCE pipeline module not found at '$artificeModulePath'."
}
Import-Module -Name $artificeModulePath -Force

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$resolvedBaselinePath = if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    Join-Path $repoRoot "tools\emulator\baselines\artifice-e2e-baseline.json"
}
else {
    Resolve-CalcEmuOptionalPath -PathValue $BaselinePath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\record-artifice-baseline.summary.json"
}
else {
    Resolve-CalcEmuOptionalPath -PathValue $OutputPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "record-artifice-baseline.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    baseline_path = $resolvedBaselinePath
    overwrite_confirmed = $false
    cancelled = $false
    capture = $null
    existing_baseline = $null
    diff = $null
    baseline = $null
    blockers = @()
}

try {
    $capture = Invoke-CalcEmuArtificeHashCapture `
        -RomPath $RomPath `
        -AutotesterPath $AutotesterPath `
        -DownloadsDir $DownloadsDir `
        -ArtificeProgramPath $ArtificeProgramPath `
        -PreferredAssetPath $PreferredAssetPath `
        -BootstrapArtificeAsset:$BootstrapArtificeAsset `
        -ArtificeBootstrapScriptPath $ArtificeBootstrapScriptPath `
        -ManifestPath $ManifestPath `
        -ComprehensiveRunnerPath $ComprehensiveRunnerPath
    $summary.capture = $capture

    if (-not $capture.success) {
        $summary.blockers = @($capture.blockers)
        throw "arTIfiCE baseline capture prerequisites failed."
    }

    $baseline = New-CalcEmuArtificeBaselineObject `
        -RomPath ([string]$capture.options.rom_path) `
        -AutotesterPath ([string]$capture.options.autotester_path) `
        -CapturedScreens @($capture.captured_screens)
    $summary.baseline = $baseline

    if (Test-Path -LiteralPath $resolvedBaselinePath -PathType Leaf) {
        $existingBaseline = Get-Content -LiteralPath $resolvedBaselinePath -Raw | ConvertFrom-Json
        $summary.existing_baseline = $existingBaseline
        $summary.diff = Get-CalcEmuArtificeBaselineDiff -ExistingBaseline $existingBaseline -NewBaseline $baseline

        Write-Host "Existing baseline detected."
        Write-Host ("  cemu_version changed : {0}" -f [bool]$summary.diff.cemu_version_changed)
        Write-Host ("  rom_hash changed     : {0}" -f [bool]$summary.diff.rom_hash_changed)
        foreach ($row in @($summary.diff.rows)) {
            Write-Host ("  {0}: {1} -> {2} [{3}]" -f [string]$row.step, [string]$row.old_hash, [string]$row.new_hash, [string]$row.status)
        }

        if (-not $Force) {
            $response = Read-Host "Baseline already exists. Overwrite? [y/N]"
            if ($response -notmatch '^(?i)y(es)?$') {
                $summary.cancelled = $true
                $summary.blockers = @("Baseline overwrite was declined by the operator.")
                throw "Baseline overwrite cancelled."
            }
        }
    }

    $summary.overwrite_confirmed = $true
    Write-CalcEmuUtf8NoBom -Path $resolvedBaselinePath -Content ($baseline | ConvertTo-Json -Depth 8)
    $summary.success = $true
    $summary.exit_code = 0
}
catch {
    if ($summary.cancelled) {
        $summary.exit_code = 1
    }
    elseif (@($summary.blockers).Count -eq 0) {
        $summary.blockers = @($_.Exception.Message)
    }
    $summary.success = $false
}
finally {
    Write-CalcEmuUtf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
}

Write-Host "CalcEngine arTIfiCE Baseline Recorder"
Write-Host ("Success       : {0}" -f [bool]$summary.success)
Write-Host ("Baseline      : {0}" -f $resolvedBaselinePath)
Write-Host ("Summary       : {0}" -f $resolvedOutputPath)
Write-Host ("Bootstrap     : {0}" -f $(if ($BootstrapArtificeAsset) { "enabled" } else { "disabled" }))

if ($summary.baseline) {
    Write-Host ("CEmu version  : {0}" -f [string]$summary.baseline.cemu_version)
    Write-Host ("ROM SHA-256   : {0}" -f [string]$summary.baseline.rom_hash)
    foreach ($screen in @($summary.baseline.screens)) {
        Write-Host ("  {0}: {1}" -f [string]$screen.step, [string]$screen.hash)
    }
}

if (@($summary.blockers).Count -gt 0) {
    Write-Host ""
    Write-Host "Blockers:"
    foreach ($blocker in @($summary.blockers)) {
        Write-Host ("  - {0}" -f [string]$blocker)
    }
}

exit ([int]$summary.exit_code)
