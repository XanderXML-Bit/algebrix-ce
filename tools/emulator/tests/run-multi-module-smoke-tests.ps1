Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-multi-module-smoke.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "Multi-module smoke runner was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("multi-module-smoke-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-FakeArtificeRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $false)][AllowEmptyString()][string]$ObservationPath = ""
    )

    $observationWrite = ""
    if (-not [string]::IsNullOrWhiteSpace($ObservationPath)) {
        $observationWrite = @"
`$observation = [ordered]@{
    bootstrap_artifice_asset = [bool]`$BootstrapArtificeAsset
    artifice_bootstrap_script_path = [string]`$ArtificeBootstrapScriptPath
}
`$observation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath '$ObservationPath' -Encoding UTF8
"@
    }

    $content = @"
param(
    [string]`$RomPath,
    [string]`$AutotesterPath,
    [string]`$DownloadsDir,
    [string]`$ArtificeProgramPath,
    [switch]`$BootstrapArtificeAsset,
    [string]`$ArtificeBootstrapScriptPath,
    [string]`$Mode,
    [string]`$OutputPath
)

$observationWrite

`$summary = [ordered]@{
    schema_version = 1
    success = `$true
    exit_code = 0
    mode = `$Mode
    staged_downloads = [ordered]@{
        transfer_count = 40
    }
    delegated_runner = [ordered]@{
        summary_json = [ordered]@{
            verify_stage = [ordered]@{
                crc_catalog_hash_count = 19
                pass_count = 19
            }
        }
    }
}
`$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
Write-Host 'fake artifice smoke ok'
exit 0
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function New-FakeEnvironmentRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $false)][switch]$Fail
    )

    $successLiteral = if ($Fail) { '$false' } else { '$true' }
    $exitLiteral = if ($Fail) { '1' } else { '0' }
    $errorValue = if ($Fail) { "'dispatch failed'" } else { "''" }

    $content = @"
param(
    [string]`$RomPath,
    [string]`$AutotesterPath,
    [string]`$DownloadsDir,
    [switch]`$SkipCEmuVerify,
    [string]`$OutputPath
)

`$summary = [ordered]@{
    schema_version = 1
    success = $successLiteral
    exit_code = $exitLiteral
    error = $errorValue
    package_audit = [ordered]@{
        expected_program_count = 35
        expected_appvar_count = 4
    }
    ez80_dispatch = [ordered]@{
        program_count = 35
    }
}
`$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
if ($successLiteral) {
    Write-Host 'fake environment verification ok'
    exit 0
}
Write-Error 'fake environment verification failed'
exit 1
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function Invoke-Runner {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:RunnerPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = (@($captured | ForEach-Object { [string]$_ }) -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Multi-module smoke aggregates artifice and dispatch counts" -Body {
        $tempDir = New-TestDirectory
        $summaryPath = Join-Path $tempDir "summary.json"
        $artificeRunnerPath = Join-Path $tempDir "fake-artifice.ps1"
        $environmentRunnerPath = Join-Path $tempDir "fake-environment.ps1"

        New-FakeArtificeRunner -Path $artificeRunnerPath
        New-FakeEnvironmentRunner -Path $environmentRunnerPath

        $run = Invoke-Runner -Arguments @(
            "-RomPath", "rom\Rom.rom",
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", "C:\Users\user\Downloads\CalcEngine",
            "-ArtificeSmokeRunnerPath", $artificeRunnerPath,
            "-EnvironmentRunnerPath", $environmentRunnerPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when both delegated stages pass."
        Assert-Contains -Needle "Overall         : PASS" -Haystack $run.output_text -Message "Console output should report PASS."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected 40 -Actual ([int]$summary.results.staged_transfer_count) -Message "Transfer count should be copied from the artifice summary."
        Assert-Equal -Expected 35 -Actual ([int]$summary.results.manifest_module_count) -Message "Program count should come from the environment summary."
        Assert-Equal -Expected 4 -Actual ([int]$summary.results.runtime_dependency_count) -Message "Runtime dependency count should come from the environment summary."
        Assert-Equal -Expected 35 -Actual ([int]$summary.results.dispatch_program_count) -Message "Dispatch count should come from the environment summary."
        Assert-Equal -Expected 19 -Actual ([int]$summary.results.smoke_hash_count) -Message "Smoke hash count should come from the artifice summary."
        Assert-Equal -Expected 19 -Actual ([int]$summary.results.smoke_pass_count) -Message "Smoke pass count should come from the artifice summary."
    }

    Invoke-TestCase -Name "Multi-module smoke surfaces delegated environment failures as blockers" -Body {
        $tempDir = New-TestDirectory
        $summaryPath = Join-Path $tempDir "summary.json"
        $artificeRunnerPath = Join-Path $tempDir "fake-artifice.ps1"
        $environmentRunnerPath = Join-Path $tempDir "fake-environment-fail.ps1"

        New-FakeArtificeRunner -Path $artificeRunnerPath
        New-FakeEnvironmentRunner -Path $environmentRunnerPath -Fail

        $run = Invoke-Runner -Arguments @(
            "-RomPath", "rom\Rom.rom",
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", "C:\Users\user\Downloads\CalcEngine",
            "-ArtificeSmokeRunnerPath", $artificeRunnerPath,
            "-EnvironmentRunnerPath", $environmentRunnerPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when environment verification fails."
        Assert-Contains -Needle "Overall         : FAIL" -Haystack $run.output_text -Message "Console output should report FAIL."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary should report failure."
        Assert-True -Condition (@($summary.blockers) -contains "dispatch failed") -Message "Delegated environment error should be surfaced as a blocker."
    }

    Invoke-TestCase -Name "Multi-module smoke forwards bootstrap options to the artifice stage" -Body {
        $tempDir = New-TestDirectory
        $summaryPath = Join-Path $tempDir "summary.json"
        $artificeRunnerPath = Join-Path $tempDir "fake-artifice.ps1"
        $artificeObservationPath = Join-Path $tempDir "artifice-observation.json"
        $environmentRunnerPath = Join-Path $tempDir "fake-environment.ps1"
        $bootstrapScriptPath = Join-Path $tempDir "fake-bootstrap.ps1"

        New-FakeArtificeRunner -Path $artificeRunnerPath -ObservationPath $artificeObservationPath
        New-FakeEnvironmentRunner -Path $environmentRunnerPath

        $run = Invoke-Runner -Arguments @(
            "-RomPath", "rom\Rom.rom",
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", "C:\Users\user\Downloads\CalcEngine",
            "-BootstrapArtificeAsset",
            "-ArtificeBootstrapScriptPath", $bootstrapScriptPath,
            "-ArtificeSmokeRunnerPath", $artificeRunnerPath,
            "-EnvironmentRunnerPath", $environmentRunnerPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when both delegated stages pass."
        Assert-True -Condition (Test-Path -LiteralPath $artificeObservationPath -PathType Leaf) -Message "Artifice observation should have been written."

        $observation = Get-Content -LiteralPath $artificeObservationPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$observation.bootstrap_artifice_asset) -Message "Bootstrap switch should be forwarded."
        Assert-Equal -Expected $bootstrapScriptPath -Actual ([string]$observation.artifice_bootstrap_script_path) -Message "Bootstrap script path should be forwarded."
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
