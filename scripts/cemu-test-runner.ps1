#Requires -Version 5.1
[CmdletBinding()]
param(
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
    [string]$SuitePath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ParserPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$GeneratedConfigPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RunnerSummaryPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ParsedSummaryPath = ""
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
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$PathValue
    )

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

function Resolve-ExistingFilePath {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $resolvedPath = Resolve-CalcEmuPipelinePath -PathValue $PathValue
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        throw "$Label '$resolvedPath' does not exist."
    }

    return (Resolve-Path -LiteralPath $resolvedPath).Path
}

function Invoke-PowerShellFile {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $false)][string[]]$Arguments = @()
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $startTime = Get-Date
    $outputLines = New-Object System.Collections.Generic.List[string]
    $exitCode = 1
    $invocationError = ""

    try {
        $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $resolvedScriptPath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
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

    return [ordered]@{
        success = ([int]$exitCode -eq 0)
        exit_code = [int]$exitCode
        duration_ms = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
    }
}

function Get-HashResultMap {
    param([Parameter(Mandatory = $false)][AllowNull()][object[]]$HashResults = @())

    $map = @{}
    foreach ($row in @($HashResults)) {
        if ($null -eq $row) {
            continue
        }
        $map[[string]$row.hash_name] = $row
    }
    return $map
}

function Get-AssertionRow {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [Parameter(Mandatory = $true)][string]$Evidence,
        [Parameter(Mandatory = $true)][string[]]$Notes
    )

    return [ordered]@{
        name = $Name
        passed = $Passed
        evidence = $Evidence
        notes = $Notes
    }
}

