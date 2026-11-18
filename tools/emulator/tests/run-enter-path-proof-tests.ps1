Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-enter-path-proof.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "Enter-path proof runner was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("enter-path-proof-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
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

function New-MinimalManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$DownloadsDir,
        [Parameter(Mandatory = $true)][string]$ProgramName,
        [Parameter(Mandatory = $true)][string]$AppVarName
    )

    $pipelineSupportPath = Join-Path $script:RepoRoot "tools\emulator\CalcEmuPipelineSupport.psm1"
    Import-Module -Name $pipelineSupportPath -Force

    $programBytes = [System.IO.File]::ReadAllBytes((Join-Path $DownloadsDir $ProgramName))
    $appVarBytes = [System.IO.File]::ReadAllBytes((Join-Path $DownloadsDir $AppVarName))
    $readmeBytes = [System.Text.Encoding]::UTF8.GetBytes("readme")

    $manifest = [ordered]@{
        schema_version = 1
        name = "Test manifest"
        version = "1.0.0"
        modules = @(
            [ordered]@{
                name = $ProgramName
                size_bytes = $programBytes.Length
                crc32 = (Get-CalcEmuCrc32Hex -Data $programBytes)
            }
        )
        runtime_dependencies = @(
            [ordered]@{
                name = $AppVarName
                size_bytes = $appVarBytes.Length
                crc32 = (Get-CalcEmuCrc32Hex -Data $appVarBytes)
            }
        )
        readme = [ordered]@{
            name = "README.md"
            crc32 = (Get-CalcEmuCrc32Hex -Data $readmeBytes)
        }
    }

    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Write-ReadmeFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )

    [System.IO.File]::WriteAllBytes($Path, [System.Text.Encoding]::UTF8.GetBytes("readme"))
}

function New-FakeComprehensiveRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ObservationPath
    )

    $content = @"
