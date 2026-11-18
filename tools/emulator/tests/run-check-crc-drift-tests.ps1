Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\check-crc-drift.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "CRC drift checker was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("check-crc-drift-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BinaryFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$SizeBytes
    )

    $buffer = New-Object byte[] $SizeBytes
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

function New-MinimalBaseline {
    param([Parameter(Mandatory = $true)][string]$Path)

    $payload = [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{ name = "CALCENG"; isASM = $true }
        hashes = [ordered]@{
            s00_boot_home = [ordered]@{
                description = "Boot screen"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("11111111")
                timeout = 5000
            }
            s01_add_2_3 = [ordered]@{
                description = "2+3 result"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("22222222")
                timeout = 5000
            }
        }
    }

    $payload | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-FakeComprehensiveRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][bool]$Success
    )

    if ($Success) {
        $content = @(
            "param(",
            "    [string]`$SummaryPath = '',",
            "    [string]`$BaselinedPath = ''",
            ")",
            "`$summary = [ordered]@{",
            "    schema_version = 1",
            "    mode = 'Verify'",
            "    verify_stage = [ordered]@{",
            "        mismatch_count = 0",
            "        mismatches = @()",
            "        success = `$true",
            "    }",
            "    success = `$true",
            "    exit_code = 0",
            "}",
            "if (-not [string]::IsNullOrWhiteSpace(`$SummaryPath)) {",
            "    `$summaryDir = Split-Path -Path `$SummaryPath -Parent",
            "    if (-not [string]::IsNullOrWhiteSpace(`$summaryDir) -and -not (Test-Path -LiteralPath `$summaryDir -PathType Container)) {",
            "        [void](New-Item -ItemType Directory -Path `$summaryDir -Force)",
            "    }",
            "    `$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8",
            "}",
            "Write-Output '[Autotest passed] Out of 2 tests attempted, 2 passed, and 0 failed.'",
            "exit 0"
        )
    } else {
        $content = @(
            "param(",
            "    [string]`$SummaryPath = '',",
            "    [string]`$BaselinedPath = ''",
            ")",
            "`$summary = [ordered]@{",
            "    schema_version = 1",
            "    mode = 'Verify'",
            "    verify_stage = [ordered]@{",
            "        mismatch_count = 1",
            "        mismatches = @([ordered]@{ hash_name = 's01_add_2_3'; description = '2+3 result'; expected_crcs = @('22222222'); observed_crc = 'DEADBEEF'; output_line = 'fake mismatch line' })",
            "        success = `$false",
            "    }",
            "    success = `$false",
            "    exit_code = 1",
            "}",
            "if (-not [string]::IsNullOrWhiteSpace(`$SummaryPath)) {",
            "    `$summaryDir = Split-Path -Path `$SummaryPath -Parent",
            "    if (-not [string]::IsNullOrWhiteSpace(`$summaryDir) -and -not (Test-Path -LiteralPath `$summaryDir -PathType Container)) {",
            "        [void](New-Item -ItemType Directory -Path `$summaryDir -Force)",
            "    }",
            "    `$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8",
            "}",
            "Write-Output 'Hash #s01_add_2_3 expected 22222222 got DEADBEEF'",
            "exit 1"
        )
    }

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function Invoke-Checker {
    param(
        [Parameter(Mandatory = $true)][string[]]$AdditionalArgs
    )

    $captured = @()
    $exitCode = 1
    try {
        $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:RunnerPath @AdditionalArgs 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $captured = @($_.Exception.Message)
        $exitCode = 1
    }

    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($captured)) {
        if ($null -ne $entry) {
            $lines.Add([string]$entry)
        }
    }

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = ($lines.ToArray() -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Checker succeeds when verify summary reports zero drift" -Body {
        $tempDir = New-TestDirectory
        $baselinePath = Join-Path $tempDir "baseline.json"
        $summaryPath = Join-Path $tempDir "summary.json"
        $runnerSummaryPath = Join-Path $tempDir "runner-summary.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-runner-success.ps1"
        $romPath = Join-Path $tempDir "test.rom"
        $autotesterPath = Join-Path $tempDir "fake-autotester.exe"

        New-MinimalBaseline -Path $baselinePath
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Success $true
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024

        $run = Invoke-Checker -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-BaselinedPath", $baselinePath,
            "-OutputPath", $summaryPath,
            "-RunnerSummaryPath", $runnerSummaryPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Checker should return success when there is no drift."
        Assert-Contains -Needle "no comprehensive CRC drift detected" -Haystack $run.output_text -Message "Checker should print a clean pass message."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary success should be true."
        Assert-Equal -Expected 0 -Actual ([int]$summary.drift_count) -Message "Summary drift count should be zero."
        Assert-Equal -Expected 2 -Actual ([int]$summary.baseline_hash_count) -Message "Baseline hash count should match the fake baseline."
    }

    Invoke-TestCase -Name "Checker reports drift details when verify summary contains mismatches" -Body {
        $tempDir = New-TestDirectory
        $baselinePath = Join-Path $tempDir "baseline.json"
        $summaryPath = Join-Path $tempDir "summary.json"
        $runnerSummaryPath = Join-Path $tempDir "runner-summary.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-runner-fail.ps1"
        $romPath = Join-Path $tempDir "test.rom"
        $autotesterPath = Join-Path $tempDir "fake-autotester.exe"

        New-MinimalBaseline -Path $baselinePath
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Success $false
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024

        $run = Invoke-Checker -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-BaselinedPath", $baselinePath,
            "-OutputPath", $summaryPath,
            "-RunnerSummaryPath", $runnerSummaryPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Checker should fail when drift is present."
        Assert-Contains -Needle "s01_add_2_3" -Haystack $run.output_text -Message "Checker output should include the drifting hash name."
        Assert-Contains -Needle "DEADBEEF" -Haystack $run.output_text -Message "Checker output should include the observed CRC."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary success should be false."
        Assert-Equal -Expected 1 -Actual ([int]$summary.drift_count) -Message "Summary should record one drift."
        Assert-Equal -Expected "s01_add_2_3" -Actual ([string]$summary.drifts[0].hash_name) -Message "Drift summary should preserve the hash name."
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
