#Requires -Version 5.1
# get-artifice-asset.ps1
#
# Resolves the arTIfiCE ASM jailbreak .8xp needed to run CalcEngine on hardware.
#
# Resolution order (first match wins):
#   1. Output path already exists and has a valid **TI83F* header   -> reuse, exit 0
#   2. -UseLocal: copy the bundled repo asset (tools/emulator/assets/artifice-v2.8xp)
#   3. GitHub Releases API (YvanTT, TheMachine02, jacobly0 in order)
#   4. Print manual instructions and exit 1
#
# The bundled asset at tools/emulator/assets/artifice-v2.8xp (978 bytes) is the
# arTIfiCE v2 program shipped with the repo. It is a valid TI-OS .8xp confirmed
# working in Pass 71 (2026-04-12). Use -UseLocal to install it without network access.
#
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [switch]$DryRun,

    [Parameter(Mandatory = $false)]
    [switch]$Force,

    [Parameter(Mandatory = $false)]
    [switch]$UseLocal,

    [Parameter(Mandatory = $false)]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ReleaseSources = @(
    @{
        owner = "YvanTT"
        repo = "arTIfiCE"
        preferred = $true
        asset_patterns = @("arTIfiCE*.8xp")
    },
    @{
        owner = "TheMachine02"
        repo = "arTIfiCE"
        preferred = $false
        asset_patterns = @("arTIfiCE*.8xp")
    },
    @{
        owner = "jacobly0"
        repo = "arTIfiCE"
        preferred = $false
        asset_patterns = @("arTIfiCE*.8xp")
    }
)
$DefaultAssetDisplayName = "arTIfiCE .8xp"
$UserAgent = "CalcEngine-get-artifice-asset/2.0 (+https://github.com/XanderXML-Bit/algebrix-ce)"
$KnownHashes = @{
    "YvanTT/arTIfiCE@v2.1" = "46E2CD27A93BAD402DE8811D01D77F52B0419FB09A9162957703FFE0EF756CAA"
}

function Write-Status {
    param([string]$Message)
    Write-Host "[get-artifice-asset] $Message"
}

function Write-Warning-Custom {
    param([string]$Message)
    Write-Host "[get-artifice-asset] WARNING: $Message" -ForegroundColor Yellow
}

function Write-Fail {
    param([string]$Message)
    Write-Host "[get-artifice-asset] ERROR: $Message" -ForegroundColor Red
}

function Resolve-RepoRoot {
    $scriptDir = $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($scriptDir)) {
        $scriptDir = (Get-Location).Path
    }

    return (Resolve-Path (Join-Path $scriptDir "..")).Path
}

function Ensure-DirectoryExists {
    param([string]$FilePath)

    $directory = Split-Path -Path $FilePath -Parent
    if ([string]::IsNullOrWhiteSpace($directory)) {
        return
    }

    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        if ($DryRun) {
            Write-Status "[dry-run] Would create directory: $directory"
        }
        else {
            [void](New-Item -ItemType Directory -Path $directory -Force)
            Write-Status "Created directory: $directory"
        }
    }
}

function Get-FileSha256 {
    param([string]$FilePath)

    return [string](Get-FileHash -Algorithm SHA256 -LiteralPath $FilePath).Hash.ToUpperInvariant()
}

function Invoke-GitHubApiRequest {
    param([string]$Url)

    $headers = @{
        "User-Agent" = $UserAgent
        "Accept" = "application/vnd.github.v3+json"
    }

    try {
        return (Invoke-WebRequest -Uri $Url -Headers $headers -UseBasicParsing -ErrorAction Stop).Content
    }
    catch {
        $exception = $_.Exception
        $webException = $exception -as [System.Net.WebException]
        if ($null -ne $webException -and $null -ne $webException.Response) {
            $statusCode = [int]$webException.Response.StatusCode
            switch ($statusCode) {
                404 { throw "GitHub API returned 404 for '$Url'." }
                403 { throw "GitHub API returned 403 for '$Url' (likely rate limited)." }
                429 { throw "GitHub API returned 429 for '$Url' (rate limited)." }
                default { throw "GitHub API returned HTTP $statusCode for '$Url': $($webException.Message)" }
            }
        }

        throw "Failed to fetch '$Url': $($exception.Message)"
    }
}

function Invoke-FileDownload {
    param(
        [string]$Url,
        [string]$Destination
    )

    $headers = @{
        "User-Agent" = $UserAgent
        "Accept" = "application/octet-stream"
    }

    Invoke-WebRequest -Uri $Url -Headers $headers -OutFile $Destination -UseBasicParsing -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        throw "Download reported success but file was not written to '$Destination'."
    }
}

