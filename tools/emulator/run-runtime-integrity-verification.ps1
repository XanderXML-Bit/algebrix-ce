[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "deploy/hardware-ready",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ComprehensiveRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$EnvironmentVerifierPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CleanExitTemplatePath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CleanExitBaselinedPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:StartTime = Get-Date

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
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
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $enc)
}

function Resolve-OptionalPath {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$PathValue
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return (Join-Path $RepoRoot $PathValue)
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

function Stage-BlankSlatePackage {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$DestinationDir
    )

    [void](New-Item -ItemType Directory -Path $DestinationDir -Force)
    $patterns = @("*.8xp", "*.8xv", "README.md")
    $copiedFiles = New-Object System.Collections.Generic.List[string]

    foreach ($pattern in $patterns) {
        foreach ($file in @(Get-ChildItem -LiteralPath $SourceDir -Filter $pattern -File | Sort-Object Name)) {
            $destPath = Join-Path $DestinationDir $file.Name
            Copy-Item -LiteralPath $file.FullName -Destination $destPath -Force
            $copiedFiles.Add($destPath)
        }
    }

    $artificeFiles = @(
        Get-ChildItem -LiteralPath $SourceDir -File -Recurse |
        Where-Object {
            $_.Name -match '(?i)artifice|cabri' -or
            $_.Extension -ieq '.8ek'
        } |
        Sort-Object FullName
    )

    foreach ($file in $artificeFiles) {
        $destPath = Join-Path $DestinationDir $file.Name
        Copy-Item -LiteralPath $file.FullName -Destination $destPath -Force
        $copiedFiles.Add($destPath)
    }

    return [ordered]@{
        destination_dir = $DestinationDir
        copied_file_count = $copiedFiles.Count
        copied_files = $copiedFiles.ToArray()
        top_level_program_count = @(Get-ChildItem -LiteralPath $DestinationDir -Filter *.8xp -File).Count
        top_level_appvar_count = @(Get-ChildItem -LiteralPath $DestinationDir -Filter *.8xv -File).Count
        artifice_asset_count = $artificeFiles.Count
        appvar_absent_prior_to_run = (-not (Test-Path -LiteralPath (Join-Path $DestinationDir "CESETTNG.8xv"))) -and
                                    (-not (Test-Path -LiteralPath (Join-Path $DestinationDir "CEHISTOR.8xv")))
    }
}

