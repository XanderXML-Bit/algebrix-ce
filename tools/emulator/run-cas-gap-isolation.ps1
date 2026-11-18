[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RomPath,

    [Parameter(Mandatory = $false)]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputDir = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BaselineSummaryPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$ConvertPngIfAvailable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$wrapperPath = Join-Path $PSScriptRoot "run-cemu-autotest.ps1"
$artifactModulePath = Join-Path $PSScriptRoot "CalcEmuSplitSuiteArtifacts.psm1"
$scriptStart = Get-Date

if (-not (Test-Path -LiteralPath $wrapperPath -PathType Leaf)) {
    throw "Wrapper script '$wrapperPath' was not found."
}
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    throw "Artifact module '$artifactModulePath' was not found."
}

Import-Module -Name $artifactModulePath -Force

function Write-Utf8NoBomFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Content
    )

    $directory = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($directory)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

function Get-CaseDefinitions {
    $commonPrefix = @(
        "action|launch",
        "delay|4000",
        "key|clear",
        "delay|600",
        "key|clear",
        "delay|600"
    )

    return @(
        [ordered]@{
            name = "deriv_minimal"
            description = "Launcher plus CECALC only; reproduces d/dx(x^2) with the pass54 token path."
            baseline_case = "deriv_current"
            hash_prefix = "deriv_current"
            module_programs = @("CECALC")
            sequence = $commonPrefix + @(
                "key|alpha",
                "key|-1",
                "key|/",
                "key|alpha",
                "key|-1",
                "key|xton",
                "key|(",
                "key|xton",
                "key|^",
                "key|2",
                "key|)",
                "delay|400",
                "hash|deriv_current_pre",
                "key|enter",
                "delay|3000",
                "hash|deriv_current_t3",
                "delay|7000",
                "hash|deriv_current_t10"
            )
        },
        [ordered]@{
            name = "lim_minimal"
            description = "Launcher plus limit router/base/adv/cont/rel only; reproduces lim(sin(x)/x,x,0)."
            baseline_case = "lim_current"
            hash_prefix = "lim_current"
            module_programs = @("CECLIM", "CECLMB", "CECLIA", "CECLIC", "CECLIR")
            sequence = $commonPrefix + @(
                "key|alpha",
                "key|)",
                "key|alpha",
                "key|^2",
                "key|alpha",
                "key|/",
                "key|(",
                "key|sin",
                "key|xton",
                "key|)",
                "key|/",
                "key|xton",
                "key|,",
                "key|xton",
                "key|,",
                "key|0",
                "key|)",
                "delay|400",
                "hash|lim_current_pre",
                "key|enter",
                "delay|3000",
                "hash|lim_current_t3",
                "delay|7000",
                "hash|lim_current_t10"
            )
        },
        [ordered]@{
            name = "expand_minimal_control"
            description = "Launcher plus CEALG/CEALGF control; proves a routed CAS success path still works with a stripped transfer set."
            baseline_case = "expand_control"
            hash_prefix = "expand_control"
            module_programs = @("CEALG", "CEALGF")
            sequence = $commonPrefix + @(
                "key|alpha",
                "key|sin",
                "key|xton",
                "key|alpha",
                "key|8",
                "key|alpha",
                "key|math",
                "key|alpha",
                "key|log",
                "key|alpha",
                "key|-1",
                "key|(",
                "key|(",
                "key|xton",
                "key|+",
                "key|1",
                "key|)",
                "key|^",
                "key|2",
                "key|)",
                "delay|400",
                "hash|expand_control_pre",
                "key|enter",
                "delay|3000",
                "hash|expand_control_t3",
                "delay|7000",
                "hash|expand_control_t10"
            )
        }
    )
}

function Get-BaselineTimelineMap {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$Path = ""
    )

    $map = @{}
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $map
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "BaselineSummaryPath '$Path' was not found."
    }

    $content = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    foreach ($property in ($content | Get-Member -MemberType NoteProperty)) {
        $values = @()
        foreach ($entry in @($content.($property.Name))) {
            if ($null -ne $entry) {
                $values += ([string]$entry).PadLeft(8, "0").ToUpperInvariant()
            }
        }
        $map[$property.Name] = $values
    }

    return $map
}

