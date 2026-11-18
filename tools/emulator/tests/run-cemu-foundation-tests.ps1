Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "scripts\cemu-foundation.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "CEmu foundation runner was not found at '$script:RunnerPath'."
    exit 2
}

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)][AllowNull()][object]$Expected,
        [Parameter(Mandatory = $true)][AllowNull()][object]$Actual,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message`nExpected: $Expected`nActual: $Actual"
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Needle,
        [Parameter(Mandatory = $true)][string]$Haystack,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Haystack.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Message`nExpected to find: $Needle`nActual text: $Haystack"
    }
}

function Invoke-TestCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $script:TestTotal++
    try {
        & $Body
        $script:TestPassed++
        Write-Host "[PASS] $Name"
    }
    catch {
        $script:TestFailed++
        Write-Host "[FAIL] $Name"
        Write-Host $_.Exception.Message
    }
}

function New-TestDirectory {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("cemu-foundation-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-DummyFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $false)][string]$Content = "dummy"
    )

    Set-Content -LiteralPath $Path -Value $Content -Encoding ASCII
}

function New-FakeFocusedRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][bool]$Success
    )

    $successLiteral = if ($Success) { '$true' } else { '$false' }
    $exitLiteral = if ($Success) { '0' } else { '1' }

    $content = @"
param(
    [string]`$OutputPath = '',
    [string]`$RomPath = '',
    [string]`$CEmuPath = '',
    [string]`$AutotesterPath = ''
)

`$summary = [ordered]@{
    success = $successLiteral
    options = [ordered]@{
        rom_path = `$RomPath
        cemu_path = `$CEmuPath
        autotester_path = `$AutotesterPath
    }
    assertions = @(
        [ordered]@{ name = 'cemu_boots_successfully'; passed = $successLiteral },
        [ordered]@{ name = 'basic_calculations_match_expected_screen_output'; passed = $successLiteral },
        [ordered]@{ name = 'calcengine_launches'; passed = $successLiteral }
    )
    blockers = @()
}

if (-not [string]::IsNullOrWhiteSpace(`$OutputPath)) {
    `$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
}

Write-Host 'fake focused runner'
exit $exitLiteral
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function New-FakePipelineRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][bool]$Success
    )

    $successLiteral = if ($Success) { 'True' } else { 'False' }
    $exitLiteral = if ($Success) { '0' } else { '1' }

    $content = @"
from __future__ import annotations

import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--rom", default="")
parser.add_argument("--autotester", default="")
parser.add_argument("--headless", default="")
parser.add_argument("--downloads-dir", default="")
parser.add_argument("--output", default="")
parser.add_argument("--snapshot", default="")
parser.add_argument("--cemu-gui", default="")
args = parser.parse_args()

summary = {
    "success": $successLiteral,
    "availability": {
        "rom_path": args.rom,
        "autotester_path": args.autotester,
        "headless_path": args.headless,
        "gui_path": args.cemu_gui,
    },
    "checkpoint_summary": {
        "success": $successLiteral,
    },
    "blockers": [],
}

if args.output:
    Path(args.output).write_text(json.dumps(summary, indent=2), encoding="utf-8")

if args.snapshot:
    Path(args.snapshot).write_text("{}", encoding="utf-8")

print("fake pipeline runner")
raise SystemExit($exitLiteral)
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function New-FakeArtificeRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][bool]$Success
    )

    $successLiteral = if ($Success) { '$true' } else { '$false' }
    $exitLiteral = if ($Success) { '0' } else { '1' }

    $content = @"
param(
    [string]`$OutputPath = '',
    [string]`$RomPath = '',
    [string]`$AutotesterPath = '',
    [string]`$DownloadsDir = ''
)

`$summary = [ordered]@{
    success = $successLiteral
    capture = [ordered]@{
        options = [ordered]@{
            rom_path = `$RomPath
            autotester_path = `$AutotesterPath
            downloads_dir = `$DownloadsDir
        }
    }
    blockers = @()
}

