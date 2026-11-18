#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [string]$Version = "latest",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$InstallRoot = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CEmuPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [switch]$RequireExplicitAutotesterPath,

    [Parameter(Mandatory = $false)]
    [switch]$Force,

    [Parameter(Mandatory = $false)]
    [switch]$DryRun,

    [Parameter(Mandatory = $false)]
    [switch]$SkipGuiDownload,

    [Parameter(Mandatory = $false)]
    [switch]$SkipAutotesterBuild,

    [Parameter(Mandatory = $false)]
    [switch]$SkipBootVerification,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ReleaseApiUrl = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BootVerifyRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BootVerifySummaryPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:StartTime = Get-Date
$script:UserAgent = "CalcEngine-setup-cemu/1.0 (+https://github.com/XanderXML-Bit/algebrix-ce)"

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

function Invoke-GitHubJsonRequest {
    param([Parameter(Mandatory = $true)][string]$Url)

    $headers = @{
        "User-Agent" = $script:UserAgent
        "Accept" = "application/vnd.github.v3+json"
    }

    return Invoke-RestMethod -Uri $Url -Headers $headers -Method Get -ErrorAction Stop
}

function Resolve-CEmuReleaseInfo {
    param(
        [Parameter(Mandatory = $true)][string]$VersionValue,
        [Parameter(Mandatory = $false)][AllowEmptyString()][string]$ExplicitApiUrl = ""
    )

    $apiUrl = $ExplicitApiUrl
    if ([string]::IsNullOrWhiteSpace($apiUrl)) {
        if ([System.StringComparer]::OrdinalIgnoreCase.Equals($VersionValue, "latest")) {
            $apiUrl = "https://api.github.com/repos/CE-Programming/CEmu/releases/latest"
        }
        else {
            $apiUrl = "https://api.github.com/repos/CE-Programming/CEmu/releases/tags/$VersionValue"
        }
    }

    $release = Invoke-GitHubJsonRequest -Url $apiUrl
    $preferredAsset = $null
    foreach ($pattern in @("*win64*Qt6*.exe", "*win64*Qt5*.exe")) {
        $preferredAsset = @($release.assets | Where-Object { [string]$_.name -like $pattern } | Select-Object -First 1)[0]
        if ($null -ne $preferredAsset) {
            break
        }
    }

    if ($null -eq $preferredAsset) {
        throw "No supported Windows x64 CEmu release asset was found in release '$($release.tag_name)'."
    }

    return [ordered]@{
        api_url = $apiUrl
        tag_name = [string]$release.tag_name
        name = [string]$release.name
        html_url = [string]$release.html_url
        published_at = [string]$release.published_at
        asset_name = [string]$preferredAsset.name
        asset_url = [string]$preferredAsset.browser_download_url
        asset_size = [int64]$preferredAsset.size
        asset_digest = if ($preferredAsset.PSObject.Properties.Name -contains "digest") { [string]$preferredAsset.digest } else { "" }
    }
}

function Invoke-FileDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $headers = @{
        "User-Agent" = $script:UserAgent
        "Accept" = "application/octet-stream"
    }

    Invoke-WebRequest -Uri $Url -Headers $headers -OutFile $Destination -UseBasicParsing -ErrorAction Stop
}

function Find-VisualStudioBuildTools {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vsWhere -PathType Leaf)) {
        return [ordered]@{
            available = $false
            installation_path = ""
            cmake_path = ""
            msbuild_path = ""
            devenv_path = ""
        }
    }

    $installPath = & $vsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if ([string]::IsNullOrWhiteSpace($installPath)) {
        return [ordered]@{
            available = $false
            installation_path = ""
            cmake_path = ""
            msbuild_path = ""
            devenv_path = ""
        }
    }

    $cmakePath = Join-Path $installPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    $msbuildPath = Join-Path $installPath "MSBuild\Current\Bin\MSBuild.exe"
    $devenvPath = Join-Path $installPath "Common7\IDE\devenv.com"

    return [ordered]@{
        available = $true
        installation_path = $installPath
        cmake_path = if (Test-Path -LiteralPath $cmakePath -PathType Leaf) { $cmakePath } else { "" }
        msbuild_path = if (Test-Path -LiteralPath $msbuildPath -PathType Leaf) { $msbuildPath } else { "" }
        devenv_path = if (Test-Path -LiteralPath $devenvPath -PathType Leaf) { $devenvPath } else { "" }
    }
}

