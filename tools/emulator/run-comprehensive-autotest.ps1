<#
.SYNOPSIS
    CalcEngine Comprehensive CEmu Autotester — Baseline capture and regression verify.

.DESCRIPTION
    Orchestrates the full comprehensive autotest suite against CEmu autotester.exe.

    MODES
    -----
    Baseline  — Runs the suite with sentinel CRCs (00000000), captures actual CRC values
                from CEmu output, writes tests/emulator/autotest.comprehensive.baselined.json.
    Verify    — Runs the suite using the baselined JSON and checks every hash passes.
    Both      — Baseline first, then Verify immediately after.
    Run       — Auto-detect: runs Baseline if no baselined file exists, else Verify.

    KEY NAME NOTES (for reference)
    --------------------------------
    Confirmed working key names (from existing autotest.functional.json):
      0-9, +, -, *, /, ^, ., (, ), (-)
      clear, enter, mode
    Confirmed via harness mapping:
      2nd, alpha, math, apps
    Standard CE SDK names used for CAS / function keys:
      sin, cos, tan, ln, log          — function keys
      ^2        (kb_Square, x² key)   — normal: inserts "^2"
      -1        (kb_Recip, x^-1 key) — normal: inserts "^(-1)"
      xton      (kb_GraphVar, X,T,θ,n key) — always produces 'x'
    ALPHA mode produces letters as printed on the TI-84 Plus CE keycaps (alpha + key):
      math→a  apps→b  prgm→c  -1→d  sin→e  cos→f  tan→g  ^→h
      ^2→i  ,→j  (→k  )→l  /→m  log→n  7→o  8→p  9→q  *→r
      ln→s  4→t  5→u  6→v  -→w  sto→x  1→y  2→z

.PARAMETER RomPath
    Full path to TI-84 Plus CE ROM image (e.g. rom\Rom.rom).

.PARAMETER AutotesterPath
    Path to CEmu autotester.exe. Falls back to $env:CEMU_AUTOTESTER_PATH if omitted.

.PARAMETER Mode
    Baseline | Verify | Both | Run  (default: Run)

.PARAMETER DownloadsDir
    Directory containing the CalcEngine transfer package (.8xp + .8xv files).
    Default: C:\Users\user\Downloads\CalcEngine

.PARAMETER OutputDir
    Directory for generated JSON files. Default: tests\emulator\ in repo root.

.PARAMETER SummaryPath
    Optional path for machine-readable JSON summary output.

.EXAMPLE
    # First-time baseline capture
    .\run-comprehensive-autotest.ps1 -RomPath rom\Rom.rom -Mode Baseline

.EXAMPLE
    # Verify using an existing baseline
    .\run-comprehensive-autotest.ps1 -RomPath rom\Rom.rom -Mode Verify

.EXAMPLE
    # Auto mode (baseline if missing, verify if present)
    .\run-comprehensive-autotest.ps1 -RomPath rom\Rom.rom
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RomPath,

    [Parameter(Mandatory = $false)]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [ValidateSet("Auto", "Autotester", "CEmuHeadless")]
    [string]$EmulatorBackend = "Auto",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CEmuHeadlessPath = $env:CEMU_HEADLESS_PATH,

    [Parameter(Mandatory = $false)]
    [ValidateSet("Baseline", "Verify", "Both", "Run")]
    [string]$Mode = "Run",

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "C:\Users\user\Downloads\CalcEngine",

    [Parameter(Mandatory = $false)]
    [string]$OutputDir = "",

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
    [string]$SummaryPath = ""
)

$ErrorActionPreference = "Stop"
$script:startTime = Get-Date

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found: '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

# ─── Resolve repo root ────────────────────────────────────────────────────────
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

# ─── Paths ────────────────────────────────────────────────────────────────────
$templatePath  = if ([string]::IsNullOrWhiteSpace($TemplatePath)) {
                     Join-Path $repoRoot "tests\emulator\autotest.comprehensive.json"
                 } else {
                     $TemplatePath
                 }
$generatedDir  = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
                     Join-Path $repoRoot "tests\emulator"
                 } else {
                     $OutputDir
                 }
$generatedPath  = if ([string]::IsNullOrWhiteSpace($GeneratedPath)) {
                      Join-Path $generatedDir "autotest.comprehensive.generated.json"
                  } else {
                      $GeneratedPath
                  }
$baselinedPath  = if ([string]::IsNullOrWhiteSpace($BaselinedPath)) {
                      Join-Path $generatedDir "autotest.comprehensive.baselined.json"
                  } else {
                      $BaselinedPath
                  }
$crcCatalogJsonPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogJsonPath)) {
                          Join-Path $generatedDir "autotest.comprehensive.crc-catalog.json"
                      } else {
                          $CrcCatalogJsonPath
                      }
$crcCatalogMarkdownPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogMarkdownPath)) {
                              Join-Path $generatedDir "autotest.comprehensive.crc-catalog.md"
                          } else {
                              $CrcCatalogMarkdownPath
                          }

function Resolve-ExistingFilePath {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $resolvedPath = Resolve-CalcEmuPipelinePath -PathValue $PathValue
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        throw "$Label '$resolvedPath' does not exist."
    }

    return (Resolve-Path -LiteralPath $resolvedPath).Path
}

# ─── Validate inputs ──────────────────────────────────────────────────────────
if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw "Template not found: '$templatePath'. Run from the repo root or ensure the template exists."
}

$romResolution = Resolve-CalcEmuRomPath -RomPath $RomPath
$autotesterResolution = if ($PSBoundParameters.ContainsKey("AutotesterPath") -and -not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
    [ordered]@{
        path = Resolve-ExistingFilePath -PathValue $AutotesterPath -Label "Autotester path"
        source = "explicit"
    }
}
else {
    Resolve-CalcEmuAutotesterPath -AutotesterPath $(if ($PSBoundParameters.ContainsKey("AutotesterPath")) { $AutotesterPath } else { "" })
}
$resolvedRom = [string]$romResolution.path
$resolvedAutotester = [string]$autotesterResolution.path
$headlessResolution = $null
$resolvedHeadless = ""
$effectiveBackend = "Autotester"

