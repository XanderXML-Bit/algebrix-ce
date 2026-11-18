Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RunnerPath = Join-Path (Split-Path -Path $PSScriptRoot -Parent) "run-cas-gap-isolation.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "CAS gap isolation runner was not found at '$script:RunnerPath'."
    exit 2
}

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).ProviderPath
$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Expected,

        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message`nExpected: $Expected`nActual: $Actual"
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Needle,

        [Parameter(Mandatory = $true)]
        [string]$Haystack,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Haystack.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Message`nExpected to find: $Needle`nActual text: $Haystack"
    }
}

function Invoke-TestCase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Body
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("cas-gap-runner-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BinaryFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [int]$SizeBytes
    )

    $buffer = New-Object byte[] $SizeBytes
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

function New-BaselineSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $summary = [ordered]@{
        deriv_current = @("4F016462", "32BFD188", "32BFD188")
        lim_current = @("74BE39C5", "32BFD188", "32BFD188")
        expand_control = @("0BEF7481", "EB9D340C", "B7D53D33")
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, ($summary | ConvertTo-Json -Depth 8), $utf8NoBom)
}

function New-FakeAutotester {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = @(
        "@echo off",
        "set CASE_DIR=%cd%",
        "for %%I in (""%CASE_DIR%"") do set CASE_NAME=%%~nxI",
        "echo P3> vram_pre_num1.ppm",
        "echo 1 1>> vram_pre_num1.ppm",
        "echo 255>> vram_pre_num1.ppm",
        "echo 0 0 0>> vram_pre_num1.ppm",
        "echo P3> vram_t3_num2.ppm",
        "echo 1 1>> vram_t3_num2.ppm",
        "echo 255>> vram_t3_num2.ppm",
        "echo 255 255 255>> vram_t3_num2.ppm",
        "echo P3> vram_t10_num3.ppm",
        "echo 1 1>> vram_t10_num3.ppm",
        "echo 255>> vram_t10_num3.ppm",
        "echo 128 128 128>> vram_t10_num3.ppm",
        "echo dump> failure_hashsample_dump.bin",
        "if /I ""%CASE_NAME%""==""deriv_minimal"" goto deriv",
        "if /I ""%CASE_NAME%""==""lim_minimal"" goto lim",
        "if /I ""%CASE_NAME%""==""expand_minimal_control"" goto expand",
        "echo Unknown case directory %CASE_NAME%",
        "exit /b 9",
        ":deriv",
        "echo [Test failed!] Hash #deriv_current_pre (""deriv_current pre"") did not match the expected CRC 0 (got 4F016462).",
        "echo [Test failed!] Hash #deriv_current_t3 (""deriv_current t3"") did not match the expected CRC 0 (got 32BFD188).",
        "echo [Test failed!] Hash #deriv_current_t10 (""deriv_current t10"") did not match the expected CRC 0 (got 32BFD188).",
        "exit /b 3",
        ":lim",
        "echo [Test failed!] Hash #lim_current_pre (""lim_current pre"") did not match the expected CRC 0 (got 74BE39C5).",
        "echo [Test failed!] Hash #lim_current_t3 (""lim_current t3"") did not match the expected CRC 0 (got 32BFD188).",
        "echo [Test failed!] Hash #lim_current_t10 (""lim_current t10"") did not match the expected CRC 0 (got 32BFD188).",
        "exit /b 3",
        ":expand",
        "echo [Test failed!] Hash #expand_control_pre (""expand_control pre"") did not match the expected CRC 0 (got 0BEF7481).",
        "echo [Test failed!] Hash #expand_control_t3 (""expand_control t3"") did not match the expected CRC 0 (got EB9D340C).",
        "echo [Test failed!] Hash #expand_control_t10 (""expand_control t10"") did not match the expected CRC 0 (got B7D53D33).",
        "exit /b 3"
    )

    Set-Content -LiteralPath $Path -Value $content -Encoding ASCII
}

