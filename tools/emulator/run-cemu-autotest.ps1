param(
    [Parameter(Mandatory = $true)]
    [string]$RomPath,

    [Parameter(Mandatory = $false)]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$ProgramPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyCollection()]
    [string[]]$ModuleProgramPaths = @(),

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ModuleProgramPathsFile = "",

    [Parameter(Mandatory = $false)]
    [string]$TemplatePath = "",

    [Parameter(Mandatory = $false)]
    [string]$OutputConfigPath = ""
)

$ErrorActionPreference = "Stop"

$artifactModulePath = Join-Path $PSScriptRoot "CalcEmuSplitSuiteArtifacts.psm1"
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    throw "Artifact discovery module '$artifactModulePath' does not exist."
}
Import-Module -Name $artifactModulePath -Force

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module '$pipelineSupportModulePath' does not exist."
}
Import-Module -Name $pipelineSupportModulePath -Force

function Write-Utf8NoBomFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Content
    )

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

function Resolve-ExistingFilePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $resolvedPath = Resolve-CalcEmuPipelinePath -PathValue $PathValue
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        throw "$Label '$resolvedPath' does not exist."
    }

    return (Resolve-Path -LiteralPath $resolvedPath).ProviderPath
}

function Expand-DelimitedArgumentList {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [string[]]$Values = @()
    )

    $expanded = New-Object System.Collections.Generic.List[string]
    foreach ($value in @($Values)) {
        if ($null -eq $value) {
            continue
        }

        $segments = [string]$value -split "[;,\r\n]", 0, [System.StringSplitOptions]::None
        foreach ($segment in $segments) {
            $expanded.Add([string]$segment)
        }
    }

    return ,$expanded.ToArray()
}

function Resolve-WrapperArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ExplicitProgramPath = "",

        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [string[]]$ExplicitModuleProgramPaths = @()
    )

    $warnings = New-Object System.Collections.Generic.List[string]
    $errors = New-Object System.Collections.Generic.List[string]

    $officialArtifacts = $null
    try {
        $officialArtifacts = Resolve-CalcEmuOfficialSplitSuiteArtifacts -RepoRoot $RepoRoot
    }
    catch {
        $warnings.Add("Official split-suite artifact discovery failed: $($_.Exception.Message)")
    }

    if ($null -ne $officialArtifacts) {
        foreach ($warning in @($officialArtifacts.warnings)) {
            $warnings.Add([string]$warning)
        }
    }

    $resolvedProgramPath = ""
    $programSource = "explicit"
    if (-not [string]::IsNullOrWhiteSpace($ExplicitProgramPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitProgramPath -PathType Leaf)) {
            $errors.Add("ProgramPath '$ExplicitProgramPath' does not exist.")
        }
        else {
            $resolvedProgramPath = (Resolve-Path -LiteralPath $ExplicitProgramPath).ProviderPath
        }
    }
    elseif ($null -ne $officialArtifacts -and -not [string]::IsNullOrWhiteSpace([string]$officialArtifacts.program_path)) {
        $programSource = [string]$officialArtifacts.source
        $resolvedProgramPath = [string]$officialArtifacts.program_path
    }
    else {
        $programSource = "legacy_fallback"
        $fallbackCandidates = @(
            (Join-Path $RepoRoot "bin\launcher\CALCENG.8xp"),
            (Join-Path $RepoRoot "bin\CALCENG.8xp")
        )
        foreach ($candidate in $fallbackCandidates) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $resolvedProgramPath = (Resolve-Path -LiteralPath $candidate).ProviderPath
                break
            }
        }
        if ([string]::IsNullOrWhiteSpace($resolvedProgramPath)) {
            $errors.Add("ProgramPath was not provided and no CalcEngine launcher artifact was found under bin\\launcher or bin\\.")
        }
    }

    $resolvedModuleProgramPaths = New-Object System.Collections.Generic.List[string]
    $moduleSource = "explicit"
    $normalizedExplicitModules = Expand-DelimitedArgumentList -Values $ExplicitModuleProgramPaths
    if ($normalizedExplicitModules.Count -gt 0) {
        foreach ($moduleProgramPath in $normalizedExplicitModules) {
            $trimmedPath = ([string]$moduleProgramPath).Trim()
            if ([string]::IsNullOrWhiteSpace($trimmedPath)) {
                continue
            }

            if (-not (Test-Path -LiteralPath $trimmedPath -PathType Leaf)) {
                $errors.Add("ModuleProgramPath '$trimmedPath' does not exist.")
                continue
            }

            $resolvedModuleProgramPaths.Add((Resolve-Path -LiteralPath $trimmedPath).ProviderPath)
        }
    }
    elseif ($null -ne $officialArtifacts) {
        $moduleSource = [string]$officialArtifacts.source
        foreach ($moduleProgramPath in @($officialArtifacts.module_paths)) {
            $resolvedModuleProgramPaths.Add([string]$moduleProgramPath)
        }
    }

    $transferFiles = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    foreach ($candidatePath in @($resolvedProgramPath) + @($resolvedModuleProgramPaths.ToArray())) {
        $trimmedPath = ([string]$candidatePath).Trim()
        if ([string]::IsNullOrWhiteSpace($trimmedPath)) {
            continue
        }

        $dedupeKey = $trimmedPath.ToLowerInvariant()
        if ($seen.ContainsKey($dedupeKey)) {
            continue
        }

        $seen[$dedupeKey] = $true
        $transferFiles.Add($trimmedPath)
    }

    return [ordered]@{
        program_path = $resolvedProgramPath
        program_source = $programSource
        module_paths = $resolvedModuleProgramPaths.ToArray()
        module_source = $moduleSource
        transfer_files = $transferFiles.ToArray()
        warnings = $warnings.ToArray()
        errors = $errors.ToArray()
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).ProviderPath