if ($EmulatorBackend -eq "CEmuHeadless") {
    $headlessResolution = if ($PSBoundParameters.ContainsKey("CEmuHeadlessPath") -and -not [string]::IsNullOrWhiteSpace($CEmuHeadlessPath)) {
        [ordered]@{
            path = Resolve-ExistingFilePath -PathValue $CEmuHeadlessPath -Label "CEmu headless path"
            source = "explicit"
        }
    }
    else {
        Resolve-CalcEmuCEmuHeadlessPath -CEmuHeadlessPath $(if ($PSBoundParameters.ContainsKey("CEmuHeadlessPath")) { $CEmuHeadlessPath } else { "" })
    }
    $resolvedHeadless = [string]$headlessResolution.path
    $effectiveBackend = "CEmuHeadless"
}
elseif ($EmulatorBackend -eq "Auto") {
    try {
        $headlessResolution = if ($PSBoundParameters.ContainsKey("CEmuHeadlessPath") -and -not [string]::IsNullOrWhiteSpace($CEmuHeadlessPath)) {
            [ordered]@{
                path = Resolve-ExistingFilePath -PathValue $CEmuHeadlessPath -Label "CEmu headless path"
                source = "explicit"
            }
        }
        else {
            Resolve-CalcEmuCEmuHeadlessPath -CEmuHeadlessPath $(if ($PSBoundParameters.ContainsKey("CEmuHeadlessPath")) { $CEmuHeadlessPath } else { "" })
        }
        $resolvedHeadless = [string]$headlessResolution.path
        $effectiveBackend = "CEmuHeadless"
    }
    catch {
        $headlessResolution = $null
        $resolvedHeadless = ""
        $effectiveBackend = "Autotester"
    }
}

# ─── Helper: UTF-8 without BOM writer ─────────────────────────────────────────
function Write-Utf8NoBom {
    param([string]$Path, [string]$Content)
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $enc)
}

function Ensure-ParentDirectory {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
}

function Get-HashSectionName {
    param([string]$HashName)

    $match = [regex]::Match($HashName, '^(s\d{2})_', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($match.Success) {
        return $match.Groups[1].Value.ToUpperInvariant()
    }
    return "UNSPECIFIED"
}

function New-CrcCatalogArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Config,

        [Parameter(Mandatory = $true)]
        [string]$SourcePath,

        [Parameter(Mandatory = $true)]
        [string]$JsonPath,

        [Parameter(Mandatory = $true)]
        [string]$MarkdownPath
    )

    $sourceLeafBase = [System.IO.Path]::GetFileNameWithoutExtension($SourcePath)
    $suiteName = $sourceLeafBase -replace '\.baselined$', '' -replace '\.generated$', ''
    if ([string]::IsNullOrWhiteSpace($suiteName)) {
        $suiteName = "autotest"
    }

    $entries = New-Object System.Collections.Generic.List[object]
    $order = 1
    $hashProps = @($Config.hashes.PSObject.Properties |
                   Where-Object { $_.MemberType -eq "NoteProperty" })

    foreach ($prop in $hashProps) {
        $hashDef = $prop.Value
        $entries.Add([ordered]@{
            order         = $order
            hash_name     = [string]$prop.Name
            section       = Get-HashSectionName -HashName ([string]$prop.Name)
            description   = [string]$hashDef.description
            expected_crcs = [string[]]@($hashDef.expected_CRCs)
            timeout_ms    = [int]$hashDef.timeout
            start         = [string]$hashDef.start
            size          = [string]$hashDef.size
        })
        $order++
    }

    $catalog = [ordered]@{
        schema_version        = 1
        suite                 = $suiteName
        generated_at          = (Get-Date -Format "o")
        source_baselined_path = $SourcePath
        hash_count            = $entries.Count
        entries               = $entries
    }

    Ensure-ParentDirectory -Path $JsonPath
    Ensure-ParentDirectory -Path $MarkdownPath
    Write-Utf8NoBom -Path $JsonPath -Content ($catalog | ConvertTo-Json -Depth 12)

    $mdLines = New-Object System.Collections.Generic.List[string]
    $mdLines.Add(("# CalcEngine {0} CRC Catalog" -f $suiteName))
    $mdLines.Add("")
    $mdLines.Add(("Generated: {0}" -f $catalog.generated_at))
    $mdLines.Add(('Source: `{0}`' -f $SourcePath))
    $mdLines.Add(("Hashes: {0}" -f $entries.Count))
    $mdLines.Add("")
    $mdLines.Add("| # | Hash | Section | Expected CRCs | Description |")
    $mdLines.Add("| --- | --- | --- | --- | --- |")

    foreach ($entry in $entries) {
        $crcList = (@($entry.expected_crcs) -join ", ")
        $desc = ([string]$entry.description).Replace("|", "\|")
        $mdLines.Add(('| {0} | `{1}` | `{2}` | `{3}` | {4} |' -f
            $entry.order, $entry.hash_name, $entry.section, $crcList, $desc))
    }

    Write-Utf8NoBom -Path $MarkdownPath -Content ($mdLines -join "`r`n")

    return [ordered]@{
        json_path     = $JsonPath
        markdown_path = $MarkdownPath
        hash_count    = $entries.Count
    }
}

# ─── Helper: discover all CalcEngine transfer files ───────────────────────────
function Get-TransferFiles {
    param([string]$Dir)

    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) {
        Write-Warning "DownloadsDir '$Dir' does not exist. Transfer files will be missing."
        return @()
    }

    $files = Get-ChildItem -LiteralPath $Dir -File |
             Where-Object { $_.Extension -imatch '^\.(8xp|8xv)$' } |
             Sort-Object { $_.Extension }, { $_.Name } |
             Select-Object -ExpandProperty FullName

    return $files
}

