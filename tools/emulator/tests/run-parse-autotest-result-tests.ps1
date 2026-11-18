Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:ParserPath = Join-Path $script:RepoRoot "tools\emulator\parse-autotest-result.ps1"
if (-not (Test-Path -LiteralPath $script:ParserPath -PathType Leaf)) {
    Write-Error "Autotest result parser was not found at '$script:ParserPath'."
    exit 2
}

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]

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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("parse-autotest-result-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BaselineFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    $payload = [ordered]@{
        hashes = [ordered]@{
            s00_boot_home = [ordered]@{
                description = "Boot screen"
                expected_CRCs = @("11111111")
            }
            s01_add_2_3 = [ordered]@{
                description = "2+3 result"
                expected_CRCs = @("22222222")
            }
        }
    }

    $payload | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Invoke-Parser {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:ParserPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = (@($captured | ForEach-Object { [string]$_ }) -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Parser succeeds when all hashes pass" -Body {
        $tempDir = New-TestDirectory
        $inputPath = Join-Path $tempDir "input.json"
        $outputPath = Join-Path $tempDir "parsed.json"
        $baselinePath = Join-Path $tempDir "baseline.json"

        New-BaselineFile -Path $baselinePath
        $payload = [ordered]@{
            verify_stage = [ordered]@{
                baselined_path = $baselinePath
                hash_results = @(
                    [ordered]@{ hash_name = "s00_boot_home"; status = "pass"; expected_crcs = @("11111111"); observed_crc = "" },
                    [ordered]@{ hash_name = "s01_add_2_3"; status = "pass"; expected_crcs = @("22222222"); observed_crc = "" }
                )
            }
        }
        $payload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $inputPath -Encoding UTF8

        $run = Invoke-Parser -Arguments @("-InputPath", $inputPath, "-OutputPath", $outputPath)

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Parser should return success when all hashes pass."
        Assert-Contains -Needle "Passed         : 2" -Haystack $run.output_text -Message "Parser should report the pass count."

        $parsed = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$parsed.success) -Message "Parsed JSON success should be true."
        Assert-Equal -Expected 0 -Actual ([int]$parsed.total_fail_count) -Message "Parsed JSON fail count should be zero."
        Assert-Equal -Expected "not_reported" -Actual ([string]$parsed.hash_results[0].observed_crc_source) -Message "Parser should backfill not-reported provenance for pass rows without raw CRCs."
        Assert-Equal -Expected "reported_pass_without_observed_crc" -Actual ([string]$parsed.hash_results[0].status_reason) -Message "Parser should backfill an explicit pass-without-CRC status reason."
    }

    Invoke-TestCase -Name "Parser reports failures and missing hashes" -Body {
        $tempDir = New-TestDirectory
        $inputPath = Join-Path $tempDir "input.json"
        $outputPath = Join-Path $tempDir "parsed.json"
        $baselinePath = Join-Path $tempDir "baseline.json"

        New-BaselineFile -Path $baselinePath
        $payload = [ordered]@{
            verify_stage = [ordered]@{
                baselined_path = $baselinePath
                hash_results = @(
                    [ordered]@{ hash_name = "s00_boot_home"; status = "fail"; expected_crcs = @("11111111"); observed_crc = "DEADBEEF" },
                    [ordered]@{ hash_name = "s01_add_2_3"; status = "missing"; expected_crcs = @("22222222"); observed_crc = "" }
                )
            }
        }
        $payload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $inputPath -Encoding UTF8

        $run = Invoke-Parser -Arguments @("-InputPath", $inputPath, "-OutputPath", $outputPath)

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Parser should fail when hashes fail or go missing."
        Assert-Contains -Needle "DEADBEEF" -Haystack $run.output_text -Message "Parser output should include the observed failing CRC."
        Assert-Contains -Needle "Missing hashes:" -Haystack $run.output_text -Message "Parser output should enumerate missing hashes."

        $parsed = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$parsed.success) -Message "Parsed JSON success should be false."
        Assert-Equal -Expected 1 -Actual ([int]$parsed.total_fail_count) -Message "Parsed JSON fail count should be one."
        Assert-Equal -Expected 1 -Actual ([int]$parsed.total_missing_count) -Message "Parsed JSON missing count should be one."
        Assert-Equal -Expected "reported_crc" -Actual ([string]$parsed.hash_results[0].observed_crc_source) -Message "Parser should preserve reported CRC provenance for mismatches."
        Assert-Equal -Expected "crc_mismatch" -Actual ([string]$parsed.hash_results[0].status_reason) -Message "Parser should backfill the mismatch reason for failing rows."
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
