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
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PrehardwareGateRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeE2ERunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$MultiModuleSmokeRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RuntimeIntegrityRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CrcDriftRunnerPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

function Ensure-ParentDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    Ensure-ParentDirectory -Path $Path
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Resolve-OptionalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $PathValue))
}

function New-StageResult {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$SummaryPath
    )

    return [ordered]@{
        name = $Name
        required = $true
        success = $false
        exit_code = 1
        duration_ms = 0
        command = ""
        output_lines = @()
        invocation_error = ""
        summary_path = $SummaryPath
        summary_json = $null
        blockers = @()
        metrics = [ordered]@{}
    }
}

function Invoke-PowerShellStage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $false)]
        [string[]]$Arguments = @()
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $outputLines = New-Object System.Collections.Generic.List[string]
    $invocationError = ""
    $exitCode = 1
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

    $commandText = @("powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $resolvedScriptPath))
    foreach ($argument in @($Arguments)) {
        if ($null -eq $argument) {
            continue
        }

        $argumentText = [string]$argument
        if ($argumentText.Contains(" ")) {
            $commandText += ('"{0}"' -f $argumentText)
        }
        else {
            $commandText += $argumentText
        }
    }

    return [ordered]@{
        success = ([int]$exitCode -eq 0)
        exit_code = [int]$exitCode
        duration_ms = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
        command = ($commandText -join " ")
    }
}

function Get-JsonPropertyValue {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$PropertyName
    )

    if ($null -eq $Object) {
        return $null
    }

    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($PropertyName)) {
            return $Object[$PropertyName]
        }
        return $null
    }

    if ($Object.PSObject.Properties.Name -contains $PropertyName) {
        return $Object.$PropertyName
    }

    return $null
}

function Get-JsonStringArray {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Value
    )

    $values = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($Value)) {
        if ($null -ne $entry) {
            $values.Add([string]$entry)
        }
    }
    return $values.ToArray()
}

function Get-JsonIntValue {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Value,

        [Parameter(Mandatory = $false)]
        [int]$Default = 0
    )

    if ($null -eq $Value) {
        return $Default
    }

    return [int]$Value
}

function Get-ArtificeComparisonPassCount {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Comparison
    )

    $passCount = 0
    foreach ($row in @(Get-JsonPropertyValue -Object $Comparison -PropertyName "results")) {
        $status = [string](Get-JsonPropertyValue -Object $row -PropertyName "status")
        if ([System.StringComparer]::OrdinalIgnoreCase.Equals($status, "PASS")) {
            $passCount++
        }
    }

    return $passCount
}