# ─── Helper: build generated JSON with correct paths ──────────────────────────
function New-GeneratedJson {
    param(
        [string]$TemplatePath,
        [string]$RomFullPath,
        [string[]]$TransferFiles,
        [bool]$UseBaseline = $false,
        [string]$BaselinedPath = ""
    )

    $sourceFile = if ($UseBaseline -and (Test-Path -LiteralPath $BaselinedPath -PathType Leaf)) {
        $BaselinedPath
    } else {
        $TemplatePath
    }

    $config = Get-Content -LiteralPath $sourceFile -Raw | ConvertFrom-Json

    # When building the baseline-mode JSON, normalize ALL expected_CRCs to the
    # sentinel value.  This ensures that every hash check fails against the
    # autotester, causing a "got XXXX" line to be printed for EVERY hash — even
    # those that already have known CRCs in the template.  Without this, tests
    # with pre-set CRCs pass silently and their actual CRCs are never captured,
    # leaving "00000000" sentinels in the baselined file and breaking the verify.
    if (-not $UseBaseline) {
        $hashNames = $config.hashes | Get-Member -MemberType NoteProperty |
                     Select-Object -ExpandProperty Name
        foreach ($hn in $hashNames) {
            $config.hashes.$hn.expected_CRCs = [array]@("00000000")
        }
    }

    # Substitute ROM path
    $config.rom = $RomFullPath

    # Substitute transfer files
    if ($TransferFiles.Count -gt 0) {
        $config.transfer_files = $TransferFiles
    } else {
        Write-Warning "No transfer files found — autotester will run without any CalcEngine programs."
    }

    $json = $config | ConvertTo-Json -Depth 16
    return $json
}

# ─── Helper: parse autotester/headless output for CRC observations ────────────
function Get-ObservedCrcs {
    param([string[]]$Lines)

    $patterns = @(
        'Hash\s*#(?<hash>[A-Za-z0-9_]+).*?(?:\bgot\b|\(got\b)\s*(?<crc>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{1,8})\b',
        '\[headless\]\s+(?<hash>[A-Za-z0-9_]+)\s*->\s*(?<crc>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{1,8})\b'
    )
    $crcs = [ordered]@{}

    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }

        foreach ($pattern in $patterns) {
            $m = [regex]::Match($line, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if (-not $m.Success) { continue }

            $name = $m.Groups["hash"].Value
            $raw  = $m.Groups["crc"].Value
            if ($raw.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
                $raw = $raw.Substring(2)
            }
            $crc = $raw.PadLeft(8, '0').ToUpperInvariant()

            # Keep first observation for each hash name
            if (-not $crcs.Contains($name)) {
                $crcs[$name] = $crc
            }
            break
        }
    }

    return $crcs
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
    param([object[]]$HashDefinitions)

    $map = @{}
    foreach ($definition in @($HashDefinitions)) {
        $map[[string]$definition.hash_name] = $definition
    }

    return $map
}

function Get-HashDefinitions {
    param([string]$ConfigPath)

    $definitions = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
        return $definitions.ToArray()
    }

    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    $hashProps = @($config.hashes.PSObject.Properties |
                   Where-Object { $_.MemberType -eq "NoteProperty" })
    foreach ($prop in $hashProps) {
        $hashName = [string]$prop.Name
        $hashDef = $prop.Value
        $definitions.Add([ordered]@{
            hash_name     = $hashName
            description   = [string]$hashDef.description
            expected_crcs = [string[]]@($hashDef.expected_CRCs | ForEach-Object { Normalize-CrcValue -Value $_ })
            timeout_ms    = [int]$hashDef.timeout
            start         = [string]$hashDef.start
            size          = [string]$hashDef.size
            section       = Get-HashSectionName -HashName $hashName
        })
    }

    return $definitions.ToArray()
}

