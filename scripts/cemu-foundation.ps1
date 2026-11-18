#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [ValidateSet("Focused", "Pipeline", "Artifice", "All")]
    [string]$Mode = "All",

    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CEmuPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CEmuHeadlessPath = $env:CEMU_HEADLESS_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "C:\Users\user\Downloads\CalcEngine",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PythonPath = "python",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$FocusedRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PipelineRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$FocusedSummaryPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PipelineSummaryPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PipelineSnapshotPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeSummaryPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:StartTime = Get-Date

$pipelineSupportModulePath = Join-Path $PSScriptRoot "..\tools\emulator\CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Resolve-OptionalPath {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    return Resolve-CalcEmuPipelinePath -PathValue $PathValue
}

function Ensure-ParentDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    Ensure-ParentDirectory -Path $Path
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

function Resolve-RequestedStages {
    param([Parameter(Mandatory = $true)][string]$RequestedMode)

    switch ($RequestedMode) {
        "Focused" { return @("focused") }
        "Pipeline" { return @("pipeline") }
        "Artifice" { return @("artifice") }
        "All" { return @("focused", "pipeline", "artifice") }
        default { throw "Unsupported mode '$RequestedMode'." }
    }
}

function Resolve-PythonCommand {
    param([Parameter(Mandatory = $true)][string]$Candidate)

    if ([string]::IsNullOrWhiteSpace($Candidate)) {
        throw "PythonPath was empty."
    }

    if ([System.IO.Path]::IsPathRooted($Candidate) -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $Candidate).Path
    }

    $command = Get-Command -Name $Candidate -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command -or [string]::IsNullOrWhiteSpace([string]$command.Source)) {
        throw "Python command '$Candidate' could not be resolved."
    }

    return [string]$command.Source
}

function Resolve-ExistingFilePath {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $resolvedPath = Resolve-OptionalPath -PathValue $PathValue
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        throw "$Label '$resolvedPath' does not exist."
    }

    return (Resolve-Path -LiteralPath $resolvedPath).Path
}

function Invoke-StageScript {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $outputLines = New-Object System.Collections.Generic.List[string]
    $invocationError = ""
    $exitCode = 1
    $durationMs = 0
    $extension = [System.IO.Path]::GetExtension($resolvedScriptPath)
    $startTime = Get-Date

    try {
        if ($extension -ieq ".py") {
            $pythonExe = Resolve-PythonCommand -Candidate $PythonPath
            $captured = & $pythonExe $resolvedScriptPath @Arguments 2>&1
            $exitCode = $LASTEXITCODE
        }
        else {
            $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $resolvedScriptPath @Arguments 2>&1
            $exitCode = $LASTEXITCODE
        }

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
    finally {
        $durationMs = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)
    }

    return [ordered]@{
        script_path = $resolvedScriptPath
        success = ([int]$exitCode -eq 0)
        exit_code = [int]$exitCode
        duration_ms = $durationMs
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
    }
}

