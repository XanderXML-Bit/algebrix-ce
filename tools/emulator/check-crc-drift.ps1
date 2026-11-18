[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "C:\Users\user\Downloads\CalcEngine",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$BaselinedPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RunnerSummaryPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ComprehensiveRunnerPath = ""
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
        exit_code = [int]$exitCode
        success = ([int]$exitCode -eq 0)
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
    }
}

function Normalize-CrcValue {
    param([AllowNull()][string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    $crc = [string]$Value
    if ($crc.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        $crc = $crc.Substring(2)
    }
    return $crc.PadLeft(8, '0').ToUpperInvariant()
}

function Get-HashDefinitionMap {
    param([Parameter(Mandatory = $true)][string]$ConfigPath)

    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    $map = @{}
    $hashProps = @($config.hashes.PSObject.Properties |
                   Where-Object { $_.MemberType -eq "NoteProperty" })
    foreach ($prop in $hashProps) {
        $map[[string]$prop.Name] = [ordered]@{
            description   = [string]$prop.Value.description
            expected_crcs = [string[]]@($prop.Value.expected_CRCs | ForEach-Object { Normalize-CrcValue -Value $_ })
        }
    }
    return $map
}

function Get-MismatchesFromOutput {
    param(
        [Parameter(Mandatory = $true)][string[]]$Lines,
        [Parameter(Mandatory = $true)][hashtable]$HashDefinitionMap
    )

    $pattern = 'Hash\s*#(?<hash>[A-Za-z0-9_]+).*?\bgot\s+(?<crc>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{1,8})\b'
    $seen = @{}
    $mismatches = New-Object System.Collections.Generic.List[object]

    foreach ($line in @($Lines)) {
        $match = [regex]::Match([string]$line, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if (-not $match.Success) {
            continue
        }

        $hashName = [string]$match.Groups["hash"].Value
        if ($seen.ContainsKey($hashName)) {
            continue
        }
        $seen[$hashName] = $true

        $def = $null
        if ($HashDefinitionMap.ContainsKey($hashName)) {
            $def = $HashDefinitionMap[$hashName]
        }

        $mismatches.Add([ordered]@{
            hash_name = $hashName
            description = if ($def) { [string]$def.description } else { "" }
            expected_crcs = if ($def) { [string[]]@($def.expected_crcs) } else { @() }
            observed_crc = Normalize-CrcValue -Value ([string]$match.Groups["crc"].Value)
            output_line = [string]$line
        })
    }

    return $mismatches.ToArray()
}

$repoRoot = Resolve-RepoRoot
$resolvedRunnerPath = if ([string]::IsNullOrWhiteSpace($ComprehensiveRunnerPath)) {
    Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $ComprehensiveRunnerPath
}
$resolvedBaselinedPath = if ([string]::IsNullOrWhiteSpace($BaselinedPath)) {
    Join-Path $repoRoot "tests\emulator\autotest.comprehensive.baselined.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $BaselinedPath
}
$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\check-crc-drift.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $OutputPath
}
$resolvedRunnerSummaryPath = if ([string]::IsNullOrWhiteSpace($RunnerSummaryPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\check-crc-drift.runner.summary.json"
} else {
    Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $RunnerSummaryPath
}
$resolvedRomPath = Resolve-OptionalPath -RepoRoot $repoRoot -PathValue $RomPath

$summary = [ordered]@{
    schema_version = 1
    runner = "check-crc-drift.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    options = [ordered]@{
        rom_path = $resolvedRomPath
        autotester_path = $AutotesterPath
        downloads_dir = $DownloadsDir
        baselined_path = $resolvedBaselinedPath
        comprehensive_runner_path = $resolvedRunnerPath
    }
    verify_invocation = $null
    verify_summary_path = $resolvedRunnerSummaryPath
    baseline_hash_count = 0
    drift_count = 0
    drifts = @()
}

try {
    $romResolution = Resolve-CalcEmuRomPath -RomPath $RomPath
    $autotesterResolution = Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
    $resolvedRomPath = [string]$romResolution.path
    $resolvedAutotesterPath = [string]$autotesterResolution.path
    $summary.options.rom_path = $resolvedRomPath
    $summary.options.autotester_path = $resolvedAutotesterPath

    if (-not (Test-Path -LiteralPath $resolvedRunnerPath -PathType Leaf)) {
        throw "Comprehensive autotest runner was not found at '$resolvedRunnerPath'."
    }
    if (-not (Test-Path -LiteralPath $resolvedBaselinedPath -PathType Leaf)) {
        throw "Baselined comprehensive suite was not found at '$resolvedBaselinedPath'."
    }

    $hashDefinitionMap = Get-HashDefinitionMap -ConfigPath $resolvedBaselinedPath
    $summary.baseline_hash_count = $hashDefinitionMap.Count

    $verifyInvocation = Invoke-PowerShellFile -ScriptPath $resolvedRunnerPath -Arguments @(
        "-RomPath", $resolvedRomPath,
        "-AutotesterPath", $resolvedAutotesterPath,
        "-EmulatorBackend", "Autotester",
        "-Mode", "Verify",
        "-DownloadsDir", $DownloadsDir,
        "-BaselinedPath", $resolvedBaselinedPath,
        "-SummaryPath", $resolvedRunnerSummaryPath
    )
    $summary.verify_invocation = $verifyInvocation

    $runnerSummary = $null
    if (Test-Path -LiteralPath $resolvedRunnerSummaryPath -PathType Leaf) {
        $runnerSummary = Get-Content -LiteralPath $resolvedRunnerSummaryPath -Raw | ConvertFrom-Json
        $summary.runner_summary = $runnerSummary
    }

    $drifts = @()
    if ($runnerSummary -and
        $runnerSummary.verify_stage -and
        [int]$runnerSummary.verify_stage.mismatch_count -gt 0) {
        $drifts = @($runnerSummary.verify_stage.mismatches)
    } elseif (-not $verifyInvocation.success) {
        $drifts = Get-MismatchesFromOutput -Lines $verifyInvocation.output_lines -HashDefinitionMap $hashDefinitionMap
    }

    $summary.drifts = @($drifts)
    $summary.drift_count = @($drifts).Count
    $summary.success = ($summary.drift_count -eq 0 -and $verifyInvocation.success)
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
}
catch {
    $summary.success = $false
    $summary.exit_code = 1
    $summary.error = $_.Exception.Message
}
finally {
    $summary.duration_seconds = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
}

if ($summary.success) {
    Write-Host ("PASS: no comprehensive CRC drift detected across {0} hashes." -f $summary.baseline_hash_count)
    Write-Host ("Summary: {0}" -f $resolvedOutputPath)
    exit 0
}

if ($summary.drift_count -gt 0) {
    Write-Host ("FAIL: detected {0} comprehensive CRC drift(s)." -f $summary.drift_count)
    foreach ($drift in @($summary.drifts)) {
        $expectedValues = @()
        if ($drift.PSObject.Properties.Name -contains "expected_crcs") {
            $expectedValues = @($drift.expected_crcs)
        }
        $expectedText = if ($expectedValues.Count -gt 0) { $expectedValues -join ", " } else { "<unknown>" }
        $hashName = if ($drift.PSObject.Properties.Name -contains "hash_name") { [string]$drift.hash_name } else { "<unknown>" }
        $observedText = if ($drift.PSObject.Properties.Name -contains "observed_crc") { [string]$drift.observed_crc } else { "<unknown>" }
        Write-Host ("  - {0}: expected [{1}] got {2}" -f $hashName, $expectedText, $observedText)
        if (($drift.PSObject.Properties.Name -contains "description") -and
            -not [string]::IsNullOrWhiteSpace([string]$drift.description)) {
            Write-Host ("    {0}" -f [string]$drift.description)
        }
    }
    Write-Host ("Summary: {0}" -f $resolvedOutputPath)
    exit 1
}

Write-Error ("FAIL: CRC drift check could not complete. Summary: {0}" -f $resolvedOutputPath)
exit 1