function Get-VerifyMismatches {
    param(
        [string[]]$Lines,
        [hashtable]$HashDefinitionMap
    )

    $mismatches = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    $pattern = 'Hash\s*#(?<hash>[A-Za-z0-9_]+).*?(?:\bgot\b|\(got\b)\s*(?<crc>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{1,8})\b'

    foreach ($line in @($Lines)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $match = [regex]::Match($line, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if (-not $match.Success) {
            continue
        }

        $hashName = [string]$match.Groups["hash"].Value
        if ($seen.ContainsKey($hashName)) {
            continue
        }
        $seen[$hashName] = $true

        $hashDef = $null
        if ($null -ne $HashDefinitionMap -and $HashDefinitionMap.ContainsKey($hashName)) {
            $hashDef = $HashDefinitionMap[$hashName]
        }

        $mismatches.Add([ordered]@{
            hash_name     = $hashName
            section       = if ($hashDef) { [string]$hashDef.section } else { Get-HashSectionName -HashName $hashName }
            description   = if ($hashDef) { [string]$hashDef.description } else { "" }
            expected_crcs = if ($hashDef) { [string[]]@($hashDef.expected_crcs) } else { @() }
            observed_crc  = Normalize-CrcValue -Value ([string]$match.Groups["crc"].Value)
            output_line   = [string]$line
        })
    }

    return $mismatches.ToArray()
}

# ─── Helper: apply captured CRCs into the template hashes object ──────────────
function Invoke-ApplyCrcs {
    param(
        [string]$TemplatePath,
        [System.Collections.Specialized.OrderedDictionary]$ObservedCrcs
    )

    $config = Get-Content -LiteralPath $TemplatePath -Raw | ConvertFrom-Json
    $appliedCount = 0
    $missingCount = 0

    $hashNames = $config.hashes | Get-Member -MemberType NoteProperty | Select-Object -ExpandProperty Name

    foreach ($hashName in $hashNames) {
        if ($ObservedCrcs.Contains($hashName)) {
            $observedCrc = $ObservedCrcs[$hashName]

            # Collect any pre-known non-sentinel CRCs from the template.
            # These are valid CRCs established in earlier passes for timing-flap
            # tests (e.g. menu open, chained subtraction).  We merge them with
            # the newly observed CRC so the baselined file accepts all known
            # good values and a timing-induced variation doesn't break verify.
            $preKnown = [array]@(
                $config.hashes.$hashName.expected_CRCs |
                Where-Object { $_ -ne "00000000" }
            )

            # Build merged list: observed first, then pre-known; deduplicate.
            $merged = [array]@(
                (@($observedCrc) + $preKnown) |
                Sort-Object -Unique
            )

            $config.hashes.$hashName.expected_CRCs = $merged
            $appliedCount++
        } else {
            $missingCount++
            Write-Warning "No CRC observed for hash '$hashName' — keeping sentinel."
        }
    }

    return [ordered]@{
        config        = $config
        applied_count = $appliedCount
        missing_count = $missingCount
        hash_names    = $hashNames
    }
}

# ─── Helper: run autotester.exe and capture output ────────────────────────────
function Invoke-Autotester {
    param(
        [string]$AutotesterExe,
        [string]$ConfigPath,
        [string]$Label = "autotester"
    )

    Write-Host "  Running $Label..."
    Write-Host "  Config: $ConfigPath"

    # Use a temp file for output capture — avoids async event race conditions
    # that cause empty output when using BeginOutputReadLine + Register-ObjectEvent.
    $tmpOut = [System.IO.Path]::GetTempFileName()
    $tmpErr = [System.IO.Path]::GetTempFileName()

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $AutotesterExe
    $psi.Arguments              = "`"$ConfigPath`""
    $psi.UseShellExecute        = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.CreateNoWindow         = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $null = $proc.Start()

    # Read streams to end synchronously — avoids deadlock on small outputs,
    # and the autotester produces line-by-line hash results (bounded output).
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()

    $timedOut = -not $proc.WaitForExit(900000)   # 15 min timeout

    if ($timedOut) {
        Write-Warning "  autotester.exe timed out after 15 minutes — killing process."
        try { $proc.Kill() } catch { }
        try { $proc.WaitForExit(5000) } catch { }
    }

    $exitCode   = if ($timedOut) { 2 } else { $proc.ExitCode }
    $allOutput  = $stdoutTask.Result + $stderrTask.Result
    $lines      = ($allOutput -split "`r`n|`n") | Where-Object { $null -ne $_ }

    Remove-Item -LiteralPath $tmpOut -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tmpErr -ErrorAction SilentlyContinue

    # Echo output to host
    foreach ($ln in $lines) {
        Write-Host "    | $ln"
    }

    return [ordered]@{
        exit_code = $exitCode
        lines     = [string[]]$lines
        success   = ($exitCode -eq 0)
    }
}

function Convert-DelayMsToCEmuHeadlessCycles {
    param([long]$DelayMs)

    if ($DelayMs -lt 0) {
        $DelayMs = 0
    }

    return ([int64]$DelayMs * 48000L)
}

function Invoke-CEmuHeadlessCapture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CEmuHeadlessExe,

        [Parameter(Mandatory = $true)]
        [string]$RomPath,

        [Parameter(Mandatory = $true)]
        [string]$AutotesterExe,

        [Parameter(Mandatory = $true)]
        [string[]]$TransferFiles,

        [Parameter(Mandatory = $true)]
        [object]$Target,

        [Parameter(Mandatory = $false)]
        [System.Collections.Generic.List[object]]$ReplayCommands,

        [Parameter(Mandatory = $true)]
        [string]$HashName
    )

    $tempScriptPath = Join-Path ([System.IO.Path]::GetTempPath()) ("calcengine-cemu-headless-{0}.jsonl" -f [System.Guid]::NewGuid().ToString("N"))
    $scriptLines = New-Object System.Collections.Generic.List[string]

    try {
        $scriptLines.Add(([ordered]@{
            cmd = "load_rom"
            path = $RomPath
            autotester_path = $AutotesterExe
        } | ConvertTo-Json -Compress -Depth 6))

        $isFirstTransfer = $true
        foreach ($transferFile in @($TransferFiles)) {
            $loadAppCommand = [ordered]@{
                cmd = "load_app"
                path = [string]$transferFile
            }
            if ($isFirstTransfer -and $null -ne $Target) {
                $loadAppCommand.target_name = [string]$Target.name
                $loadAppCommand.target_is_asm = [bool]$Target.isASM
            }
            $scriptLines.Add(($loadAppCommand | ConvertTo-Json -Compress -Depth 6))
            $isFirstTransfer = $false
        }

        foreach ($command in @($ReplayCommands.ToArray())) {
            $scriptLines.Add(($command | ConvertTo-Json -Compress -Depth 6))
        }

        $scriptLines.Add(([ordered]@{
            cmd = "get_screen"
            format = "crc"
        } | ConvertTo-Json -Compress -Depth 6))
        $scriptLines.Add('{"cmd":"exit"}')

        Write-Utf8NoBom -Path $tempScriptPath -Content ($scriptLines -join "`r`n")

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $CEmuHeadlessExe
        $psi.Arguments = "`"$tempScriptPath`""
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true

        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $psi
        $null = $process.Start()

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit(900000)
        if ($timedOut) {
            try { $process.Kill() } catch { }
            try { $process.WaitForExit(5000) } catch { }
            throw "cemu-headless timed out after 15 minutes while capturing hash '$HashName'."
        }

        $stdoutLines = @($stdoutTask.Result -split "`r`n|`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $stderrLines = @($stderrTask.Result -split "`r`n|`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $responses = New-Object System.Collections.Generic.List[object]

        foreach ($line in $stdoutLines) {
            try {
                $responses.Add(($line | ConvertFrom-Json -ErrorAction Stop))
            }
            catch {
            }
        }

        $screenResponse = $responses |
            Where-Object { $_.cmd -eq "get_screen" } |
            Select-Object -Last 1

        if ($null -eq $screenResponse) {
            $combinedText = (@($stdoutLines) + @($stderrLines)) -join "`n"
            throw ("cemu-headless did not return a get_screen response for hash '{0}'. Output:`n{1}" -f $HashName, $combinedText)
        }

        if (-not [bool]$screenResponse.ok) {
            throw ("cemu-headless get_screen failed for hash '{0}': {1}" -f $HashName, [string]$screenResponse.error)
        }

        return [ordered]@{
            crc = Normalize-CrcValue -Value ([string]$screenResponse.crc)
            backend = [string]$screenResponse.backend
            stdout_lines = [string[]]$stdoutLines
            stderr_lines = [string[]]$stderrLines
            exit_code = [int]$process.ExitCode
        }
    }
    finally {
        Remove-Item -LiteralPath $tempScriptPath -ErrorAction SilentlyContinue
    }
}