function Invoke-BootVerification {
    param(
        [Parameter(Mandatory = $true)][string]$RunnerPath,
        [Parameter(Mandatory = $true)][string]$RomFullPath,
        [Parameter(Mandatory = $true)][string]$AutotesterFullPath,
        [Parameter(Mandatory = $true)][string]$CEmuFullPath,
        [Parameter(Mandatory = $true)][string]$SummaryPath
    )

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $RunnerPath `
        -RomPath $RomFullPath `
        -AutotesterPath $AutotesterFullPath `
        -CEmuPath $CEmuFullPath `
        -OutputPath $SummaryPath 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        success = ([int]$exitCode -eq 0)
        exit_code = [int]$exitCode
        output_lines = @($captured | ForEach-Object { [string]$_ })
        summary_path = $SummaryPath
    }
}

$repoRoot = Resolve-RepoRoot
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\setup-cemu.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $OutputPath
}
$resolvedBootVerifyRunnerPath = if ([string]::IsNullOrWhiteSpace($BootVerifyRunnerPath)) {
    Join-Path $repoRoot "scripts\cemu-test-runner.ps1"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $BootVerifyRunnerPath
}
$resolvedBootVerifySummaryPath = if ([string]::IsNullOrWhiteSpace($BootVerifySummaryPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\setup-cemu.boot.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $BootVerifySummaryPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "scripts/setup-cemu.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    rom = $null
    release = $null
    install = [ordered]@{
        install_root = ""
        cemu_path = ""
        cemu_source = ""
        autotester_path = ""
        autotester_source = ""
        autotester_ready = $false
    }
    prerequisites = [ordered]@{
        git_path = ""
        visual_studio = Find-VisualStudioBuildTools
    }
    autotester_build = [ordered]@{
        attempted = $false
        success = $false
        source_dir = ""
        notes = @()
        manual_steps = @()
    }
    boot_verification = [ordered]@{
        attempted = $false
        success = $false
        exit_code = -1
        summary_path = $resolvedBootVerifySummaryPath
        notes = @()
    }
    manual_steps = @()
    blockers = @()
    notes = @(
        "Official CE-Programming releases currently publish Windows CEmu GUI binaries but not a standalone autotester.exe.",
        "When autotester.exe is already installed, this setup script reuses it; otherwise it records the exact manual build prerequisites and commands needed on Windows."
    )
}

try {
    $gitCommand = Get-Command -Name "git.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $gitCommand) {
        $summary.prerequisites.git_path = [string]$gitCommand.Source
    }

    $romResolution = Resolve-CalcEmuRomPath -RomPath $RomPath
    $summary.rom = $romResolution

    $releaseInfo = $null
    try {
        $releaseInfo = Resolve-CEmuReleaseInfo -VersionValue $Version -ExplicitApiUrl $ReleaseApiUrl
        $summary.release = $releaseInfo
    }
    catch {
        if (-not $SkipGuiDownload) {
            throw
        }
        $summary.blockers += ("Release lookup skipped or unavailable: {0}" -f $_.Exception.Message)
    }

    $resolvedInstallRoot = if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
        $releaseTag = if ($null -ne $releaseInfo -and -not [string]::IsNullOrWhiteSpace([string]$releaseInfo.tag_name)) {
            [string]$releaseInfo.tag_name
        }
        else {
            "latest"
        }
        Join-Path $HOME "Tools\CEmu\$releaseTag"
    }
    else {
        Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $InstallRoot
    }
    $summary.install.install_root = $resolvedInstallRoot

    $guiResolved = $null
    try {
        $guiResolved = Resolve-CalcEmuGuiPath -CEmuPath $CEmuPath
        $summary.install.cemu_path = [string]$guiResolved.path
        $summary.install.cemu_source = [string]$guiResolved.source
    }
    catch {
        if ($SkipGuiDownload) {
            throw
        }
    }

    if ([string]::IsNullOrWhiteSpace($summary.install.cemu_path) -or $Force) {
        if ($null -eq $releaseInfo) {
            throw "Cannot download CEmu.exe because release metadata was not resolved."
        }

        $downloadTarget = Join-Path $resolvedInstallRoot "CEmu.exe"
        if (-not $DryRun) {
            [void](New-Item -ItemType Directory -Path $resolvedInstallRoot -Force)
        }

        if (-not $DryRun) {
            $tempDownloadPath = Join-Path $resolvedInstallRoot "$($releaseInfo.asset_name).download"
            if (Test-Path -LiteralPath $tempDownloadPath -PathType Leaf) {
                Remove-Item -LiteralPath $tempDownloadPath -Force
            }
            Invoke-FileDownload -Url ([string]$releaseInfo.asset_url) -Destination $tempDownloadPath
            if (Test-Path -LiteralPath $downloadTarget -PathType Leaf) {
                Remove-Item -LiteralPath $downloadTarget -Force
            }
            Move-Item -LiteralPath $tempDownloadPath -Destination $downloadTarget
            Write-Utf8NoBom -Path (Join-Path $resolvedInstallRoot "release.json") -Content ($releaseInfo | ConvertTo-Json -Depth 8)
        }

        $summary.install.cemu_path = $downloadTarget
        $summary.install.cemu_source = if ($DryRun) { "planned_download" } else { "downloaded_release" }
    }

    $autotesterResolved = $null
    try {
        if ($RequireExplicitAutotesterPath) {
            if ([string]::IsNullOrWhiteSpace($AutotesterPath)) {
                throw "RequireExplicitAutotesterPath was set but -AutotesterPath was empty."
            }

            $resolvedExplicitAutotesterPath = Resolve-CalcEmuPipelinePath -PathValue $AutotesterPath
            if (-not (Test-Path -LiteralPath $resolvedExplicitAutotesterPath -PathType Leaf)) {
                throw "Explicit autotester path '$resolvedExplicitAutotesterPath' does not exist."
            }

            $summary.install.autotester_path = (Resolve-Path -LiteralPath $resolvedExplicitAutotesterPath).Path
            $summary.install.autotester_source = "explicit"
            $summary.install.autotester_ready = $true
        }
        else {
            $autotesterResolved = Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
            $summary.install.autotester_path = [string]$autotesterResolved.path
            $summary.install.autotester_source = [string]$autotesterResolved.source
            $summary.install.autotester_ready = $true
        }
    }
    catch {
        $summary.install.autotester_ready = $false
    }

    if (-not $summary.install.autotester_ready) {
        $summary.autotester_build.attempted = (-not $SkipAutotesterBuild)
        $sourceDir = if ($null -ne $releaseInfo -and -not [string]::IsNullOrWhiteSpace([string]$releaseInfo.tag_name)) {
            Join-Path $repoRoot ".tools\CEmu-src\$([string]$releaseInfo.tag_name)"
        }
        else {
            Join-Path $repoRoot ".tools\CEmu-src\latest"
        }
        $summary.autotester_build.source_dir = $sourceDir
        $summary.autotester_build.manual_steps = @(
            ("git clone --branch {0} --depth 1 https://github.com/CE-Programming/CEmu.git ""{1}""" -f
                $(if ($null -ne $releaseInfo) { [string]$releaseInfo.tag_name } else { "v2.0" }),
                $sourceDir),
            ("Open a Developer PowerShell for Visual Studio and build the upstream CEmu core for the matching tag."),
            ("Build the standalone AutoTester target from tests\\autotester against the matching cemucore output."),
            ("Place the resulting autotester.exe at ""{0}"" or set CEMU_AUTOTESTER_PATH." -f (Join-Path $resolvedInstallRoot "autotester.exe"))
        )
        $summary.autotester_build.notes = @(
            "Upstream releases do not currently ship autotester.exe as a downloadable Windows asset.",
            "The repository's tests/autotester/autotester_cli.cpp and tests/autotester/json_file_documentation.txt are the authoritative upstream references for the CLI."
        )
        $summary.manual_steps += @($summary.autotester_build.manual_steps)
        $summary.blockers += "autotester.exe could not be resolved automatically."
    }

    if (-not $SkipBootVerification) {
        if (-not (Test-Path -LiteralPath $resolvedBootVerifyRunnerPath -PathType Leaf)) {
            throw "Boot verification runner '$resolvedBootVerifyRunnerPath' does not exist."
        }
        if (-not $summary.install.autotester_ready) {
            $summary.boot_verification.attempted = $false
            $summary.boot_verification.notes += "Skipped boot verification because autotester.exe is not ready."
        }
        elseif ($DryRun) {
            $summary.boot_verification.attempted = $false
            $summary.boot_verification.notes += "Dry-run mode skipped the real-ROM boot verification stage."
        }
        else {
            $summary.boot_verification.attempted = $true
            $bootRun = Invoke-BootVerification `
                -RunnerPath $resolvedBootVerifyRunnerPath `
                -RomFullPath ([string]$summary.rom.path) `
                -AutotesterFullPath ([string]$summary.install.autotester_path) `
                -CEmuFullPath ([string]$summary.install.cemu_path) `
                -SummaryPath $resolvedBootVerifySummaryPath
            $summary.boot_verification.success = [bool]$bootRun.success
            $summary.boot_verification.exit_code = [int]$bootRun.exit_code
            $summary.boot_verification.output_lines = [string[]]@($bootRun.output_lines)
            if (-not $bootRun.success) {
                $summary.blockers += "Real-ROM boot verification failed."
            }
        }
    }
    else {
        $summary.boot_verification.notes += "Boot verification was skipped by request."
    }

    $cemuReady = (-not [string]::IsNullOrWhiteSpace([string]$summary.install.cemu_path))
    $bootReady = ((-not $summary.boot_verification.attempted) -or [bool]$summary.boot_verification.success)
    $summary.success = $cemuReady -and $summary.install.autotester_ready -and $bootReady -and (@($summary.blockers).Count -eq 0)
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
}
catch {
    $summary.success = $false
    $summary.exit_code = 1
    $summary.blockers += $_.Exception.Message
}
finally {
    $summary.blockers = @($summary.blockers | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } | Sort-Object -Unique)
    $summary.duration_seconds = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 16)
}

Write-Host "CalcEngine CEmu Setup"
Write-Host ("Summary         : {0}" -f $resolvedOutputPath)
Write-Host ("ROM             : {0}" -f $(if ($summary.rom) { [string]$summary.rom.path } else { "" }))
Write-Host ("Install root    : {0}" -f [string]$summary.install.install_root)
Write-Host ("CEmu            : {0}" -f [string]$summary.install.cemu_path)
Write-Host ("Autotester      : {0}" -f [string]$summary.install.autotester_path)
Write-Host ("Boot verify     : {0}" -f $(if ($summary.boot_verification.success) { "PASS" } elseif ($summary.boot_verification.attempted) { "FAIL" } else { "SKIP" }))
Write-Host ("Overall         : {0}" -f $(if ($summary.success) { "PASS" } else { "FAIL" }))
if (@($summary.manual_steps).Count -gt 0) {
    Write-Host ""
    Write-Host "Manual steps:"
    foreach ($step in @($summary.manual_steps)) {
        Write-Host ("  - {0}" -f [string]$step)
    }
}
if (@($summary.blockers).Count -gt 0) {
    Write-Host ""
    Write-Host "Blockers:"
    foreach ($blocker in @($summary.blockers)) {
        Write-Host ("  - {0}" -f [string]$blocker)
    }
}

exit ([int]$summary.exit_code)