function Find-LatestReleaseSource {
    param([object[]]$Sources)

    $attempts = New-Object System.Collections.Generic.List[object]

    foreach ($source in @($Sources)) {
        $apiUrl = "https://api.github.com/repos/$($source.owner)/$($source.repo)/releases/latest"
        Write-Status "Querying GitHub API: $apiUrl"
        try {
            $releaseJson = Invoke-GitHubApiRequest -Url $apiUrl
            return [ordered]@{
                success = $true
                source = $source
                api_url = $apiUrl
                release_json = $releaseJson
                attempts = $attempts.ToArray()
            }
        }
        catch {
            $attempts.Add([ordered]@{
                owner = [string]$source.owner
                repo = [string]$source.repo
                api_url = $apiUrl
                error = $_.Exception.Message
            })
        }
    }

    return [ordered]@{
        success = $false
        source = $null
        api_url = ""
        release_json = ""
        attempts = $attempts.ToArray()
    }
}

function Find-ReleaseAsset {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Assets,

        [Parameter(Mandatory = $true)]
        [string[]]$Patterns
    )

    foreach ($pattern in @($Patterns)) {
        foreach ($asset in @($Assets)) {
            $assetName = [string]$asset.name
            if ($assetName -like $pattern) {
                return $asset
            }
        }
    }

    return $null
}

function Test-ValidTI8xp {
    param([string]$FilePath)
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) { return $false }
    try {
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)
        if ($bytes.Length -lt 8) { return $false }
        # TI .8xp magic: **TI83F* (bytes 0-7 = 2A 2A 54 49 38 33 46 2A)
        $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
        return $magic -eq "**TI83F*"
    }
    catch { return $false }
}

function Write-ManualDownloadInstructions {
    param(
        [string]$DestinationPath,
        [object[]]$Attempts
    )

    Write-Host ""
    Write-Host "-- FAST LOCAL OPTION (no network required) --"
    Write-Host "  Re-run with the bundled repo asset:"
    Write-Host "    .\scripts\get-artifice-asset.ps1 -UseLocal"
    Write-Host "  This copies tools/emulator/assets/artifice-v2.8xp (Pass 71 verified, 978 bytes)."
    Write-Host ""
    Write-Host "Manual download instructions:"
    Write-Host "  Option A - Maintained upstream release page:"
    Write-Host "    1. Visit: https://github.com/YvanTT/arTIfiCE/releases"
    Write-Host "    2. Download the latest arTIfiCE .8xp asset"
    Write-Host ""
    Write-Host "  Option B - Project website:"
    Write-Host "    1. Visit: https://yvantt.github.io/arTIfiCE/"
    Write-Host "    2. Follow the install/download links for the latest .8xp"
    Write-Host ""
    Write-Host "  Option C - Legacy/community mirrors:"
    Write-Host "    1. Visit: https://github.com/TheMachine02/arTIfiCE"
    Write-Host "    2. Visit: https://github.com/jacobly0/arTIfiCE"
    Write-Host "    3. Copy the .8xp manually if a release asset is not published"
    Write-Host ""
    Write-Host "Stage the final file at: $DestinationPath"
    Write-Host ""

    if (@($Attempts).Count -gt 0) {
        Write-Host "Attempted release endpoints:"
        foreach ($attempt in @($Attempts)) {
            Write-Host ("  - {0}: {1}" -f [string]$attempt.api_url, [string]$attempt.error)
        }
        Write-Host ""
    }

    Write-Host "After placing the file, run:"
    Write-Host "  tools\emulator\record-artifice-baseline.ps1"
}

$repoRoot = Resolve-RepoRoot
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tools\emulator\assets\artifice-v2.8xp"
}
else {
    $OutputPath
}

# ── Fast-path 1: output file already exists and is a valid .8xp ───────────────
if ((Test-Path -LiteralPath $resolvedOutputPath -PathType Leaf) -and (-not $Force)) {
    if (Test-ValidTI8xp -FilePath $resolvedOutputPath) {
        $existingFile = Get-Item -LiteralPath $resolvedOutputPath
        $existingHash = Get-FileSha256 -FilePath $resolvedOutputPath
        Write-Host ""
        Write-Status "Valid arTIfiCE .8xp already present at output path."
        Write-Status "Path     : $resolvedOutputPath"
        Write-Status "Size     : $($existingFile.Length) bytes"
        Write-Status "Modified : $($existingFile.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
        Write-Status "SHA256   : $existingHash"
        Write-Status "Use -Force to replace with a fresh upstream download."
        exit 0
    }
    else {
        Write-Warning-Custom "File at '$resolvedOutputPath' does not have a valid **TI83F* header. Will attempt re-acquire."
    }
}