$repoRoot = Resolve-RepoRoot
$resolvedRunnerPath = if ([string]::IsNullOrWhiteSpace($RunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $RunnerPath
}
$resolvedParserPath = if ([string]::IsNullOrWhiteSpace($ParserPath)) {
    Join-Path $repoRoot "tools\emulator\parse-autotest-result.ps1"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $ParserPath
}
$resolvedSuitePath = if ([string]::IsNullOrWhiteSpace($SuitePath)) {
    Join-Path $repoRoot "tests\emulator\autotest.cemu-e2e.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $SuitePath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\cemu-test-runner.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $OutputPath
}
$resolvedGeneratedConfigPath = if ([string]::IsNullOrWhiteSpace($GeneratedConfigPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\autotest.cemu-e2e.generated.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $GeneratedConfigPath
}
$resolvedCrcCatalogJsonPath = Join-Path $repoRoot "tests\emulator\outputs\autotest.cemu-e2e.crc-catalog.json"
$resolvedCrcCatalogMarkdownPath = Join-Path $repoRoot "tests\emulator\outputs\autotest.cemu-e2e.crc-catalog.md"
$resolvedRunnerSummaryPath = if ([string]::IsNullOrWhiteSpace($RunnerSummaryPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\cemu-test-runner.autotester.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $RunnerSummaryPath
}
$resolvedParsedSummaryPath = if ([string]::IsNullOrWhiteSpace($ParsedSummaryPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\cemu-test-runner.autotester.parsed.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $ParsedSummaryPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "scripts/cemu-test-runner.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    options = [ordered]@{
        rom_path = ""
        cemu_path = ""
        autotester_path = ""
        suite_path = $resolvedSuitePath
        generated_config_path = $resolvedGeneratedConfigPath
        runner_summary_path = $resolvedRunnerSummaryPath
        parsed_summary_path = $resolvedParsedSummaryPath
    }
    discovery = [ordered]@{
        launch_target = ""
        transfer_file_count = 0
        transfer_file_names = @()
        suite_hash_count = 0
        archive_load_proof_mode = "inferred_from_autotester_transfer_plus_successful_real_rom_launch"
        archive_load_limitations = @(
            "CEmu AutoTester transfers the .8xp/.8xv files and can launch the target program, but it does not expose a supported archive inventory API.",
            "This runner treats archive load as proven when the generated AutoTester config contains the transferred files and the real-ROM launch hashes pass."
        )
    }
    stages = @()
    assertions = @()
    blockers = @()
    notes = @(
        "This runner uses the real-ROM AutoTester lane, not the eZ80 scaffold, for CPU-level execution of the .8xp artifacts.",
        "The focused suite covers boot, launch, prompt return, multiple arithmetic results, and divide-by-zero behavior through real TI-OS screen CRC checks."
    )
}

try {
    if (-not (Test-Path -LiteralPath $resolvedRunnerPath -PathType Leaf)) {
        throw "Comprehensive AutoTester runner was not found at '$resolvedRunnerPath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedParserPath -PathType Leaf)) {
        throw "AutoTester parser was not found at '$resolvedParserPath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedSuitePath -PathType Leaf)) {
        throw "Focused CEmu suite was not found at '$resolvedSuitePath'."
    }

    $romResolution = Resolve-CalcEmuRomPath -RomPath $RomPath
    $guiResolution = if (-not [string]::IsNullOrWhiteSpace($CEmuPath)) {
        [ordered]@{
            path = Resolve-ExistingFilePath -PathValue $CEmuPath -Label "CEmu path"
            source = "explicit"
        }
    }
    else {
        Resolve-CalcEmuGuiPath -CEmuPath $CEmuPath
    }
    $autotesterResolution = if (-not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
        [ordered]@{
            path = Resolve-ExistingFilePath -PathValue $AutotesterPath -Label "Autotester path"
            source = "explicit"
        }
    }
    else {
        Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
    }
    $summary.options.rom_path = [string]$romResolution.path
    $summary.options.cemu_path = [string]$guiResolution.path
    $summary.options.autotester_path = [string]$autotesterResolution.path

    $suiteConfig = Get-Content -LiteralPath $resolvedSuitePath -Raw | ConvertFrom-Json
    $summary.discovery.launch_target = [string]$suiteConfig.target.name
    $summary.discovery.suite_hash_count = @($suiteConfig.hashes.PSObject.Properties).Count

    $verifyStage = Invoke-PowerShellFile -ScriptPath $resolvedRunnerPath -Arguments @(
        "-RomPath", [string]$romResolution.path,
        "-AutotesterPath", [string]$autotesterResolution.path,
        "-EmulatorBackend", "Autotester",
        "-Mode", "Verify",
        "-TemplatePath", $resolvedSuitePath,
        "-BaselinedPath", $resolvedSuitePath,
        "-GeneratedPath", $resolvedGeneratedConfigPath,
        "-CrcCatalogJsonPath", $resolvedCrcCatalogJsonPath,
        "-CrcCatalogMarkdownPath", $resolvedCrcCatalogMarkdownPath,
        "-SummaryPath", $resolvedRunnerSummaryPath
    )
    $stageVerifyRow = [ordered]@{
        name = "real_rom_verify"
        success = [bool]$verifyStage.success
        exit_code = [int]$verifyStage.exit_code
        duration_ms = [int]$verifyStage.duration_ms
        output_lines = [string[]]@($verifyStage.output_lines)
        invocation_error = [string]$verifyStage.invocation_error
        summary_path = $resolvedRunnerSummaryPath
        summary_json = $null
    }
    if (Test-Path -LiteralPath $resolvedRunnerSummaryPath -PathType Leaf) {
        $stageVerifyRow.summary_json = Get-Content -LiteralPath $resolvedRunnerSummaryPath -Raw | ConvertFrom-Json
    }
    $summary.stages += $stageVerifyRow

    $parserStage = Invoke-PowerShellFile -ScriptPath $resolvedParserPath -Arguments @(
        "-InputPath", $resolvedRunnerSummaryPath,
        "-OutputPath", $resolvedParsedSummaryPath
    )
    $stageParserRow = [ordered]@{
        name = "result_parse"
        success = [bool]$parserStage.success
        exit_code = [int]$parserStage.exit_code
        duration_ms = [int]$parserStage.duration_ms
        output_lines = [string[]]@($parserStage.output_lines)
        invocation_error = [string]$parserStage.invocation_error
        summary_path = $resolvedParsedSummaryPath
        summary_json = $null
    }
    if (Test-Path -LiteralPath $resolvedParsedSummaryPath -PathType Leaf) {
        $stageParserRow.summary_json = Get-Content -LiteralPath $resolvedParsedSummaryPath -Raw | ConvertFrom-Json
    }
    $summary.stages += $stageParserRow

    if (-not (Test-Path -LiteralPath $resolvedGeneratedConfigPath -PathType Leaf)) {
        throw "Generated AutoTester config '$resolvedGeneratedConfigPath' was not written by the verify stage."
    }

    $generatedConfig = Get-Content -LiteralPath $resolvedGeneratedConfigPath -Raw | ConvertFrom-Json
    $summary.discovery.transfer_file_count = @($generatedConfig.transfer_files).Count
    $summary.discovery.transfer_file_names = @($generatedConfig.transfer_files | ForEach-Object { [System.IO.Path]::GetFileName([string]$_) })

    if ($null -eq $stageParserRow.summary_json) {
        throw "Parsed AutoTester summary was not available."
    }

    $hashMap = Get-HashResultMap -HashResults @($stageParserRow.summary_json.hash_results)
    $requiredBasicHashes = @(
        "addition_result",
        "multiplication_result",
        "negative_result",
        "decimal_result",
        "divide_by_zero_error"
    )

    $bootPassed = ($hashMap.ContainsKey("boot_home") -and [string]$hashMap["boot_home"].status -eq "pass")
    $launchPassed = $bootPassed
    $promptRoundTripPassed = ($hashMap.ContainsKey("prompt_after_addition") -and [string]$hashMap["prompt_after_addition"].status -eq "pass")
    $basicPassed = $true
    foreach ($hashName in $requiredBasicHashes) {
        if (-not $hashMap.ContainsKey($hashName) -or [string]$hashMap[$hashName].status -ne "pass") {
            $basicPassed = $false
            break
        }
    }

    $transferPassed = ($summary.discovery.transfer_file_count -gt 0 -and $launchPassed)
    $transferEvidence = ("AutoTester generated config transferred {0} file(s): {1}" -f
        $summary.discovery.transfer_file_count,
        ($summary.discovery.transfer_file_names -join ", "))

    $summary.assertions = @(
        Get-AssertionRow -Name "cemu_boots_successfully" -Passed $bootPassed -Evidence "Hash 'boot_home' is captured after action|launch on the real ROM." -Notes @(
            "This is the first post-launch prompt screen for CALCENG.",
            "A failing boot would prevent all downstream hashes."
        )
        Get-AssertionRow -Name "xp_files_transfer_into_emulator_archive" -Passed $transferPassed -Evidence $transferEvidence -Notes @(
            "AutoTester does not expose archive enumeration, so archive residency is inferred from transfer_files plus a successful launch hash.",
            "The generated config is persisted for inspection."
        )
        Get-AssertionRow -Name "calcengine_launches" -Passed $launchPassed -Evidence ("Target '{0}' launched and reached the boot prompt hash." -f $summary.discovery.launch_target) -Notes @(
            "The suite uses action|launch against the transferred target.",
            "If the target were not launchable, boot_home would not pass."
        )
        Get-AssertionRow -Name "basic_calculations_match_expected_screen_output" -Passed $basicPassed -Evidence ("Required passing hashes: {0}" -f ($requiredBasicHashes -join ", ")) -Notes @(
            "The focused suite verifies 2+3, 6*7, -5+10, 3.14*2, and 1/0.",
            "Each assertion is a real TI-OS screen CRC comparison."
        )
        Get-AssertionRow -Name "prompt_round_trip_survives_real_enter_path" -Passed $promptRoundTripPassed -Evidence "Hash 'prompt_after_addition' passes after dismissing the addition result." -Notes @(
            "This checks that CalcEngine returns cleanly to the prompt instead of hanging on the result screen."
        )
    )

    $assertionFailures = @($summary.assertions | Where-Object { -not $_.passed })
    foreach ($stage in @($summary.stages | Where-Object { -not $_.success })) {
        if (-not [string]::IsNullOrWhiteSpace([string]$stage.invocation_error)) {
            $summary.blockers += [string]$stage.invocation_error
        }
    }
    foreach ($failedAssertion in $assertionFailures) {
        $summary.blockers += ("Assertion failed: {0}" -f [string]$failedAssertion.name)
    }
    $summary.blockers = @($summary.blockers | Sort-Object -Unique)

    $summary.success = (@($summary.stages | Where-Object { -not $_.success }).Count -eq 0) -and ($assertionFailures.Count -eq 0)
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
}
catch {
    $summary.success = $false
    $summary.exit_code = 1
    $summary.blockers += $_.Exception.Message
    $summary.blockers = @($summary.blockers | Sort-Object -Unique)
}
finally {
    $summary.duration_seconds = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 16)
}

Write-Host "CalcEngine CEmu Test Runner"
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
Write-Host ("ROM             : {0}" -f [string]$summary.options.rom_path)
Write-Host ("CEmu            : {0}" -f [string]$summary.options.cemu_path)
Write-Host ("Autotester      : {0}" -f [string]$summary.options.autotester_path)
Write-Host ("Transfer files  : {0}" -f [int]$summary.discovery.transfer_file_count)
Write-Host ("Overall         : {0}" -f $(if ($summary.success) { "PASS" } else { "FAIL" }))
foreach ($assertion in @($summary.assertions)) {
    Write-Host ("{0,-42}: {1}" -f [string]$assertion.name, $(if ($assertion.passed) { "PASS" } else { "FAIL" }))
}
if (@($summary.blockers).Count -gt 0) {
    Write-Host ""
    Write-Host "Blockers:"
    foreach ($blocker in @($summary.blockers)) {
        Write-Host ("  - {0}" -f [string]$blocker)
    }
}

exit ([int]$summary.exit_code)
