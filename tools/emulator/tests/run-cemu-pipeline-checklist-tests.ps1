Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-cemu-pipeline-checklist.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "CEmu pipeline checklist runner was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("cemu-pipeline-checklist-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-FakeStageRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RunnerName,
        [Parameter(Mandatory = $true)][bool]$Success,
        [Parameter(Mandatory = $true)][string]$StageType
    )

    $successLiteral = if ($Success) { '$true' } else { '$false' }
    $exitLiteral = if ($Success) { '0' } else { '1' }
    $blockersLiteral = if ($Success) { '@()' } else { "@('stage blocker from $StageType')" }

    $summaryLiteral = switch ($StageType) {
        "prehardware" {
            @"
`$summary = [ordered]@{
    runner = '$RunnerName'
    success = $successLiteral
    exit_code = $exitLiteral
    totals = [ordered]@{
        total_stages = 7
        passed = $(if ($Success) { 7 } else { 6 })
        failed = $(if ($Success) { 0 } else { 1 })
        required_failed = $(if ($Success) { 0 } else { 1 })
    }
    blockers = $blockersLiteral
}
"@
        }
        "artifice" {
            @"
`$summary = [ordered]@{
    runner = '$RunnerName'
    success = $successLiteral
    exit_code = $exitLiteral
    blockers = $blockersLiteral
    comparison = [ordered]@{
        overall_status = '$(if ($Success) { "PASS" } else { "FAIL" })'
        results = @(
            [ordered]@{ step = 'artifice_shell_loaded'; status = 'PASS' },
            [ordered]@{ step = 'artifice_quit_transition'; status = 'PASS' },
            [ordered]@{ step = 'ti_os_program_menu_open'; status = 'PASS' },
            [ordered]@{ step = 'calcengine_launch_menu'; status = 'PASS' },
            [ordered]@{ step = 'calcengine_result_2plus3'; status = 'PASS' },
            [ordered]@{ step = 'calcengine_menu_open'; status = 'PASS' }
        )
    }
    capture = [ordered]@{
        captured_screens = @(
            [ordered]@{ step = 'artifice_shell_loaded' },
            [ordered]@{ step = 'artifice_quit_transition' },
            [ordered]@{ step = 'ti_os_program_menu_open' },
            [ordered]@{ step = 'calcengine_launch_menu' },
            [ordered]@{ step = 'calcengine_result_2plus3' },
            [ordered]@{ step = 'calcengine_menu_open' }
        )
    }
}
"@
        }
        "multi-module" {
            @"
`$summary = [ordered]@{
    runner = '$RunnerName'
    success = $successLiteral
    exit_code = $exitLiteral
    blockers = $blockersLiteral
    results = [ordered]@{
        staged_transfer_count = 40
        dispatch_program_count = 35
        smoke_hash_count = 19
        smoke_pass_count = 19
    }
}
"@
        }
        "runtime" {
            @"
`$summary = [ordered]@{
    runner = '$RunnerName'
    success = $successLiteral
    exit_code = $exitLiteral
    blockers = $blockersLiteral
    blank_slate_package = [ordered]@{
        copied_file_count = 40
    }
    blank_slate_comprehensive = [ordered]@{
        summary_json = [ordered]@{
            verify_stage = [ordered]@{
                crc_catalog_hash_count = 189
                pass_count = 189
            }
        }
    }
    clean_exit_probe = [ordered]@{
        summary_json = [ordered]@{
            verify_stage = [ordered]@{
                crc_catalog_hash_count = 4
                pass_count = 4
            }
        }
    }
}
"@
        }
        "crc-drift" {
            @"
`$summary = [ordered]@{
    runner = '$RunnerName'
    success = $successLiteral
    exit_code = $exitLiteral
    blockers = $blockersLiteral
    baseline_hash_count = 189
    drift_count = $(if ($Success) { 0 } else { 3 })
}
"@
        }
        default {
            throw "Unsupported fake stage type '$StageType'."
        }
    }

    $content = @"
param(
    [string]`$OutputPath = ''
)

$summaryLiteral

if (-not [string]::IsNullOrWhiteSpace(`$OutputPath)) {
    `$summaryDir = Split-Path -Path `$OutputPath -Parent
    if (-not [string]::IsNullOrWhiteSpace(`$summaryDir) -and -not (Test-Path -LiteralPath `$summaryDir -PathType Container)) {
        [void](New-Item -ItemType Directory -Path `$summaryDir -Force)
    }
    `$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
}

Write-Host 'fake stage $StageType complete'
exit $exitLiteral
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function Invoke-Checklist {
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
    Invoke-TestCase -Name "Checklist aggregates child metrics into one passing summary" -Body {
        $tempDir = New-TestDirectory
        $summaryPath = Join-Path $tempDir "summary.json"
        $prehardwarePath = Join-Path $tempDir "prehardware.ps1"
        $artificePath = Join-Path $tempDir "artifice.ps1"
        $multiModulePath = Join-Path $tempDir "multi-module.ps1"
        $runtimePath = Join-Path $tempDir "runtime.ps1"
        $crcDriftPath = Join-Path $tempDir "crc-drift.ps1"

        New-FakeStageRunner -Path $prehardwarePath -RunnerName "run-prehardware-validation-gate.ps1" -Success $true -StageType "prehardware"
        New-FakeStageRunner -Path $artificePath -RunnerName "run-artifice-e2e-verification.ps1" -Success $true -StageType "artifice"
        New-FakeStageRunner -Path $multiModulePath -RunnerName "run-multi-module-smoke.ps1" -Success $true -StageType "multi-module"
        New-FakeStageRunner -Path $runtimePath -RunnerName "run-runtime-integrity-verification.ps1" -Success $true -StageType "runtime"
        New-FakeStageRunner -Path $crcDriftPath -RunnerName "check-crc-drift.ps1" -Success $true -StageType "crc-drift"

        $run = Invoke-Checklist -Arguments @(
            "-OutputPath", $summaryPath,
            "-PrehardwareGateRunnerPath", $prehardwarePath,
            "-ArtificeE2ERunnerPath", $artificePath,
            "-MultiModuleSmokeRunnerPath", $multiModulePath,
            "-RuntimeIntegrityRunnerPath", $runtimePath,
            "-CrcDriftRunnerPath", $crcDriftPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Checklist should succeed when every delegated stage succeeds."
        Assert-Contains -Needle "Overall         : PASS" -Haystack $run.output_text -Message "Console output should report PASS."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected 5 -Actual ([int]$summary.totals.total_stages) -Message "Checklist should record five delegated stages."
        Assert-Equal -Expected 0 -Actual ([int]$summary.totals.required_failed) -Message "Required failures should be zero on success."
        Assert-Equal -Expected 7 -Actual ([int]$summary.results.prehardware_stage_count) -Message "Prehardware stage count should be copied from the delegated summary."
        Assert-Equal -Expected 6 -Actual ([int]$summary.results.artifice_e2e_pass_count) -Message "Artifice pass count should be derived from comparison results."
        Assert-Equal -Expected 6 -Actual ([int]$summary.results.artifice_e2e_screen_count) -Message "Artifice screen count should come from captured screens."
        Assert-Equal -Expected 35 -Actual ([int]$summary.results.multi_module_dispatch_program_count) -Message "Dispatch count should come from multi-module smoke."
        Assert-Equal -Expected 19 -Actual ([int]$summary.results.multi_module_smoke_pass_count) -Message "Smoke pass count should come from multi-module smoke."
        Assert-Equal -Expected 189 -Actual ([int]$summary.results.runtime_blank_slate_pass_count) -Message "Blank-slate pass count should come from runtime integrity."
        Assert-Equal -Expected 4 -Actual ([int]$summary.results.runtime_clean_exit_pass_count) -Message "Clean-exit pass count should come from runtime integrity."
        Assert-Equal -Expected 40 -Actual ([int]$summary.results.runtime_blank_slate_transfer_count) -Message "Blank-slate transfer count should come from runtime integrity."
        Assert-Equal -Expected 0 -Actual ([int]$summary.results.crc_drift_count) -Message "CRC drift count should come from the drift checker."
        Assert-Equal -Expected 189 -Actual ([int]$summary.results.crc_baseline_hash_count) -Message "CRC baseline hash count should come from the drift checker."
    }

    Invoke-TestCase -Name "Checklist bubbles structured blockers up from a failed stage" -Body {
        $tempDir = New-TestDirectory
        $summaryPath = Join-Path $tempDir "summary.json"
        $prehardwarePath = Join-Path $tempDir "prehardware.ps1"
        $artificePath = Join-Path $tempDir "artifice.ps1"
        $multiModulePath = Join-Path $tempDir "multi-module.ps1"
        $runtimePath = Join-Path $tempDir "runtime.ps1"
        $crcDriftPath = Join-Path $tempDir "crc-drift.ps1"

        New-FakeStageRunner -Path $prehardwarePath -RunnerName "run-prehardware-validation-gate.ps1" -Success $true -StageType "prehardware"
        New-FakeStageRunner -Path $artificePath -RunnerName "run-artifice-e2e-verification.ps1" -Success $true -StageType "artifice"
        New-FakeStageRunner -Path $multiModulePath -RunnerName "run-multi-module-smoke.ps1" -Success $false -StageType "multi-module"
        New-FakeStageRunner -Path $runtimePath -RunnerName "run-runtime-integrity-verification.ps1" -Success $true -StageType "runtime"
        New-FakeStageRunner -Path $crcDriftPath -RunnerName "check-crc-drift.ps1" -Success $true -StageType "crc-drift"

        $run = Invoke-Checklist -Arguments @(
            "-OutputPath", $summaryPath,
            "-PrehardwareGateRunnerPath", $prehardwarePath,
            "-ArtificeE2ERunnerPath", $artificePath,
            "-MultiModuleSmokeRunnerPath", $multiModulePath,
            "-RuntimeIntegrityRunnerPath", $runtimePath,
            "-CrcDriftRunnerPath", $crcDriftPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Checklist should fail when any required stage fails."
        Assert-Contains -Needle "Overall         : FAIL" -Haystack $run.output_text -Message "Console output should report FAIL."
        Assert-Contains -Needle "stage blocker from multi-module" -Haystack $run.output_text -Message "Console output should surface the delegated blocker."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary should report failure."
        Assert-Equal -Expected 1 -Actual ([int]$summary.totals.required_failed) -Message "Required failure count should increment."
        Assert-True -Condition (@($summary.blockers) -contains "stage blocker from multi-module") -Message "Delegated blocker should be bubbled to the top-level summary."
        $failedStage = @($summary.stages | Where-Object { $_.name -eq "multi_module_smoke" } | Select-Object -First 1)[0]
        Assert-Equal -Expected $false -Actual ([bool]$failedStage.success) -Message "Failed delegated stage should be marked as failed."
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