function Invoke-Runner {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdditionalArgs
    )

    $powershellPath = (Get-Command -Name "powershell.exe" -ErrorAction Stop).Source
    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:RunnerPath
    )
    foreach ($argument in @($AdditionalArgs)) {
        $arguments += [string]$argument
    }

    $captured = @()
    $exitCode = 1
    try {
        $captured = & $powershellPath @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $captured = @($_.Exception.Message)
        $exitCode = 1
    }

    $outputLines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($captured)) {
        if ($null -ne $entry) {
            $outputLines.Add([string]$entry)
        }
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = ($outputLines.ToArray() -join "`n")
    }
}

try {
    Invoke-TestCase -Name "CAS gap runner writes minimal module summaries and harvests ppm artifacts" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $autotesterPath = Join-Path $tempDir "autotester.cmd"
        $outputDir = Join-Path $tempDir "output"
        $baselinePath = Join-Path $tempDir "pass54-summary.json"

        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-FakeAutotester -Path $autotesterPath
        New-BaselineSummary -Path $baselinePath

        $run = Invoke-Runner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-OutputDir", $outputDir,
            "-BaselineSummaryPath", $baselinePath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "CAS gap runner should succeed with the fake autotester.`nOutput:`n$($run.output_text)"
        }

        $summaryPath = Join-Path $outputDir "summary.json"
        Assert-True -Condition (Test-Path -LiteralPath $summaryPath -PathType Leaf) -Message "Runner summary should exist."
        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json

        Assert-Equal -Expected "run-cas-gap-isolation.ps1" -Actual ([string]$summary.runner) -Message "Runner name mismatch."
        Assert-Equal -Expected 3 -Actual ([int]@($summary.cases).Count) -Message "Runner should emit three isolation cases."

        $deriv = @($summary.cases) | Where-Object { $_.name -eq "deriv_minimal" } | Select-Object -First 1
        $lim = @($summary.cases) | Where-Object { $_.name -eq "lim_minimal" } | Select-Object -First 1
        $expand = @($summary.cases) | Where-Object { $_.name -eq "expand_minimal_control" } | Select-Object -First 1

        Assert-Equal -Expected 1 -Actual ([int]$deriv.module_count) -Message "Derivative case should transfer only CECALC beside launcher."
        Assert-Equal -Expected 5 -Actual ([int]$lim.module_count) -Message "Limit case should transfer router/base/adv/cont/rel."
        Assert-Equal -Expected 2 -Actual ([int]$expand.module_count) -Message "Expand control should transfer CEALG and CEALGF."

        Assert-Equal -Expected "32BFD188" -Actual ([string]$deriv.observed_timeline.t3) -Message "Derivative t3 CRC mismatch."
        Assert-Equal -Expected "32BFD188" -Actual ([string]$lim.observed_timeline.t10) -Message "Limit t10 CRC mismatch."
        Assert-Equal -Expected "B7D53D33" -Actual ([string]$expand.observed_timeline.t10) -Message "Expand control t10 CRC mismatch."

        Assert-True -Condition ([bool]$deriv.matches_pass54_baseline) -Message "Derivative case should compare equal to the pass54 baseline."
        Assert-True -Condition ([bool]$lim.matches_pass54_baseline) -Message "Limit case should compare equal to the pass54 baseline."
        Assert-True -Condition ([bool]$expand.matches_pass54_baseline) -Message "Expand control should compare equal to the pass54 baseline."

        $derivDir = Join-Path $outputDir "deriv_minimal"
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $derivDir "vram_pre_num1.ppm") -PathType Leaf) -Message "Derivative case should retain the pre-enter PPM."
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $derivDir "failure_hashsample_dump.bin") -PathType Leaf) -Message "Derivative case should retain failure dump artifacts."
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $derivDir "modules.txt") -PathType Leaf) -Message "Derivative case should emit a module list manifest."

        Assert-Contains -Needle "deriv_minimal" -Haystack $run.output_text -Message "Runner output should mention deriv_minimal."
        Assert-Contains -Needle "lim_minimal" -Haystack $run.output_text -Message "Runner output should mention lim_minimal."
        Assert-Contains -Needle "expand_minimal_control" -Haystack $run.output_text -Message "Runner output should mention expand_minimal_control."
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