function Populate-StageMetrics {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Stage
    )

    $summaryJson = $Stage.summary_json
    if ($null -eq $summaryJson) {
        return
    }

    switch ([string]$Stage.name) {
        "prehardware_gate" {
            $totals = Get-JsonPropertyValue -Object $summaryJson -PropertyName "totals"
            $Stage.metrics = [ordered]@{
                total_stages = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $totals -PropertyName "total_stages")
                passed = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $totals -PropertyName "passed")
                failed = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $totals -PropertyName "failed")
                required_failed = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $totals -PropertyName "required_failed")
            }
            return
        }
        "artifice_e2e" {
            $comparison = Get-JsonPropertyValue -Object $summaryJson -PropertyName "comparison"
            $captured = Get-JsonPropertyValue -Object (Get-JsonPropertyValue -Object $summaryJson -PropertyName "capture") -PropertyName "captured_screens"
            $Stage.metrics = [ordered]@{
                comparison_status = [string](Get-JsonPropertyValue -Object $comparison -PropertyName "overall_status")
                pass_count = Get-ArtificeComparisonPassCount -Comparison $comparison
                captured_screen_count = @($captured).Count
            }
            return
        }
        "multi_module_smoke" {
            $results = Get-JsonPropertyValue -Object $summaryJson -PropertyName "results"
            $Stage.metrics = [ordered]@{
                staged_transfer_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $results -PropertyName "staged_transfer_count")
                dispatch_program_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $results -PropertyName "dispatch_program_count")
                smoke_hash_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $results -PropertyName "smoke_hash_count")
                smoke_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $results -PropertyName "smoke_pass_count")
            }
            return
        }
        "runtime_integrity" {
            $blankSlateSummary = Get-JsonPropertyValue -Object (Get-JsonPropertyValue -Object $summaryJson -PropertyName "blank_slate_comprehensive") -PropertyName "summary_json"
            $blankSlateVerify = Get-JsonPropertyValue -Object $blankSlateSummary -PropertyName "verify_stage"
            $cleanExitSummary = Get-JsonPropertyValue -Object (Get-JsonPropertyValue -Object $summaryJson -PropertyName "clean_exit_probe") -PropertyName "summary_json"
            $cleanExitVerify = Get-JsonPropertyValue -Object $cleanExitSummary -PropertyName "verify_stage"
            $blankSlatePackage = Get-JsonPropertyValue -Object $summaryJson -PropertyName "blank_slate_package"
            $Stage.metrics = [ordered]@{
                blank_slate_transfer_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $blankSlatePackage -PropertyName "copied_file_count")
                blank_slate_hash_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $blankSlateVerify -PropertyName "crc_catalog_hash_count")
                blank_slate_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $blankSlateVerify -PropertyName "pass_count")
                clean_exit_hash_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $cleanExitVerify -PropertyName "crc_catalog_hash_count")
                clean_exit_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $cleanExitVerify -PropertyName "pass_count")
            }
            return
        }
        "crc_drift" {
            $Stage.metrics = [ordered]@{
                baseline_hash_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $summaryJson -PropertyName "baseline_hash_count")
                drift_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $summaryJson -PropertyName "drift_count")
            }
            return
        }
    }
}

function Add-StageBlockers {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Stage,

        [Parameter(Mandatory = $true)]
        [object]$AggregateBlockers
    )

    $blockers = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace([string]$Stage.invocation_error)) {
        $blockers.Add([string]$Stage.invocation_error)
    }

    foreach ($entry in @(Get-JsonStringArray -Value (Get-JsonPropertyValue -Object $Stage.summary_json -PropertyName "blockers"))) {
        if (-not [string]::IsNullOrWhiteSpace($entry)) {
            $blockers.Add($entry)
        }
    }

    if ($blockers.Count -eq 0 -and -not $Stage.success) {
        $blockers.Add(("Stage '{0}' failed without a structured blocker." -f [string]$Stage.name))
    }

    $dedupedStageBlockers = New-Object System.Collections.Generic.List[string]
    $seenStageBlockers = @{}
    foreach ($blocker in $blockers) {
        $key = $blocker.ToUpperInvariant()
        if ($seenStageBlockers.ContainsKey($key)) {
            continue
        }

        $seenStageBlockers[$key] = $true
        $dedupedStageBlockers.Add($blocker)

        if (-not $AggregateBlockers.Contains($blocker)) {
            $AggregateBlockers.Add($blocker)
        }
    }

    $Stage.blockers = $dedupedStageBlockers.ToArray()
}

function Invoke-ChecklistStage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $true)]
        [string]$SummaryPath,

        [Parameter(Mandatory = $false)]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [object]$AggregateBlockers
    )

    $stage = New-StageResult -Name $Name -SummaryPath $SummaryPath
    $invocation = Invoke-PowerShellStage -ScriptPath $ScriptPath -Arguments $Arguments
    $stage.success = [bool]$invocation.success
    $stage.exit_code = [int]$invocation.exit_code
    $stage.duration_ms = [int]$invocation.duration_ms
    $stage.command = [string]$invocation.command
    $stage.output_lines = [string[]]@($invocation.output_lines)
    $stage.invocation_error = [string]$invocation.invocation_error

    if (Test-Path -LiteralPath $SummaryPath -PathType Leaf) {
        try {
            $stage.summary_json = Get-Content -LiteralPath $SummaryPath -Raw | ConvertFrom-Json
        }
        catch {
            $stage.invocation_error = if ([string]::IsNullOrWhiteSpace($stage.invocation_error)) {
                $_.Exception.Message
            }
            else {
                "{0} | {1}" -f $stage.invocation_error, $_.Exception.Message
            }
        }
    }

    Populate-StageMetrics -Stage $stage
    Add-StageBlockers -Stage $stage -AggregateBlockers $AggregateBlockers
    return $stage
}

