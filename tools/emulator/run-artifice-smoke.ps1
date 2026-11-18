[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [ValidateSet("Auto", "Autotester", "CEmuHeadless")]
    [string]$EmulatorBackend = "Auto",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CEmuHeadlessPath = $env:CEMU_HEADLESS_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "C:\Users\user\Downloads\CalcEngine",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeProgramPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$PreferredAssetPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$BootstrapArtificeAsset,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArtificeBootstrapScriptPath = "",

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
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

$artificeModulePath = Join-Path $PSScriptRoot "CalcEmuArtificePipeline.psm1"
if (-not (Test-Path -LiteralPath $artificeModulePath -PathType Leaf)) {
    throw "arTIfiCE pipeline module not found at '$artificeModulePath'."
}
Import-Module -Name $artificeModulePath -Force

function Resolve-LocalRepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).ProviderPath
}

function Resolve-LocalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path (Resolve-LocalRepoRoot) $PathValue))
}

function Get-LocalCrc32Hex {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Data
    )

    $mask = [uint64]4294967295
    $polynomial = [uint64]3988292384
    $crc = [uint64]4294967295

    foreach ($byteValue in $Data) {
        $crc = (($crc -bxor [uint64]$byteValue) -band $mask)
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) {
                $crc = ((($crc -shr 1) -bxor $polynomial) -band $mask)
            }
            else {
                $crc = (($crc -shr 1) -band $mask)
            }
        }
    }

    return ("{0:X8}" -f [uint32](($crc -bxor $mask) -band $mask))
}

function Resolve-LocalRomPath {
    param([string]$PathValue)

    $effectivePath = if ([string]::IsNullOrWhiteSpace($PathValue)) {
        Join-Path (Resolve-LocalRepoRoot) "rom\Rom.rom"
    }
    else {
        Resolve-LocalPath -PathValue $PathValue
    }

    if (-not (Test-Path -LiteralPath $effectivePath -PathType Leaf)) {
        throw "ROM path '$effectivePath' does not exist."
    }

    return (Resolve-Path -LiteralPath $effectivePath).ProviderPath
}

function Resolve-LocalAutotesterPath {
    param([string]$PathValue)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($PathValue)) {
        $candidates.Add((Resolve-LocalPath -PathValue $PathValue))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:CEMU_AUTOTESTER_PATH)) {
        $candidates.Add([System.IO.Path]::GetFullPath($env:CEMU_AUTOTESTER_PATH))
    }
    $command = Get-Command -Name "autotester.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace([string]$command.Source)) {
        $candidates.Add([string]$command.Source)
    }
    $candidates.Add("C:\Users\user\Tools\CEmu\v2.0\autotester.exe")
    $candidates.Add("C:\Tools\CEmu\autotester.exe")

    foreach ($candidate in @($candidates | Select-Object -Unique)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }

    throw "autotester.exe could not be resolved."
}

