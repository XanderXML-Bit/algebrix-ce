[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$DestinationPath = "",

    [Parameter(Mandatory = $false)]
    [string[]]$SearchRoots = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$artificeModulePath = Join-Path $PSScriptRoot "CalcEmuArtificePipeline.psm1"
if (-not (Test-Path -LiteralPath $artificeModulePath -PathType Leaf)) {
    throw "arTIfiCE pipeline module not found at '$artificeModulePath'."
}
Import-Module -Name $artificeModulePath -Force

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$resolvedDestinationPath = if ([string]::IsNullOrWhiteSpace($DestinationPath)) {
    Join-Path $repoRoot "tools\emulator\assets\artifice-v2.8xp"
}
else {
    Resolve-CalcEmuOptionalPath -PathValue $DestinationPath
}

$effectiveSearchRoots = if (@($SearchRoots).Count -gt 0) {
    @($SearchRoots | ForEach-Object { Resolve-CalcEmuOptionalPath -PathValue ([string]$_) })
}
else {
    @(
        (Join-Path $HOME "Downloads"),
        (Join-Path $HOME "Desktop"),
        (Get-Location).Path
    )
}

$resolution = Resolve-CalcEmuArtificeAsset `
    -PreferredAssetPath "" `
    -SearchRoots $effectiveSearchRoots

if ($resolution.assets_present) {
    Ensure-CalcEmuParentDirectory -Path $resolvedDestinationPath
    Copy-Item -LiteralPath ([string]$resolution.path) -Destination $resolvedDestinationPath -Force
    Write-Host ("FOUND {0}" -f [string]$resolution.path)
    Write-Host ("COPIED {0}" -f $resolvedDestinationPath)
    exit 0
}

Write-Host "MISSING"
Write-Host "Download arTIfiCE v2 from https://github.com/YvanTT/arTIfiCE/releases and place the .8xp file in your Downloads folder"
exit 1