$repoRoot = Resolve-RepoRoot
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\cemu-pipeline-checklist.summary.json"
}
else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $OutputPath
}
$outputsDirectory = Split-Path -Path $resolvedOutputPath -Parent
if ([string]::IsNullOrWhiteSpace($outputsDirectory)) {
    $outputsDirectory = Join-Path $repoRoot "tests\emulator\outputs"
}

$resolvedPrehardwareGateRunnerPath = if ([string]::IsNullOrWhiteSpace($PrehardwareGateRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-prehardware-validation-gate.ps1"
}
else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $PrehardwareGateRunnerPath
}
$resolvedArtificeE2ERunnerPath = if ([string]::IsNullOrWhiteSpace($ArtificeE2ERunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-artifice-e2e-verification.ps1"
}
else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $ArtificeE2ERunnerPath
}
$resolvedMultiModuleSmokeRunnerPath = if ([string]::IsNullOrWhiteSpace($MultiModuleSmokeRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-multi-module-smoke.ps1"
}
else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $MultiModuleSmokeRunnerPath
}
$resolvedRuntimeIntegrityRunnerPath = if ([string]::IsNullOrWhiteSpace($RuntimeIntegrityRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-runtime-integrity-verification.ps1"
}
else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $RuntimeIntegrityRunnerPath
}
$resolvedCrcDriftRunnerPath = if ([string]::IsNullOrWhiteSpace($CrcDriftRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\check-crc-drift.ps1"
}
else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $CrcDriftRunnerPath
}

$prehardwareSummaryPath = Join-Path $outputsDirectory "cemu-pipeline-checklist.prehardware.summary.json"
$artificeSummaryPath = Join-Path $outputsDirectory "cemu-pipeline-checklist.artifice-e2e.summary.json"
$multiModuleSummaryPath = Join-Path $outputsDirectory "cemu-pipeline-checklist.multi-module.summary.json"
$runtimeIntegritySummaryPath = Join-Path $outputsDirectory "cemu-pipeline-checklist.runtime-integrity.summary.json"
$crcDriftSummaryPath = Join-Path $outputsDirectory "cemu-pipeline-checklist.crc-drift.summary.json"

$summary = [ordered]@{
    schema_version = 1
    runner = "run-cemu-pipeline-checklist.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    options = [ordered]@{
        rom_path = $RomPath
        autotester_path = $AutotesterPath
        downloads_dir = $DownloadsDir
        artifice_program_path = $ArtificeProgramPath
        prehardware_gate_runner_path = $resolvedPrehardwareGateRunnerPath
        artifice_e2e_runner_path = $resolvedArtificeE2ERunnerPath
        multi_module_smoke_runner_path = $resolvedMultiModuleSmokeRunnerPath
        runtime_integrity_runner_path = $resolvedRuntimeIntegrityRunnerPath
        crc_drift_runner_path = $resolvedCrcDriftRunnerPath
    }
    totals = [ordered]@{
        total_stages = 0
        passed = 0
        failed = 0
        required_failed = 0
    }
    results = [ordered]@{
        prehardware_required_failed = 0
        prehardware_stage_count = 0
        artifice_e2e_pass_count = 0
        artifice_e2e_screen_count = 0
        multi_module_dispatch_program_count = 0
        multi_module_smoke_pass_count = 0
        runtime_blank_slate_pass_count = 0
        runtime_clean_exit_pass_count = 0
        runtime_blank_slate_transfer_count = 0
        crc_drift_count = 0
        crc_baseline_hash_count = 0
    }
    blockers = @()
    notes = @(
        "This checklist is the canonical one-command CEmu proof: strict prehardware gate with CEmu enabled, explicit arTIfiCE E2E verification, multi-module smoke, blank-slate runtime integrity, and comprehensive CRC drift detection.",
        "Each child summary is preserved so failures stay attributable to a specific lane instead of collapsing into a single opaque wrapper failure."
    )
    stages = @()
}