param(
    [string]`$RomPath,
    [string]`$AutotesterPath,
    [string]`$EmulatorBackend,
    [string]`$Mode,
    [string]`$DownloadsDir,
    [string]`$TemplatePath,
    [string]`$GeneratedPath,
    [string]`$BaselinedPath,
    [string]`$CrcCatalogJsonPath,
    [string]`$CrcCatalogMarkdownPath,
    [string]`$SummaryPath
)

`$template = Get-Content -LiteralPath `$TemplatePath -Raw | ConvertFrom-Json
`$observation = [ordered]@{
    mode = `$Mode
    emulator_backend = `$EmulatorBackend
    downloads_dir = `$DownloadsDir
    generated_path = `$GeneratedPath
    baselined_path = `$BaselinedPath
    crc_catalog_json_path = `$CrcCatalogJsonPath
    crc_catalog_markdown_path = `$CrcCatalogMarkdownPath
    hash_names = @(
        `$template.hashes.PSObject.Properties |
        Select-Object -ExpandProperty Name
    )
}
`$observation | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath '$ObservationPath' -Encoding UTF8

`$summary = [ordered]@{
    schema_version = 1
    mode = `$Mode
    success = `$true
    exit_code = 0
    verify_stage = [ordered]@{
        baselined_path = `$BaselinedPath
        hash_results = @(
            [ordered]@{ hash_name = "ep00_boot_prompt"; description = "boot"; expected_crcs = @("AAAABBBB"); observed_crc = ""; status = "pass" },
            [ordered]@{ hash_name = "ep01_empty_enter_stays_on_prompt"; description = "empty enter"; expected_crcs = @("CCCCDDDD"); observed_crc = ""; status = "pass" }
        )
        success = `$true
        pass_count = 2
        fail_count = 0
        missing_count = 0
        missing_hashes = @()
    }
}
`$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8
Write-Host 'fake enter-path comprehensive runner ok'
exit 0
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function New-FakeParser {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ObservationPath
    )

    $content = @"
param(
    [string]`$InputPath,
    [string]`$OutputPath
)

`$observation = [ordered]@{
    input_path = `$InputPath
    output_path = `$OutputPath
}
`$observation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath '$ObservationPath' -Encoding UTF8

`$parsed = [ordered]@{
    schema_version = 1
    success = `$true
    total_hash_count = 2
    total_pass_count = 2
    total_fail_count = 0
    total_missing_count = 0
    missing_hashes = @()
    hash_results = @(
        [ordered]@{ hash_name = "ep00_boot_prompt"; status = "pass" },
        [ordered]@{ hash_name = "ep01_empty_enter_stays_on_prompt"; status = "pass" }
    )
}
`$parsed | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
Write-Host 'fake parse ok'
exit 0
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
    Invoke-TestCase -Name "Enter-path proof fails fast on invalid transfer package" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $summaryPath = Join-Path $tempDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $fakeParserPath = Join-Path $tempDir "fake-parser.ps1"
        $runnerObservationPath = Join-Path $tempDir "runner-observation.json"
        $parserObservationPath = Join-Path $tempDir "parser-observation.json"
        $fakeAutotesterPath = Join-Path $tempDir "fake-autotester.exe"
        $romPath = Join-Path $tempDir "Rom.rom"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $fakeAutotesterPath -SizeBytes 16
        Write-ReadmeFile -Path (Join-Path $downloadsDir "README.md")
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $runnerObservationPath
        New-FakeParser -Path $fakeParserPath -ObservationPath $parserObservationPath
        Set-Content -LiteralPath $manifestPath -Value '{"schema_version":1,"name":"invalid","version":"1.0.0","modules":[],"runtime_dependencies":[],"readme":{"name":"README.md"}}' -Encoding UTF8

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $fakeAutotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-ResultParserPath", $fakeParserPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when manifest validation fails."
        Assert-Contains -Needle "does not contain any module entries" -Haystack $run.output_text -Message "Failure output should surface the manifest validation error."
        Assert-True -Condition (-not (Test-Path -LiteralPath $runnerObservationPath -PathType Leaf)) -Message "Delegated runner should not run when manifest validation fails."
        Assert-True -Condition (-not (Test-Path -LiteralPath $parserObservationPath -PathType Leaf)) -Message "Parser should not run when manifest validation fails."
    }

    Invoke-TestCase -Name "Enter-path proof delegates focused suite and parses verify output" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $summaryPath = Join-Path $tempDir "summary.json"
        $parsedPath = Join-Path $tempDir "parsed.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $fakeParserPath = Join-Path $tempDir "fake-parser.ps1"
        $runnerObservationPath = Join-Path $tempDir "runner-observation.json"
        $parserObservationPath = Join-Path $tempDir "parser-observation.json"
        $fakeAutotesterPath = Join-Path $tempDir "fake-autotester.exe"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $fakeAutotesterPath -SizeBytes 16
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        Write-ReadmeFile -Path (Join-Path $downloadsDir "README.md")
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $runnerObservationPath
        New-FakeParser -Path $fakeParserPath -ObservationPath $parserObservationPath

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $fakeAutotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-ResultParserPath", $fakeParserPath,
            "-Mode", "Verify",
            "-ParsedOutputPath", $parsedPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when delegated verification and parsing succeed."
        Assert-True -Condition (Test-Path -LiteralPath $runnerObservationPath -PathType Leaf) -Message "Delegated runner should be invoked."
        Assert-True -Condition (Test-Path -LiteralPath $parserObservationPath -PathType Leaf) -Message "Parser should be invoked."

        $runnerObservation = Get-Content -LiteralPath $runnerObservationPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "Verify" -Actual ([string]$runnerObservation.mode) -Message "Requested mode should be forwarded."
        Assert-Equal -Expected "Autotester" -Actual ([string]$runnerObservation.emulator_backend) -Message "Enter-path proof should pin the canonical autotester backend."
        Assert-True -Condition (@($runnerObservation.hash_names) -contains "ep05_rapid_enter_burst_stays_in_app") -Message "Focused template should include the rapid-enter checkpoint."
        Assert-True -Condition (@($runnerObservation.hash_names) -contains "ep10_long_expr_result_16") -Message "Focused template should include the long-expression result checkpoint."
        Assert-True -Condition (@($runnerObservation.hash_names) -contains "ep13_large_mul_result") -Message "Focused template should include the large-number checkpoint."
        Assert-True -Condition (@($runnerObservation.hash_names) -contains "ep18_error_quit_to_tios_home") -Message "Focused template should include the TI-OS quit checkpoint."

        $parserObservation = Get-Content -LiteralPath $parserObservationPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $parsedPath -Actual ([string]$parserObservation.output_path) -Message "Parsed output path should be forwarded to the parser."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected $true -Actual ([bool]$summary.parsed_result.success) -Message "Parsed result should be included in the summary."
        Assert-Equal -Expected 2 -Actual ([int]$summary.parsed_result.total_hash_count) -Message "Parsed result should surface the parser counts."
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