function Invoke-CEmuHeadlessSuite {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CEmuHeadlessExe,

        [Parameter(Mandatory = $true)]
        [string]$AutotesterExe,

        [Parameter(Mandatory = $true)]
        [string]$ConfigPath,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    Write-Host "  Running $Label..."
    Write-Host "  Config: $ConfigPath"

    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    $replayCommands = New-Object 'System.Collections.Generic.List[object]'
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $attemptedCount = 0
    $passedCount = 0
    $failedCount = 0
    $hashCount = 0

    foreach ($entry in @($config.sequence)) {
        $entryText = [string]$entry
        if ([string]::IsNullOrWhiteSpace($entryText)) {
            continue
        }

        if ($entryText.StartsWith("action|", [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        if ($entryText.StartsWith("key|", [System.StringComparison]::OrdinalIgnoreCase)) {
            $keyName = $entryText.Substring(4)
            $replayCommands.Add([ordered]@{
                cmd = "inject_key"
                key = $keyName
            })
            continue
        }

        if ($entryText.StartsWith("delay|", [System.StringComparison]::OrdinalIgnoreCase)) {
            $delayMs = 0L
            if (-not [long]::TryParse($entryText.Substring(6), [ref]$delayMs)) {
                throw "Invalid delay entry '$entryText' in '$ConfigPath'."
            }
            $replayCommands.Add([ordered]@{
                cmd = "step"
                cycles = (Convert-DelayMsToCEmuHeadlessCycles -DelayMs $delayMs)
            })
            continue
        }

        if ($entryText.StartsWith("hash|", [System.StringComparison]::OrdinalIgnoreCase)) {
            $hashName = $entryText.Substring(5)
            $hashDefinition = $null
            if ($config.hashes.PSObject.Properties.Name -contains $hashName) {
                $hashDefinition = $config.hashes.$hashName
            }
            if ($null -eq $hashDefinition) {
                throw "Hash '$hashName' was referenced in the sequence but not defined in '$ConfigPath'."
            }

            $capture = Invoke-CEmuHeadlessCapture `
                -CEmuHeadlessExe $CEmuHeadlessExe `
                -RomPath ([string]$config.rom) `
                -AutotesterExe $AutotesterExe `
                -TransferFiles ([string[]]@($config.transfer_files)) `
                -Target $config.target `
                -ReplayCommands $replayCommands `
                -HashName $hashName

            $expectedCrcs = [string[]]@($hashDefinition.expected_CRCs | ForEach-Object {
                Normalize-CrcValue -Value ([string]$_)
            })
            $attemptedCount++
            $hashCount++
            $lines.Add(("[headless] {0} -> {1}" -f $hashName, $capture.crc))
            Write-Host ("    | [headless] {0} -> {1}" -f $hashName, $capture.crc)

            if ($capture.crc -in $expectedCrcs) {
                $passedCount++
            }
            else {
                $failedCount++
                $expectedText = if ($expectedCrcs.Count -gt 0) { $expectedCrcs[0] } else { "00000000" }
                $lines.Add(("Hash #{0} expected {1} got {2}" -f $hashName, $expectedText, $capture.crc))
            }

            continue
        }

        throw "Unsupported sequence entry '$entryText' in '$ConfigPath'."
    }

    $lines.Add(("{0} loaded and verified. {1} unique tests found." -f $ConfigPath, $hashCount))
    $statusText = if ($failedCount -eq 0) { "Autotest passed" } else { "Autotest failed" }
    $lines.Add(("[{0}] Out of {1} tests attempted, {2} passed, and {3} failed." -f
        $statusText,
        $attemptedCount,
        $passedCount,
        $failedCount))

    foreach ($line in $lines) {
        Write-Host "    | $line"
    }

    return [ordered]@{
        exit_code = if ($failedCount -eq 0) { 0 } else { 1 }
        lines = [string[]]$lines.ToArray()
        success = ($failedCount -eq 0)
    }
}

function Invoke-CalcEmuSuite {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Backend,

        [Parameter(Mandatory = $true)]
        [string]$AutotesterExe,

        [Parameter(Mandatory = $true)]
        [string]$ConfigPath,

        [Parameter(Mandatory = $true)]
        [string]$Label,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$CEmuHeadlessExe = ""
    )

    if ($Backend -eq "CEmuHeadless") {
        if ([string]::IsNullOrWhiteSpace($CEmuHeadlessExe)) {
            throw "CEmuHeadless backend was selected but no cemu-headless executable was resolved."
        }

        return Invoke-CEmuHeadlessSuite `
            -CEmuHeadlessExe $CEmuHeadlessExe `
            -AutotesterExe $AutotesterExe `
            -ConfigPath $ConfigPath `
            -Label $Label
    }

    return Invoke-Autotester `
        -AutotesterExe $AutotesterExe `
        -ConfigPath $ConfigPath `
        -Label $Label
}

function Get-AutotestOutputStats {
    param([string[]]$Lines)

    $summaryPattern = '\[(?<status>Autotest passed|Autotest failed)\]\s+Out of\s+(?<attempted>\d+)\s+tests attempted,\s+(?<passed>\d+)\s+passed,\s+and\s+(?<failed>\d+)\s+failed\.'
    $uniquePattern = 'loaded and verified\.\s+(?<count>\d+)\s+unique tests found\.'

    $summaryLine = ""
    $status = ""
    $attemptedCount = -1
    $passedCount = -1
    $failedCount = -1
    $uniqueTestCount = -1

    foreach ($line in @($Lines)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        if ($uniqueTestCount -lt 0) {
            $uniqueMatch = [regex]::Match([string]$line, $uniquePattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if ($uniqueMatch.Success) {
                $uniqueTestCount = [int]$uniqueMatch.Groups["count"].Value
            }
        }

        if ($attemptedCount -lt 0) {
            $summaryMatch = [regex]::Match([string]$line, $summaryPattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if ($summaryMatch.Success) {
                $summaryLine = [string]$line
                $status = [string]$summaryMatch.Groups["status"].Value
                $attemptedCount = [int]$summaryMatch.Groups["attempted"].Value
                $passedCount = [int]$summaryMatch.Groups["passed"].Value
                $failedCount = [int]$summaryMatch.Groups["failed"].Value
            }
        }
    }

    return [ordered]@{
        summary_line = $summaryLine
        status = $status
        unique_test_count = $uniqueTestCount
        attempted_count = $attemptedCount
        passed_count = $passedCount
        failed_count = $failedCount
    }
}

function New-VerifyHashResults {
    param(
        [object[]]$HashDefinitions,
        [hashtable]$HashDefinitionMap,
        [object[]]$Mismatches,
        [hashtable]$ObservedCrcs,
        [object]$OutputStats,
        [bool]$VerifySuccess
    )

    $mismatchMap = @{}
    foreach ($mismatch in @($Mismatches)) {
        $mismatchMap[[string]$mismatch.hash_name] = $mismatch
    }
    if ($null -eq $ObservedCrcs) {
        $ObservedCrcs = @{}
    }

    $expectedCount = @($HashDefinitions).Count
    $attemptedCount = if ($null -ne $OutputStats -and [int]$OutputStats.attempted_count -ge 0) {
        [int]$OutputStats.attempted_count
    } else {
        if ($VerifySuccess) { $expectedCount } else { 0 }
    }
    $passedBudget = if ($null -ne $OutputStats -and [int]$OutputStats.passed_count -ge 0) {
        [int]$OutputStats.passed_count
    } else {
        if ($VerifySuccess) { $expectedCount } else { 0 }
    }
    $failedBudget = if ($null -ne $OutputStats -and [int]$OutputStats.failed_count -ge 0) {
        [int]$OutputStats.failed_count
    } else {
        if ($VerifySuccess) { 0 } else { @($Mismatches).Count }
    }
    $unattributedFailureCount = 0

    $inferenceApplied = $false
    $hashResults = New-Object System.Collections.Generic.List[object]
    $missingHashes = New-Object System.Collections.Generic.List[string]
    $remainingPassBudget = [Math]::Max(0, $passedBudget)
    $remainingFailBudget = [Math]::Max(0, ($failedBudget - $mismatchMap.Count))

    foreach ($definition in @($HashDefinitions)) {
        $hashName = [string]$definition.hash_name
        $reportedObservedCrc = if ($ObservedCrcs.ContainsKey($hashName)) {
            [string]$ObservedCrcs[$hashName]
        } else {
            ""
        }

        if ($mismatchMap.ContainsKey($hashName)) {
            $mismatch = $mismatchMap[$hashName]
            $observedCrc = if (-not [string]::IsNullOrWhiteSpace([string]$mismatch.observed_crc)) {
                [string]$mismatch.observed_crc
            } else {
                $reportedObservedCrc
            }
            $hashResults.Add([ordered]@{
                hash_name = $hashName
                section = [string]$definition.section
                description = [string]$definition.description
                expected_crcs = [string[]]@($definition.expected_crcs)
                observed_crc = $observedCrc
                observed_crc_source = if ([string]::IsNullOrWhiteSpace($observedCrc)) { "not_reported" } else { "reported_crc" }
                observed_crc_inferred = $false
                status = "fail"
                status_reason = "crc_mismatch"
            })
            continue
        }

        $status = "missing"
        $statusReason = "not_reported"
        $observedCrc = $reportedObservedCrc
        $observedCrcSource = if ([string]::IsNullOrWhiteSpace($reportedObservedCrc)) { "not_reported" } else { "reported_crc" }
        $observedCrcInferred = $false

        if ($remainingFailBudget -gt 0) {
            $status = "fail"
            $statusReason = "reported_failure_without_hash_line"
            $remainingFailBudget--
            $inferenceApplied = $true
        }
        elseif ($remainingPassBudget -gt 0) {
            $status = "pass"
            if ([string]::IsNullOrWhiteSpace($reportedObservedCrc)) {
                $statusReason = if ($attemptedCount -eq $expectedCount -and $failedBudget -eq 0) {
                    "reported_pass_without_observed_crc"
                } else {
                    "reported_pass_budget_without_observed_crc"
                }
            }
            else {
                $statusReason = "reported_pass_with_observed_crc"
            }
            $remainingPassBudget--
            if (($attemptedCount -ne $expectedCount -or $failedBudget -ne 0) -and [string]::IsNullOrWhiteSpace($reportedObservedCrc)) {
                $observedCrcInferred = $true
                $inferenceApplied = $true
            }
        }
        else {
            $missingHashes.Add($hashName)
            $inferenceApplied = $true
        }

        $hashResults.Add([ordered]@{
            hash_name = $hashName
            section = [string]$definition.section
            description = [string]$definition.description
            expected_crcs = [string[]]@($definition.expected_crcs)
            observed_crc = $observedCrc
            observed_crc_source = $observedCrcSource
            observed_crc_inferred = $observedCrcInferred
            status = $status
            status_reason = $statusReason
        })
    }

    if ($remainingFailBudget -gt 0) {
        $unattributedFailureCount = $remainingFailBudget
    }

    return [ordered]@{
        hash_results = $hashResults.ToArray()
        expected_hash_count = $expectedCount
        pass_count = @($hashResults | Where-Object { $_.status -eq "pass" }).Count
        fail_count = @($hashResults | Where-Object { $_.status -eq "fail" }).Count
        missing_count = @($missingHashes).Count
        missing_hashes = $missingHashes.ToArray()
        inference_applied = $inferenceApplied
        unattributed_failure_count = $unattributedFailureCount
        unique_test_count = if ($null -ne $OutputStats) { [int]$OutputStats.unique_test_count } else { -1 }
        attempted_count = $attemptedCount
        summary_line = if ($null -ne $OutputStats) { [string]$OutputStats.summary_line } else { "" }
    }
}

Ensure-ParentDirectory -Path $generatedPath
Ensure-ParentDirectory -Path $baselinedPath
Ensure-ParentDirectory -Path $crcCatalogJsonPath
Ensure-ParentDirectory -Path $crcCatalogMarkdownPath
if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
    Ensure-ParentDirectory -Path $SummaryPath
}

# ─── STEP: determine effective mode ───────────────────────────────────────────
$effectiveMode = $Mode
if ($effectiveMode -eq "Run") {
    if (Test-Path -LiteralPath $baselinedPath -PathType Leaf) {
        $effectiveMode = "Verify"
        Write-Host "[Run] Baselined file found — switching to Verify mode."
    } else {
        $effectiveMode = "Baseline"
        Write-Host "[Run] No baselined file — switching to Baseline mode."
    }
}

Write-Host ""
Write-Host "============================================================"
Write-Host "  CalcEngine Comprehensive CEmu Autotest"
Write-Host "  Mode       : $effectiveMode"
Write-Host "  Backend    : $effectiveBackend"
Write-Host "  ROM        : $resolvedRom"
Write-Host "  Autotester : $resolvedAutotester"
Write-Host "  Source     : $($autotesterResolution.source)"
if ($effectiveBackend -eq "CEmuHeadless" -and -not [string]::IsNullOrWhiteSpace($resolvedHeadless)) {
    Write-Host "  Headless   : $resolvedHeadless"
    Write-Host "  HeadlessSrc: $($headlessResolution.source)"
}
Write-Host "  Template   : $templatePath"
Write-Host "  Downloads  : $DownloadsDir"
Write-Host "============================================================"
Write-Host ""

# ─── Discover transfer files ──────────────────────────────────────────────────
$transferFiles = Get-TransferFiles -Dir $DownloadsDir
Write-Host "Transfer files discovered: $($transferFiles.Count)"
foreach ($tf in $transferFiles) {
    Write-Host "  + $(Split-Path $tf -Leaf)"
}
Write-Host ""

if ($transferFiles.Count -eq 0) {
    Write-Warning "No transfer files found. Tests requiring CalcEngine modules will likely fail."
}

# ─── Result accumulator ───────────────────────────────────────────────────────
$summary = [ordered]@{
    schema_version   = 1
    mode             = $effectiveMode
    emulator_backend = $effectiveBackend
    timestamp        = (Get-Date -Format "o")
    rom              = $resolvedRom
    rom_source       = $romResolution.source
    autotester       = $resolvedAutotester
    autotester_source = $autotesterResolution.source
    cemu_headless    = $resolvedHeadless
    cemu_headless_source = if ($headlessResolution) { $headlessResolution.source } else { "" }
    transfer_count   = $transferFiles.Count
    template         = $templatePath
    baselined_path   = $baselinedPath
    crc_catalog      = $null
    baseline_stage   = $null
    verify_stage     = $null
    success          = $false
    exit_code        = 1
}

# ─── BASELINE STAGE ───────────────────────────────────────────────────────────
if ($effectiveMode -eq "Baseline" -or $effectiveMode -eq "Both") {
    Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    Write-Host "  BASELINE CAPTURE"
    Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    Write-Host ""
    Write-Host "  Running with sentinel CRCs (00000000)."
    Write-Host "  All hash checks will FAIL — this is expected."
    Write-Host "  CEmu output will be parsed to capture actual CRC values."
    Write-Host ""

    # Build generated JSON from template (sentinel CRCs, all paths resolved)
    $baselineJson = New-GeneratedJson `
        -TemplatePath $templatePath `
        -RomFullPath $resolvedRom `
        -TransferFiles $transferFiles `
        -UseBaseline $false

    Write-Utf8NoBom -Path $generatedPath -Content $baselineJson

    # Run autotester
    $baselineRun = Invoke-CalcEmuSuite `
        -Backend $effectiveBackend `
        -AutotesterExe $resolvedAutotester `
        -CEmuHeadlessExe $resolvedHeadless `
        -ConfigPath $generatedPath `
        -Label "baseline capture run"
    $baselineOutputStats = Get-AutotestOutputStats -Lines $baselineRun.lines

    # Parse CRCs from output
    $observedCrcs = Get-ObservedCrcs -Lines $baselineRun.lines

    Write-Host ""
    Write-Host "  Observed CRCs from autotester output:"
    if ($observedCrcs.Count -eq 0) {
        Write-Warning "  No CRCs were observed! Check that autotester.exe ran correctly and the program launched."
    }
    foreach ($key in $observedCrcs.Keys) {
        Write-Host ("  {0,-40} {1}" -f $key, $observedCrcs[$key])
    }
    Write-Host ""

    # Apply CRCs to template → write baselined file
    $applyResult = Invoke-ApplyCrcs -TemplatePath $templatePath -ObservedCrcs $observedCrcs

    # Re-inject correct paths into baselined config
    $applyResult.config.rom = $resolvedRom
    if ($transferFiles.Count -gt 0) {
        $applyResult.config.transfer_files = $transferFiles
    }

    $baselinedJson = $applyResult.config | ConvertTo-Json -Depth 16
    Write-Utf8NoBom -Path $baselinedPath -Content $baselinedJson

    $catalogInfo = New-CrcCatalogArtifacts `
        -Config $applyResult.config `
        -SourcePath $baselinedPath `
        -JsonPath $crcCatalogJsonPath `
        -MarkdownPath $crcCatalogMarkdownPath

    Write-Host "  Baseline written: $baselinedPath"
    Write-Host ("  Applied: {0} CRCs   Missing: {1} hashes" -f $applyResult.applied_count, $applyResult.missing_count)
    Write-Host ("  CRC catalog: {0} hashes" -f $catalogInfo.hash_count)
    Write-Host ""

    $summary.baseline_stage = [ordered]@{
        generated_path   = $generatedPath
        baselined_path   = $baselinedPath
        emulator_backend = $effectiveBackend
        crc_catalog_json = $catalogInfo.json_path
        crc_catalog_md   = $catalogInfo.markdown_path
        observed_crcs    = $observedCrcs
        crc_catalog_hash_count = $catalogInfo.hash_count
        applied_count    = $applyResult.applied_count
        missing_count    = $applyResult.missing_count
        autotester_exit  = $baselineRun.exit_code
        unique_test_count = $baselineOutputStats.unique_test_count
        attempted_count  = $baselineOutputStats.attempted_count
        passed_count     = $baselineOutputStats.passed_count
        failed_count     = $baselineOutputStats.failed_count
        summary_line     = $baselineOutputStats.summary_line
        success          = ($applyResult.applied_count -gt 0)
    }
    $summary.crc_catalog = $catalogInfo

    if ($applyResult.missing_count -gt 0) {
        Write-Warning ("$($applyResult.missing_count) hash(es) had no observed CRC. " +
            "Check that the corresponding key sequences executed correctly and that CalcEngine launched.")
    }
}

# ─── VERIFY STAGE ─────────────────────────────────────────────────────────────
if ($effectiveMode -eq "Verify" -or $effectiveMode -eq "Both") {
    Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    Write-Host "  VERIFY"
    Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    Write-Host ""

    if (-not (Test-Path -LiteralPath $baselinedPath -PathType Leaf)) {
        throw "Baselined file not found at '$baselinedPath'. Run with -Mode Baseline first."
    }

    # Build generated JSON from baselined file (real CRCs, paths resolved)
    $verifyJson = New-GeneratedJson `
        -TemplatePath $templatePath `
        -RomFullPath $resolvedRom `
        -TransferFiles $transferFiles `
        -UseBaseline $true `
        -BaselinedPath $baselinedPath

    Write-Utf8NoBom -Path $generatedPath -Content $verifyJson

    # Run autotester
    $verifyRun = Invoke-CalcEmuSuite `
        -Backend $effectiveBackend `
        -AutotesterExe $resolvedAutotester `
        -CEmuHeadlessExe $resolvedHeadless `
        -ConfigPath $generatedPath `
        -Label "verify run"

    $verifySuccess = ($verifyRun.exit_code -eq 0)
    $hashDefinitions = Get-HashDefinitions -ConfigPath $baselinedPath
    $hashDefinitionMap = Get-HashDefinitionMap -HashDefinitions $hashDefinitions
    $verifyMismatches = Get-VerifyMismatches -Lines $verifyRun.lines -HashDefinitionMap $hashDefinitionMap
    $verifyObservedCrcs = Get-ObservedCrcs -Lines $verifyRun.lines
    $verifyOutputStats = Get-AutotestOutputStats -Lines $verifyRun.lines
    $verifyHashResults = New-VerifyHashResults `
        -HashDefinitions $hashDefinitions `
        -HashDefinitionMap $hashDefinitionMap `
        -Mismatches $verifyMismatches `
        -ObservedCrcs $verifyObservedCrcs `
        -OutputStats $verifyOutputStats `
        -VerifySuccess $verifySuccess
    $catalogInfo = $null
    if (Test-Path -LiteralPath $baselinedPath -PathType Leaf) {
        $catalogConfig = Get-Content -LiteralPath $baselinedPath -Raw | ConvertFrom-Json
        $catalogInfo = New-CrcCatalogArtifacts `
            -Config $catalogConfig `
            -SourcePath $baselinedPath `
            -JsonPath $crcCatalogJsonPath `
            -MarkdownPath $crcCatalogMarkdownPath
        $summary.crc_catalog = $catalogInfo
    }
    Write-Host ""
    Write-Host ("  Autotester exit code: {0}  →  {1}" -f $verifyRun.exit_code, $(if ($verifySuccess) { "PASS" } else { "FAIL" }))
    if ($catalogInfo) {
        Write-Host ("  CRC catalog: {0} hashes" -f $catalogInfo.hash_count)
    }
    if (-not $verifySuccess -and $verifyMismatches.Count -gt 0) {
        Write-Host "  CRC mismatches:"
        foreach ($mismatch in $verifyMismatches) {
            $expectedText = if ($mismatch.expected_crcs.Count -gt 0) {
                $mismatch.expected_crcs -join ", "
            } else {
                "<unknown>"
            }
            Write-Host ("    - {0}: expected [{1}] got {2}" -f
                $mismatch.hash_name,
                $expectedText,
                $mismatch.observed_crc)
        }
    }
    Write-Host ""

    $summary.verify_stage = [ordered]@{
        generated_path  = $generatedPath
        baselined_path  = $baselinedPath
        emulator_backend = $effectiveBackend
        autotester_exit = $verifyRun.exit_code
        crc_catalog_json = if ($catalogInfo) { $catalogInfo.json_path } else { $null }
        crc_catalog_md   = if ($catalogInfo) { $catalogInfo.markdown_path } else { $null }
        crc_catalog_hash_count = if ($catalogInfo) { $catalogInfo.hash_count } else { 0 }
        mismatch_count  = $verifyMismatches.Count
        mismatches      = [object[]]@($verifyMismatches)
        unique_test_count = $verifyHashResults.unique_test_count
        attempted_count = $verifyHashResults.attempted_count
        pass_count = $verifyHashResults.pass_count
        fail_count = $verifyHashResults.fail_count
        missing_count = $verifyHashResults.missing_count
        missing_hashes = $verifyHashResults.missing_hashes
        hash_results = $verifyHashResults.hash_results
        inference_applied = $verifyHashResults.inference_applied
        unattributed_failure_count = $verifyHashResults.unattributed_failure_count
        summary_line = $verifyHashResults.summary_line
        success = ($verifySuccess -and
                   $verifyHashResults.fail_count -eq 0 -and
                   $verifyHashResults.missing_count -eq 0 -and
                   $verifyHashResults.unattributed_failure_count -eq 0)
    }
    $summary.success = [bool]$summary.verify_stage.success
    $summary.exit_code = if ($summary.success) { 0 } else { $(if ($verifyRun.exit_code -ne 0) { $verifyRun.exit_code } else { 1 }) }
}

# ─── Baseline-only success ────────────────────────────────────────────────────
if ($effectiveMode -eq "Baseline") {
    $bl = $summary.baseline_stage
    $summary.success   = ($bl -ne $null -and $bl.applied_count -gt 0 -and $bl.missing_count -eq 0)
    $summary.exit_code = if ($summary.success) { 0 } else { 1 }
}

# ─── Duration ─────────────────────────────────────────────────────────────────
$elapsed = (Get-Date) - $script:startTime
$summary.duration_seconds = [math]::Round($elapsed.TotalSeconds, 1)

# ─── Print final summary ──────────────────────────────────────────────────────
Write-Host ""
Write-Host "============================================================"
Write-Host ("  RESULT: {0}" -f $(if ($summary.success) { "SUCCESS" } else { "FAILURE" }))
Write-Host ("  Mode          : {0}" -f $effectiveMode)
Write-Host ("  Backend       : {0}" -f $effectiveBackend)
Write-Host ("  Duration      : {0}s" -f $summary.duration_seconds)
if ($summary.baseline_stage) {
    $bl = $summary.baseline_stage
    Write-Host ("  Baseline CRCs : {0} applied, {1} missing" -f $bl.applied_count, $bl.missing_count)
    Write-Host ("  Baselined at  : {0}" -f $baselinedPath)
}
if ($summary.verify_stage) {
    $vr = $summary.verify_stage
    Write-Host ("  Verify        : exit {0} ({1})" -f $vr.autotester_exit, $(if ($vr.success) { "PASS" } else { "FAIL" }))
}
if ($summary.crc_catalog) {
    Write-Host ("  CRC Catalog   : {0} hashes" -f $summary.crc_catalog.hash_count)
}
Write-Host "============================================================"
Write-Host ""

# ─── Optional JSON summary output ────────────────────────────────────────────
if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
    $summaryJson = $summary | ConvertTo-Json -Depth 10
    Write-Utf8NoBom -Path $SummaryPath -Content $summaryJson
    Write-Host "Summary written: $SummaryPath"
}

# ─── Exit code ────────────────────────────────────────────────────────────────
exit $summary.exit_code