$aggregateBlockers = New-Object System.Collections.Generic.List[string]

try {
    foreach ($scriptPath in @(
        $resolvedPrehardwareGateRunnerPath,
        $resolvedArtificeE2ERunnerPath,
        $resolvedMultiModuleSmokeRunnerPath,
        $resolvedRuntimeIntegrityRunnerPath,
        $resolvedCrcDriftRunnerPath
    )) {
        if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) {
            throw "Required checklist stage runner was not found at '$scriptPath'."
        }
    }

    $prehardwareArgs = @(
        "-Mode", "strict",
        "-EnableCEmuChecks",
        "-RomPath", $RomPath,
        "-AutotesterPath", $AutotesterPath,
        "-OutputPath", $prehardwareSummaryPath
    )
    $artificeArgs = New-Object System.Collections.Generic.List[string]
    foreach ($argumentPart in @(
        "-RomPath", $RomPath,
        "-AutotesterPath", $AutotesterPath,
        "-DownloadsDir", $DownloadsDir
    )) {
        $artificeArgs.Add([string]$argumentPart)
    }
    if (-not [string]::IsNullOrWhiteSpace($ArtificeProgramPath)) {
        $artificeArgs.Add("-ArtificeProgramPath")
        $artificeArgs.Add($ArtificeProgramPath)
    }
    foreach ($argumentPart in @("-OutputPath", $artificeSummaryPath)) {
        $artificeArgs.Add([string]$argumentPart)
    }

    $multiModuleArgs = New-Object System.Collections.Generic.List[string]
    foreach ($argumentPart in @(
        "-RomPath", $RomPath,
        "-AutotesterPath", $AutotesterPath,
        "-DownloadsDir", $DownloadsDir
    )) {
        $multiModuleArgs.Add([string]$argumentPart)
    }
    if (-not [string]::IsNullOrWhiteSpace($ArtificeProgramPath)) {
        $multiModuleArgs.Add("-ArtificeProgramPath")
        $multiModuleArgs.Add($ArtificeProgramPath)
    }
    foreach ($argumentPart in @("-OutputPath", $multiModuleSummaryPath)) {
        $multiModuleArgs.Add([string]$argumentPart)
    }

    $runtimeArgs = @(
        "-RomPath", $RomPath,
        "-AutotesterPath", $AutotesterPath,
        "-DownloadsDir", $DownloadsDir,
        "-OutputPath", $runtimeIntegritySummaryPath
    )
    $crcDriftArgs = @(
        "-RomPath", $RomPath,
        "-AutotesterPath", $AutotesterPath,
        "-DownloadsDir", $DownloadsDir,
        "-OutputPath", $crcDriftSummaryPath,
        "-RunnerSummaryPath", (Join-Path $outputsDirectory "cemu-pipeline-checklist.crc-drift.runner.summary.json")
    )

    $summary.stages = @(
        Invoke-ChecklistStage -Name "prehardware_gate" -ScriptPath $resolvedPrehardwareGateRunnerPath -SummaryPath $prehardwareSummaryPath -Arguments $prehardwareArgs -AggregateBlockers $aggregateBlockers
        Invoke-ChecklistStage -Name "artifice_e2e" -ScriptPath $resolvedArtificeE2ERunnerPath -SummaryPath $artificeSummaryPath -Arguments $artificeArgs.ToArray() -AggregateBlockers $aggregateBlockers
        Invoke-ChecklistStage -Name "multi_module_smoke" -ScriptPath $resolvedMultiModuleSmokeRunnerPath -SummaryPath $multiModuleSummaryPath -Arguments $multiModuleArgs.ToArray() -AggregateBlockers $aggregateBlockers
        Invoke-ChecklistStage -Name "runtime_integrity" -ScriptPath $resolvedRuntimeIntegrityRunnerPath -SummaryPath $runtimeIntegritySummaryPath -Arguments $runtimeArgs -AggregateBlockers $aggregateBlockers
        Invoke-ChecklistStage -Name "crc_drift" -ScriptPath $resolvedCrcDriftRunnerPath -SummaryPath $crcDriftSummaryPath -Arguments $crcDriftArgs -AggregateBlockers $aggregateBlockers
    )

    $summary.totals.total_stages = @($summary.stages).Count
    $summary.totals.passed = @($summary.stages | Where-Object { $_.success }).Count
    $summary.totals.failed = @($summary.stages | Where-Object { -not $_.success }).Count
    $summary.totals.required_failed = @($summary.stages | Where-Object { $_.required -and -not $_.success }).Count

    $prehardwareStage = @($summary.stages | Where-Object { $_.name -eq "prehardware_gate" } | Select-Object -First 1)[0]
    $artificeStage = @($summary.stages | Where-Object { $_.name -eq "artifice_e2e" } | Select-Object -First 1)[0]
    $multiModuleStage = @($summary.stages | Where-Object { $_.name -eq "multi_module_smoke" } | Select-Object -First 1)[0]
    $runtimeStage = @($summary.stages | Where-Object { $_.name -eq "runtime_integrity" } | Select-Object -First 1)[0]
    $crcStage = @($summary.stages | Where-Object { $_.name -eq "crc_drift" } | Select-Object -First 1)[0]

    $summary.results.prehardware_required_failed = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $prehardwareStage.metrics -PropertyName "required_failed")
    $summary.results.prehardware_stage_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $prehardwareStage.metrics -PropertyName "total_stages")
    $summary.results.artifice_e2e_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $artificeStage.metrics -PropertyName "pass_count")
    $summary.results.artifice_e2e_screen_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $artificeStage.metrics -PropertyName "captured_screen_count")
    $summary.results.multi_module_dispatch_program_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $multiModuleStage.metrics -PropertyName "dispatch_program_count")
    $summary.results.multi_module_smoke_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $multiModuleStage.metrics -PropertyName "smoke_pass_count")
    $summary.results.runtime_blank_slate_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $runtimeStage.metrics -PropertyName "blank_slate_pass_count")
    $summary.results.runtime_clean_exit_pass_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $runtimeStage.metrics -PropertyName "clean_exit_pass_count")
    $summary.results.runtime_blank_slate_transfer_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $runtimeStage.metrics -PropertyName "blank_slate_transfer_count")
    $summary.results.crc_drift_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $crcStage.metrics -PropertyName "drift_count")
    $summary.results.crc_baseline_hash_count = Get-JsonIntValue -Value (Get-JsonPropertyValue -Object $crcStage.metrics -PropertyName "baseline_hash_count")

    $summary.blockers = $aggregateBlockers.ToArray()
    $summary.success = ($summary.totals.required_failed -eq 0)
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
}
catch {
    if (-not $aggregateBlockers.Contains($_.Exception.Message)) {
        $aggregateBlockers.Add($_.Exception.Message)
    }
    $summary.blockers = $aggregateBlockers.ToArray()
    $summary.success = $false
    $summary.exit_code = 1
}
finally {
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 16)
}

Write-Host "CalcEngine CEmu Pipeline Checklist"
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
foreach ($stage in @($summary.stages)) {
    Write-Host ("{0,-16}: exit {1} ({2})" -f [string]$stage.name, [int]$stage.exit_code, $(if ($stage.success) { "PASS" } else { "FAIL" }))
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
