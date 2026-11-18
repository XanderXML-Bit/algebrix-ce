Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "scripts\cemu-test-runner.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "CEmu test runner was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("cemu-test-runner-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-SuiteFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    $suite = [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "CALCENG"
            isASM = $true
        }
        sequence = @("action|launch", "hash|boot_home", "hash|addition_result")
        hashes = [ordered]@{
            boot_home = [ordered]@{
                description = "boot"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("AAAA1111")
                timeout = 8000
            }
            addition_result = [ordered]@{
                description = "add"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("BBBB2222")
                timeout = 8000
            }
            prompt_after_addition = [ordered]@{
                description = "prompt"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("CCCC3333")
                timeout = 8000
            }
            multiplication_result = [ordered]@{
                description = "mul"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("DDDD4444")
                timeout = 8000
            }
            negative_result = [ordered]@{
                description = "neg"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("EEEE5555")
                timeout = 8000
            }
            decimal_result = [ordered]@{
                description = "dec"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("FFFF6666")
                timeout = 8000
            }
            divide_by_zero_error = [ordered]@{
                description = "div0"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("99997777")
                timeout = 8000
            }
        }
    }

    $suite | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-FakeRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][bool]$Success
    )

    $successLiteral = if ($Success) { '$true' } else { '$false' }
    $exitLiteral = if ($Success) { '0' } else { '3' }
    $statusLiteral = if ($Success) { 'pass' } else { 'fail' }
    $observedLiteral = if ($Success) { "''" } else { "'00000000'" }

    $content = @"
param(
    [string]`$GeneratedPath = '',
    [string]`$SummaryPath = '',
    [string]`$TemplatePath = ''
)

`$generated = [ordered]@{
    rom = 'C:\fake\Rom.rom'
    transfer_files = @(
        'C:\fake\CALCENG.8xp',
        'C:\fake\CEALG.8xp'
    )
    target = [ordered]@{
        name = 'CALCENG'
        isASM = `$true
    }
}

if (-not [string]::IsNullOrWhiteSpace(`$GeneratedPath)) {
    `$generated | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath `$GeneratedPath -Encoding UTF8
}

`$verifyStage = [ordered]@{
    generated_path = `$GeneratedPath
    baselined_path = `$TemplatePath
    crc_catalog_hash_count = 7
    pass_count = $(if ($Success) { 7 } else { 6 })
    fail_count = $(if ($Success) { 0 } else { 1 })
    missing_count = 0
    hash_results = @(
        [ordered]@{ hash_name = 'boot_home'; status = '$statusLiteral'; expected_crcs = @('AAAA1111'); observed_crc = $observedLiteral },
        [ordered]@{ hash_name = 'addition_result'; status = 'pass'; expected_crcs = @('BBBB2222'); observed_crc = '' },
        [ordered]@{ hash_name = 'prompt_after_addition'; status = 'pass'; expected_crcs = @('CCCC3333'); observed_crc = '' },
        [ordered]@{ hash_name = 'multiplication_result'; status = 'pass'; expected_crcs = @('DDDD4444'); observed_crc = '' },
        [ordered]@{ hash_name = 'negative_result'; status = 'pass'; expected_crcs = @('EEEE5555'); observed_crc = '' },
        [ordered]@{ hash_name = 'decimal_result'; status = 'pass'; expected_crcs = @('FFFF6666'); observed_crc = '' },
        [ordered]@{ hash_name = 'divide_by_zero_error'; status = 'pass'; expected_crcs = @('99997777'); observed_crc = '' }
    )
    mismatches = @()
    success = $successLiteral
}

`$summary = [ordered]@{
    success = $successLiteral
    exit_code = $exitLiteral
    verify_stage = `$verifyStage
}

if (-not [string]::IsNullOrWhiteSpace(`$SummaryPath)) {
    `$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8
}

Write-Host 'fake comprehensive autotest runner complete'
exit $exitLiteral
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function New-FakeParser {
    param([Parameter(Mandatory = $true)][string]$Path)

    $content = @"
param(
    [string]`$InputPath = '',
    [string]`$OutputPath = ''
)

`$inputJson = Get-Content -LiteralPath `$InputPath -Raw | ConvertFrom-Json
`$result = [ordered]@{
    success = [bool]`$inputJson.success
    total_hash_count = [int]`$inputJson.verify_stage.crc_catalog_hash_count
    total_pass_count = [int]`$inputJson.verify_stage.pass_count
    total_fail_count = [int]`$inputJson.verify_stage.fail_count
    total_missing_count = [int]`$inputJson.verify_stage.missing_count
    hash_results = @(`$inputJson.verify_stage.hash_results)
}