function Test-LocalTransferManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DirectoryPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ManifestPath = ""
    )

    $resolvedDirectoryPath = Resolve-LocalPath -PathValue $DirectoryPath
    $resolvedManifestPath = if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        Join-Path (Resolve-LocalRepoRoot) "tests\emulator\manifest.json"
    }
    else {
        Resolve-LocalPath -PathValue $ManifestPath
    }

    if (-not (Test-Path -LiteralPath $resolvedDirectoryPath -PathType Container)) {
        throw "Transfer directory '$resolvedDirectoryPath' does not exist."
    }
    if (-not (Test-Path -LiteralPath $resolvedManifestPath -PathType Leaf)) {
        throw "Transfer manifest '$resolvedManifestPath' was not found."
    }

    $manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json
    $fileMap = @{}
    foreach ($file in @(Get-ChildItem -LiteralPath $resolvedDirectoryPath -File)) {
        $fileMap[$file.Name.ToUpperInvariant()] = $file
    }

    $missingModules = New-Object System.Collections.Generic.List[string]
    $moduleCrcMismatches = New-Object System.Collections.Generic.List[object]
    foreach ($module in @($manifest.modules)) {
        $moduleName = [string]$module.name
        $lookupKey = $moduleName.ToUpperInvariant()
        if (-not $fileMap.ContainsKey($lookupKey)) {
            $missingModules.Add($moduleName)
            continue
        }

        $actualCrc = Get-LocalCrc32Hex -Data ([System.IO.File]::ReadAllBytes($fileMap[$lookupKey].FullName))
        if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($actualCrc, [string]$module.crc32)) {
            $moduleCrcMismatches.Add([ordered]@{
                name = $moduleName
                expected_crc32 = [string]$module.crc32
                observed_crc32 = $actualCrc
            })
        }
    }

    $missingRuntimeDependencies = New-Object System.Collections.Generic.List[string]
    $runtimeDependencyCrcMismatches = New-Object System.Collections.Generic.List[object]
    foreach ($dependency in @($manifest.runtime_dependencies)) {
        $dependencyName = [string]$dependency.name
        $lookupKey = $dependencyName.ToUpperInvariant()
        if (-not $fileMap.ContainsKey($lookupKey)) {
            $missingRuntimeDependencies.Add($dependencyName)
            continue
        }

        $actualCrc = Get-LocalCrc32Hex -Data ([System.IO.File]::ReadAllBytes($fileMap[$lookupKey].FullName))
        if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($actualCrc, [string]$dependency.crc32)) {
            $runtimeDependencyCrcMismatches.Add([ordered]@{
                name = $dependencyName
                expected_crc32 = [string]$dependency.crc32
                observed_crc32 = $actualCrc
            })
        }
    }

    $readmeName = [string]$manifest.readme.name
    $readmePresent = $fileMap.ContainsKey($readmeName.ToUpperInvariant())

    return [ordered]@{
        manifest_path = $resolvedManifestPath
        downloads_dir = $resolvedDirectoryPath
        missing_modules = $missingModules.ToArray()
        module_crc_mismatches = $moduleCrcMismatches.ToArray()
        missing_runtime_dependencies = $missingRuntimeDependencies.ToArray()
        runtime_dependency_crc_mismatches = $runtimeDependencyCrcMismatches.ToArray()
        readme_present = $readmePresent
        success = ($missingModules.Count -eq 0) -and
                  ($moduleCrcMismatches.Count -eq 0) -and
                  ($missingRuntimeDependencies.Count -eq 0) -and
                  ($runtimeDependencyCrcMismatches.Count -eq 0) -and
                  $readmePresent
    }
}

