Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-comprehensive-autotest.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "Comprehensive autotest runner was not found at '$script:RunnerPath'."
    exit 2
}

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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("comprehensive-autotest-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
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

function New-MinimalTemplate {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $template = [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "CALCENG"
            isASM = $true
        }
        sequence = @(
            "action|launch",
            "hash|s00_boot_home",
            "hash|s01_add_2_3"
        )
        hashes = [ordered]@{
            s00_boot_home = [ordered]@{
                description = "Boot screen"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("00000000")
                timeout = 5000
            }
            s01_add_2_3 = [ordered]@{
                description = "2+3 result"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("00000000")
                timeout = 5000
            }
        }
    }

    $template | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-MinimalHeadlessTemplate {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $template = [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "CALCENG"
            isASM = $true
        }
        sequence = @(
            "action|launch",
            "hash|s00_boot_home",
            "key|1",
            "delay|200",
            "hash|s01_after_key_1"
        )
        hashes = [ordered]@{
            s00_boot_home = [ordered]@{
                description = "Boot screen"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("00000000")
                timeout = 5000
            }
            s01_after_key_1 = [ordered]@{
                description = "State after pressing 1"
                start = "vram_start"
                size = "vram_16_size"
                expected_CRCs = @("00000000")
                timeout = 5000
            }
        }
    }

    $template | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-FakeAutotester {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = @(
        "@echo off",
        "setlocal",
        "set ""CONFIG=%~1""",
        "findstr /C:""00000000"" ""%CONFIG%"" >nul",
        "if %ERRORLEVEL%==0 (",
        "  echo ^I[Test failed!^] Hash #s00_boot_home ^(""Boot screen""^) did not match the expected CRC 0 ^(got 11111111^).",
        "  echo ^I[Test failed!^] Hash #s01_add_2_3 ^(""2+3 result""^) did not match the expected CRC 0 ^(got 22222222^).",
        "  exit /b 1",
        ")",
        "echo %CONFIG% loaded and verified. 2 unique tests found.",
        "echo [Autotest passed] Out of 2 tests attempted, 2 passed, and 0 failed.",
        "exit /b 0"
    )

    Set-Content -LiteralPath $Path -Value $content -Encoding ASCII
}

function New-FakeAutotesterSummaryFail {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = @(
        "@echo off",
        "setlocal",
        "set ""CONFIG=%~1""",
        "findstr /C:""00000000"" ""%CONFIG%"" >nul",
        "if %ERRORLEVEL%==0 (",
        "  echo ^I[Test failed!^] Hash #s00_boot_home ^(""Boot screen""^) did not match the expected CRC 0 ^(got 11111111^).",
        "  echo ^I[Test failed!^] Hash #s01_add_2_3 ^(""2+3 result""^) did not match the expected CRC 0 ^(got 22222222^).",
        "  exit /b 1",
        ")",
        "echo %CONFIG% loaded and verified. 2 unique tests found.",
        "echo [Autotest failed] Out of 2 tests attempted, 1 passed, and 1 failed.",
        "exit /b 1"
    )

    Set-Content -LiteralPath $Path -Value $content -Encoding ASCII
}

function New-FakeCEmuHeadless {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = @(
        "@echo off",
        "setlocal",
        "set ""SCRIPT=%~1""",
        "findstr /C:""\""key\"":\""1\"""" ""%SCRIPT%"" >nul",
        "if %ERRORLEVEL%==0 (",
        "  echo {""ok"":true,""cmd"":""load_rom""}",
        "  echo {""ok"":true,""cmd"":""load_app""}",
        "  echo {""ok"":true,""cmd"":""inject_key""}",
        "  echo {""ok"":true,""cmd"":""get_screen"",""format"":""crc"",""crc"":""22222222"",""backend"":""autotester_wrapper""}",
        "  echo {""ok"":true,""cmd"":""exit""}",
        "  exit /b 0",
        ")",
        "echo {""ok"":true,""cmd"":""load_rom""}",
        "echo {""ok"":true,""cmd"":""load_app""}",
        "echo {""ok"":true,""cmd"":""get_screen"",""format"":""crc"",""crc"":""11111111"",""backend"":""autotester_wrapper""}",
        "echo {""ok"":true,""cmd"":""exit""}",
        "exit /b 0"
    )

    Set-Content -LiteralPath $Path -Value $content -Encoding ASCII
}

function Invoke-ComprehensiveRunner {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdditionalArgs
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required for comprehensive autotest runner tests."
    }

    $powershellPath = $powershellCommand.Source
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
    $invocationError = ""
    try {
        $captured = & $powershellPath @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    $outputLines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($captured)) {
        if ($null -ne $entry) {
            $outputLines.Add([string]$entry)
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($invocationError)) {
        $outputLines.Add("runner invocation error: $invocationError")
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = ($outputLines.ToArray() -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Baseline mode writes baselined suite and CRC catalog artifacts" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $downloadsDir = Join-Path $tempDir "downloads"
        $templatePath = Join-Path $tempDir "template.json"
        $generatedPath = Join-Path $tempDir "generated.json"
        $baselinedPath = Join-Path $tempDir "baselined.json"
        $catalogJsonPath = Join-Path $tempDir "crc-catalog.json"
        $catalogMdPath = Join-Path $tempDir "crc-catalog.md"
        $summaryPath = Join-Path $tempDir "baseline-summary.json"
        $autotesterPath = Join-Path $tempDir "fake-autotester.cmd"
        $programPath = Join-Path $downloadsDir "CALCENG.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-MinimalTemplate -Path $templatePath
        New-FakeAutotester -Path $autotesterPath

        $run = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Baseline",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath,
            "-CrcCatalogJsonPath", $catalogJsonPath,
            "-CrcCatalogMarkdownPath", $catalogMdPath,
            "-SummaryPath", $summaryPath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "Baseline mode should succeed when all fake CRCs are captured.`nOutput:`n$($run.output_text)"
        }

        Assert-True -Condition (Test-Path -LiteralPath $baselinedPath -PathType Leaf) -Message "Baselined config should exist."
        Assert-True -Condition (Test-Path -LiteralPath $catalogJsonPath -PathType Leaf) -Message "CRC catalog JSON should exist."
        Assert-True -Condition (Test-Path -LiteralPath $catalogMdPath -PathType Leaf) -Message "CRC catalog markdown should exist."
        Assert-True -Condition (Test-Path -LiteralPath $summaryPath -PathType Leaf) -Message "Summary JSON should exist."

        $baselined = Get-Content -LiteralPath $baselinedPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "11111111" -Actual ([string]$baselined.hashes.s00_boot_home.expected_CRCs[0]) -Message "Boot hash should be captured into the baselined file."
        Assert-Equal -Expected "22222222" -Actual ([string]$baselined.hashes.s01_add_2_3.expected_CRCs[0]) -Message "Addition hash should be captured into the baselined file."

        $catalog = Get-Content -LiteralPath $catalogJsonPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected 2 -Actual ([int]$catalog.hash_count) -Message "CRC catalog should report both hashes."
        Assert-Equal -Expected "s00_boot_home" -Actual ([string]$catalog.entries[0].hash_name) -Message "Catalog should keep hash order."
        Assert-Equal -Expected "S00" -Actual ([string]$catalog.entries[0].section) -Message "Catalog should infer the S00 section."

        $catalogMd = Get-Content -LiteralPath $catalogMdPath -Raw
        Assert-Contains -Needle '`s00_boot_home`' -Haystack $catalogMd -Message "Markdown catalog should include the boot hash."
        Assert-Contains -Needle "11111111" -Haystack $catalogMd -Message "Markdown catalog should include the captured CRC."
    }

    Invoke-TestCase -Name "Verify mode succeeds from baselined suite and refreshes catalog summary" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $downloadsDir = Join-Path $tempDir "downloads"
        $templatePath = Join-Path $tempDir "template.json"
        $generatedPath = Join-Path $tempDir "generated.json"
        $baselinedPath = Join-Path $tempDir "baselined.json"
        $catalogJsonPath = Join-Path $tempDir "crc-catalog.json"
        $catalogMdPath = Join-Path $tempDir "crc-catalog.md"
        $summaryPath = Join-Path $tempDir "verify-summary.json"
        $autotesterPath = Join-Path $tempDir "fake-autotester.cmd"
        $programPath = Join-Path $downloadsDir "CALCENG.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-MinimalTemplate -Path $templatePath
        New-FakeAutotester -Path $autotesterPath

        $baselineRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Baseline",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath,
            "-CrcCatalogJsonPath", $catalogJsonPath,
            "-CrcCatalogMarkdownPath", $catalogMdPath
        )
        if ([int]$baselineRun.exit_code -ne 0) {
            throw "Baseline priming should succeed before verify.`nOutput:`n$($baselineRun.output_text)"
        }

        $verifyRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Verify",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath,
            "-CrcCatalogJsonPath", $catalogJsonPath,
            "-CrcCatalogMarkdownPath", $catalogMdPath,
            "-SummaryPath", $summaryPath
        )

        if ([int]$verifyRun.exit_code -ne 0) {
            throw "Verify mode should pass once the baselined CRCs exist.`nOutput:`n$($verifyRun.output_text)"
        }

        Assert-Contains -Needle "RESULT: SUCCESS" -Haystack $verifyRun.output_text -Message "Verify output should report success."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Verify summary success should be true."
        Assert-Equal -Expected 2 -Actual ([int]$summary.verify_stage.crc_catalog_hash_count) -Message "Verify summary should report catalog hash count."
        Assert-Equal -Expected 2 -Actual ([int]$summary.verify_stage.pass_count) -Message "Verify summary should record a pass count for every hash."
        Assert-Equal -Expected 0 -Actual ([int]$summary.verify_stage.fail_count) -Message "Verify summary should record zero failing hashes."
        Assert-Equal -Expected 0 -Actual ([int]$summary.verify_stage.missing_count) -Message "Verify summary should record zero missing hashes."
        Assert-Equal -Expected "pass" -Actual ([string]$summary.verify_stage.hash_results[0].status) -Message "Verify hash results should mark the boot screen as pass."
        Assert-Equal -Expected "reported_pass_without_observed_crc" -Actual ([string]$summary.verify_stage.hash_results[0].status_reason) -Message "Autotester verify should admit that passing hashes were reported without raw CRCs."
        Assert-Equal -Expected "not_reported" -Actual ([string]$summary.verify_stage.hash_results[0].observed_crc_source) -Message "Autotester verify should mark passing CRC provenance as not reported."

        $catalog = Get-Content -LiteralPath $catalogJsonPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "22222222" -Actual ([string]$catalog.entries[1].expected_crcs[0]) -Message "Verify should preserve the captured addition CRC in the catalog."
    }

    Invoke-TestCase -Name "Verify mode fails when autotester reports failures without per-hash mismatch lines" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $downloadsDir = Join-Path $tempDir "downloads"
        $templatePath = Join-Path $tempDir "template.json"
        $generatedPath = Join-Path $tempDir "generated.json"
        $baselinedPath = Join-Path $tempDir "baselined.json"
        $summaryPath = Join-Path $tempDir "verify-summary.json"
        $catalogJsonPath = Join-Path $tempDir "crc-catalog.json"
        $catalogMdPath = Join-Path $tempDir "crc-catalog.md"
        $baselineAutotesterPath = Join-Path $tempDir "fake-autotester-baseline.cmd"
        $verifyAutotesterPath = Join-Path $tempDir "fake-autotester-summary-fail.cmd"
        $programPath = Join-Path $downloadsDir "CALCENG.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-MinimalTemplate -Path $templatePath
        New-FakeAutotester -Path $baselineAutotesterPath
        New-FakeAutotesterSummaryFail -Path $verifyAutotesterPath

        $baselineRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $baselineAutotesterPath,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Baseline",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath,
            "-CrcCatalogJsonPath", $catalogJsonPath,
            "-CrcCatalogMarkdownPath", $catalogMdPath
        )
        if ([int]$baselineRun.exit_code -ne 0) {
            throw "Baseline priming should succeed before failure-path verify.`nOutput:`n$($baselineRun.output_text)"
        }

        $verifyRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $verifyAutotesterPath,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Verify",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath,
            "-CrcCatalogJsonPath", $catalogJsonPath,
            "-CrcCatalogMarkdownPath", $catalogMdPath,
            "-SummaryPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$verifyRun.exit_code) -Message "Verify should fail when the autotester summary reports failed hashes."
        Assert-Contains -Needle "RESULT: FAILURE" -Haystack $verifyRun.output_text -Message "Verify output should report failure."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Verify summary success should be false."
        Assert-Equal -Expected 1 -Actual ([int]$summary.verify_stage.fail_count) -Message "Verify summary should record one failed hash."
        Assert-Equal -Expected 0 -Actual ([int]$summary.verify_stage.missing_count) -Message "Verify summary should not mark reported failures as missing."
        Assert-Equal -Expected "reported_failure_without_hash_line" -Actual ([string]$summary.verify_stage.hash_results[0].status_reason) -Message "The inferred failure should be called out explicitly."
    }

    Invoke-TestCase -Name "Verify mode auto-discovers autotester from CEMU_AUTOTESTER_PATH" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $downloadsDir = Join-Path $tempDir "downloads"
        $templatePath = Join-Path $tempDir "template.json"
        $generatedPath = Join-Path $tempDir "generated.json"
        $baselinedPath = Join-Path $tempDir "baselined.json"
        $summaryPath = Join-Path $tempDir "verify-summary.json"
        $autotesterPath = Join-Path $tempDir "fake-autotester.cmd"
        $programPath = Join-Path $downloadsDir "CALCENG.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-MinimalTemplate -Path $templatePath
        New-FakeAutotester -Path $autotesterPath

        $previousAutotesterPath = $env:CEMU_AUTOTESTER_PATH
        try {
            $env:CEMU_AUTOTESTER_PATH = $autotesterPath

            $baselineRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
                "-RomPath", $romPath,
                "-EmulatorBackend", "Autotester",
                "-Mode", "Baseline",
                "-DownloadsDir", $downloadsDir,
                "-TemplatePath", $templatePath,
                "-GeneratedPath", $generatedPath,
                "-BaselinedPath", $baselinedPath
            )
            if ([int]$baselineRun.exit_code -ne 0) {
                throw "Baseline priming should succeed before env-driven verify.`nOutput:`n$($baselineRun.output_text)"
            }

            $verifyRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
                "-RomPath", $romPath,
                "-EmulatorBackend", "Autotester",
                "-Mode", "Verify",
                "-DownloadsDir", $downloadsDir,
                "-TemplatePath", $templatePath,
                "-GeneratedPath", $generatedPath,
                "-BaselinedPath", $baselinedPath,
                "-SummaryPath", $summaryPath
            )

            Assert-Equal -Expected 0 -Actual ([int]$verifyRun.exit_code) -Message "Verify should succeed when autotester is resolved from CEMU_AUTOTESTER_PATH."
            Assert-Contains -Needle "Source     : env:CEMU_AUTOTESTER_PATH" -Haystack $verifyRun.output_text -Message "Runner output should log the resolved autotester source."

            $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
            Assert-Equal -Expected "env:CEMU_AUTOTESTER_PATH" -Actual ([string]$summary.autotester_source) -Message "Summary should preserve the autotester discovery source."
        }
        finally {
            $env:CEMU_AUTOTESTER_PATH = $previousAutotesterPath
        }
    }

    Invoke-TestCase -Name "CEmuHeadless backend baselines and verifies through wrapper replay" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $downloadsDir = Join-Path $tempDir "downloads"
        $templatePath = Join-Path $tempDir "headless-template.json"
        $generatedPath = Join-Path $tempDir "generated.json"
        $baselinedPath = Join-Path $tempDir "baselined.json"
        $summaryPath = Join-Path $tempDir "headless-summary.json"
        $autotesterPath = Join-Path $tempDir "fake-autotester.cmd"
        $headlessPath = Join-Path $tempDir "fake-cemu-headless.cmd"
        $programPath = Join-Path $downloadsDir "CALCENG.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-MinimalHeadlessTemplate -Path $templatePath
        New-FakeAutotester -Path $autotesterPath
        New-FakeCEmuHeadless -Path $headlessPath

        $baselineRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-CEmuHeadlessPath", $headlessPath,
            "-EmulatorBackend", "CEmuHeadless",
            "-Mode", "Baseline",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath
        )
        if ([int]$baselineRun.exit_code -ne 0) {
            throw "Headless baseline should succeed.`nOutput:`n$($baselineRun.output_text)"
        }

        $baselined = Get-Content -LiteralPath $baselinedPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "11111111" -Actual ([string]$baselined.hashes.s00_boot_home.expected_CRCs[0]) -Message "Headless baseline should capture the first CRC."
        Assert-Equal -Expected "22222222" -Actual ([string]$baselined.hashes.s01_after_key_1.expected_CRCs[0]) -Message "Headless baseline should capture the replayed-key CRC."

        $verifyRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-CEmuHeadlessPath", $headlessPath,
            "-EmulatorBackend", "CEmuHeadless",
            "-Mode", "Verify",
            "-DownloadsDir", $downloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $generatedPath,
            "-BaselinedPath", $baselinedPath,
            "-SummaryPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$verifyRun.exit_code) -Message "Headless verify should succeed once the wrapper-backed CRCs are baselined."
        Assert-Contains -Needle "Backend    : CEmuHeadless" -Haystack $verifyRun.output_text -Message "Runner output should report the headless backend."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "CEmuHeadless" -Actual ([string]$summary.emulator_backend) -Message "Summary should record the selected backend."
        Assert-Equal -Expected "CEmuHeadless" -Actual ([string]$summary.verify_stage.emulator_backend) -Message "Verify stage should record the selected backend."
        Assert-Equal -Expected 2 -Actual ([int]$summary.verify_stage.pass_count) -Message "Headless verify should pass both hashes."
        Assert-Equal -Expected 0 -Actual ([int]$summary.verify_stage.fail_count) -Message "Headless verify should report zero hash failures."
        Assert-Equal -Expected "11111111" -Actual ([string]$summary.verify_stage.hash_results[0].observed_crc) -Message "Headless verify should preserve the raw observed CRC for passing hashes."
        Assert-Equal -Expected "reported_crc" -Actual ([string]$summary.verify_stage.hash_results[0].observed_crc_source) -Message "Headless verify should label passing CRC provenance as reported."
        Assert-Equal -Expected "reported_pass_with_observed_crc" -Actual ([string]$summary.verify_stage.hash_results[0].status_reason) -Message "Headless verify should call out direct pass CRC evidence."
    }

    Invoke-TestCase -Name "Auto backend prefers cemu-headless when a wrapper path is available" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $downloadsDir = Join-Path $tempDir "downloads"
        $templatePath = Join-Path $tempDir "headless-template.json"
        $generatedPath = Join-Path $tempDir "generated.json"
        $baselinedPath = Join-Path $tempDir "baselined.json"
        $summaryPath = Join-Path $tempDir "auto-headless-summary.json"
        $autotesterPath = Join-Path $tempDir "fake-autotester.cmd"
        $headlessPath = Join-Path $tempDir "fake-cemu-headless.cmd"
        $programPath = Join-Path $downloadsDir "CALCENG.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-MinimalHeadlessTemplate -Path $templatePath
        New-FakeAutotester -Path $autotesterPath
        New-FakeCEmuHeadless -Path $headlessPath

        $previousAutotesterPath = $env:CEMU_AUTOTESTER_PATH
        $previousHeadlessPath = $env:CEMU_HEADLESS_PATH
        try {
            $env:CEMU_AUTOTESTER_PATH = $autotesterPath
            $env:CEMU_HEADLESS_PATH = $headlessPath

            $baselineRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
                "-RomPath", $romPath,
                "-Mode", "Baseline",
                "-DownloadsDir", $downloadsDir,
                "-TemplatePath", $templatePath,
                "-GeneratedPath", $generatedPath,
                "-BaselinedPath", $baselinedPath
            )
            if ([int]$baselineRun.exit_code -ne 0) {
                throw "Auto backend baseline should succeed via cemu-headless discovery.`nOutput:`n$($baselineRun.output_text)"
            }
            Assert-Contains -Needle "Backend    : CEmuHeadless" -Haystack $baselineRun.output_text -Message "Auto baseline should announce the headless backend."
            Assert-Contains -Needle "HeadlessSrc: env:CEMU_HEADLESS_PATH" -Haystack $baselineRun.output_text -Message "Auto baseline should record the discovered headless source."

            $verifyRun = Invoke-ComprehensiveRunner -AdditionalArgs @(
                "-RomPath", $romPath,
                "-Mode", "Verify",
                "-DownloadsDir", $downloadsDir,
                "-TemplatePath", $templatePath,
                "-GeneratedPath", $generatedPath,
                "-BaselinedPath", $baselinedPath,
                "-SummaryPath", $summaryPath
            )

            Assert-Equal -Expected 0 -Actual ([int]$verifyRun.exit_code) -Message "Auto backend verify should succeed once headless baseline exists."
            Assert-Contains -Needle "Backend    : CEmuHeadless" -Haystack $verifyRun.output_text -Message "Auto verify should keep the headless backend."
            Assert-Contains -Needle "HeadlessSrc: env:CEMU_HEADLESS_PATH" -Haystack $verifyRun.output_text -Message "Auto verify should preserve the discovered headless source."

            $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
            Assert-Equal -Expected "CEmuHeadless" -Actual ([string]$summary.emulator_backend) -Message "Auto summary should record the selected headless backend."
            Assert-Equal -Expected "env:CEMU_HEADLESS_PATH" -Actual ([string]$summary.cemu_headless_source) -Message "Auto summary should record the headless discovery source."
            Assert-Equal -Expected "CEmuHeadless" -Actual ([string]$summary.verify_stage.emulator_backend) -Message "Auto verify stage should record the headless backend."
        }
        finally {
            $env:CEMU_AUTOTESTER_PATH = $previousAutotesterPath
            $env:CEMU_HEADLESS_PATH = $previousHeadlessPath
        }
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