function Get-ArtifactLookup {
    param(
        [Parameter(Mandatory = $true)]
        [object]$OfficialArtifacts
    )

    $lookup = @{}
    foreach ($artifact in @($OfficialArtifacts.artifacts)) {
        $artifactPath = [string]$artifact.artifact_path
        if ([string]::IsNullOrWhiteSpace($artifactPath)) {
            continue
        }

        $basename = [System.IO.Path]::GetFileNameWithoutExtension($artifactPath).ToUpperInvariant()
        if (-not $lookup.ContainsKey($basename)) {
            $lookup[$basename] = $artifactPath
        }
    }

    return $lookup
}

function Resolve-CaseModulePaths {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Lookup,

        [Parameter(Mandatory = $true)]
        [string[]]$ModulePrograms
    )

    $resolved = New-Object System.Collections.Generic.List[string]
    foreach ($moduleProgram in @($ModulePrograms)) {
        $key = ([string]$moduleProgram).Trim().ToUpperInvariant()
        if ([string]::IsNullOrWhiteSpace($key)) {
            continue
        }
        if (-not $Lookup.ContainsKey($key)) {
            throw "Official split-suite artifact for module '$moduleProgram' was not found."
        }
        $resolved.Add([string]$Lookup[$key])
    }

    return $resolved.ToArray()
}

function New-CaseTemplateObject {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Case
    )

    $hashes = [ordered]@{}
    foreach ($suffix in @("pre", "t3", "t10")) {
        $hashName = "{0}_{1}" -f $Case.hash_prefix, $suffix
        $hashes[$hashName] = [ordered]@{
            description = "{0} {1}" -f $Case.hash_prefix, $suffix
            start = "vram_start"
            size = "vram_16_size"
            expected_CRCs = @("00000000")
            timeout = 15000
        }
    }

    return [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "CALCENG"
            isASM = $true
        }
        delay_after_key = 250
        delay_after_step = 600
        sequence = @($Case.sequence)
        hashes = $hashes
    }
}

function Get-ObservedCrcs {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines
    )

    $observed = [ordered]@{}
    $pattern = "Hash\s*#(?<hash>[A-Za-z0-9_]+).*?\bgot\s+(?<crc>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{1,8})\b"

    foreach ($line in @($Lines)) {
        if ([string]::IsNullOrWhiteSpace([string]$line)) {
            continue
        }

        $match = [regex]::Match([string]$line, $pattern)
        if (-not $match.Success) {
            continue
        }

        $name = [string]$match.Groups["hash"].Value
        $crc = [string]$match.Groups["crc"].Value
        if ($crc.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
            $crc = $crc.Substring(2)
        }
        if (-not $observed.Contains($name)) {
            $observed[$name] = $crc.PadLeft(8, "0").ToUpperInvariant()
        }
    }

    return $observed
}

function Get-TimelineForCase {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Case,

        [Parameter(Mandatory = $true)]
        [System.Collections.Specialized.OrderedDictionary]$ObservedCrcs
    )

    $timeline = [ordered]@{}
    foreach ($suffix in @("pre", "t3", "t10")) {
        $hashName = "{0}_{1}" -f $Case.hash_prefix, $suffix
        $timeline[$suffix] = if ($ObservedCrcs.Contains($hashName)) {
            [string]$ObservedCrcs[$hashName]
        }
        else {
            ""
        }
    }

    return $timeline
}

function Get-FfmpegPath {
    $ffmpeg = Get-Command -Name "ffmpeg.exe" -ErrorAction SilentlyContinue
    if ($null -ne $ffmpeg) {
        return $ffmpeg.Source
    }

    $ffmpeg = Get-Command -Name "ffmpeg" -ErrorAction SilentlyContinue
    if ($null -ne $ffmpeg) {
        return $ffmpeg.Source
    }

    return ""
}

function Convert-PpmArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CaseDir,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$FfmpegPath = ""
    )

    $converted = New-Object System.Collections.Generic.List[string]
    if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
        return $converted.ToArray()
    }

    foreach ($ppmPath in @(Get-ChildItem -LiteralPath $CaseDir -File -Filter "*.ppm" | Sort-Object -Property Name)) {
        $pngPath = [System.IO.Path]::ChangeExtension($ppmPath.FullName, ".png")
        & $FfmpegPath -y -i $ppmPath.FullName $pngPath *> $null
        if (Test-Path -LiteralPath $pngPath -PathType Leaf) {
            $converted.Add((Resolve-Path -LiteralPath $pngPath).Path)
        }
    }

    return $converted.ToArray()
}