$repoRoot = Resolve-RepoRoot
$requestedStages = Resolve-RequestedStages -RequestedMode $Mode
$resolvedFocusedRunnerPath = if ([string]::IsNullOrWhiteSpace($FocusedRunnerPath)) {
    Join-Path $repoRoot "scripts\cemu-test-runner.ps1"
}
else {
    Resolve-OptionalPath -PathValue $FocusedRunnerPath
}
$resolvedPipelineRunnerPath = if ([string]::IsNullOrWhiteSpace($PipelineRunnerPath)) {
    Join-Path $repoRoot "scripts\cemu_pipeline.py"
}
else {
    Resolve-OptionalPath -PathValue $PipelineRunnerPath
}
$resolvedArtificeRunnerPath = if ([string]::IsNullOrWhiteSpace($ArtificeRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-artifice-e2e-verification.ps1"
}
else {
    Resolve-OptionalPath -PathValue $ArtificeRunnerPath
}
$resolvedFocusedSummaryPath = if ([string]::IsNullOrWhiteSpace($FocusedSummaryPath)) {
    Join-Path $repoRoot "tests\cemu\outputs\cemu-foundation.focused.summary.json"
}
else {
    Resolve-OptionalPath -PathValue $FocusedSummaryPath
}
$resolvedPipelineSummaryPath = if ([string]::IsNullOrWhiteSpace($PipelineSummaryPath)) {
    Join-Path $repoRoot "tests\cemu\outputs\cemu-foundation.pipeline.summary.json"
}
else {
    Resolve-OptionalPath -PathValue $PipelineSummaryPath
}
$resolvedPipelineSnapshotPath = if ([string]::IsNullOrWhiteSpace($PipelineSnapshotPath)) {
    Join-Path $repoRoot "tests\cemu\outputs\cemu-foundation.pipeline.snapshot.json"
}
else {
    Resolve-OptionalPath -PathValue $PipelineSnapshotPath
}
$resolvedArtificeSummaryPath = if ([string]::IsNullOrWhiteSpace($ArtificeSummaryPath)) {
    Join-Path $repoRoot "tests\cemu\outputs\cemu-foundation.artifice.summary.json"
}
else {
    Resolve-OptionalPath -PathValue $ArtificeSummaryPath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\cemu\outputs\cemu-foundation.summary.json"
}
else {
    Resolve-OptionalPath -PathValue $OutputPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "scripts/cemu-foundation.ps1"
    generated_at = (Get-Date -Format "o")
    requested_mode = $Mode
    requested_stages = $requestedStages
    success = $false
    exit_code = 2
    options = [ordered]@{
        rom_path = $RomPath
        cemu_path = $CEmuPath
        autotester_path = $AutotesterPath
        cemu_headless_path = $CEmuHeadlessPath
        downloads_dir = $DownloadsDir
        python_path = $PythonPath
        focused_runner_path = $resolvedFocusedRunnerPath
        pipeline_runner_path = $resolvedPipelineRunnerPath
        artifice_runner_path = $resolvedArtificeRunnerPath
        focused_summary_path = $resolvedFocusedSummaryPath
        pipeline_summary_path = $resolvedPipelineSummaryPath
        pipeline_snapshot_path = $resolvedPipelineSnapshotPath
        artifice_summary_path = $resolvedArtificeSummaryPath
        output_path = $resolvedOutputPath
    }
    environment = [ordered]@{
        repo_root = $repoRoot
        rom_path = ""
        cemu_path = ""
        autotester_path = ""
        cemu_headless_path = ""
        downloads_dir = ""
        downloads_dir_exists = $false
    }
    stages = @()
    focused_assertions = @()
    blockers = @()
    notes = @(
        "Focused mode delegates to scripts/cemu-test-runner.ps1 for launch, 2+3, 6*7, prompt round-trip, and menu reachability proofs on the real ROM.",
        "Pipeline mode delegates to scripts/cemu_pipeline.py for manifest-backed transfer staging, cemu-headless orchestration, visible smoke, and accuracy audit coverage.",
        "Artifice mode delegates to tools/emulator/run-artifice-e2e-verification.ps1 for the OS 5.8 prgmA jailbreak path."
    )
}

try {
    $resolvedRom = [string](Resolve-CalcEmuRomPath -RomPath $RomPath).path
    $summary.environment.rom_path = $resolvedRom

    $resolvedAutotester = ""
    $resolvedCEmu = ""
    $resolvedHeadless = ""
    $resolvedDownloadsDir = ""

    if ($requestedStages -contains "focused" -or $requestedStages -contains "artifice") {
        $resolvedAutotester = if (-not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
            Resolve-ExistingFilePath -PathValue $AutotesterPath -Label "Autotester path"
        }
        else {
            [string](Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath).path
        }
        $summary.environment.autotester_path = $resolvedAutotester
    }

    if ($requestedStages -contains "focused") {
        $resolvedCEmu = if (-not [string]::IsNullOrWhiteSpace($CEmuPath)) {
            Resolve-ExistingFilePath -PathValue $CEmuPath -Label "CEmu path"
        }
        else {
            [string](Resolve-CalcEmuGuiPath -CEmuPath $CEmuPath).path
        }
        $summary.environment.cemu_path = $resolvedCEmu
    }

    if ($requestedStages -contains "pipeline") {
        if ([string]::IsNullOrWhiteSpace($resolvedAutotester)) {
            $resolvedAutotester = if (-not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
                Resolve-ExistingFilePath -PathValue $AutotesterPath -Label "Autotester path"
            }
            else {
                [string](Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath).path
            }
            $summary.environment.autotester_path = $resolvedAutotester
        }
        $resolvedHeadless = if (-not [string]::IsNullOrWhiteSpace($CEmuHeadlessPath)) {
            Resolve-ExistingFilePath -PathValue $CEmuHeadlessPath -Label "CEmu headless path"
        }
        else {
            [string](Resolve-CalcEmuCEmuHeadlessPath -CEmuHeadlessPath $CEmuHeadlessPath).path
        }
        $summary.environment.cemu_headless_path = $resolvedHeadless
    }

    if ($requestedStages -contains "artifice" -or $requestedStages -contains "pipeline") {
        $resolvedDownloadsDir = Resolve-CalcEmuPipelinePath -PathValue $DownloadsDir
        $summary.environment.downloads_dir = $resolvedDownloadsDir
        $summary.environment.downloads_dir_exists = (Test-Path -LiteralPath $resolvedDownloadsDir -PathType Container)
        if (-not $summary.environment.downloads_dir_exists) {
            throw "Downloads directory '$resolvedDownloadsDir' does not exist."
        }
    }

    if ($requestedStages -contains "focused") {
        if (-not (Test-Path -LiteralPath $resolvedFocusedRunnerPath -PathType Leaf)) {
            throw "Focused runner '$resolvedFocusedRunnerPath' does not exist."
        }

        $focusedStage = Invoke-StageScript -ScriptPath $resolvedFocusedRunnerPath -Arguments @(
            "-RomPath", $resolvedRom,
            "-CEmuPath", $resolvedCEmu,
            "-AutotesterPath", $resolvedAutotester,
            "-OutputPath", $resolvedFocusedSummaryPath
        )
        $focusedStage.name = "focused"
        $focusedStage.summary_path = $resolvedFocusedSummaryPath
        $focusedStage.summary_json = if (Test-Path -LiteralPath $resolvedFocusedSummaryPath -PathType Leaf) {
            Get-Content -LiteralPath $resolvedFocusedSummaryPath -Raw | ConvertFrom-Json
        }
        else {
            $null
        }
        $summary.stages += $focusedStage
        if ($null -ne $focusedStage.summary_json -and $null -ne $focusedStage.summary_json.assertions) {
            $summary.focused_assertions = @($focusedStage.summary_json.assertions)
        }
    }

    if ($requestedStages -contains "pipeline") {
        if (-not (Test-Path -LiteralPath $resolvedPipelineRunnerPath -PathType Leaf)) {
            throw "Pipeline runner '$resolvedPipelineRunnerPath' does not exist."
        }

        $pipelineArguments = @(
            "--rom", $resolvedRom,
            "--autotester", $resolvedAutotester,
            "--headless", $resolvedHeadless,
            "--downloads-dir", $resolvedDownloadsDir,
            "--output", $resolvedPipelineSummaryPath,
            "--snapshot", $resolvedPipelineSnapshotPath
        )
        if (-not [string]::IsNullOrWhiteSpace($resolvedCEmu)) {
            $pipelineArguments += @("--cemu-gui", $resolvedCEmu)
        }

        $pipelineStage = Invoke-StageScript -ScriptPath $resolvedPipelineRunnerPath -Arguments $pipelineArguments
        $pipelineStage.name = "pipeline"
        $pipelineStage.summary_path = $resolvedPipelineSummaryPath
        $pipelineStage.snapshot_path = $resolvedPipelineSnapshotPath
        $pipelineStage.summary_json = if (Test-Path -LiteralPath $resolvedPipelineSummaryPath -PathType Leaf) {
            Get-Content -LiteralPath $resolvedPipelineSummaryPath -Raw | ConvertFrom-Json
        }
        else {
            $null
        }
        $summary.stages += $pipelineStage
    }

    if ($requestedStages -contains "artifice") {
        if (-not (Test-Path -LiteralPath $resolvedArtificeRunnerPath -PathType Leaf)) {
            throw "arTIfiCE runner '$resolvedArtificeRunnerPath' does not exist."
        }

        $artificeStage = Invoke-StageScript -ScriptPath $resolvedArtificeRunnerPath -Arguments @(
            "-RomPath", $resolvedRom,
            "-AutotesterPath", $resolvedAutotester,
            "-DownloadsDir", $resolvedDownloadsDir,
            "-OutputPath", $resolvedArtificeSummaryPath
        )
        $artificeStage.name = "artifice"
        $artificeStage.summary_path = $resolvedArtificeSummaryPath
        $artificeStage.summary_json = if (Test-Path -LiteralPath $resolvedArtificeSummaryPath -PathType Leaf) {
            Get-Content -LiteralPath $resolvedArtificeSummaryPath -Raw | ConvertFrom-Json
        }
        else {
            $null
        }
        $summary.stages += $artificeStage
    }

    foreach ($stage in @($summary.stages)) {
        if (-not [bool]$stage.success) {
            $summary.blockers += ("Stage failed: {0}" -f [string]$stage.name)
        }
        if (-not [string]::IsNullOrWhiteSpace([string]$stage.invocation_error)) {
            $summary.blockers += [string]$stage.invocation_error
        }
        if ($null -ne $stage.summary_json -and
            $stage.summary_json.PSObject.Properties.Name -contains "blockers" -and
            $null -ne $stage.summary_json.blockers) {
            foreach ($blocker in @($stage.summary_json.blockers)) {
                if (-not [string]::IsNullOrWhiteSpace([string]$blocker)) {
                    $summary.blockers += ("{0}: {1}" -f [string]$stage.name, [string]$blocker)
                }
            }
        }
    }

    $summary.blockers = @($summary.blockers | Sort-Object -Unique)
    $summary.success = (@($summary.stages | Where-Object { -not $_.success }).Count -eq 0)
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
}
catch {
    $summary.success = $false
    $summary.exit_code = 2
    $summary.blockers += $_.Exception.Message
    $summary.blockers = @($summary.blockers | Sort-Object -Unique)
}
finally {
    $summary.duration_seconds = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 20)
}

Write-Host "CalcEngine CEmu Foundation"
Write-Host ("Mode            : {0}" -f $Mode)
Write-Host ("ROM             : {0}" -f [string]$summary.environment.rom_path)
Write-Host ("CEmu            : {0}" -f [string]$summary.environment.cemu_path)
Write-Host ("Autotester      : {0}" -f [string]$summary.environment.autotester_path)
Write-Host ("CEmu Headless   : {0}" -f [string]$summary.environment.cemu_headless_path)
Write-Host ("Downloads       : {0}" -f [string]$summary.environment.downloads_dir)
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
foreach ($stage in @($summary.stages)) {
    Write-Host ("{0,-16}: {1}" -f ([string]$stage.name), $(if ($stage.success) { "PASS" } else { "FAIL" }))
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