$repoRoot = Resolve-LocalRepoRoot
$resolvedTemplatePath = if ([string]::IsNullOrWhiteSpace($TemplatePath)) {
    Join-Path $repoRoot "tests\emulator\autotest.artifice-smoke.json"
}
else {
    Resolve-LocalPath -PathValue $TemplatePath
}
$resolvedGeneratedPath = if ([string]::IsNullOrWhiteSpace($GeneratedPath)) {
    Join-Path $repoRoot "tests\emulator\autotest.artifice-smoke.generated.json"
}
else {
    Resolve-LocalPath -PathValue $GeneratedPath
}
$resolvedBaselinedPath = if ([string]::IsNullOrWhiteSpace($BaselinedPath)) {
    Join-Path $repoRoot "tests\emulator\autotest.artifice-smoke.baselined.json"
}
else {
    Resolve-LocalPath -PathValue $BaselinedPath
}
$resolvedCrcCatalogJsonPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogJsonPath)) {
    Join-Path $repoRoot "tests\emulator\autotest.artifice-smoke.crc-catalog.json"
}
else {
    Resolve-LocalPath -PathValue $CrcCatalogJsonPath
}
$resolvedCrcCatalogMarkdownPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogMarkdownPath)) {
    Join-Path $repoRoot "tests\emulator\autotest.artifice-smoke.crc-catalog.md"
}
else {
    Resolve-LocalPath -PathValue $CrcCatalogMarkdownPath
}
$resolvedComprehensiveRunnerPath = if ([string]::IsNullOrWhiteSpace($ComprehensiveRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
}
else {
    Resolve-LocalPath -PathValue $ComprehensiveRunnerPath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\artifice-smoke.summary.json"
}
else {
    Resolve-LocalPath -PathValue $OutputPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "run-artifice-smoke.ps1"
    generated_at = (Get-Date -Format "o")
    mode = $Mode
    success = $false
    exit_code = 1
    template_path = $resolvedTemplatePath
    generated_path = $resolvedGeneratedPath
    baselined_path = $resolvedBaselinedPath
    crc_catalog_json_path = $resolvedCrcCatalogJsonPath
    crc_catalog_markdown_path = $resolvedCrcCatalogMarkdownPath
    package_validation = $null
    artifice = $null
    options = [ordered]@{
        emulator_backend = $EmulatorBackend
        cemu_headless_path = $CEmuHeadlessPath
        bootstrap_artifice_asset = [bool]$BootstrapArtificeAsset
        artifice_bootstrap_script_path = $ArtificeBootstrapScriptPath
    }
    staged_downloads = $null
    delegated_runner = $null
    blockers = @()
}

$stagingRoot = ""
try {
    if (-not (Test-Path -LiteralPath $resolvedTemplatePath -PathType Leaf)) {
        throw "Smoke template was not found at '$resolvedTemplatePath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedComprehensiveRunnerPath -PathType Leaf)) {
        throw "Comprehensive autotest runner was not found at '$resolvedComprehensiveRunnerPath'."
    }

    $resolvedRom = Resolve-LocalRomPath -PathValue $RomPath
    $resolvedAutotester = Resolve-LocalAutotesterPath -PathValue $AutotesterPath
    $resolvedDownloadsDir = Resolve-LocalPath -PathValue $DownloadsDir
    if (-not (Test-Path -LiteralPath $resolvedDownloadsDir -PathType Container)) {
        throw "DownloadsDir '$resolvedDownloadsDir' does not exist."
    }

    $manifestValidation = Test-LocalTransferManifest `
        -DirectoryPath $resolvedDownloadsDir `
        -ManifestPath $ManifestPath
    $summary.package_validation = $manifestValidation
    if (-not $manifestValidation.success) {
        $summary.blockers = @(
            "CalcEngine transfer package validation failed against the manifest; artifice smoke should not run on an invalid package."
        )
        throw "CalcEngine transfer package validation failed."
    }

    $resolvedExplicitArtificePath = ""
    if (-not [string]::IsNullOrWhiteSpace($ArtificeProgramPath)) {
        $resolvedExplicitArtificePath = Resolve-LocalPath -PathValue $ArtificeProgramPath
        if (-not (Test-Path -LiteralPath $resolvedExplicitArtificePath -PathType Leaf)) {
            $summary.blockers = @(
                "Explicit arTIfiCE path '$resolvedExplicitArtificePath' does not exist."
            )
            throw "Explicit arTIfiCE path was not found."
        }
    }
    $preferredAssetPath = if ([string]::IsNullOrWhiteSpace($PreferredAssetPath)) {
        Join-Path $repoRoot "tools\emulator\assets\artifice-v2.8xp"
    }
    else {
        Resolve-LocalPath -PathValue $PreferredAssetPath
    }
    $resolvedArtifice = Resolve-CalcEmuArtificeAsset `
        -ExplicitPath $resolvedExplicitArtificePath `
        -PreferredAssetPath $preferredAssetPath `
        -SearchRoots @(
            $resolvedDownloadsDir,
            (Split-Path -Path $resolvedDownloadsDir -Parent),
            (Get-Location).Path
        )

    $bootstrap = [ordered]@{
        attempted = $false
        success = $false
        exit_code = 0
        script_path = if ([string]::IsNullOrWhiteSpace($ArtificeBootstrapScriptPath)) {
            (Join-Path $repoRoot "scripts\get-artifice-asset.ps1")
        }
        else {
            $ArtificeBootstrapScriptPath
        }
        output_lines = @()
        invocation_error = ""
        asset_present_after = $false
        asset_path_after = ""
        asset_sha256 = ""
        asset_size_bytes = 0
    }
    if (-not $resolvedArtifice.assets_present -and $BootstrapArtificeAsset) {
        $bootstrap = Invoke-CalcEmuArtificeBootstrap `
            -BootstrapScriptPath $ArtificeBootstrapScriptPath `
            -PreferredAssetPath $preferredAssetPath

        $resolvedArtifice = Resolve-CalcEmuArtificeAsset `
            -ExplicitPath $resolvedExplicitArtificePath `
            -PreferredAssetPath $preferredAssetPath `
            -SearchRoots @(
                $resolvedDownloadsDir,
                (Split-Path -Path $resolvedDownloadsDir -Parent),
                (Get-Location).Path
            )
    }

    $summary.artifice = [ordered]@{
        assets_present = [bool]$resolvedArtifice.assets_present
        program_path = [string]$resolvedArtifice.path
        source = [string]$resolvedArtifice.source
        repo_asset_path = $preferredAssetPath
        sha256 = if ([bool]$resolvedArtifice.assets_present) { Get-CalcEmuFileSha256Hex -Path ([string]$resolvedArtifice.path) } else { "" }
        size_bytes = if ([bool]$resolvedArtifice.assets_present) { [int](Get-Item -LiteralPath ([string]$resolvedArtifice.path)).Length } else { 0 }
        warnings = @($resolvedArtifice.warnings)
        checked_candidates = @($resolvedArtifice.checked_candidates)
        bootstrap = $bootstrap
    }
    if (-not $resolvedArtifice.assets_present) {
        $summary.blockers = @(
            $(if ($BootstrapArtificeAsset) {
                "No local arTIfiCE v2 .8xp asset was found, and bootstrap did not stage one. Review the bootstrap output or run scripts/get-artifice-asset.ps1 manually."
            }
            else {
                "No local arTIfiCE v2 .8xp asset was found. Run scripts/get-artifice-asset.ps1 or place the upstream .8xp in tools/emulator/assets/artifice-v2.8xp."
            })
        )
        throw "arTIfiCE v2 asset was not found."
    }

    $stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("calcengine-artifice-smoke-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    $stagedDownloadsDir = Join-Path $stagingRoot "downloads"
    $runnerSummaryPath = Join-Path $stagingRoot "artifice-smoke.runner.summary.json"
    $summary.staged_downloads = Stage-CalcEmuArtificeDownloadsDirectory `
        -SourceDownloadsDir $resolvedDownloadsDir `
        -ArtificeSourcePath ([string]$resolvedArtifice.path) `
        -DestinationDir $stagedDownloadsDir
    $dedupedTransferFiles = New-Object System.Collections.Generic.List[string]
    $seenTransferNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($copiedPath in @($summary.staged_downloads.copied_files)) {
        $copiedPathString = [string]$copiedPath
        $copiedName = [System.IO.Path]::GetFileName($copiedPathString)
        if (-not [string]::IsNullOrWhiteSpace($copiedName) -and $seenTransferNames.Add($copiedName)) {
            $dedupedTransferFiles.Add($copiedPathString)
        }
    }
    $artificeTransferPath = @($dedupedTransferFiles | Where-Object { [System.IO.Path]::GetFileName([string]$_) -ieq 'A.8xp' } | Select-Object -First 1)
    if (@($artificeTransferPath).Count -gt 0) {
        $summary.transfer_files = @(
            [string]$artificeTransferPath[0]
            @($dedupedTransferFiles | Where-Object { [System.IO.Path]::GetFileName([string]$_) -ine 'A.8xp' })
        )
    }
    else {
        $summary.transfer_files = @($dedupedTransferFiles)
    }

    $delegatedRunnerArguments = @(
        "-RomPath", $resolvedRom,
        "-AutotesterPath", $resolvedAutotester,
        "-EmulatorBackend", $EmulatorBackend,
        "-Mode", $Mode,
        "-DownloadsDir", $stagedDownloadsDir,
        "-TemplatePath", $resolvedTemplatePath,
        "-GeneratedPath", $resolvedGeneratedPath,
        "-BaselinedPath", $resolvedBaselinedPath,
        "-CrcCatalogJsonPath", $resolvedCrcCatalogJsonPath,
        "-CrcCatalogMarkdownPath", $resolvedCrcCatalogMarkdownPath,
        "-SummaryPath", $runnerSummaryPath
    )
    if (-not [string]::IsNullOrWhiteSpace($CEmuHeadlessPath)) {
        $delegatedRunnerArguments += @("-CEmuHeadlessPath", (Resolve-LocalPath -PathValue $CEmuHeadlessPath))
    }

    $delegatedRunner = Invoke-CalcEmuPowerShellFile -ScriptPath $resolvedComprehensiveRunnerPath -Arguments $delegatedRunnerArguments
    if (Test-Path -LiteralPath $runnerSummaryPath -PathType Leaf) {
        $delegatedRunner.summary_json = Get-Content -LiteralPath $runnerSummaryPath -Raw | ConvertFrom-Json
    }
    $summary.delegated_runner = $delegatedRunner

    $summary.success = [bool]$delegatedRunner.success
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
    if (-not $summary.success -and @($summary.blockers).Count -eq 0) {
        $summary.blockers = @("The delegated comprehensive autotest runner reported failure for the artifice smoke suite.")
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
    Write-CalcEmuUtf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
    if (-not [string]::IsNullOrWhiteSpace($stagingRoot) -and (Test-Path -LiteralPath $stagingRoot)) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "CalcEngine arTIfiCE Smoke Suite"
Write-Host ("Mode            : {0}" -f $Mode)
Write-Host ("Template        : {0}" -f $resolvedTemplatePath)
Write-Host ("Generated       : {0}" -f $resolvedGeneratedPath)
Write-Host ("Baselined       : {0}" -f $resolvedBaselinedPath)
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
if ($summary.artifice) {
    Write-Host ("arTIfiCE asset  : {0}" -f [string]$summary.artifice.program_path)
    Write-Host ("Bootstrap       : {0}" -f $(if ($BootstrapArtificeAsset) { "enabled" } else { "disabled" }))
    if ($summary.artifice.bootstrap -and [bool]$summary.artifice.bootstrap.attempted) {
        Write-Host ("Bootstrap exit  : {0}" -f [int]$summary.artifice.bootstrap.exit_code)
    }
}
if ($summary.delegated_runner) {
    Write-Host ("Delegated exit  : {0}" -f [int]$summary.delegated_runner.exit_code)
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
