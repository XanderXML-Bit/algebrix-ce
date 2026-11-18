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
    [AllowEmptyString()]
    [string]$PreferredAssetPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$BootstrapArtificeAsset,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeBootstrapScriptPath = "",

    [Parameter(Mandatory = $false)]
    [ValidateSet("Baseline", "Verify", "Run")]
    [string]$Mode = "Run",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ManifestPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ComprehensiveRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$GeneratedPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BaselinedPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CrcCatalogJsonPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CrcCatalogMarkdownPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BaselinePath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$artificeModulePath = Join-Path $PSScriptRoot "CalcEmuArtificePipeline.psm1"
if (-not (Test-Path -LiteralPath $artificeModulePath -PathType Leaf)) {
    throw "arTIfiCE pipeline module not found at '$artificeModulePath'."
}
Import-Module -Name $artificeModulePath -Force

function Get-OptionalMemberValue {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }

    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) {
            return $Object[$Name]
        }
        return $null
    }

    if ($Object.PSObject.Properties.Name -contains $Name) {
        return $Object.$Name
    }

    return $null
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$resolvedBaselinePath = if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    Join-Path $repoRoot "tools\emulator\baselines\artifice-e2e-baseline.json"
}
else {
    Resolve-CalcEmuOptionalPath -PathValue $BaselinePath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\artifice-e2e.summary.json"
}
else {
    Resolve-CalcEmuOptionalPath -PathValue $OutputPath
}

$summary = [ordered]@{
    schema_version = 2
    runner = "run-artifice-e2e-verification.ps1"
    generated_at = (Get-Date -Format "o")
    mode = $Mode
    success = $false
    exit_code = 1
    baseline_path = $resolvedBaselinePath
    baseline_present = $false
    baseline = $null
    capture = $null
    comparison = $null
    blockers = @()
    notes = @(
        "The verifier always captures the six arTIfiCE v2 checkpoints first, including the verified CLEAR -> MODE -> PRGM exit choreography, then compares those hashes against tools/emulator/baselines/artifice-e2e-baseline.json when it exists.",
        "BASELINE_MISSING is intentionally reported as a non-pass so the lane does not claim hardware confidence before a real baseline has been recorded.",
        "When -BootstrapArtificeAsset is set, the verifier will try scripts/get-artifice-asset.ps1 before failing on a missing local arTIfiCE payload."
    )
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
        -ComprehensiveRunnerPath $ComprehensiveRunnerPath `
        -GeneratedPath $GeneratedPath `
        -BaselinedPath $BaselinedPath `
        -CrcCatalogJsonPath $CrcCatalogJsonPath `
        -CrcCatalogMarkdownPath $CrcCatalogMarkdownPath
    $summary.capture = $capture

    if (-not $capture.success) {
        $summary.blockers = @($capture.blockers)
        throw "arTIfiCE capture failed."
    }

    $baseline = $null
    if (Test-Path -LiteralPath $resolvedBaselinePath -PathType Leaf) {
        $baseline = Get-Content -LiteralPath $resolvedBaselinePath -Raw | ConvertFrom-Json
        $summary.baseline_present = $true
        $summary.baseline = $baseline
    }

    $comparison = Compare-CalcEmuArtificeCaptureToBaseline `
        -CapturedScreens @($capture.captured_screens) `
        -Baseline $baseline
    $summary.comparison = $comparison

    if ($comparison.overall_status -eq "PASS") {
        $summary.success = $true
        $summary.exit_code = 0
    }
    else {
        $summary.success = $false
        $summary.exit_code = 1
        if ($comparison.overall_status -eq "BASELINE_MISSING") {
            $summary.blockers = @(
                "BASELINE_MISSING: no baseline exists at '$resolvedBaselinePath'. Run tools/emulator/record-artifice-baseline.ps1 after staging the user-supplied arTIfiCE v2 asset."
            )
        }
        elseif (@($summary.blockers).Count -eq 0) {
            $summary.blockers = @(
                "One or more captured arTIfiCE verification hashes did not match the recorded baseline."
            )
        }
    }
}
catch {
    if (@($summary.blockers).Count -eq 0) {
        $summary.blockers = @($_.Exception.Message)
    }
    $summary.success = $false
    $summary.exit_code = 1
}
finally {
    Write-CalcEmuUtf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
}

Write-Host "CalcEngine arTIfiCE E2E Verification"
Write-Host ("Mode            : {0}" -f $Mode)
if ($summary.capture) {
    $captureOptions = Get-OptionalMemberValue -Object $summary.capture -Name "options"
    $captureArtifice = Get-OptionalMemberValue -Object $summary.capture -Name "artifice"
    $captureBootstrap = Get-OptionalMemberValue -Object $captureArtifice -Name "bootstrap"

    Write-Host ("ROM             : {0}" -f [string](Get-OptionalMemberValue -Object $captureOptions -Name "rom_path"))
    Write-Host ("Autotester      : {0}" -f [string](Get-OptionalMemberValue -Object $captureOptions -Name "autotester_path"))
    Write-Host ("Downloads       : {0}" -f [string](Get-OptionalMemberValue -Object $captureOptions -Name "downloads_dir"))
    Write-Host ("arTIfiCE asset  : {0}" -f [string](Get-OptionalMemberValue -Object $captureArtifice -Name "program_path"))
    Write-Host ("Bootstrap       : {0}" -f $(if ([bool](Get-OptionalMemberValue -Object $captureOptions -Name "bootstrap_artifice_asset")) { "enabled" } else { "disabled" }))
    if ([bool](Get-OptionalMemberValue -Object $captureBootstrap -Name "attempted")) {
        Write-Host ("Bootstrap exit  : {0}" -f [int](Get-OptionalMemberValue -Object $captureBootstrap -Name "exit_code"))
    }
}
Write-Host ("Baseline        : {0}" -f $resolvedBaselinePath)
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)

if ($summary.comparison) {
    Write-Host ("Overall         : {0}" -f [string]$summary.comparison.overall_status)
    foreach ($row in @($summary.comparison.results)) {
        if ([string]$row.status -eq "PASS") {
            Write-Host ("PASS  {0}  {1}" -f [string]$row.step, [string]$row.actual_hash)
        }
        elseif ([string]$row.status -eq "BASELINE_MISSING") {
            Write-Host ("BASELINE_MISSING  {0}  actual={1}" -f [string]$row.step, [string]$row.actual_hash)
        }
        else {
            Write-Host ("FAIL  {0}  expected={1} actual={2}" -f [string]$row.step, [string]$row.expected_hash, [string]$row.actual_hash)
        }
    }
}
else {
    Write-Host ("Overall         : {0}" -f $(if ($summary.success) { "PASS" } else { "FAIL" }))
}

if (@($summary.blockers).Count -gt 0) {
    Write-Host ""
    Write-Host "Blockers:"
    foreach ($blocker in @($summary.blockers)) {
        Write-Host ("  - {0}" -f [string]$blocker)
    }
}

exit ([int]$summary.exit_code)
