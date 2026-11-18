param(
    [Parameter(Mandatory = $true)]
    [string]$Keys,

    [Parameter(Mandatory = $false)]
    [ValidateSet("json", "text")]
    [string]$Format = "json",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RomPath = $env:CEMU_ROM_PATH,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ProgramPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyCollection()]
    [string[]]$ModuleProgramPaths = @(),

    [Parameter(Mandatory = $false)]
    [ValidateSet("none", "explicit", "official_split_suite", "manifest_transfer_suite")]
    [string]$ModuleProgramPathSource = "none",

    [Parameter(Mandatory = $false)]
    [ValidateRange(1024, 1048576)]
    [int]$UsableUserRamLimitBytes = 65536,

    [Parameter(Mandatory = $false)]
    [ValidateRange(1024, 1048576)]
    [int]$ModuleLinkedBinaryLimitBytes = 51200,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArchiveRamDispatchModulePath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyCollection()]
    [string[]]$ArchiveRamModulePaths = @(),

    [Parameter(Mandatory = $false)]
    [ValidateRange(1024, 1048576)]
    [int]$ArchiveRamSwapRamCapacityBytes = 65536,

    [Parameter(Mandatory = $false)]
    [ValidateSet("none", "dispatch_load", "return_to_archive", "partial_load", "memory_pressure", "archive_full")]
    [string]$ArchiveRamFailurePhase = "none",

    [Parameter(Mandatory = $false)]
    [ValidateRange(1, 256)]
    [int]$ArchiveRamRapidSwapCycles = 1,

    [Parameter(Mandatory = $false)]
    [ValidateRange(0, 1048576)]
    [int]$ArchiveRamReservedRamBytes = 0,

    [Parameter(Mandatory = $false)]
    [switch]$ArchiveRamSimulatePartialLoad,

    [Parameter(Mandatory = $false)]
    [switch]$ForceMock,

    [Parameter(Mandatory = $false)]
    [switch]$NoMockFallback,

    [Parameter(Mandatory = $false)]
    [AllowEmptyCollection()]
    [string[]]$ExpectedVramCrcs = @(),

    [Parameter(Mandatory = $false)]
    [string]$CEmuHashName = "terminal_vram",

    [Parameter(Mandatory = $false)]
    [string]$CEmuHashStart = "vram_start",

    [Parameter(Mandatory = $false)]
    [string]$CEmuHashSize = "vram_16_size",

    [Parameter(Mandatory = $false)]
    [ValidateRange(1, 120000)]
    [int]$CEmuHashTimeoutMs = 8000,

    [Parameter(Mandatory = $false)]
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"

function Expand-DelimitedArgumentList {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [string[]]$Values = @()
    )

    $expanded = New-Object System.Collections.Generic.List[string]
    foreach ($value in @($Values)) {
        if ($null -eq $value) {
            $expanded.Add("")
            continue
        }

        $segments = [string]$value -split ";", 0, [System.StringSplitOptions]::None
        foreach ($segment in $segments) {
            $expanded.Add([string]$segment)
        }
    }

    return ,$expanded.ToArray()
}

$modulePath = Join-Path $PSScriptRoot "CalcEmuTerminalHarness.psm1"
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    Write-Error "CalcEmu terminal harness module not found at '$modulePath'."
    exit 2
}

$artifactModulePath = Join-Path $PSScriptRoot "CalcEmuSplitSuiteArtifacts.psm1"
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    Write-Error "CalcEmu split-suite artifact module not found at '$artifactModulePath'."
    exit 2
}

Import-Module -Name $modulePath -Force
Import-Module -Name $artifactModulePath -Force

$normalizedModuleProgramPaths = Expand-DelimitedArgumentList -Values $ModuleProgramPaths
$normalizedArchiveRamModulePaths = Expand-DelimitedArgumentList -Values $ArchiveRamModulePaths
$normalizedExpectedVramCrcs = Expand-DelimitedArgumentList -Values $ExpectedVramCrcs

if ($normalizedModuleProgramPaths.Count -eq 0 -and @("official_split_suite", "manifest_transfer_suite") -contains $ModuleProgramPathSource) {
    $repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).ProviderPath
    if ($ModuleProgramPathSource -eq "manifest_transfer_suite") {
        $resolvedArtifactSet = Get-CalcEmuTransferManifestArtifacts -RepoRoot $repoRoot
    }
    else {
        $resolvedArtifactSet = Resolve-CalcEmuOfficialSplitSuiteArtifacts -RepoRoot $repoRoot
    }

    if (@($resolvedArtifactSet.errors).Count -gt 0) {
        Write-Error ("Unable to resolve module program paths for source '{0}': {1}" -f $ModuleProgramPathSource, ((@($resolvedArtifactSet.errors)) -join " | "))
        exit 1
    }

    # Resolver warnings go to stderr: stdout must stay pure JSON for callers
    # that parse it (tests/archive-swap-verification.js). powershell.exe -File
    # writes Write-Warning to stdout when stdout is redirected.
    foreach ($warning in @($resolvedArtifactSet.warnings)) {
        [Console]::Error.WriteLine("WARNING: " + [string]$warning)
    }

    $normalizedModuleProgramPaths = @($resolvedArtifactSet.module_paths)
}

try {
    $result = Invoke-CalcEmuTerminalHarness `
        -KeySequence $Keys `
        -RomPath $RomPath `
        -AutotesterPath $AutotesterPath `
        -ProgramPath $ProgramPath `
        -ModuleProgramPaths $normalizedModuleProgramPaths `
        -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
        -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes `
        -ArchiveRamDispatchModulePath $ArchiveRamDispatchModulePath `
        -ArchiveRamModulePaths $normalizedArchiveRamModulePaths `
        -ArchiveRamSwapRamCapacityBytes $ArchiveRamSwapRamCapacityBytes `
        -ArchiveRamFailurePhase $ArchiveRamFailurePhase `
        -ArchiveRamRapidSwapCycles $ArchiveRamRapidSwapCycles `
        -ArchiveRamReservedRamBytes $ArchiveRamReservedRamBytes `
        -ArchiveRamSimulatePartialLoad:$ArchiveRamSimulatePartialLoad `
        -ForceMock:$ForceMock `
        -NoMockFallback:$NoMockFallback `
        -ExpectedVramCrcs $normalizedExpectedVramCrcs `
        -CEmuHashName $CEmuHashName `
        -CEmuHashStart $CEmuHashStart `
        -CEmuHashSize $CEmuHashSize `
        -CEmuHashTimeoutMs $CEmuHashTimeoutMs
}
catch {
    Write-Error "Terminal harness execution failed: $($_.Exception.Message)"
    exit 1
}

$rendered = ""
if ($Format -eq "json") {
    $rendered = ConvertTo-CalcEmuHarnessJson -Result $result
}
else {
    $rendered = Format-CalcEmuHarnessTextOutput -Result $result
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    Set-Content -LiteralPath $OutputPath -Value $rendered -Encoding UTF8
}

Write-Output $rendered

if ($result.success) {
    exit 0
}

exit 1