function Remove-StaleArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CaseDir
    )

    $patterns = @(
        "*.ppm",
        "*.png",
        "failure_hash*_dump.bin",
        "*.generated.json",
        "*.template.json",
        "*.log.txt",
        "modules.txt"
    )

    foreach ($pattern in $patterns) {
        foreach ($item in @(Get-ChildItem -LiteralPath $CaseDir -File -Filter $pattern -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $item.FullName -Force -ErrorAction SilentlyContinue
        }
    }
}

if ([string]::IsNullOrWhiteSpace($AutotesterPath)) {
    throw "AutotesterPath is required (pass -AutotesterPath or set CEMU_AUTOTESTER_PATH)."
}

$resolvedRom = (Resolve-Path -LiteralPath $RomPath).Path
$resolvedAutotester = (Resolve-Path -LiteralPath $AutotesterPath).Path
$resolvedOutputDir = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    Join-Path $repoRoot "_research_tmp\pass55_cas_gap_isolation"
}
else {
    [void](New-Item -ItemType Directory -Path $OutputDir -Force)
    (Resolve-Path -LiteralPath $OutputDir).Path
}

[void](New-Item -ItemType Directory -Path $resolvedOutputDir -Force)

$officialArtifacts = Resolve-CalcEmuOfficialSplitSuiteArtifacts -RepoRoot $repoRoot
if (@($officialArtifacts.errors).Count -gt 0) {
    throw (@($officialArtifacts.errors) -join " | ")
}
foreach ($warning in @($officialArtifacts.warnings)) {
    Write-Warning ([string]$warning)
}
if ([string]::IsNullOrWhiteSpace([string]$officialArtifacts.program_path)) {
    throw "Official split-suite launcher artifact was not resolved."
}

$launcherPath = [string]$officialArtifacts.program_path
$artifactLookup = Get-ArtifactLookup -OfficialArtifacts $officialArtifacts
$baselineMap = Get-BaselineTimelineMap -Path $BaselineSummaryPath
$ffmpegPath = if ($ConvertPngIfAvailable) { Get-FfmpegPath } else { "" }
$summaryCases = New-Object System.Collections.Generic.List[object]

Write-Host ""
Write-Host "============================================================"
Write-Host "  CalcEngine CAS Gap Isolation Runner"
Write-Host ("  ROM            : {0}" -f $resolvedRom)
Write-Host ("  Autotester     : {0}" -f $resolvedAutotester)
Write-Host ("  Launcher       : {0}" -f $launcherPath)
Write-Host ("  OutputDir      : {0}" -f $resolvedOutputDir)
Write-Host ("  Pass54 summary : {0}" -f $(if ([string]::IsNullOrWhiteSpace($BaselineSummaryPath)) { "(none)" } else { $BaselineSummaryPath }))
Write-Host ("  PNG convert    : {0}" -f $(if ([string]::IsNullOrWhiteSpace($ffmpegPath)) { "disabled" } else { $ffmpegPath }))
Write-Host "============================================================"
Write-Host ""