if (-not [string]::IsNullOrWhiteSpace(`$OutputPath)) {
    `$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
}

Write-Host 'fake artifice runner'
exit $exitLiteral
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function Invoke-Runner {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:RunnerPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = (@($captured | ForEach-Object { [string]$_ }) -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Foundation wrapper aggregates all three delegated stages" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "Rom.rom"
        $cemuPath = Join-Path $tempDir "CEmu.exe"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $headlessPath = Join-Path $tempDir "cemu-headless.exe"
        $downloadsDir = Join-Path $tempDir "downloads"
        $focusedRunnerPath = Join-Path $tempDir "focused.ps1"
        $pipelineRunnerPath = Join-Path $tempDir "pipeline.py"
        $artificeRunnerPath = Join-Path $tempDir "artifice.ps1"
        $summaryPath = Join-Path $tempDir "summary.json"
        $focusedSummaryPath = Join-Path $tempDir "focused-summary.json"
        $pipelineSummaryPath = Join-Path $tempDir "pipeline-summary.json"
        $pipelineSnapshotPath = Join-Path $tempDir "pipeline-snapshot.json"
        $artificeSummaryPath = Join-Path $tempDir "artifice-summary.json"

        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 64))
        New-DummyFile -Path $cemuPath
        New-DummyFile -Path $autotesterPath
        New-DummyFile -Path $headlessPath
        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-FakeFocusedRunner -Path $focusedRunnerPath -Success $true
        New-FakePipelineRunner -Path $pipelineRunnerPath -Success $true
        New-FakeArtificeRunner -Path $artificeRunnerPath -Success $true

        $run = Invoke-Runner -Arguments @(
            "-Mode", "All",
            "-RomPath", $romPath,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", $autotesterPath,
            "-CEmuHeadlessPath", $headlessPath,
            "-DownloadsDir", $downloadsDir,
            "-FocusedRunnerPath", $focusedRunnerPath,
            "-PipelineRunnerPath", $pipelineRunnerPath,
            "-ArtificeRunnerPath", $artificeRunnerPath,
            "-FocusedSummaryPath", $focusedSummaryPath,
            "-PipelineSummaryPath", $pipelineSummaryPath,
            "-PipelineSnapshotPath", $pipelineSnapshotPath,
            "-ArtificeSummaryPath", $artificeSummaryPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Wrapper should pass when all delegated stages pass."
        Assert-Contains -Needle "Overall         : PASS" -Haystack $run.output_text -Message "Console output should report PASS."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected 3 -Actual ([int]@($summary.stages).Count) -Message "All mode should run three stages."
        Assert-Equal -Expected 3 -Actual ([int]@($summary.focused_assertions).Count) -Message "Focused assertions should be surfaced in the top-level summary."
        Assert-True -Condition (Test-Path -LiteralPath $pipelineSnapshotPath -PathType Leaf) -Message "Pipeline stage should be allowed to write the snapshot artifact."
    }

    Invoke-TestCase -Name "Focused mode only requires the focused delegated runner" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "Rom.rom"
        $cemuPath = Join-Path $tempDir "CEmu.exe"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $focusedRunnerPath = Join-Path $tempDir "focused.ps1"
        $summaryPath = Join-Path $tempDir "summary.json"
        $focusedSummaryPath = Join-Path $tempDir "focused-summary.json"

        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 64))
        New-DummyFile -Path $cemuPath
        New-DummyFile -Path $autotesterPath
        New-FakeFocusedRunner -Path $focusedRunnerPath -Success $true

        $run = Invoke-Runner -Arguments @(
            "-Mode", "Focused",
            "-RomPath", $romPath,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", $autotesterPath,
            "-FocusedRunnerPath", $focusedRunnerPath,
            "-FocusedSummaryPath", $focusedSummaryPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Focused mode should not require pipeline or artifice dependencies."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected 1 -Actual ([int]@($summary.stages).Count) -Message "Focused mode should only run one stage."
        Assert-Equal -Expected "focused" -Actual ([string]$summary.stages[0].name) -Message "Focused mode should delegate only to the focused runner."
    }

    Invoke-TestCase -Name "Delegated stage failures bubble up into the foundation summary" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "Rom.rom"
        $cemuPath = Join-Path $tempDir "CEmu.exe"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $focusedRunnerPath = Join-Path $tempDir "focused.ps1"
        $summaryPath = Join-Path $tempDir "summary.json"
        $focusedSummaryPath = Join-Path $tempDir "focused-summary.json"

        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 64))
        New-DummyFile -Path $cemuPath
        New-DummyFile -Path $autotesterPath
        New-FakeFocusedRunner -Path $focusedRunnerPath -Success $false

        $run = Invoke-Runner -Arguments @(
            "-Mode", "Focused",
            "-RomPath", $romPath,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", $autotesterPath,
            "-FocusedRunnerPath", $focusedRunnerPath,
            "-FocusedSummaryPath", $focusedSummaryPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Wrapper should fail when the focused delegated stage fails."
        Assert-Contains -Needle "Stage failed: focused" -Haystack ((Get-Content -LiteralPath $summaryPath -Raw)) -Message "Top-level blockers should record the failed stage."
    }
}
finally {
    foreach ($directory in $script:CreatedTempDirectories) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ""
Write-Host "Total tests: $script:TestTotal"
Write-Host "Passed: $script:TestPassed"
Write-Host "Failed: $script:TestFailed"

if ($script:TestFailed -gt 0) {
    exit 1
}

exit 0
