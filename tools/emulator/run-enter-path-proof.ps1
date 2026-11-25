[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "deploy/hardware-ready",

    [Parameter(Mandatory = $false)]
    [ValidateSet("Baseline", "Verify", "Both", "Run")]
    [string]$Mode = "Run",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ManifestPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ComprehensiveRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ResultParserPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$TemplatePath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$GeneratedPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BaselinedPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CrcCatalogJsonPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CrcCatalogMarkdownPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ParsedOutputPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Invoke-PowerShellFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $false)]
        [string[]]$Arguments = @()
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $outputLines = New-Object System.Collections.Generic.List[string]
    $exitCode = 1
    $invocationError = ""
    $startTime = Get-Date

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

$repoRoot = Resolve-CalcEmuPipelineRepoRoot
$resolvedTemplatePath = if ([string]::IsNullOrWhiteSpace($TemplatePath)) {
    Join-Path $repoRoot "tests\\emulator\\autotest.enter-path-proof.json"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $TemplatePath
}
$resolvedGeneratedPath = if ([string]::IsNullOrWhiteSpace($GeneratedPath)) {
    Join-Path $repoRoot "tests\\emulator\\autotest.enter-path-proof.generated.json"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $GeneratedPath
}
$resolvedBaselinedPath = if ([string]::IsNullOrWhiteSpace($BaselinedPath)) {
    Join-Path $repoRoot "tests\\emulator\\autotest.enter-path-proof.baselined.json"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $BaselinedPath
}
$resolvedCrcCatalogJsonPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogJsonPath)) {
    Join-Path $repoRoot "tests\\emulator\\autotest.enter-path-proof.crc-catalog.json"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $CrcCatalogJsonPath
}
$resolvedCrcCatalogMarkdownPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogMarkdownPath)) {
    Join-Path $repoRoot "tests\\emulator\\autotest.enter-path-proof.crc-catalog.md"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $CrcCatalogMarkdownPath
}
$resolvedComprehensiveRunnerPath = if ([string]::IsNullOrWhiteSpace($ComprehensiveRunnerPath)) {
    Join-Path $repoRoot "tools\\emulator\\run-comprehensive-autotest.ps1"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $ComprehensiveRunnerPath
}
$resolvedResultParserPath = if ([string]::IsNullOrWhiteSpace($ResultParserPath)) {
    Join-Path $repoRoot "tools\\emulator\\parse-autotest-result.ps1"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $ResultParserPath
}
$resolvedParsedOutputPath = if ([string]::IsNullOrWhiteSpace($ParsedOutputPath)) {
    Join-Path $repoRoot "tests\\emulator\\outputs\\enter-path-proof.parsed.json"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $ParsedOutputPath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\\emulator\\outputs\\enter-path-proof.summary.json"
}
else {
    Resolve-CalcEmuPipelinePath -PathValue $OutputPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "run-enter-path-proof.ps1"
    generated_at = (Get-Date -Format "o")
    mode = $Mode
    success = $false
    exit_code = 1
    template_path = $resolvedTemplatePath
    generated_path = $resolvedGeneratedPath
    baselined_path = $resolvedBaselinedPath
    crc_catalog_json_path = $resolvedCrcCatalogJsonPath
    crc_catalog_markdown_path = $resolvedCrcCatalogMarkdownPath
    parsed_output_path = $resolvedParsedOutputPath
    package_validation = $null
    runner_summary = $null
    parsed_result = $null
    blockers = @()
}