if ([string]::IsNullOrWhiteSpace($TemplatePath)) {
    $TemplatePath = Join-Path $repoRoot "tests\emulator\autotest.smoke.template.json"
}
if ([string]::IsNullOrWhiteSpace($OutputConfigPath)) {
    $OutputConfigPath = Join-Path $repoRoot "tests\emulator\autotest.smoke.generated.json"
}

$moduleProgramPathInputs = New-Object System.Collections.Generic.List[string]
foreach ($moduleProgramPath in @($ModuleProgramPaths)) {
    $moduleProgramPathInputs.Add([string]$moduleProgramPath)
}

if (-not [string]::IsNullOrWhiteSpace($ModuleProgramPathsFile)) {
    if (-not (Test-Path -LiteralPath $ModuleProgramPathsFile -PathType Leaf)) {
        throw "ModuleProgramPathsFile '$ModuleProgramPathsFile' does not exist."
    }

    foreach ($moduleProgramPath in @(Get-Content -LiteralPath $ModuleProgramPathsFile)) {
        if ($null -ne $moduleProgramPath) {
            $moduleProgramPathInputs.Add([string]$moduleProgramPath)
        }
    }
}

$artifactResolution = Resolve-WrapperArtifacts `
    -RepoRoot $repoRoot `
    -ExplicitProgramPath $ProgramPath `
    -ExplicitModuleProgramPaths $moduleProgramPathInputs.ToArray()

if ($artifactResolution.errors.Count -gt 0) {
    throw ($artifactResolution.errors -join " | ")
}

foreach ($warning in @($artifactResolution.warnings)) {
    Write-Warning ([string]$warning)
}

$resolvedRom = [string](Resolve-CalcEmuRomPath -RomPath $RomPath).path
$resolvedAutotesterInfo = if (-not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
    [ordered]@{
        path = Resolve-ExistingFilePath -PathValue $AutotesterPath -Label "Autotester path"
        source = "explicit"
    }
}
else {
    Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
}
$resolvedAutotester = [string]$resolvedAutotesterInfo.path
$resolvedProgram = [string]$artifactResolution.program_path
$resolvedTemplate = (Resolve-Path $TemplatePath).ProviderPath

$config = Get-Content -LiteralPath $resolvedTemplate -Raw | ConvertFrom-Json
$config.rom = $resolvedRom
$config.transfer_files = @($artifactResolution.transfer_files)
$config.target.name = [System.IO.Path]::GetFileNameWithoutExtension($resolvedProgram)

$configJson = $config | ConvertTo-Json -Depth 12
Write-Utf8NoBomFile -Path $OutputConfigPath -Content $configJson

Write-Host "ROM: $resolvedRom"
Write-Host "Program: $resolvedProgram"
Write-Host "Program source: $($artifactResolution.program_source)"
Write-Host "Modules transferred: $(@($artifactResolution.module_paths).Count)"
Write-Host "Module source: $($artifactResolution.module_source)"
Write-Host "Transfer files: $(@($artifactResolution.transfer_files).Count)"
Write-Host "Autotester: $resolvedAutotester"
Write-Host "Autotester source: $($resolvedAutotesterInfo.source)"
Write-Host "Config: $OutputConfigPath"

& $resolvedAutotester $OutputConfigPath
$exitCode = $LASTEXITCODE

Write-Host "Autotester exit code: $exitCode"
exit $exitCode