foreach ($case in @(Get-CaseDefinitions)) {
    $caseDir = Join-Path $resolvedOutputDir $case.name
    [void](New-Item -ItemType Directory -Path $caseDir -Force)
    Remove-StaleArtifacts -CaseDir $caseDir

    $modulePaths = Resolve-CaseModulePaths -Lookup $artifactLookup -ModulePrograms $case.module_programs
    $moduleListPath = Join-Path $caseDir "modules.txt"
    $templatePath = Join-Path $caseDir ($case.name + ".template.json")
    $generatedPath = Join-Path $caseDir ($case.name + ".generated.json")
    $logPath = Join-Path $caseDir ($case.name + ".log.txt")

    Write-Utf8NoBomFile -Path $moduleListPath -Content ((@($modulePaths) -join [Environment]::NewLine))
    $templateJson = (New-CaseTemplateObject -Case $case) | ConvertTo-Json -Depth 12
    Write-Utf8NoBomFile -Path $templatePath -Content $templateJson

    Write-Host ("=== {0} ===" -f $case.name)
    Write-Host ("  Modules: {0}" -f (@($case.module_programs) -join ", "))

    $powershellPath = (Get-Command -Name "powershell.exe" -ErrorAction Stop).Source
    $captured = @()
    Push-Location $caseDir
    try {
        $captured = & $powershellPath `
            -NoProfile `
            -NonInteractive `
            -ExecutionPolicy Bypass `
            -File $wrapperPath `
            -RomPath $resolvedRom `
            -AutotesterPath $resolvedAutotester `
            -ProgramPath $launcherPath `
            -ModuleProgramPathsFile $moduleListPath `
            -TemplatePath $templatePath `
            -OutputConfigPath $generatedPath `
            2>&1
        $caseExitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    $logLines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($captured)) {
        if ($null -ne $entry) {
            $logLines.Add([string]$entry)
        }
    }
    Write-Utf8NoBomFile -Path $logPath -Content (($logLines.ToArray()) -join [Environment]::NewLine)

    $observed = Get-ObservedCrcs -Lines $logLines.ToArray()
    $timeline = Get-TimelineForCase -Case $case -ObservedCrcs $observed
    $baselineTimeline = if ($baselineMap.ContainsKey($case.baseline_case)) {
        @($baselineMap[$case.baseline_case])
    }
    else {
        @()
    }
    $observedTimelineList = @($timeline.pre, $timeline.t3, $timeline.t10) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    $matchesBaseline = $false
    if ($baselineTimeline.Count -gt 0 -and $baselineTimeline.Count -eq $observedTimelineList.Count) {
        $matchesBaseline = [string]::Join("|", $baselineTimeline) -eq [string]::Join("|", $observedTimelineList)
    }

    $pngArtifacts = Convert-PpmArtifacts -CaseDir $caseDir -FfmpegPath $ffmpegPath
    $artifactFiles = Get-ChildItem -LiteralPath $caseDir -File | Sort-Object -Property Name | Select-Object -ExpandProperty Name
    $capturedAutotesterArtifacts = @($artifactFiles | Where-Object {
        $_ -notin @(
            [System.IO.Path]::GetFileName($generatedPath),
            [System.IO.Path]::GetFileName($logPath),
            [System.IO.Path]::GetFileName($templatePath),
            [System.IO.Path]::GetFileName($moduleListPath)
        )
    })

    Write-Host ("  Exit code: {0}" -f $caseExitCode)
    Write-Host ("  Observed : {0} -> {1} -> {2}" -f $timeline.pre, $timeline.t3, $timeline.t10)
    if ($baselineTimeline.Count -gt 0) {
        Write-Host ("  Pass54   : {0}" -f ($baselineTimeline -join " -> "))
        Write-Host ("  Match    : {0}" -f $matchesBaseline)
    }
    Write-Host ""

    $summaryCases.Add([ordered]@{
        name = [string]$case.name
        description = [string]$case.description
        baseline_case = [string]$case.baseline_case
        module_programs = @($case.module_programs)
        module_paths = @($modulePaths)
        module_count = [int]@($modulePaths).Count
        transfer_file_count = [int](@($modulePaths).Count + 1)
        wrapper_exit_code = [int]$caseExitCode
        observed_timeline = [ordered]@{
            pre = [string]$timeline.pre
            t3 = [string]$timeline.t3
            t10 = [string]$timeline.t10
        }
        pass54_baseline_timeline = @($baselineTimeline)
        matches_pass54_baseline = [bool]$matchesBaseline
        log_path = $logPath
        template_path = $templatePath
        generated_config_path = $generatedPath
        module_list_path = $moduleListPath
        png_artifacts = @($pngArtifacts)
        captured_autotester_artifacts = @($capturedAutotesterArtifacts)
        emitted_autotester_artifacts = ([bool](@($capturedAutotesterArtifacts).Count -gt 0))
        artifact_files = @($artifactFiles)
    })
}

$durationSeconds = [math]::Round(((Get-Date) - $scriptStart).TotalSeconds, 1)
$summary = [ordered]@{
    schema_version = 1
    runner = "run-cas-gap-isolation.ps1"
    timestamp = (Get-Date -Format "o")
    rom = $resolvedRom
    autotester = $resolvedAutotester
    launcher = $launcherPath
    output_dir = $resolvedOutputDir
    baseline_summary_path = if ([string]::IsNullOrWhiteSpace($BaselineSummaryPath)) { "" } else { (Resolve-Path -LiteralPath $BaselineSummaryPath).Path }
    png_conversion = [ordered]@{
        requested = [bool]$ConvertPngIfAvailable
        ffmpeg_path = [string]$ffmpegPath
        enabled = (-not [string]::IsNullOrWhiteSpace($ffmpegPath))
    }
    cases = $summaryCases.ToArray()
    duration_seconds = $durationSeconds
}

$summaryPath = Join-Path $resolvedOutputDir "summary.json"
Write-Utf8NoBomFile -Path $summaryPath -Content ($summary | ConvertTo-Json -Depth 10)

Write-Host ("Summary: {0}" -f $summaryPath)