try {
    if (-not (Test-Path -LiteralPath $resolvedTemplatePath -PathType Leaf)) {
        throw "Enter-path proof template was not found at '$resolvedTemplatePath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedComprehensiveRunnerPath -PathType Leaf)) {
        throw "Comprehensive autotest runner was not found at '$resolvedComprehensiveRunnerPath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedResultParserPath -PathType Leaf)) {
        throw "Autotest result parser was not found at '$resolvedResultParserPath'."
    }

    $resolvedRom = Resolve-CalcEmuRomPath -RomPath $RomPath
    $resolvedAutotester = Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
    $resolvedDownloadsDir = Resolve-CalcEmuPipelinePath -PathValue $DownloadsDir

    $manifestValidation = Test-CalcEmuTransferManifest `
        -DirectoryPath $resolvedDownloadsDir `
        -ManifestPath $ManifestPath `
        -ValidateModuleCrcs `
        -ValidateRuntimeDependencyCrcs `
        -RequireReadme
    $summary.package_validation = $manifestValidation
    if (-not $manifestValidation.success) {
        $summary.blockers = @(
            "CalcEngine transfer package validation failed against the manifest; the Enter-path proof should only run on a known-good package."
        )
        throw "CalcEngine transfer package validation failed."
    }

    $runnerSummaryPath = [System.IO.Path]::GetTempFileName()
    try {
        $delegatedRunner = Invoke-PowerShellFile -ScriptPath $resolvedComprehensiveRunnerPath -Arguments @(
            "-RomPath", [string]$resolvedRom.path,
            "-AutotesterPath", [string]$resolvedAutotester.path,
            "-EmulatorBackend", "Autotester",
            "-Mode", $Mode,
            "-DownloadsDir", $resolvedDownloadsDir,
            "-TemplatePath", $resolvedTemplatePath,
            "-GeneratedPath", $resolvedGeneratedPath,
            "-BaselinedPath", $resolvedBaselinedPath,
            "-CrcCatalogJsonPath", $resolvedCrcCatalogJsonPath,
            "-CrcCatalogMarkdownPath", $resolvedCrcCatalogMarkdownPath,
            "-SummaryPath", $runnerSummaryPath
        )

        if (Test-Path -LiteralPath $runnerSummaryPath -PathType Leaf) {
            $summary.runner_summary = Get-Content -LiteralPath $runnerSummaryPath -Raw | ConvertFrom-Json
        }

        $summary.success = [bool]$delegatedRunner.success
        $summary.exit_code = if ($summary.success) { 0 } else { 1 }

        $shouldParse = $null -ne $summary.runner_summary -and
            $summary.runner_summary.PSObject.Properties.Name -contains "verify_stage" -and
            $null -ne $summary.runner_summary.verify_stage
        if ($shouldParse) {
            $parserInvocation = Invoke-PowerShellFile -ScriptPath $resolvedResultParserPath -Arguments @(
                "-InputPath", $runnerSummaryPath,
                "-OutputPath", $resolvedParsedOutputPath
            )

            if (Test-Path -LiteralPath $resolvedParsedOutputPath -PathType Leaf) {
                $summary.parsed_result = Get-Content -LiteralPath $resolvedParsedOutputPath -Raw | ConvertFrom-Json
                if ($null -ne $summary.parsed_result -and $summary.parsed_result.PSObject.Properties.Name -contains "input_path") {
                    $summary.parsed_result.input_path = $resolvedOutputPath
                    Write-Utf8NoBom -Path $resolvedParsedOutputPath -Content ($summary.parsed_result | ConvertTo-Json -Depth 12)
                }
            }

            if ($null -ne $summary.parsed_result -and -not [bool]$summary.parsed_result.success) {
                $summary.success = $false
                $summary.exit_code = 1
                $summary.blockers = @("The Enter-path proof verify stage reported one or more failing or missing hashes.")
            }
            elseif (-not [bool]$parserInvocation.success) {
                $summary.success = $false
                $summary.exit_code = 1
                $summary.blockers = @("The Enter-path proof parser failed to summarize the verify stage.")
            }
        }

        if (-not $summary.success -and @($summary.blockers).Count -eq 0) {
            $summary.blockers = @("The delegated comprehensive autotest runner reported failure for the Enter-path proof suite.")
        }
    }
    finally {
        if (Test-Path -LiteralPath $runnerSummaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $runnerSummaryPath -Force -ErrorAction SilentlyContinue
        }
    }
}
catch {
    if (@($summary.blockers).Count -eq 0) {
        $summary.blockers = @($_.Exception.Message)
    }
    $summary.success = $false
    $summary.exit_code = 1
}
finally {
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
}

Write-Host "CalcEngine Enter-Path Proof"
Write-Host ("Mode            : {0}" -f $Mode)
Write-Host ("Template        : {0}" -f $resolvedTemplatePath)
Write-Host ("Generated       : {0}" -f $resolvedGeneratedPath)
Write-Host ("Baselined       : {0}" -f $resolvedBaselinedPath)
Write-Host ("Parsed          : {0}" -f $resolvedParsedOutputPath)
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
if ($summary.runner_summary) {
    Write-Host ("Delegated mode  : {0}" -f [string]$summary.runner_summary.mode)
}
Write-Host ("Overall         : {0}" -f $(if ($summary.success) { "PASS" } else { "FAIL" }))

if (@($summary.blockers).Count -gt 0) {
    Write-Host ""
    Write-Host "Blockers:"
    foreach ($blocker in @($summary.blockers)) {
        Write-Host ("  - {0}" -f [string]$blocker)
    }
}

exit ([int]$summary.exit_code)