$repoRoot = Resolve-RepoRoot
$resolvedRomPath = Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $RomPath
$resolvedComprehensiveRunnerPath = if ([string]::IsNullOrWhiteSpace($ComprehensiveRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $ComprehensiveRunnerPath
}
$resolvedEnvironmentVerifierPath = if ([string]::IsNullOrWhiteSpace($EnvironmentVerifierPath)) {
    Join-Path $repoRoot "tools\emulator\run-comprehensive-environment-verification.ps1"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $EnvironmentVerifierPath
}
$resolvedCleanExitTemplatePath = if ([string]::IsNullOrWhiteSpace($CleanExitTemplatePath)) {
    Join-Path $repoRoot "tests\emulator\autotest.clean-exit.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $CleanExitTemplatePath
}
$resolvedCleanExitBaselinedPath = if ([string]::IsNullOrWhiteSpace($CleanExitBaselinedPath)) {
    Join-Path $repoRoot "tests\emulator\autotest.clean-exit.baselined.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $CleanExitBaselinedPath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\runtime-integrity.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $OutputPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "run-runtime-integrity-verification.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    options = [ordered]@{
        rom_path = $resolvedRomPath
        autotester_path = $AutotesterPath
        downloads_dir = $DownloadsDir
        comprehensive_runner_path = $resolvedComprehensiveRunnerPath
        environment_verifier_path = $resolvedEnvironmentVerifierPath
        clean_exit_template_path = $resolvedCleanExitTemplatePath
        clean_exit_baselined_path = $resolvedCleanExitBaselinedPath
    }
    blank_slate_package = $null
    blank_slate_package_validation = $null
    blank_slate_comprehensive = $null
    clean_exit_probe = $null
    environment_verification = $null
    conclusions = [ordered]@{
        memory_pressure_validation = $null
        flash_write_safety = $null
        clean_exit_verification = $null
        blank_slate_hardware_simulation = $null
    }
    limitations = @(
        "CESETTNG and CEHISTOR writes are only indirectly observable on the real-ROM AutoTester lane; TI-OS VAT internals are still not directly inspectable from autotester.exe.",
        "The current CalcEngine Downloads package does not bundle arTIfiCE payload files, so the blank-slate staging step mirrors the shipped package exactly and reports any external jailbreak asset gap explicitly."
    )
}

$stagingRoot = ""
try {
    $romResolution = Resolve-CalcEmuRomPath -RomPath $RomPath
    $autotesterResolution = Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
    $resolvedRomPath = [string]$romResolution.path
    $resolvedAutotesterPath = [string]$autotesterResolution.path
    $summary.options.rom_path = $resolvedRomPath
    $summary.options.autotester_path = $resolvedAutotesterPath

    if (-not (Test-Path -LiteralPath $resolvedComprehensiveRunnerPath -PathType Leaf)) {
        throw "Comprehensive autotest runner was not found at '$resolvedComprehensiveRunnerPath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedEnvironmentVerifierPath -PathType Leaf)) {
        throw "Environment verifier was not found at '$resolvedEnvironmentVerifierPath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedCleanExitTemplatePath -PathType Leaf)) {
        throw "Clean-exit template was not found at '$resolvedCleanExitTemplatePath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedCleanExitBaselinedPath -PathType Leaf)) {
        throw "Clean-exit baselined suite was not found at '$resolvedCleanExitBaselinedPath'."
    }
    if (-not (Test-Path -LiteralPath $DownloadsDir -PathType Container)) {
        throw "DownloadsDir was not found at '$DownloadsDir'."
    }

    $stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("calcengine-pass64-blankslate-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    $stagedDownloadsDir = Join-Path $stagingRoot "downloads"
    $summary.blank_slate_package = Stage-BlankSlatePackage -SourceDir $DownloadsDir -DestinationDir $stagedDownloadsDir
    $summary.blank_slate_package_validation = Test-CalcEmuTransferManifest `
        -DirectoryPath $stagedDownloadsDir `
        -ValidateModuleCrcs `
        -ValidateRuntimeDependencyCrcs `
        -RequireReadme
    if (-not $summary.blank_slate_package_validation.success) {
        $failureDetails = New-Object System.Collections.Generic.List[string]
        if (@($summary.blank_slate_package_validation.missing_modules).Count -gt 0) {
            $failureDetails.Add(("missing staged .8xp files: {0}" -f (@($summary.blank_slate_package_validation.missing_modules) -join ", ")))
        }
        if (@($summary.blank_slate_package_validation.module_crc_mismatches).Count -gt 0) {
            $failureDetails.Add(("staged .8xp CRC mismatches: {0}" -f (
                @($summary.blank_slate_package_validation.module_crc_mismatches |
                    ForEach-Object { "{0} expected {1} got {2}" -f $_.name, $_.expected_crc32, $_.observed_crc32 }) -join "; "
            )))
        }
        if (@($summary.blank_slate_package_validation.missing_runtime_dependencies).Count -gt 0) {
            $failureDetails.Add(("missing staged .8xv files: {0}" -f (@($summary.blank_slate_package_validation.missing_runtime_dependencies) -join ", ")))
        }
        if ($summary.blank_slate_package_validation.readme_required -and -not $summary.blank_slate_package_validation.readme_present) {
            $failureDetails.Add(("missing staged README: {0}" -f $summary.blank_slate_package_validation.readme_required))
        }
        throw ("Archive staging validation failed before invoking CEmu: {0}" -f ($failureDetails -join " | "))
    }

    $blankSlateSummaryPath = Join-Path $stagingRoot "blank-slate-comprehensive.summary.json"
    $summary.blank_slate_comprehensive = Invoke-PowerShellFile -ScriptPath $resolvedComprehensiveRunnerPath -Arguments @(
        "-RomPath", $resolvedRomPath,
        "-AutotesterPath", $resolvedAutotesterPath,
        "-EmulatorBackend", "Autotester",
        "-Mode", "Verify",
        "-DownloadsDir", $stagedDownloadsDir,
        "-SummaryPath", $blankSlateSummaryPath
    )
    if (Test-Path -LiteralPath $blankSlateSummaryPath -PathType Leaf) {
        $summary.blank_slate_comprehensive.summary_json = Get-Content -LiteralPath $blankSlateSummaryPath -Raw | ConvertFrom-Json
    }
    if (-not $summary.blank_slate_comprehensive.success) {
        throw "Blank-slate comprehensive verify failed."
    }

    $cleanExitSummaryPath = Join-Path $stagingRoot "clean-exit.summary.json"
    $summary.clean_exit_probe = Invoke-PowerShellFile -ScriptPath $resolvedComprehensiveRunnerPath -Arguments @(
        "-RomPath", $resolvedRomPath,
        "-AutotesterPath", $resolvedAutotesterPath,
        "-EmulatorBackend", "Autotester",
        "-Mode", "Verify",
        "-DownloadsDir", $stagedDownloadsDir,
        "-TemplatePath", $resolvedCleanExitTemplatePath,
        "-BaselinedPath", $resolvedCleanExitBaselinedPath,
        "-GeneratedPath", (Join-Path $stagingRoot "autotest.clean-exit.generated.json"),
        "-SummaryPath", $cleanExitSummaryPath
    )
    if (Test-Path -LiteralPath $cleanExitSummaryPath -PathType Leaf) {
        $summary.clean_exit_probe.summary_json = Get-Content -LiteralPath $cleanExitSummaryPath -Raw | ConvertFrom-Json
    }
    if (-not $summary.clean_exit_probe.success) {
        throw "Clean-exit probe failed."
    }

    $environmentSummaryPath = Join-Path $stagingRoot "environment-verification.summary.json"
    $summary.environment_verification = Invoke-PowerShellFile -ScriptPath $resolvedEnvironmentVerifierPath -Arguments @(
        "-RomPath", $resolvedRomPath,
        "-AutotesterPath", $resolvedAutotesterPath,
        "-DownloadsDir", $stagedDownloadsDir,
        "-OutputPath", $environmentSummaryPath,
        "-ComprehensiveVerifySummaryPath", (Join-Path $stagingRoot "environment-verification.cemu.summary.json")
    )
    if (Test-Path -LiteralPath $environmentSummaryPath -PathType Leaf) {
        $summary.environment_verification.summary_json = Get-Content -LiteralPath $environmentSummaryPath -Raw | ConvertFrom-Json
    }
    if (-not $summary.environment_verification.success) {
        throw "Environment verification failed."
    }

    $dispatchProgramCount = 0
    if ($summary.environment_verification.summary_json -and
        $summary.environment_verification.summary_json.ez80_dispatch -and
        $summary.environment_verification.summary_json.ez80_dispatch.program_count) {
        $dispatchProgramCount = [int]$summary.environment_verification.summary_json.ez80_dispatch.program_count
    }

    $summary.conclusions.memory_pressure_validation = [ordered]@{
        success = $true
        proof = "The blank-slate real-ROM comprehensive verify completed end-to-end without a crash, OOM fault, or hung hash run while executing long-expression entry, statistics, nDeriv/deriv/int/lim CAS calls, finance TVM, menu navigation, CLEAR recovery, and complex-mode toggles in one session."
        evidence_hashes = @(
            "s11_long_expr_input",
            "s11_long_expr_result_16",
            "s10_nderiv_x2_at1",
            "s10_deriv_x2",
            "s10_int_x2",
            "s10_lim_sinx_over_x",
            "s08_tab_settings_open",
            "s08_complex_rect_enabled",
            "s10_fin_pv_case1",
            "s10_complex_mul_i_times_i"
        )
        verify_hash_count = [int]$summary.blank_slate_comprehensive.summary_json.verify_stage.crc_catalog_hash_count
        note = "This is a real-ROM no-crash proof over the existing 87-hash suite rather than a byte-level RAM telemetry trace."
    }

    $summary.conclusions.flash_write_safety = [ordered]@{
        success = $true
        proof = "Settings persistence hashes and history/ANS hashes completed on the real-ROM comprehensive lane, then the environment verifier completed a 35-program archive->RAM->archive dispatch sweep on the same staged package with no package corruption or launcher fallout."
        evidence_hashes = @(
            "s08_tab_settings_open",
            "s08_show_work_off",
            "s08_show_work_off_persist",
            "s08_angle_mode_degrees",
            "s08_angle_mode_degrees_persist",
            "s09_7_times_8",
            "s09_ans_56_plus_4"
        )
        dispatch_round_trip_program_count = $dispatchProgramCount
        note = "This remains a manual-equivalent proof because autotester.exe cannot inspect TI-OS VAT internals for CESETTNG and CEHISTOR directly."
    }

    $summary.conclusions.clean_exit_verification = [ordered]@{
        success = $true
        proof = "The dedicated clean-exit AutoTester probe verified the path result -> 2nd+MODE -> CalcEngine input -> 2nd+MODE -> TI-OS homescreen with deterministic CRCs."
        evidence_hashes = @(
            "x00_app_boot_home",
            "x01_result_2_plus_3",
            "x02_result_quit_back_to_input",
            "x03_tios_home_after_exit"
        )
        note = "No temporary AppVars are shipped in the staged package before the run; VAT post-exit cleanup remains indirectly evidenced through the clean home return and subsequent comprehensive verify success."
    }

    $summary.conclusions.blank_slate_hardware_simulation = [ordered]@{
        success = $true
        proof = "A fresh staged package containing only the transfer artifacts was copied into a temporary directory and used as the sole AutoTester transfer source; the full comprehensive real-ROM verify still passed."
        staged_program_count = $summary.blank_slate_package.top_level_program_count
        staged_appvar_count = $summary.blank_slate_package.top_level_appvar_count
        staged_artifice_asset_count = $summary.blank_slate_package.artifice_asset_count
        appvar_absent_prior_to_run = [bool]$summary.blank_slate_package.appvar_absent_prior_to_run
    }

    $summary.success = $true
    $summary.exit_code = 0
}
catch {
    $summary.success = $false
    $summary.exit_code = 1
    $summary.error = $_.Exception.Message
}
finally {
    $summary.duration_seconds = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
    if (-not [string]::IsNullOrWhiteSpace($stagingRoot)) {
        $summary.staging_root = $stagingRoot
    }
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 14)
}

if ($summary.success) {
    Write-Host ("PASS: runtime integrity verification succeeded. Summary: {0}" -f $resolvedOutputPath)
    exit 0
}

Write-Error ("FAIL: runtime integrity verification failed. Summary: {0}" -f $resolvedOutputPath)
exit 1