if (-not [string]::IsNullOrWhiteSpace(`$OutputPath)) {
    `$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
}

Write-Host 'fake parser complete'
if (`$result.success) {
    exit 0
}
exit 1
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
    Invoke-TestCase -Name "Runner aggregates focused CEmu assertions from structured hash results" -Body {
        $tempDir = New-TestDirectory
        $suitePath = Join-Path $tempDir "suite.json"
        $summaryPath = Join-Path $tempDir "summary.json"
        $generatedConfigPath = Join-Path $tempDir "generated.json"
        $runnerSummaryPath = Join-Path $tempDir "runner-summary.json"
        $parsedSummaryPath = Join-Path $tempDir "parsed-summary.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-runner.ps1"
        $fakeParserPath = Join-Path $tempDir "fake-parser.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $cemuPath = Join-Path $tempDir "CEmu.exe"
        $autotesterPath = Join-Path $tempDir "autotester.exe"

        New-SuiteFile -Path $suitePath
        New-FakeRunner -Path $fakeRunnerPath -Success $true
        New-FakeParser -Path $fakeParserPath
        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 16))
        Set-Content -LiteralPath $cemuPath -Value "fake gui" -Encoding ASCII
        Set-Content -LiteralPath $autotesterPath -Value "fake autotester" -Encoding ASCII

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", $autotesterPath,
            "-SuitePath", $suitePath,
            "-RunnerPath", $fakeRunnerPath,
            "-ParserPath", $fakeParserPath,
            "-OutputPath", $summaryPath,
            "-GeneratedConfigPath", $generatedConfigPath,
            "-RunnerSummaryPath", $runnerSummaryPath,
            "-ParsedSummaryPath", $parsedSummaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Focused runner should pass when all hash assertions pass."
        Assert-Contains -Needle "Overall         : PASS" -Haystack $run.output_text -Message "Console output should report PASS."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected 2 -Actual ([int]$summary.discovery.transfer_file_count) -Message "Transfer count should come from the generated config."
        Assert-Equal -Expected "CALCENG" -Actual ([string]$summary.discovery.launch_target) -Message "Launch target should come from the suite config."
        Assert-Equal -Expected 5 -Actual ([int](@($summary.assertions | Where-Object { $_.passed }).Count)) -Message "All focused assertions should pass."
    }

    Invoke-TestCase -Name "Runner fails when the boot hash fails even if other hashes pass" -Body {
        $tempDir = New-TestDirectory
        $suitePath = Join-Path $tempDir "suite.json"
        $summaryPath = Join-Path $tempDir "summary.json"
        $generatedConfigPath = Join-Path $tempDir "generated.json"
        $runnerSummaryPath = Join-Path $tempDir "runner-summary.json"
        $parsedSummaryPath = Join-Path $tempDir "parsed-summary.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-runner.ps1"
        $fakeParserPath = Join-Path $tempDir "fake-parser.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $cemuPath = Join-Path $tempDir "CEmu.exe"
        $autotesterPath = Join-Path $tempDir "autotester.exe"

        New-SuiteFile -Path $suitePath
        New-FakeRunner -Path $fakeRunnerPath -Success $false
        New-FakeParser -Path $fakeParserPath
        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 16))
        Set-Content -LiteralPath $cemuPath -Value "fake gui" -Encoding ASCII
        Set-Content -LiteralPath $autotesterPath -Value "fake autotester" -Encoding ASCII

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", $autotesterPath,
            "-SuitePath", $suitePath,
            "-RunnerPath", $fakeRunnerPath,
            "-ParserPath", $fakeParserPath,
            "-OutputPath", $summaryPath,
            "-GeneratedConfigPath", $generatedConfigPath,
            "-RunnerSummaryPath", $runnerSummaryPath,
            "-ParsedSummaryPath", $parsedSummaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Focused runner should fail when the delegated verify stage fails."
        Assert-Contains -Needle "cemu_boots_successfully" -Haystack $run.output_text -Message "Console output should surface the failed boot assertion."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary should report failure."
        Assert-True -Condition (@($summary.blockers) -contains "Assertion failed: cemu_boots_successfully") -Message "Failed boot assertion should bubble into blockers."
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