# ── Fast-path 2: -UseLocal copies the bundled repo asset ──────────────────────
if ($UseLocal) {
    $bundledAsset = Join-Path $repoRoot "tools\emulator\assets\artifice-v2.8xp"
    if (-not (Test-ValidTI8xp -FilePath $bundledAsset)) {
        Write-Fail "Bundled asset '$bundledAsset' is missing or invalid. Cannot use -UseLocal."
        exit 1
    }
    $bundledHash = Get-FileSha256 -FilePath $bundledAsset
    Write-Host ""
    Write-Status "Using bundled arTIfiCE asset (no network required)."
    Write-Status "Source   : $bundledAsset"
    Write-Status "SHA256   : $bundledHash"
    if ($DryRun) {
        Write-Status "[dry-run] Would copy to: $resolvedOutputPath"
        exit 0
    }
    Ensure-DirectoryExists -FilePath $resolvedOutputPath
    Copy-Item -LiteralPath $bundledAsset -Destination $resolvedOutputPath -Force
    Write-Status "Installed to: $resolvedOutputPath"
    exit 0
}

Write-Host ""
Write-Status "Resolving latest arTIfiCE release source..."
$releaseLookup = Find-LatestReleaseSource -Sources $ReleaseSources
if (-not $releaseLookup.success) {
    Write-Fail "No supported GitHub release endpoint returned a downloadable latest release."
    # Auto-fallback: if the bundled repo asset is valid, use it automatically
    $bundledAsset = Join-Path $repoRoot "tools\emulator\assets\artifice-v2.8xp"
    if (Test-ValidTI8xp -FilePath $bundledAsset) {
        Write-Status "Auto-fallback: installing bundled arTIfiCE asset (network unavailable)."
        $bundledHash = Get-FileSha256 -FilePath $bundledAsset
        Write-Status "Source   : $bundledAsset"
        Write-Status "SHA256   : $bundledHash"
        if (-not $DryRun) {
            Ensure-DirectoryExists -FilePath $resolvedOutputPath
            Copy-Item -LiteralPath $bundledAsset -Destination $resolvedOutputPath -Force
            Write-Status "Installed to: $resolvedOutputPath"
            exit 0
        }
        else {
            Write-Status "[dry-run] Would auto-install bundled asset to: $resolvedOutputPath"
            exit 0
        }
    }
    Write-ManualDownloadInstructions -DestinationPath $resolvedOutputPath -Attempts $releaseLookup.attempts
    exit 1
}

$release = $null
try {
    $release = $releaseLookup.release_json | ConvertFrom-Json
}
catch {
    Write-Fail "Failed to parse GitHub release JSON from '$($releaseLookup.api_url)': $($_.Exception.Message)"
    Write-ManualDownloadInstructions -DestinationPath $resolvedOutputPath -Attempts $releaseLookup.attempts
    exit 1
}

$releaseSource = $releaseLookup.source
$sourceName = "$($releaseSource.owner)/$($releaseSource.repo)"
$releaseTag = [string]$release.tag_name
$releaseName = [string]$release.name
$releaseUrl = [string]$release.html_url
$publishedAt = [string]$release.published_at
$releaseAssets = @($release.assets)

if ([string]::IsNullOrWhiteSpace($releaseTag) -or $releaseAssets.Count -eq 0) {
    Write-Fail "Release data from '$sourceName' is missing a tag or assets."
    Write-ManualDownloadInstructions -DestinationPath $resolvedOutputPath -Attempts $releaseLookup.attempts
    exit 1
}

$targetAsset = Find-ReleaseAsset -Assets $releaseAssets -Patterns @($releaseSource.asset_patterns)
if ($null -eq $targetAsset) {
    Write-Fail "Release '$releaseTag' from '$sourceName' does not contain a matching $DefaultAssetDisplayName asset."
    Write-Host ""
    Write-Host "Assets published in this release:"
    foreach ($asset in $releaseAssets) {
        Write-Host ("  - {0}" -f [string]$asset.name)
    }
    Write-ManualDownloadInstructions -DestinationPath $resolvedOutputPath -Attempts $releaseLookup.attempts
    exit 1
}

$assetName = [string]$targetAsset.name
$downloadUrl = [string]$targetAsset.browser_download_url
$expectedSizeBytes = [long]$targetAsset.size
$githubDigest = [string]$targetAsset.digest
$compositeHashKey = "$sourceName@$releaseTag"
$knownHash = if ($KnownHashes.ContainsKey($compositeHashKey)) { [string]$KnownHashes[$compositeHashKey] } else { "" }

Write-Status "Release source : $sourceName"
Write-Status "Release name   : $releaseName"
Write-Status "Tag            : $releaseTag"
Write-Status "Published      : $publishedAt"
Write-Status "Release page   : $releaseUrl"
Write-Status "Asset          : $assetName"
Write-Status "Download URL   : $downloadUrl"
Write-Status "Expected size  : $expectedSizeBytes bytes"
Write-Status "Output path    : $resolvedOutputPath"

if ($DryRun) {
    Write-Host ""
    Write-Status "[dry-run] Download would use '$sourceName' and asset '$assetName'."
    if (-not [string]::IsNullOrWhiteSpace($githubDigest)) {
        Write-Status "[dry-run] GitHub digest is available: $githubDigest"
    }
    elseif (-not [string]::IsNullOrWhiteSpace($knownHash)) {
        Write-Status "[dry-run] Known SHA256 is available: $knownHash"
    }
    else {
        Write-Status "[dry-run] No built-in SHA256 is available; the computed hash would be printed."
    }
    exit 0
}

Ensure-DirectoryExists -FilePath $resolvedOutputPath
$tempPath = "$resolvedOutputPath.tmp"
if (Test-Path -LiteralPath $tempPath -PathType Leaf) {
    Remove-Item -LiteralPath $tempPath -Force
}

Write-Host ""
Write-Status "Downloading $assetName..."
try {
    Invoke-FileDownload -Url $downloadUrl -Destination $tempPath
}
catch {
    Write-Fail $_.Exception.Message
    if (Test-Path -LiteralPath $tempPath -PathType Leaf) {
        Remove-Item -LiteralPath $tempPath -Force
    }
    Write-ManualDownloadInstructions -DestinationPath $resolvedOutputPath -Attempts $releaseLookup.attempts
    exit 1
}

$downloadedFile = Get-Item -LiteralPath $tempPath
if ([long]$downloadedFile.Length -ne $expectedSizeBytes) {
    Write-Fail "Downloaded file size mismatch. Expected $expectedSizeBytes bytes, got $($downloadedFile.Length)."
    Remove-Item -LiteralPath $tempPath -Force
    exit 1
}

if (Test-Path -LiteralPath $resolvedOutputPath -PathType Leaf) {
    Remove-Item -LiteralPath $resolvedOutputPath -Force
}
Move-Item -LiteralPath $tempPath -Destination $resolvedOutputPath

$downloadedHash = Get-FileSha256 -FilePath $resolvedOutputPath
$expectedHash = ""
if (-not [string]::IsNullOrWhiteSpace($githubDigest) -and $githubDigest -match '^sha256:(?<hash>[0-9A-Fa-f]{64})$') {
    $expectedHash = [string]$Matches["hash"].ToUpperInvariant()
}
elseif (-not [string]::IsNullOrWhiteSpace($knownHash)) {
    $expectedHash = $knownHash
}

Write-Host ""
Write-Status "Download successful."
Write-Status "Saved to      : $resolvedOutputPath"
Write-Status "File size     : $($downloadedFile.Length) bytes"
Write-Status "SHA256        : $downloadedHash"

if (-not [string]::IsNullOrWhiteSpace($expectedHash)) {
    if ([System.StringComparer]::OrdinalIgnoreCase.Equals($expectedHash, $downloadedHash)) {
        Write-Status "Hash check    : PASS"
    }
    else {
        Write-Fail "Hash check failed. Expected $expectedHash but got $downloadedHash."
        exit 1
    }
}
else {
    Write-Warning-Custom "No authoritative SHA256 was available from GitHub digest metadata or the local known-hash table."
}

Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Record the real-ROM arTIfiCE baseline:"
Write-Host "       tools\emulator\record-artifice-baseline.ps1"
Write-Host "  2. Verify the jailbroken end-to-end lane:"
Write-Host "       tools\emulator\run-artifice-e2e-verification.ps1"
Write-Host "  3. Run the dedicated artifice smoke suite:"
Write-Host "       tools\emulator\run-artifice-smoke.ps1 -Mode Verify"

exit 0
