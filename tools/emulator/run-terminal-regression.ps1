param(
    [Parameter(Mandatory = $true)]
    [string]$CaseFile,

    [Parameter(Mandatory = $false)]
    [string]$HarnessScriptPath = "",

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
    [ValidateRange(1024, 1048576)]
    [int]$UsableUserRamLimitBytes = 65536,

    [Parameter(Mandatory = $false)]
    [ValidateRange(1024, 1048576)]
    [int]$ModuleLinkedBinaryLimitBytes = 51200,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ArchiveRamDispatchModulePath = "",

    [Parameter(Mandatory = $false)]
    [ValidateRange(1024, 1048576)]
    [int]$ArchiveRamSwapRamCapacityBytes = 65536,

    [Parameter(Mandatory = $false)]
    [ValidateSet("none", "dispatch_load", "return_to_archive", "partial_load", "memory_pressure", "archive_full")]
    [string]$ArchiveRamFailurePhase = "none",

    [Parameter(Mandatory = $false)]
    [switch]$ForceMock,

    [Parameter(Mandatory = $false)]
    [switch]$NoMockFallback,

    [Parameter(Mandatory = $false)]
    [string]$SummaryPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$CaptureCEmuHashBaselines,

    [Parameter(Mandatory = $false)]
    [string]$BaselineOutputCaseFile = "",

    [Parameter(Mandatory = $false)]
    [string]$BaselinePlaceholder = "__BASELINE_REQUIRED__",

    [Parameter(Mandatory = $false)]
    [ValidatePattern("^(?:0x)?[0-9a-fA-F]{8}$")]
    [string]$BaselineSeedCrc = "00000000",

    [Parameter(Mandatory = $false)]
    [ValidateRange(1, 20)]
    [int]$BaselineCaptureSampleCount = 1,

    [Parameter(Mandatory = $false)]
    [ValidateRange(1, 20)]
    [int]$BaselineCaptureConsensus = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$artifactModulePath = Join-Path $PSScriptRoot "CalcEmuSplitSuiteArtifacts.psm1"
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    throw "Artifact discovery module '$artifactModulePath' does not exist."
}
Import-Module -Name $artifactModulePath -Force

function Test-ObjectHasProperty {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($null -eq $Object) {
        return $false
    }

    return ($Object.PSObject.Properties.Name -contains $Name)
}

function Get-ObjectPropertyValue {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-ObjectHasProperty -Object $Object -Name $Name)) {
        return $null
    }

    return $Object.$Name
}

function Set-ObjectPropertyValue {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Value
    )

    if (Test-ObjectHasProperty -Object $Object -Name $Name) {
        $Object.$Name = $Value
        return
    }

    $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
}

function ConvertTo-NormalizedCrcString {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Value
    )

    $trimmed = $Value.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return $null
    }

    if ($trimmed.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        $trimmed = $trimmed.Substring(2)
    }

    if ($trimmed -notmatch "^[0-9a-fA-F]{8}$") {
        return $null
    }

    return $trimmed.ToUpperInvariant()
}

function ConvertTo-StringArray {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Value
    )

    if ($null -eq $Value) {
        return @()
    }

    if ($Value -is [string]) {
        return ,@([string]$Value)
    }

    $converted = New-Object System.Collections.Generic.List[string]
    foreach ($item in @($Value)) {
        if ($null -eq $item) {
            $converted.Add("")
        }
        else {
            $converted.Add([string]$item)
        }
    }

    return ,$converted.ToArray()
}

function ConvertTo-DisplayCommandArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Argument
    )

    if ($Argument -match "[\s`"]") {
        return ('"{0}"' -f $Argument.Replace('"', '\"'))
    }

    return $Argument
}

function Join-DelimitedCommandArgumentList {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [string[]]$Values = @()
    )

    $normalized = New-Object System.Collections.Generic.List[string]
    foreach ($value in @($Values)) {
        if ($null -eq $value) {
            $normalized.Add("")
        }
        else {
            $normalized.Add([string]$value)
        }
    }

    return ($normalized.ToArray() -join ";")
}

function ConvertTo-BooleanText {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Value
    )

    if ($Value) {
        return "true"
    }

    return "false"
}

function ConvertTo-StrictBoolean {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Value
    )

    if ($Value -is [bool]) {
        return [bool]$Value
    }

    if ($Value -is [byte] -or $Value -is [int16] -or $Value -is [int32] -or $Value -is [int64]) {
        if ([int64]$Value -eq 1) {
            return $true
        }

        if ([int64]$Value -eq 0) {
            return $false
        }

        throw "Numeric boolean values must be 0 or 1."
    }

    if ($Value -is [string]) {
        $trimmed = $Value.Trim().ToLowerInvariant()
        switch ($trimmed) {
            "true" { return $true }
            "false" { return $false }
            "1" { return $true }
            "0" { return $false }
            "yes" { return $true }
            "no" { return $false }
            default { throw "String boolean values must be one of: true, false, 1, 0, yes, no." }
        }
    }

    throw "Value '$Value' cannot be converted to boolean."
}

function Test-StringArrayEqual {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Expected,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Actual
    )

    if ($Expected.Count -ne $Actual.Count) {
        return $false
    }

    for ($index = 0; $index -lt $Expected.Count; $index++) {
        if ($Expected[$index] -cne $Actual[$index]) {
            return $false
        }
    }

    return $true
}

function Try-ParseJsonText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    $trimmed = $Text.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return [ordered]@{
            success = $false
            object = $null
            json_text = ""
            error = "No JSON text was provided."
        }
    }

    try {
        $parsed = $trimmed | ConvertFrom-Json
        return [ordered]@{
            success = $true
            object = $parsed
            json_text = $trimmed
            error = ""
        }
    }
    catch {
        return [ordered]@{
            success = $false
            object = $null
            json_text = $trimmed
            error = $_.Exception.Message
        }
    }
}

function Find-HarnessJsonInOutput {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$OutputLines
    )

    if ($OutputLines.Count -eq 0) {
        return [ordered]@{
            success = $false
            object = $null
            json_text = ""
            error = "Harness output was empty; no JSON payload was found."
        }
    }

    for ($start = 0; $start -lt $OutputLines.Count; $start++) {
        $candidate = ($OutputLines[$start..($OutputLines.Count - 1)] -join "`n")
        $attempt = Try-ParseJsonText -Text $candidate
        if ($attempt.success) {
            return $attempt
        }
    }

    $joined = ($OutputLines -join "`n")
    $openBraceIndex = $joined.IndexOf("{")
    $closeBraceIndex = $joined.LastIndexOf("}")

    if ($openBraceIndex -ge 0 -and $closeBraceIndex -gt $openBraceIndex) {
        $candidate = $joined.Substring($openBraceIndex, $closeBraceIndex - $openBraceIndex + 1)
        $attempt = Try-ParseJsonText -Text $candidate
        if ($attempt.success) {
            return $attempt
        }
    }

    return [ordered]@{
        success = $false
        object = $null
        json_text = ""
        error = "Unable to parse JSON from harness output."
    }
}

function Invoke-HarnessCase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$HarnessScriptPath,

        [Parameter(Mandatory = $true)]
        [string]$KeySequence,

        [Parameter(Mandatory = $true)]
        [bool]$IncludeRomPath,

        [Parameter(Mandatory = $true)]
        [bool]$IncludeAutotesterPath,

        [Parameter(Mandatory = $true)]
        [bool]$IncludeProgramPath,

        [Parameter(Mandatory = $true)]
        [bool]$IncludeModuleProgramPaths,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$RomPath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$AutotesterPath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$ProgramPath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$ModuleProgramPaths,

        [Parameter(Mandatory = $true)]
        [int]$UsableUserRamLimitBytes,

        [Parameter(Mandatory = $true)]
        [int]$ModuleLinkedBinaryLimitBytes,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$ArchiveRamDispatchModulePath,

        [Parameter(Mandatory = $true)]
        [int]$ArchiveRamSwapRamCapacityBytes,

        [Parameter(Mandatory = $true)]
        [ValidateSet("none", "dispatch_load", "return_to_archive", "partial_load", "memory_pressure", "archive_full")]
        [string]$ArchiveRamFailurePhase,

        [Parameter(Mandatory = $true)]
        [bool]$ForceMock,

        [Parameter(Mandatory = $true)]
        [bool]$NoMockFallback,

        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$CEmuHashExpectation,

        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [string[]]$OverrideExpectedVramCrcs = $null
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required to run harness cases."
    }

    $powershellPath = $powershellCommand.Source

    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $HarnessScriptPath,
        "-Keys",
        $KeySequence,
        "-Format",
        "json"
    )

    if ($IncludeRomPath) {
        $arguments += @("-RomPath", $RomPath)
    }

    if ($IncludeAutotesterPath) {
        $arguments += @("-AutotesterPath", $AutotesterPath)
    }

    if ($IncludeProgramPath) {
        $arguments += @("-ProgramPath", $ProgramPath)
    }

    if ($IncludeModuleProgramPaths) {
        $joinedModuleProgramPaths = Join-DelimitedCommandArgumentList -Values $ModuleProgramPaths
        if ($joinedModuleProgramPaths.Length -gt 0) {
            $arguments += @("-ModuleProgramPaths", $joinedModuleProgramPaths)
        }
    }

    $arguments += @(
        "-UsableUserRamLimitBytes",
        [string]$UsableUserRamLimitBytes,
        "-ModuleLinkedBinaryLimitBytes",
        [string]$ModuleLinkedBinaryLimitBytes,
        "-ArchiveRamSwapRamCapacityBytes",
        [string]$ArchiveRamSwapRamCapacityBytes,
        "-ArchiveRamFailurePhase",
        [string]$ArchiveRamFailurePhase
    )

    if (-not [string]::IsNullOrWhiteSpace($ArchiveRamDispatchModulePath)) {
        $arguments += @("-ArchiveRamDispatchModulePath", $ArchiveRamDispatchModulePath)
    }

    if ($ForceMock) {
        $arguments += "-ForceMock"
    }

    if ($NoMockFallback) {
        $arguments += "-NoMockFallback"
    }

    if ($null -ne $CEmuHashExpectation -and [bool]$CEmuHashExpectation.enabled) {
        $expectedCrcs = @($CEmuHashExpectation.expected_crcs)
        if ($null -ne $OverrideExpectedVramCrcs) {
            $expectedCrcs = @($OverrideExpectedVramCrcs)
        }

        if ($expectedCrcs.Count -eq 0) {
            throw "CEmu hash expectation is enabled, but no expected CRCs were provided for harness invocation."
        }

        $joinedExpectedCrcs = Join-DelimitedCommandArgumentList -Values $expectedCrcs
        if ($joinedExpectedCrcs.Length -eq 0) {
            throw "CEmu hash expectation is enabled, but expected CRC normalization produced an empty argument."
        }
        $arguments += @("-ExpectedVramCrcs", $joinedExpectedCrcs)
        $arguments += @(
            "-CEmuHashName",
            [string]$CEmuHashExpectation.name,
            "-CEmuHashStart",
            [string]$CEmuHashExpectation.start,
            "-CEmuHashSize",
            [string]$CEmuHashExpectation.size,
            "-CEmuHashTimeoutMs",
            [string]$CEmuHashExpectation.timeout_ms
        )
    }

    $displayParts = @($powershellPath)
    foreach ($argument in $arguments) {
        $displayParts += (ConvertTo-DisplayCommandArgument -Argument $argument)
    }

    $captured = @()
    $exitCode = 1
    $invocationError = ""

    try {
        $captured = & $powershellPath @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    $outputLines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($captured)) {
        if ($null -ne $entry) {
            $outputLines.Add([string]$entry)
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($invocationError)) {
        $outputLines.Add("Harness invocation error: $invocationError")
    }

    $jsonResult = Find-HarnessJsonInOutput -OutputLines $outputLines.ToArray()

    return [ordered]@{
        command = ($displayParts -join " ")
        exit_code = $exitCode
        output_lines = $outputLines.ToArray()
        json_parse_success = [bool]$jsonResult.success
        json_parse_error = [string]$jsonResult.error
        json_text = [string]$jsonResult.json_text
        json_object = $jsonResult.object
    }
}

function Get-ObservedBaselineCrcFromHarnessJson {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$HarnessJson,

        [Parameter(Mandatory = $true)]
        [string]$AttemptLabel
    )

    if ($null -eq $HarnessJson) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel did not provide harness JSON data."
        }
    }

    if (-not (Test-ObjectHasProperty -Object $HarnessJson -Name "autotest")) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel did not include harness JSON field 'autotest'."
        }
    }

    $autotestObject = Get-ObjectPropertyValue -Object $HarnessJson -Name "autotest"
    if ($null -eq $autotestObject) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel provided null harness JSON field 'autotest'."
        }
    }

    if (-not (Test-ObjectHasProperty -Object $autotestObject -Name "hash_assertion")) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel did not include harness JSON field 'autotest.hash_assertion'."
        }
    }

    $hashAssertionObject = Get-ObjectPropertyValue -Object $autotestObject -Name "hash_assertion"
    if ($null -eq $hashAssertionObject) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel provided null harness JSON field 'autotest.hash_assertion'."
        }
    }

    if (-not (Test-ObjectHasProperty -Object $hashAssertionObject -Name "observed_crcs")) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel did not report 'autotest.hash_assertion.observed_crcs'."
        }
    }

    $observedCrcs = @(ConvertTo-StringArray -Value (Get-ObjectPropertyValue -Object $hashAssertionObject -Name "observed_crcs"))
    if ($observedCrcs.Length -eq 0) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel did not report any observed CRC values."
        }
    }

    $rawCaptured = [string]$observedCrcs[$observedCrcs.Length - 1]
    $normalizedCaptured = ConvertTo-NormalizedCrcString -Value $rawCaptured
    if ($null -eq $normalizedCaptured) {
        return [ordered]@{
            success = $false
            crc = ""
            error = "$AttemptLabel reported invalid observed CRC '$rawCaptured'."
        }
    }

    return [ordered]@{
        success = $true
        crc = $normalizedCaptured
        error = ""
    }
}

function Get-CrcFrequencyRows {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Samples
    )

    $counts = @{}
    foreach ($sample in @($Samples)) {
        if ([string]::IsNullOrWhiteSpace($sample)) {
            continue
        }

        if (-not $counts.ContainsKey($sample)) {
            $counts[$sample] = 0
        }

        $counts[$sample] = [int]$counts[$sample] + 1
    }

    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($crc in $counts.Keys) {
        $rows.Add([ordered]@{
            crc = [string]$crc
            count = [int]$counts[$crc]
        })
    }

    if ($rows.Count -eq 0) {
        return @()
    }

    $sortedRows = $rows.ToArray() | Sort-Object -Property @{ Expression = "count"; Descending = $true }, @{ Expression = "crc"; Descending = $false }
    return @($sortedRows)
}

function Format-CrcFrequencyRows {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowNull()]
        [object[]]$Rows
    )

    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($row in @($Rows)) {
        if ($null -eq $row) {
            continue
        }

        if ((-not (Test-ObjectHasProperty -Object $row -Name "crc")) -or (-not (Test-ObjectHasProperty -Object $row -Name "count"))) {
            continue
        }

        $parts.Add(("{0}:{1}" -f [string]$row.crc, [int]$row.count))
    }

    if ($parts.Count -eq 0) {
        return "<none>"
    }

    return ($parts.ToArray() -join ", ")
}

function Get-BaselineConsensusSelection {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Samples,

        [Parameter(Mandatory = $true)]
        [ValidateRange(1, 20)]
        [int]$ConsensusRequired
    )

    $frequencyRows = @(Get-CrcFrequencyRows -Samples $Samples)
    if ($frequencyRows.Count -eq 0) {
        return [ordered]@{
            success = $false
            crc = ""
            frequency_rows = @()
            error = "Baseline capture did not produce any valid observed CRC samples."
        }
    }

    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($row in $frequencyRows) {
        if ([int]$row.count -ge $ConsensusRequired) {
            $candidates.Add([string]$row.crc)
        }
    }

    if ($candidates.Count -eq 0) {
        return [ordered]@{
            success = $false
            crc = ""
            frequency_rows = $frequencyRows
            error = ("Observed CRC samples did not reach consensus >= {0}. Frequencies: {1}" -f $ConsensusRequired, (Format-CrcFrequencyRows -Rows $frequencyRows))
        }
    }

    if ($candidates.Count -gt 1) {
        return [ordered]@{
            success = $false
            crc = ""
            frequency_rows = $frequencyRows
            error = ("Observed CRC samples are ambiguous at consensus >= {0}. Candidates: {1}. Frequencies: {2}" -f $ConsensusRequired, (($candidates.ToArray()) -join ","), (Format-CrcFrequencyRows -Rows $frequencyRows))
        }
    }

    return [ordered]@{
        success = $true
        crc = [string]$candidates[0]
        frequency_rows = $frequencyRows
        error = ""
    }
}

function New-CaseResultSkeleton {
    param(
        [Parameter(Mandatory = $true)]
        [int]$Index,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    return [ordered]@{
        index = $Index
        name = $Name
        keys = ""
        passed = $false
        duration_ms = 0
        expected = [ordered]@{
            result = ""
            status = ""
            error = ""
            ignored_keys = @()
            mode = $null
            harness_success = $null
            cemu_hash = [ordered]@{
                enabled = $false
                name = ""
                start = ""
                size = ""
                timeout_ms = $null
                expected_crcs = @()
                baseline_placeholder = $false
            }
            archive_ram_swap = [ordered]@{
                enabled = $false
                simulated = $null
                pass = $null
                failure_phase = ""
                dispatch_loaded_to_ram = $null
                return_to_archive_completed = $null
            }
        }
        actual = [ordered]@{
            mode = ""
            result = ""
            status = ""
            error = ""
            ignored_keys = @()
            harness_success = $false
            script_success = $false
            cemu_hash = [ordered]@{
                enabled = $false
                name = ""
                start = ""
                size = ""
                timeout_ms = $null
                expected_crcs = @()
                observed_crcs = @()
                last_observed_crc = ""
                matched = $null
            }
            hardware_constraints = [ordered]@{
                validated = $false
                pass = $true
                memory_insufficient = $false
                module_size_exceeded = $false
                errors = @()
            }
            archive_ram_swap = [ordered]@{
                simulated = $false
                pass = $true
                failure_phase = "none"
                dispatch = [ordered]@{
                    requested = $false
                    module_path = ""
                    resolved_module_path = ""
                    module_size_bytes = $null
                    loaded_to_ram = $false
                    error = ""
                }
                return_to_archive = [ordered]@{
                    attempted = $false
                    returned_to_archive = $false
                    error = ""
                }
                state = [ordered]@{
                    module_in_archive = $false
                    module_in_ram = $false
                }
                modules = [ordered]@{
                    requested_count = 0
                    rapid_swap_cycles = 1
                    reserved_ram_bytes = 0
                    effective_ram_capacity_bytes = 0
                    dispatch_events = 0
                    return_events = 0
                    corruption_detected = $false
                    partial_load_detected = $false
                }
                diagnostics = @()
                errors = @()
            }
        }
        baseline_capture = [ordered]@{
            requested = $false
            captured = $false
            captured_crc = ""
            target_case_updated = $false
            attempts_requested = 0
            attempts_executed = 0
            consensus_required = 0
            consensus_reached = $false
            observed_crc_samples = @()
            observed_crc_counts = @()
        }
        harness = [ordered]@{
            command = ""
            exit_code = $null
            output_lines = @()
            json_parse_success = $false
            json_parse_error = ""
        }
        mismatches = @()
    }
}

function Resolve-RequiredScriptPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InputPath,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($InputPath)) {
        throw "$Description path cannot be empty."
    }

    if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
        throw "$Description was not found at '$InputPath'."
    }

    return (Resolve-Path -LiteralPath $InputPath).Path
}

function Resolve-RunnerRepoRoot {
    return (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
}

function Resolve-ModuleProgramPathInputs {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$IncludeExplicitModuleProgramPaths,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$ExplicitModuleProgramPaths
    )

    $candidatePaths = New-Object System.Collections.Generic.List[string]
    $source = "none"

    if ($IncludeExplicitModuleProgramPaths) {
        $source = "explicit"
        foreach ($explicitPath in @($ExplicitModuleProgramPaths)) {
            if ($null -eq $explicitPath) {
                throw "ModuleProgramPaths cannot contain null values."
            }

            $trimmedPath = ([string]$explicitPath).Trim()
            if ([string]::IsNullOrWhiteSpace($trimmedPath)) {
                throw "ModuleProgramPaths cannot contain empty values."
            }

            $pathSegments = $trimmedPath -split "[;,]"
            foreach ($segment in @($pathSegments)) {
                $trimmedSegment = ([string]$segment).Trim()
                if ([string]::IsNullOrWhiteSpace($trimmedSegment)) {
                    continue
                }

                $candidatePaths.Add($trimmedSegment)
            }
        }
    }
    else {
        $repoRoot = Resolve-RunnerRepoRoot
        $officialArtifacts = Resolve-CalcEmuOfficialSplitSuiteArtifacts -RepoRoot $repoRoot
        if ($officialArtifacts.errors.Count -gt 0) {
            throw ($officialArtifacts.errors -join " | ")
        }
        foreach ($artifactPath in @($officialArtifacts.module_paths)) {
            $candidatePaths.Add([string]$artifactPath)
        }

        if ($candidatePaths.Count -gt 0) {
            $source = [string]$officialArtifacts.source
        }
        elseif ($officialArtifacts.warnings.Count -gt 0) {
            foreach ($warning in @($officialArtifacts.warnings)) {
                Write-Warning ([string]$warning)
            }
        }
    }

    $deduped = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    foreach ($candidatePath in $candidatePaths) {
        $dedupeKey = ([string]$candidatePath).ToLowerInvariant()
        if ($seen.ContainsKey($dedupeKey)) {
            continue
        }

        $seen[$dedupeKey] = $true
        $deduped.Add([string]$candidatePath)
    }

    return [ordered]@{
        include = ($deduped.Count -gt 0)
        source = $source
        paths = $deduped.ToArray()
    }
}

function Get-RunnerHardwareConstraintValidation {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ProgramPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ModuleProgramPaths = @(),

        [Parameter(Mandatory = $true)]
        [int]$UsableUserRamLimitBytes,

        [Parameter(Mandatory = $true)]
        [int]$ModuleLinkedBinaryLimitBytes
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $programValidation = [ordered]@{
        checked = $false
        path = $ProgramPath
        resolved_path = ""
        exists = $false
        size_bytes = $null
        within_limit = $null
        memory_insufficient = $false
    }

    if (-not [string]::IsNullOrWhiteSpace($ProgramPath)) {
        $programValidation.checked = $true
        if (-not (Test-Path -LiteralPath $ProgramPath -PathType Leaf)) {
            $programValidation.exists = $false
            $programValidation.within_limit = $false
            $errors.Add("ProgramPath '$ProgramPath' does not exist.")
        }
        else {
            $resolvedProgramPath = (Resolve-Path -LiteralPath $ProgramPath).Path
            $programValidation.resolved_path = $resolvedProgramPath
            $programValidation.exists = $true
            $programValidation.size_bytes = [int64](Get-Item -LiteralPath $resolvedProgramPath).Length
            $programValidation.within_limit = ($programValidation.size_bytes -le $UsableUserRamLimitBytes)
            if (-not $programValidation.within_limit) {
                $programValidation.memory_insufficient = $true
                $errors.Add(
                    "Memory is insufficient to perform the calculation. Program '$resolvedProgramPath' is $($programValidation.size_bytes) bytes and exceeds the usable user RAM limit of $UsableUserRamLimitBytes bytes (~64KB)."
                )
            }
        }
    }

    $moduleItems = New-Object System.Collections.Generic.List[object]
    $moduleOverLimitCount = 0
    foreach ($moduleProgramPath in @($ModuleProgramPaths)) {
        $item = [ordered]@{
            path = [string]$moduleProgramPath
            resolved_path = ""
            exists = $false
            size_bytes = $null
            within_limit = $null
        }

        if (-not (Test-Path -LiteralPath $moduleProgramPath -PathType Leaf)) {
            $item.exists = $false
            $item.within_limit = $false
            $errors.Add("Module program path '$moduleProgramPath' does not exist.")
            $moduleItems.Add($item)
            continue
        }

        $resolvedModulePath = (Resolve-Path -LiteralPath $moduleProgramPath).Path
        $item.resolved_path = $resolvedModulePath
        $item.exists = $true
        $item.size_bytes = [int64](Get-Item -LiteralPath $resolvedModulePath).Length
        $item.within_limit = ($item.size_bytes -le $ModuleLinkedBinaryLimitBytes)
        if (-not $item.within_limit) {
            $moduleOverLimitCount++
            $errors.Add(
                "Module linked binary exceeds limit. Module '$resolvedModulePath' is $($item.size_bytes) bytes and must be <= $ModuleLinkedBinaryLimitBytes bytes (~50KB)."
            )
        }

        $moduleItems.Add($item)
    }

    $validated = ($programValidation.checked -or $moduleItems.Count -gt 0)
    $pass = $true
    if ($validated) {
        $pass = ($errors.Count -eq 0)
    }

    return [ordered]@{
        validated = $validated
        pass = $pass
        memory_insufficient = [bool]$programValidation.memory_insufficient
        module_size_exceeded = ($moduleOverLimitCount -gt 0)
        errors = $errors.ToArray()
        program = $programValidation
        modules = [ordered]@{
            checked_count = $moduleItems.Count
            items = $moduleItems.ToArray()
        }
    }
}

function Resolve-CaseCollection {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ParsedJson
    )

    if ($ParsedJson -is [System.Array]) {
        return @($ParsedJson)
    }

    if (Test-ObjectHasProperty -Object $ParsedJson -Name "cases") {
        return @($ParsedJson.cases)
    }

    throw "Case file JSON must be either an array of cases or an object containing a 'cases' array."
}

function Set-ExpectedCaseCEmuHashCrcs {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ExpectedObject,

        [Parameter(Mandatory = $true)]
        [object]$ExpectedCEmuHash,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedCrcs
    )

    $hashPropertyName = [string]$ExpectedCEmuHash.hash_property_name
    if ([string]::IsNullOrWhiteSpace($hashPropertyName)) {
        $hashPropertyName = "cemu_hash"
    }

    $hashObject = $null
    if (Test-ObjectHasProperty -Object $ExpectedObject -Name $hashPropertyName) {
        $hashObject = Get-ObjectPropertyValue -Object $ExpectedObject -Name $hashPropertyName
    }

    if ($null -eq $hashObject) {
        $hashObject = [ordered]@{}
        Set-ObjectPropertyValue -Object $ExpectedObject -Name $hashPropertyName -Value $hashObject
    }

    $expectedCrcPropertyName = [string]$ExpectedCEmuHash.expected_crc_property_name
    if ([string]::IsNullOrWhiteSpace($expectedCrcPropertyName)) {
        $expectedCrcPropertyName = "expected_crcs"
    }

    Set-ObjectPropertyValue -Object $hashObject -Name $expectedCrcPropertyName -Value @($ExpectedCrcs)
}

function Get-ExpectedFieldValue {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ExpectedObject,

        [Parameter(Mandatory = $true)]
        [string]$FieldName,

        [Parameter(Mandatory = $true)]
        [object]$Mismatches
    )

    if (-not (Test-ObjectHasProperty -Object $ExpectedObject -Name $FieldName)) {
        $Mismatches.Add("Missing required expected field '$FieldName'.")
        return ""
    }

    $value = Get-ObjectPropertyValue -Object $ExpectedObject -Name $FieldName
    if ($null -eq $value) {
        return ""
    }

    return [string]$value
}

function Get-ExpectedIgnoredKeys {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ExpectedObject,

        [Parameter(Mandatory = $true)]
        [object]$Mismatches
    )

    if (Test-ObjectHasProperty -Object $ExpectedObject -Name "ignored_keys") {
        return ConvertTo-StringArray -Value $ExpectedObject.ignored_keys
    }

    if (Test-ObjectHasProperty -Object $ExpectedObject -Name "ignoredKeys") {
        return ConvertTo-StringArray -Value $ExpectedObject.ignoredKeys
    }

    $Mismatches.Add("Missing required expected field 'ignored_keys'.")
    return @()
}

function Get-ExpectedArchiveRamSwap {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ExpectedObject,

        [Parameter(Mandatory = $true)]
        [object]$Mismatches
    )

    $expectedArchiveRamSwap = [ordered]@{
        enabled = $false
        simulated = $null
        pass = $null
        failure_phase = ""
        dispatch_loaded_to_ram = $null
        return_to_archive_completed = $null
    }

    $archiveSwapObject = $null
    if (Test-ObjectHasProperty -Object $ExpectedObject -Name "archive_ram_swap") {
        $archiveSwapObject = Get-ObjectPropertyValue -Object $ExpectedObject -Name "archive_ram_swap"
    }
    elseif (Test-ObjectHasProperty -Object $ExpectedObject -Name "archiveRamSwap") {
        $archiveSwapObject = Get-ObjectPropertyValue -Object $ExpectedObject -Name "archiveRamSwap"
    }
    else {
        return $expectedArchiveRamSwap
    }

    if ($null -eq $archiveSwapObject) {
        $Mismatches.Add("Expected field 'archive_ram_swap' cannot be null when provided.")
        return $expectedArchiveRamSwap
    }

    $expectedArchiveRamSwap.enabled = $true

    if (Test-ObjectHasProperty -Object $archiveSwapObject -Name "simulated") {
        try {
            $expectedArchiveRamSwap.simulated = ConvertTo-StrictBoolean -Value (Get-ObjectPropertyValue -Object $archiveSwapObject -Name "simulated")
        }
        catch {
            $Mismatches.Add("Expected field 'archive_ram_swap.simulated' is invalid: $($_.Exception.Message)")
        }
    }

    if (Test-ObjectHasProperty -Object $archiveSwapObject -Name "pass") {
        try {
            $expectedArchiveRamSwap.pass = ConvertTo-StrictBoolean -Value (Get-ObjectPropertyValue -Object $archiveSwapObject -Name "pass")
        }
        catch {
            $Mismatches.Add("Expected field 'archive_ram_swap.pass' is invalid: $($_.Exception.Message)")
        }
    }

    if (Test-ObjectHasProperty -Object $archiveSwapObject -Name "failure_phase") {
        $expectedArchiveRamSwap.failure_phase = [string](Get-ObjectPropertyValue -Object $archiveSwapObject -Name "failure_phase")
    }

    if (Test-ObjectHasProperty -Object $archiveSwapObject -Name "dispatch_loaded_to_ram") {
        try {
            $expectedArchiveRamSwap.dispatch_loaded_to_ram = ConvertTo-StrictBoolean -Value (Get-ObjectPropertyValue -Object $archiveSwapObject -Name "dispatch_loaded_to_ram")
        }
        catch {
            $Mismatches.Add("Expected field 'archive_ram_swap.dispatch_loaded_to_ram' is invalid: $($_.Exception.Message)")
        }
    }

    if (Test-ObjectHasProperty -Object $archiveSwapObject -Name "return_to_archive_completed") {
        try {
            $expectedArchiveRamSwap.return_to_archive_completed = ConvertTo-StrictBoolean -Value (Get-ObjectPropertyValue -Object $archiveSwapObject -Name "return_to_archive_completed")
        }
        catch {
            $Mismatches.Add("Expected field 'archive_ram_swap.return_to_archive_completed' is invalid: $($_.Exception.Message)")
        }
    }

    return $expectedArchiveRamSwap
}

function ConvertTo-NormalizedCrcList {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [object]$Mismatches,

        [Parameter(Mandatory = $true)]
        [string]$FieldDescription,

        [Parameter(Mandatory = $false)]
        [bool]$AllowBaselinePlaceholder = $false,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$BaselinePlaceholder = "__BASELINE_REQUIRED__"
    )

    $rawValues = ConvertTo-StringArray -Value $Value
    if ($rawValues.Count -eq 0) {
        $Mismatches.Add("$FieldDescription must contain at least one CRC value.")
        return [ordered]@{
            expected_crcs = @()
            has_baseline_placeholder = $false
        }
    }

    $normalized = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    $hasBaselinePlaceholder = $false

    foreach ($rawValue in $rawValues) {
        $trimmed = $rawValue.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) {
            $Mismatches.Add("$FieldDescription cannot contain empty CRC values.")
            continue
        }

        if ($trimmed -ceq $BaselinePlaceholder) {
            $hasBaselinePlaceholder = $true
            continue
        }

        $canonical = ConvertTo-NormalizedCrcString -Value $trimmed
        if ($null -eq $canonical) {
            $Mismatches.Add("$FieldDescription contains invalid CRC '$trimmed'. CRC values must be 8 hexadecimal characters (optionally prefixed with 0x).")
            continue
        }

        if (-not $seen.ContainsKey($canonical)) {
            $seen[$canonical] = $true
            $normalized.Add($canonical)
        }
    }

    if ($hasBaselinePlaceholder -and -not $AllowBaselinePlaceholder) {
        $Mismatches.Add(
            "$FieldDescription contains '$BaselinePlaceholder'. Re-run with -CaptureCEmuHashBaselines and valid CEmu inputs to collect strict hash baselines."
        )
    }

    if ($hasBaselinePlaceholder -and $normalized.Count -gt 0) {
        $Mismatches.Add("$FieldDescription cannot combine '$BaselinePlaceholder' with concrete CRC values.")
    }

    if (-not $hasBaselinePlaceholder -and $normalized.Count -eq 0) {
        $Mismatches.Add("$FieldDescription must contain at least one valid CRC value.")
    }

    return [ordered]@{
        expected_crcs = $normalized.ToArray()
        has_baseline_placeholder = $hasBaselinePlaceholder
    }
}

function Get-ExpectedCEmuHash {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ExpectedObject,

        [Parameter(Mandatory = $true)]
        [object]$Mismatches,

        [Parameter(Mandatory = $false)]
        [bool]$AllowBaselinePlaceholder = $false,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$BaselinePlaceholder = "__BASELINE_REQUIRED__"
    )

    $expectedHash = [ordered]@{
        enabled = $false
        name = "terminal_vram"
        start = "vram_start"
        size = "vram_16_size"
        timeout_ms = 8000
        expected_crcs = @()
        baseline_placeholder = $false
        hash_property_name = ""
        expected_crc_property_name = ""
    }

    $hashObject = $null
    if (Test-ObjectHasProperty -Object $ExpectedObject -Name "cemu_hash") {
        $hashObject = Get-ObjectPropertyValue -Object $ExpectedObject -Name "cemu_hash"
        $expectedHash.hash_property_name = "cemu_hash"
    }
    elseif (Test-ObjectHasProperty -Object $ExpectedObject -Name "cemuHash") {
        $hashObject = Get-ObjectPropertyValue -Object $ExpectedObject -Name "cemuHash"
        $expectedHash.hash_property_name = "cemuHash"
    }

    if ($null -eq $hashObject) {
        return $expectedHash
    }

    $expectedHash.enabled = $true

    if (Test-ObjectHasProperty -Object $hashObject -Name "name") {
        $nameCandidate = [string](Get-ObjectPropertyValue -Object $hashObject -Name "name")
        if ([string]::IsNullOrWhiteSpace($nameCandidate)) {
            $Mismatches.Add("Expected field 'cemu_hash.name' cannot be empty.")
        }
        elseif ($nameCandidate -notmatch "^[A-Za-z0-9_]+$") {
            $Mismatches.Add("Expected field 'cemu_hash.name' is invalid. Only letters, numbers, and underscores are supported.")
        }
        else {
            $expectedHash.name = $nameCandidate
        }
    }

    if (Test-ObjectHasProperty -Object $hashObject -Name "start") {
        $startCandidate = [string](Get-ObjectPropertyValue -Object $hashObject -Name "start")
        if ([string]::IsNullOrWhiteSpace($startCandidate)) {
            $Mismatches.Add("Expected field 'cemu_hash.start' cannot be empty.")
        }
        else {
            $expectedHash.start = $startCandidate
        }
    }

    if (Test-ObjectHasProperty -Object $hashObject -Name "size") {
        $sizeCandidate = [string](Get-ObjectPropertyValue -Object $hashObject -Name "size")
        if ([string]::IsNullOrWhiteSpace($sizeCandidate)) {
            $Mismatches.Add("Expected field 'cemu_hash.size' cannot be empty.")
        }
        else {
            $expectedHash.size = $sizeCandidate
        }
    }

    if (Test-ObjectHasProperty -Object $hashObject -Name "timeout_ms") {
        $timeoutCandidate = Get-ObjectPropertyValue -Object $hashObject -Name "timeout_ms"
        try {
            $timeoutAsInt = [int]$timeoutCandidate
            if ($timeoutAsInt -lt 1) {
                throw "timeout_ms must be >= 1."
            }

            $expectedHash.timeout_ms = $timeoutAsInt
        }
        catch {
            $Mismatches.Add("Expected field 'cemu_hash.timeout_ms' is invalid: $($_.Exception.Message)")
        }
    }

    if (Test-ObjectHasProperty -Object $hashObject -Name "expected_crcs") {
        $expectedHash.expected_crc_property_name = "expected_crcs"
        $crcParseResult = ConvertTo-NormalizedCrcList `
            -Value (Get-ObjectPropertyValue -Object $hashObject -Name "expected_crcs") `
            -Mismatches $Mismatches `
            -FieldDescription "Expected field 'cemu_hash.expected_crcs'" `
            -AllowBaselinePlaceholder:$AllowBaselinePlaceholder `
            -BaselinePlaceholder $BaselinePlaceholder
        $expectedHash.expected_crcs = @($crcParseResult.expected_crcs)
        $expectedHash.baseline_placeholder = [bool]$crcParseResult.has_baseline_placeholder
    }
    elseif (Test-ObjectHasProperty -Object $hashObject -Name "expectedCRCs") {
        $expectedHash.expected_crc_property_name = "expectedCRCs"
        $crcParseResult = ConvertTo-NormalizedCrcList `
            -Value (Get-ObjectPropertyValue -Object $hashObject -Name "expectedCRCs") `
            -Mismatches $Mismatches `
            -FieldDescription "Expected field 'cemu_hash.expectedCRCs'" `
            -AllowBaselinePlaceholder:$AllowBaselinePlaceholder `
            -BaselinePlaceholder $BaselinePlaceholder
        $expectedHash.expected_crcs = @($crcParseResult.expected_crcs)
        $expectedHash.baseline_placeholder = [bool]$crcParseResult.has_baseline_placeholder
    }
    else {
        $Mismatches.Add("Expected field 'cemu_hash.expected_crcs' is required when 'cemu_hash' is provided.")
    }

    return $expectedHash
}

try {
    $includeRomPath = $PSBoundParameters.ContainsKey("RomPath")
    $includeAutotesterPath = $PSBoundParameters.ContainsKey("AutotesterPath")
    $includeProgramPath = $PSBoundParameters.ContainsKey("ProgramPath")
    $includeExplicitModuleProgramPaths = $PSBoundParameters.ContainsKey("ModuleProgramPaths")
    $captureHashBaselines = $CaptureCEmuHashBaselines.IsPresent

    $moduleProgramPathResolution = Resolve-ModuleProgramPathInputs `
        -IncludeExplicitModuleProgramPaths $includeExplicitModuleProgramPaths `
        -ExplicitModuleProgramPaths $ModuleProgramPaths
    $includeModuleProgramPaths = [bool]$moduleProgramPathResolution.include
    $moduleProgramPathSource = [string]$moduleProgramPathResolution.source
    $resolvedModuleProgramPaths = @($moduleProgramPathResolution.paths)
    $passModuleProgramPathsToHarness = $includeModuleProgramPaths

    $programPathForConstraintValidation = ""
    if ($includeProgramPath) {
        $programPathForConstraintValidation = $ProgramPath
    }

    $runnerHardwareConstraints = Get-RunnerHardwareConstraintValidation `
        -ProgramPath $programPathForConstraintValidation `
        -ModuleProgramPaths $resolvedModuleProgramPaths `
        -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
        -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes

    if ([string]::IsNullOrWhiteSpace($BaselinePlaceholder)) {
        throw "BaselinePlaceholder cannot be empty."
    }

    if ($captureHashBaselines -and $BaselineCaptureConsensus -gt $BaselineCaptureSampleCount) {
        throw ("BaselineCaptureConsensus ({0}) cannot be greater than BaselineCaptureSampleCount ({1})." -f $BaselineCaptureConsensus, $BaselineCaptureSampleCount)
    }

    $normalizedBaselineSeedCrc = ""
    if ($captureHashBaselines) {
        $normalizedBaselineSeedCrc = ConvertTo-NormalizedCrcString -Value $BaselineSeedCrc
        if ($null -eq $normalizedBaselineSeedCrc) {
            throw "BaselineSeedCrc '$BaselineSeedCrc' is invalid. Use an 8-digit hexadecimal CRC."
        }
    }

    $resolvedCaseFile = Resolve-RequiredScriptPath -InputPath $CaseFile -Description "Case file"
    $resolvedBaselineOutputCaseFile = ""
    if ($captureHashBaselines) {
        if ([string]::IsNullOrWhiteSpace($BaselineOutputCaseFile)) {
            $caseDirectory = Split-Path -Path $resolvedCaseFile -Parent
            $caseNameWithoutExtension = [System.IO.Path]::GetFileNameWithoutExtension($resolvedCaseFile)
            $resolvedBaselineOutputCaseFile = Join-Path $caseDirectory ("{0}.baselined.json" -f $caseNameWithoutExtension)
        }
        else {
            $resolvedBaselineOutputCaseFile = $BaselineOutputCaseFile
        }
    }

    if ([string]::IsNullOrWhiteSpace($HarnessScriptPath)) {
        $HarnessScriptPath = Join-Path $PSScriptRoot "run-terminal-harness.ps1"
    }
    $resolvedHarnessScript = Resolve-RequiredScriptPath -InputPath $HarnessScriptPath -Description "Harness script"

    $rawCaseText = Get-Content -LiteralPath $resolvedCaseFile -Raw
    $parsedCaseJson = $rawCaseText | ConvertFrom-Json
    $cases = @(Resolve-CaseCollection -ParsedJson $parsedCaseJson)

    if ($cases.Count -eq 0) {
        throw "No cases were found in '$resolvedCaseFile'."
    }

    $caseResults = New-Object System.Collections.Generic.List[object]
    $baselineUpdatedCases = 0
    $baselineCapturedCases = 0

    for ($i = 0; $i -lt $cases.Count; $i++) {
        $caseStart = Get-Date
        $case = $cases[$i]
        $caseName = "case-{0}" -f ($i + 1)
        if (Test-ObjectHasProperty -Object $case -Name "name") {
            $nameCandidate = [string]$case.name
            if (-not [string]::IsNullOrWhiteSpace($nameCandidate)) {
                $caseName = $nameCandidate
            }
        }

        $mismatches = New-Object System.Collections.Generic.List[string]
        $caseResult = New-CaseResultSkeleton -Index ($i + 1) -Name $caseName

        if (-not (Test-ObjectHasProperty -Object $case -Name "keys")) {
            $mismatches.Add("Case is missing required field 'keys'.")
        }

        $keys = ""
        if ($mismatches.Count -eq 0) {
            $keys = [string]$case.keys
            if ([string]::IsNullOrWhiteSpace($keys)) {
                $mismatches.Add("Case field 'keys' cannot be empty.")
            }
        }
        $caseResult.keys = $keys

        $expectedObject = $null
        if (Test-ObjectHasProperty -Object $case -Name "expect") {
            $expectedObject = $case.expect
        }
        elseif (Test-ObjectHasProperty -Object $case -Name "expected") {
            $expectedObject = $case.expected
        }
        else {
            $mismatches.Add("Case is missing required field 'expect'.")
        }

        $expectedResult = ""
        $expectedStatus = ""
        $expectedError = ""
        $expectedIgnoredKeys = @()
        $expectedMode = $null
        $expectedHarnessSuccess = $true
        $hasExpectedHarnessSuccess = $false
        $expectedCEmuHash = [ordered]@{
            enabled = $false
            name = ""
            start = ""
            size = ""
            timeout_ms = $null
            expected_crcs = @()
            baseline_placeholder = $false
            hash_property_name = ""
            expected_crc_property_name = ""
        }
        $expectedArchiveRamSwap = [ordered]@{
            enabled = $false
            simulated = $null
            pass = $null
            failure_phase = ""
            dispatch_loaded_to_ram = $null
            return_to_archive_completed = $null
        }

        if ($null -ne $expectedObject) {
            $expectedResult = Get-ExpectedFieldValue -ExpectedObject $expectedObject -FieldName "result" -Mismatches $mismatches
            $expectedStatus = Get-ExpectedFieldValue -ExpectedObject $expectedObject -FieldName "status" -Mismatches $mismatches
            $expectedError = Get-ExpectedFieldValue -ExpectedObject $expectedObject -FieldName "error" -Mismatches $mismatches
            $expectedIgnoredKeys = Get-ExpectedIgnoredKeys -ExpectedObject $expectedObject -Mismatches $mismatches
            $expectedCEmuHash = Get-ExpectedCEmuHash `
                -ExpectedObject $expectedObject `
                -Mismatches $mismatches `
                -AllowBaselinePlaceholder:$captureHashBaselines `
                -BaselinePlaceholder $BaselinePlaceholder
            $expectedArchiveRamSwap = Get-ExpectedArchiveRamSwap `
                -ExpectedObject $expectedObject `
                -Mismatches $mismatches

            if (Test-ObjectHasProperty -Object $expectedObject -Name "mode") {
                $expectedMode = [string](Get-ObjectPropertyValue -Object $expectedObject -Name "mode")
            }

            if (Test-ObjectHasProperty -Object $expectedObject -Name "harness_success") {
                try {
                    $expectedHarnessSuccess = ConvertTo-StrictBoolean -Value (Get-ObjectPropertyValue -Object $expectedObject -Name "harness_success")
                    $hasExpectedHarnessSuccess = $true
                }
                catch {
                    $mismatches.Add("Expected field 'harness_success' is invalid: $($_.Exception.Message)")
                }
            }
        }

        $caseResult.expected.result = $expectedResult
        $caseResult.expected.status = $expectedStatus
        $caseResult.expected.error = $expectedError
        $caseResult.expected.ignored_keys = $expectedIgnoredKeys
        $caseResult.expected.mode = $expectedMode
        if ($hasExpectedHarnessSuccess) {
            $caseResult.expected.harness_success = $expectedHarnessSuccess
        }
        $caseResult.expected.cemu_hash.enabled = [bool]$expectedCEmuHash.enabled
        $caseResult.expected.cemu_hash.name = [string]$expectedCEmuHash.name
        $caseResult.expected.cemu_hash.start = [string]$expectedCEmuHash.start
        $caseResult.expected.cemu_hash.size = [string]$expectedCEmuHash.size
        $caseResult.expected.cemu_hash.timeout_ms = $expectedCEmuHash.timeout_ms
        $caseResult.expected.cemu_hash.expected_crcs = @($expectedCEmuHash.expected_crcs)
        $caseResult.expected.cemu_hash.baseline_placeholder = [bool]$expectedCEmuHash.baseline_placeholder
        $caseResult.expected.archive_ram_swap.enabled = [bool]$expectedArchiveRamSwap.enabled
        $caseResult.expected.archive_ram_swap.simulated = $expectedArchiveRamSwap.simulated
        $caseResult.expected.archive_ram_swap.pass = $expectedArchiveRamSwap.pass
        $caseResult.expected.archive_ram_swap.failure_phase = [string]$expectedArchiveRamSwap.failure_phase
        $caseResult.expected.archive_ram_swap.dispatch_loaded_to_ram = $expectedArchiveRamSwap.dispatch_loaded_to_ram
        $caseResult.expected.archive_ram_swap.return_to_archive_completed = $expectedArchiveRamSwap.return_to_archive_completed
        $caseCaptureBaselineRequested = ($captureHashBaselines -and [bool]$expectedCEmuHash.enabled -and [bool]$expectedCEmuHash.baseline_placeholder)
        $caseResult.baseline_capture.requested = $caseCaptureBaselineRequested
        if ($caseCaptureBaselineRequested) {
            $caseResult.baseline_capture.attempts_requested = [int]$BaselineCaptureSampleCount
            $caseResult.baseline_capture.consensus_required = [int]$BaselineCaptureConsensus
        }

        $caseResult.actual.hardware_constraints.validated = [bool]$runnerHardwareConstraints.validated
        $caseResult.actual.hardware_constraints.pass = [bool]$runnerHardwareConstraints.pass
        $caseResult.actual.hardware_constraints.memory_insufficient = [bool]$runnerHardwareConstraints.memory_insufficient
        $caseResult.actual.hardware_constraints.module_size_exceeded = [bool]$runnerHardwareConstraints.module_size_exceeded
        $caseResult.actual.hardware_constraints.errors = @($runnerHardwareConstraints.errors)

        if ($runnerHardwareConstraints.validated -and -not $runnerHardwareConstraints.pass) {
            foreach ($constraintError in @($runnerHardwareConstraints.errors)) {
                $mismatches.Add("Hardware constraint validation failed: $constraintError")
            }
        }

        $harnessInvocation = $null
        if ($mismatches.Count -eq 0) {
            $overrideExpectedVramCrcs = $null
            if ($caseCaptureBaselineRequested) {
                $overrideExpectedVramCrcs = @($normalizedBaselineSeedCrc)
            }

            $harnessInvocation = Invoke-HarnessCase `
                -HarnessScriptPath $resolvedHarnessScript `
                -KeySequence $keys `
                -IncludeRomPath $includeRomPath `
                -IncludeAutotesterPath $includeAutotesterPath `
                -IncludeProgramPath $includeProgramPath `
                -IncludeModuleProgramPaths $passModuleProgramPathsToHarness `
                -RomPath $RomPath `
                -AutotesterPath $AutotesterPath `
                -ProgramPath $ProgramPath `
                -ModuleProgramPaths $resolvedModuleProgramPaths `
                -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
                -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes `
                -ArchiveRamDispatchModulePath $ArchiveRamDispatchModulePath `
                -ArchiveRamSwapRamCapacityBytes $ArchiveRamSwapRamCapacityBytes `
                -ArchiveRamFailurePhase $ArchiveRamFailurePhase `
                -ForceMock:$ForceMock `
                -NoMockFallback:$NoMockFallback `
                -CEmuHashExpectation $expectedCEmuHash `
                -OverrideExpectedVramCrcs $overrideExpectedVramCrcs

            $caseResult.harness.command = $harnessInvocation.command
            $caseResult.harness.exit_code = $harnessInvocation.exit_code
            $caseResult.harness.output_lines = $harnessInvocation.output_lines
            $caseResult.harness.json_parse_success = [bool]$harnessInvocation.json_parse_success
            $caseResult.harness.json_parse_error = [string]$harnessInvocation.json_parse_error

            $actualHarnessSuccess = ($harnessInvocation.exit_code -eq 0)
            $caseResult.actual.harness_success = $actualHarnessSuccess

            if ($harnessInvocation.json_parse_success) {
                $harnessJson = $harnessInvocation.json_object
                $actualMode = ""
                $actualResult = ""
                $actualStatus = ""
                $actualError = ""
                $actualIgnored = @()
                $actualScriptSuccess = $false
                $actualCEmuHash = [ordered]@{
                    enabled = $false
                    name = ""
                    start = ""
                    size = ""
                    timeout_ms = $null
                    expected_crcs = @()
                    observed_crcs = @()
                    last_observed_crc = ""
                    matched = $null
                }
                $actualHardwareConstraints = [ordered]@{
                    validated = [bool]$runnerHardwareConstraints.validated
                    pass = [bool]$runnerHardwareConstraints.pass
                    memory_insufficient = [bool]$runnerHardwareConstraints.memory_insufficient
                    module_size_exceeded = [bool]$runnerHardwareConstraints.module_size_exceeded
                    errors = ConvertTo-StringArray -Value $runnerHardwareConstraints.errors
                }
                $actualArchiveRamSwap = [ordered]@{
                    simulated = $false
                    pass = $true
                    failure_phase = "none"
                    dispatch = [ordered]@{
                        requested = $false
                        module_path = ""
                        resolved_module_path = ""
                        module_size_bytes = $null
                        loaded_to_ram = $false
                        error = ""
                    }
                    return_to_archive = [ordered]@{
                        attempted = $false
                        returned_to_archive = $false
                        error = ""
                    }
                    state = [ordered]@{
                        module_in_archive = $false
                        module_in_ram = $false
                    }
                    modules = [ordered]@{
                        requested_count = 0
                        rapid_swap_cycles = 1
                        reserved_ram_bytes = 0
                        effective_ram_capacity_bytes = 0
                        dispatch_events = 0
                        return_events = 0
                        corruption_detected = $false
                        partial_load_detected = $false
                    }
                    diagnostics = @()
                    errors = @()
                }

                if (Test-ObjectHasProperty -Object $harnessJson -Name "mode") {
                    $actualMode = [string]$harnessJson.mode
                }

                if (Test-ObjectHasProperty -Object $harnessJson -Name "success") {
                    try {
                        $actualScriptSuccess = ConvertTo-StrictBoolean -Value $harnessJson.success
                    }
                    catch {
                        $mismatches.Add("Harness JSON 'success' field is not boolean-compatible.")
                    }
                }

                if ((Test-ObjectHasProperty -Object $harnessJson -Name "screen") -and ($null -ne $harnessJson.screen)) {
                    $screen = $harnessJson.screen
                    if (Test-ObjectHasProperty -Object $screen -Name "result") {
                        $actualResult = [string]$screen.result
                    }
                    if (Test-ObjectHasProperty -Object $screen -Name "status") {
                        $actualStatus = [string]$screen.status
                    }
                    if (Test-ObjectHasProperty -Object $screen -Name "error") {
                        $actualError = [string]$screen.error
                    }
                    if (Test-ObjectHasProperty -Object $screen -Name "ignored_keys") {
                        $actualIgnored = ConvertTo-StringArray -Value $screen.ignored_keys
                    }
                }
                else {
                    $mismatches.Add("Harness JSON does not contain the required 'screen' object.")
                }

                $caseResult.actual.mode = $actualMode
                $caseResult.actual.result = $actualResult
                $caseResult.actual.status = $actualStatus
                $caseResult.actual.error = $actualError
                $caseResult.actual.ignored_keys = $actualIgnored
                $caseResult.actual.script_success = $actualScriptSuccess

                if ((Test-ObjectHasProperty -Object $harnessJson -Name "autotest") -and ($null -ne $harnessJson.autotest)) {
                    $autotestObject = $harnessJson.autotest
                    if ((Test-ObjectHasProperty -Object $autotestObject -Name "hash_assertion") -and ($null -ne $autotestObject.hash_assertion)) {
                        $actualHashAssertionObject = $autotestObject.hash_assertion

                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "enabled") {
                            try {
                                $actualCEmuHash.enabled = ConvertTo-StrictBoolean -Value $actualHashAssertionObject.enabled
                            }
                            catch {
                                $mismatches.Add("Harness JSON 'autotest.hash_assertion.enabled' is not boolean-compatible.")
                            }
                        }

                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "name") {
                            $actualCEmuHash.name = [string]$actualHashAssertionObject.name
                        }
                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "start") {
                            $actualCEmuHash.start = [string]$actualHashAssertionObject.start
                        }
                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "size") {
                            $actualCEmuHash.size = [string]$actualHashAssertionObject.size
                        }
                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "timeout_ms") {
                            $actualCEmuHash.timeout_ms = $actualHashAssertionObject.timeout_ms
                        }
                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "expected_crcs") {
                            $actualCEmuHash.expected_crcs = ConvertTo-StringArray -Value $actualHashAssertionObject.expected_crcs
                        }
                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "observed_crcs") {
                            $actualCEmuHash.observed_crcs = ConvertTo-StringArray -Value $actualHashAssertionObject.observed_crcs
                        }
                        if (Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "last_observed_crc") {
                            $actualCEmuHash.last_observed_crc = [string]$actualHashAssertionObject.last_observed_crc
                        }
                        if ((Test-ObjectHasProperty -Object $actualHashAssertionObject -Name "matched") -and ($null -ne $actualHashAssertionObject.matched)) {
                            try {
                                $actualCEmuHash.matched = ConvertTo-StrictBoolean -Value $actualHashAssertionObject.matched
                            }
                            catch {
                                $mismatches.Add("Harness JSON 'autotest.hash_assertion.matched' is not boolean-compatible.")
                            }
                        }
                    }
                }

                if ((Test-ObjectHasProperty -Object $harnessJson -Name "hardware_constraints") -and ($null -ne $harnessJson.hardware_constraints)) {
                    $hardwareConstraintsObject = $harnessJson.hardware_constraints
                    $harnessConstraintValidated = $false
                    $harnessConstraintPass = $true
                    $harnessConstraintMemoryInsufficient = $false
                    $harnessConstraintModuleSizeExceeded = $false
                    $harnessConstraintErrors = @()

                    if (Test-ObjectHasProperty -Object $hardwareConstraintsObject -Name "validated") {
                        try {
                            $harnessConstraintValidated = ConvertTo-StrictBoolean -Value $hardwareConstraintsObject.validated
                        }
                        catch {
                            $mismatches.Add("Harness JSON 'hardware_constraints.validated' is not boolean-compatible.")
                        }
                    }

                    if (Test-ObjectHasProperty -Object $hardwareConstraintsObject -Name "pass") {
                        try {
                            $harnessConstraintPass = ConvertTo-StrictBoolean -Value $hardwareConstraintsObject.pass
                        }
                        catch {
                            $mismatches.Add("Harness JSON 'hardware_constraints.pass' is not boolean-compatible.")
                        }
                    }

                    if (Test-ObjectHasProperty -Object $hardwareConstraintsObject -Name "memory_insufficient") {
                        try {
                            $harnessConstraintMemoryInsufficient = ConvertTo-StrictBoolean -Value $hardwareConstraintsObject.memory_insufficient
                        }
                        catch {
                            $mismatches.Add("Harness JSON 'hardware_constraints.memory_insufficient' is not boolean-compatible.")
                        }
                    }

                    if (Test-ObjectHasProperty -Object $hardwareConstraintsObject -Name "module_size_exceeded") {
                        try {
                            $harnessConstraintModuleSizeExceeded = ConvertTo-StrictBoolean -Value $hardwareConstraintsObject.module_size_exceeded
                        }
                        catch {
                            $mismatches.Add("Harness JSON 'hardware_constraints.module_size_exceeded' is not boolean-compatible.")
                        }
                    }

                    if (Test-ObjectHasProperty -Object $hardwareConstraintsObject -Name "errors") {
                        $harnessConstraintErrors = ConvertTo-StringArray -Value $hardwareConstraintsObject.errors
                    }

                    $actualHardwareConstraints.validated = (
                        [bool]$actualHardwareConstraints.validated -or
                        [bool]$harnessConstraintValidated
                    )
                    if ($harnessConstraintValidated) {
                        $actualHardwareConstraints.pass = (
                            [bool]$actualHardwareConstraints.pass -and
                            [bool]$harnessConstraintPass
                        )
                    }
                    $actualHardwareConstraints.memory_insufficient = (
                        [bool]$actualHardwareConstraints.memory_insufficient -or
                        [bool]$harnessConstraintMemoryInsufficient
                    )
                    $actualHardwareConstraints.module_size_exceeded = (
                        [bool]$actualHardwareConstraints.module_size_exceeded -or
                        [bool]$harnessConstraintModuleSizeExceeded
                    )

                    $combinedConstraintErrors = New-Object System.Collections.Generic.List[string]
                    $seenConstraintErrors = @{}
                    foreach ($existingConstraintError in @($actualHardwareConstraints.errors)) {
                        $errorText = [string]$existingConstraintError
                        if ([string]::IsNullOrWhiteSpace($errorText)) {
                            continue
                        }
                        if (-not $seenConstraintErrors.ContainsKey($errorText)) {
                            $seenConstraintErrors[$errorText] = $true
                            $combinedConstraintErrors.Add($errorText)
                        }
                    }
                    foreach ($harnessConstraintError in @($harnessConstraintErrors)) {
                        $errorText = [string]$harnessConstraintError
                        if ([string]::IsNullOrWhiteSpace($errorText)) {
                            continue
                        }
                        if (-not $seenConstraintErrors.ContainsKey($errorText)) {
                            $seenConstraintErrors[$errorText] = $true
                            $combinedConstraintErrors.Add($errorText)
                        }
                    }
                    $actualHardwareConstraints.errors = $combinedConstraintErrors.ToArray()
                }

                if ((Test-ObjectHasProperty -Object $harnessJson -Name "archive_ram_swap") -and ($null -ne $harnessJson.archive_ram_swap)) {
                    $archiveRamSwapObject = $harnessJson.archive_ram_swap

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "simulated") {
                        try {
                            $actualArchiveRamSwap.simulated = ConvertTo-StrictBoolean -Value $archiveRamSwapObject.simulated
                        }
                        catch {
                            $mismatches.Add("Harness JSON 'archive_ram_swap.simulated' is not boolean-compatible.")
                        }
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "pass") {
                        try {
                            $actualArchiveRamSwap.pass = ConvertTo-StrictBoolean -Value $archiveRamSwapObject.pass
                        }
                        catch {
                            $mismatches.Add("Harness JSON 'archive_ram_swap.pass' is not boolean-compatible.")
                        }
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "failure_phase") {
                        $actualArchiveRamSwap.failure_phase = [string]$archiveRamSwapObject.failure_phase
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "dispatch") {
                        $dispatchObject = Get-ObjectPropertyValue -Object $archiveRamSwapObject -Name "dispatch"
                        if ($null -ne $dispatchObject) {
                            if (Test-ObjectHasProperty -Object $dispatchObject -Name "requested") {
                                try {
                                    $actualArchiveRamSwap.dispatch.requested = ConvertTo-StrictBoolean -Value $dispatchObject.requested
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.dispatch.requested' is not boolean-compatible.")
                                }
                            }
                            if (Test-ObjectHasProperty -Object $dispatchObject -Name "module_path") {
                                $actualArchiveRamSwap.dispatch.module_path = [string]$dispatchObject.module_path
                            }
                            if (Test-ObjectHasProperty -Object $dispatchObject -Name "resolved_module_path") {
                                $actualArchiveRamSwap.dispatch.resolved_module_path = [string]$dispatchObject.resolved_module_path
                            }
                            if (Test-ObjectHasProperty -Object $dispatchObject -Name "module_size_bytes") {
                                $actualArchiveRamSwap.dispatch.module_size_bytes = $dispatchObject.module_size_bytes
                            }
                            if (Test-ObjectHasProperty -Object $dispatchObject -Name "loaded_to_ram") {
                                try {
                                    $actualArchiveRamSwap.dispatch.loaded_to_ram = ConvertTo-StrictBoolean -Value $dispatchObject.loaded_to_ram
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.dispatch.loaded_to_ram' is not boolean-compatible.")
                                }
                            }
                            if (Test-ObjectHasProperty -Object $dispatchObject -Name "error") {
                                $actualArchiveRamSwap.dispatch.error = [string]$dispatchObject.error
                            }
                        }
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "return_to_archive") {
                        $returnObject = Get-ObjectPropertyValue -Object $archiveRamSwapObject -Name "return_to_archive"
                        if ($null -ne $returnObject) {
                            if (Test-ObjectHasProperty -Object $returnObject -Name "attempted") {
                                try {
                                    $actualArchiveRamSwap.return_to_archive.attempted = ConvertTo-StrictBoolean -Value $returnObject.attempted
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.return_to_archive.attempted' is not boolean-compatible.")
                                }
                            }
                            if (Test-ObjectHasProperty -Object $returnObject -Name "returned_to_archive") {
                                try {
                                    $actualArchiveRamSwap.return_to_archive.returned_to_archive = ConvertTo-StrictBoolean -Value $returnObject.returned_to_archive
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.return_to_archive.returned_to_archive' is not boolean-compatible.")
                                }
                            }
                            if (Test-ObjectHasProperty -Object $returnObject -Name "error") {
                                $actualArchiveRamSwap.return_to_archive.error = [string]$returnObject.error
                            }
                        }
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "state") {
                        $stateObject = Get-ObjectPropertyValue -Object $archiveRamSwapObject -Name "state"
                        if ($null -ne $stateObject) {
                            if (Test-ObjectHasProperty -Object $stateObject -Name "module_in_archive") {
                                try {
                                    $actualArchiveRamSwap.state.module_in_archive = ConvertTo-StrictBoolean -Value $stateObject.module_in_archive
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.state.module_in_archive' is not boolean-compatible.")
                                }
                            }
                            if (Test-ObjectHasProperty -Object $stateObject -Name "module_in_ram") {
                                try {
                                    $actualArchiveRamSwap.state.module_in_ram = ConvertTo-StrictBoolean -Value $stateObject.module_in_ram
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.state.module_in_ram' is not boolean-compatible.")
                                }
                            }
                        }
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "modules") {
                        $modulesObject = Get-ObjectPropertyValue -Object $archiveRamSwapObject -Name "modules"
                        if ($null -ne $modulesObject) {
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "requested_count") {
                                $actualArchiveRamSwap.modules.requested_count = [int]$modulesObject.requested_count
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "rapid_swap_cycles") {
                                $actualArchiveRamSwap.modules.rapid_swap_cycles = [int]$modulesObject.rapid_swap_cycles
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "reserved_ram_bytes") {
                                $actualArchiveRamSwap.modules.reserved_ram_bytes = [int]$modulesObject.reserved_ram_bytes
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "effective_ram_capacity_bytes") {
                                $actualArchiveRamSwap.modules.effective_ram_capacity_bytes = [int]$modulesObject.effective_ram_capacity_bytes
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "dispatch_events") {
                                $actualArchiveRamSwap.modules.dispatch_events = [int]$modulesObject.dispatch_events
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "return_events") {
                                $actualArchiveRamSwap.modules.return_events = [int]$modulesObject.return_events
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "corruption_detected") {
                                try {
                                    $actualArchiveRamSwap.modules.corruption_detected = ConvertTo-StrictBoolean -Value $modulesObject.corruption_detected
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.modules.corruption_detected' is not boolean-compatible.")
                                }
                            }
                            if (Test-ObjectHasProperty -Object $modulesObject -Name "partial_load_detected") {
                                try {
                                    $actualArchiveRamSwap.modules.partial_load_detected = ConvertTo-StrictBoolean -Value $modulesObject.partial_load_detected
                                }
                                catch {
                                    $mismatches.Add("Harness JSON 'archive_ram_swap.modules.partial_load_detected' is not boolean-compatible.")
                                }
                            }
                        }
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "diagnostics") {
                        $actualArchiveRamSwap.diagnostics = ConvertTo-StringArray -Value $archiveRamSwapObject.diagnostics
                    }

                    if (Test-ObjectHasProperty -Object $archiveRamSwapObject -Name "errors") {
                        $actualArchiveRamSwap.errors = ConvertTo-StringArray -Value $archiveRamSwapObject.errors
                    }
                }

                $caseResult.actual.cemu_hash.enabled = [bool]$actualCEmuHash.enabled
                $caseResult.actual.cemu_hash.name = [string]$actualCEmuHash.name
                $caseResult.actual.cemu_hash.start = [string]$actualCEmuHash.start
                $caseResult.actual.cemu_hash.size = [string]$actualCEmuHash.size
                $caseResult.actual.cemu_hash.timeout_ms = $actualCEmuHash.timeout_ms
                $caseResult.actual.cemu_hash.expected_crcs = @($actualCEmuHash.expected_crcs)
                $caseResult.actual.cemu_hash.observed_crcs = @($actualCEmuHash.observed_crcs)
                $caseResult.actual.cemu_hash.last_observed_crc = [string]$actualCEmuHash.last_observed_crc
                $caseResult.actual.cemu_hash.matched = $actualCEmuHash.matched
                $caseResult.actual.hardware_constraints.validated = [bool]$actualHardwareConstraints.validated
                $caseResult.actual.hardware_constraints.pass = [bool]$actualHardwareConstraints.pass
                $caseResult.actual.hardware_constraints.memory_insufficient = [bool]$actualHardwareConstraints.memory_insufficient
                $caseResult.actual.hardware_constraints.module_size_exceeded = [bool]$actualHardwareConstraints.module_size_exceeded
                $caseResult.actual.hardware_constraints.errors = @($actualHardwareConstraints.errors)
                $caseResult.actual.archive_ram_swap.simulated = [bool]$actualArchiveRamSwap.simulated
                $caseResult.actual.archive_ram_swap.pass = [bool]$actualArchiveRamSwap.pass
                $caseResult.actual.archive_ram_swap.failure_phase = [string]$actualArchiveRamSwap.failure_phase
                $caseResult.actual.archive_ram_swap.dispatch.requested = [bool]$actualArchiveRamSwap.dispatch.requested
                $caseResult.actual.archive_ram_swap.dispatch.module_path = [string]$actualArchiveRamSwap.dispatch.module_path
                $caseResult.actual.archive_ram_swap.dispatch.resolved_module_path = [string]$actualArchiveRamSwap.dispatch.resolved_module_path
                $caseResult.actual.archive_ram_swap.dispatch.module_size_bytes = $actualArchiveRamSwap.dispatch.module_size_bytes
                $caseResult.actual.archive_ram_swap.dispatch.loaded_to_ram = [bool]$actualArchiveRamSwap.dispatch.loaded_to_ram
                $caseResult.actual.archive_ram_swap.dispatch.error = [string]$actualArchiveRamSwap.dispatch.error
                $caseResult.actual.archive_ram_swap.return_to_archive.attempted = [bool]$actualArchiveRamSwap.return_to_archive.attempted
                $caseResult.actual.archive_ram_swap.return_to_archive.returned_to_archive = [bool]$actualArchiveRamSwap.return_to_archive.returned_to_archive
                $caseResult.actual.archive_ram_swap.return_to_archive.error = [string]$actualArchiveRamSwap.return_to_archive.error
                $caseResult.actual.archive_ram_swap.state.module_in_archive = [bool]$actualArchiveRamSwap.state.module_in_archive
                $caseResult.actual.archive_ram_swap.state.module_in_ram = [bool]$actualArchiveRamSwap.state.module_in_ram
                $caseResult.actual.archive_ram_swap.modules.requested_count = [int]$actualArchiveRamSwap.modules.requested_count
                $caseResult.actual.archive_ram_swap.modules.rapid_swap_cycles = [int]$actualArchiveRamSwap.modules.rapid_swap_cycles
                $caseResult.actual.archive_ram_swap.modules.reserved_ram_bytes = [int]$actualArchiveRamSwap.modules.reserved_ram_bytes
                $caseResult.actual.archive_ram_swap.modules.effective_ram_capacity_bytes = [int]$actualArchiveRamSwap.modules.effective_ram_capacity_bytes
                $caseResult.actual.archive_ram_swap.modules.dispatch_events = [int]$actualArchiveRamSwap.modules.dispatch_events
                $caseResult.actual.archive_ram_swap.modules.return_events = [int]$actualArchiveRamSwap.modules.return_events
                $caseResult.actual.archive_ram_swap.modules.corruption_detected = [bool]$actualArchiveRamSwap.modules.corruption_detected
                $caseResult.actual.archive_ram_swap.modules.partial_load_detected = [bool]$actualArchiveRamSwap.modules.partial_load_detected
                $caseResult.actual.archive_ram_swap.diagnostics = @($actualArchiveRamSwap.diagnostics)
                $caseResult.actual.archive_ram_swap.errors = @($actualArchiveRamSwap.errors)

                if ($actualResult -cne $expectedResult) {
                    $mismatches.Add("Expected result '$expectedResult' but received '$actualResult'.")
                }

                if ($actualStatus -cne $expectedStatus) {
                    $mismatches.Add("Expected status '$expectedStatus' but received '$actualStatus'.")
                }

                if ($actualError -cne $expectedError) {
                    $mismatches.Add("Expected error '$expectedError' but received '$actualError'.")
                }

                if (-not (Test-StringArrayEqual -Expected $expectedIgnoredKeys -Actual $actualIgnored)) {
                    $mismatches.Add(
                        ("Expected ignored_keys '{0}' but received '{1}'." -f ($expectedIgnoredKeys -join ","), ($actualIgnored -join ","))
                    )
                }

                if ($null -ne $expectedMode -and $actualMode -cne $expectedMode) {
                    $mismatches.Add("Expected mode '$expectedMode' but received '$actualMode'.")
                }

                if ($actualHardwareConstraints.validated -and -not $actualHardwareConstraints.pass) {
                    if ($actualHardwareConstraints.errors.Count -gt 0) {
                        foreach ($constraintError in $actualHardwareConstraints.errors) {
                            $mismatches.Add("Hardware constraint validation failed: $constraintError")
                        }
                    }
                    else {
                        $mismatches.Add("Hardware constraint validation failed.")
                    }
                }

                if ($actualArchiveRamSwap.simulated -and -not $actualArchiveRamSwap.pass) {
                    if ($actualArchiveRamSwap.errors.Count -gt 0) {
                        foreach ($swapError in @($actualArchiveRamSwap.errors)) {
                            $mismatches.Add("Archive<->RAM swap simulation failed: $swapError")
                        }
                    }
                    else {
                        $mismatches.Add("Archive<->RAM swap simulation failed.")
                    }
                }

                if ($expectedArchiveRamSwap.enabled) {
                    if ($null -ne $expectedArchiveRamSwap.simulated) {
                        if ([bool]$actualArchiveRamSwap.simulated -ne [bool]$expectedArchiveRamSwap.simulated) {
                            $mismatches.Add(
                                ("Expected archive_ram_swap.simulated '{0}' but received '{1}'." -f
                                    (ConvertTo-BooleanText -Value ([bool]$expectedArchiveRamSwap.simulated)),
                                    (ConvertTo-BooleanText -Value ([bool]$actualArchiveRamSwap.simulated)))
                            )
                        }
                    }

                    if ($null -ne $expectedArchiveRamSwap.pass) {
                        if ([bool]$actualArchiveRamSwap.pass -ne [bool]$expectedArchiveRamSwap.pass) {
                            $mismatches.Add(
                                ("Expected archive_ram_swap.pass '{0}' but received '{1}'." -f
                                    (ConvertTo-BooleanText -Value ([bool]$expectedArchiveRamSwap.pass)),
                                    (ConvertTo-BooleanText -Value ([bool]$actualArchiveRamSwap.pass)))
                            )
                        }
                    }

                    if (-not [string]::IsNullOrWhiteSpace($expectedArchiveRamSwap.failure_phase)) {
                        if ([string]$actualArchiveRamSwap.failure_phase -cne [string]$expectedArchiveRamSwap.failure_phase) {
                            $mismatches.Add("Expected archive_ram_swap.failure_phase '$($expectedArchiveRamSwap.failure_phase)' but received '$($actualArchiveRamSwap.failure_phase)'.")
                        }
                    }

                    if ($null -ne $expectedArchiveRamSwap.dispatch_loaded_to_ram) {
                        if ([bool]$actualArchiveRamSwap.dispatch.loaded_to_ram -ne [bool]$expectedArchiveRamSwap.dispatch_loaded_to_ram) {
                            $mismatches.Add(
                                ("Expected archive_ram_swap.dispatch_loaded_to_ram '{0}' but received '{1}'." -f
                                    (ConvertTo-BooleanText -Value ([bool]$expectedArchiveRamSwap.dispatch_loaded_to_ram)),
                                    (ConvertTo-BooleanText -Value ([bool]$actualArchiveRamSwap.dispatch.loaded_to_ram)))
                            )
                        }
                    }

                    if ($null -ne $expectedArchiveRamSwap.return_to_archive_completed) {
                        if ([bool]$actualArchiveRamSwap.return_to_archive.returned_to_archive -ne [bool]$expectedArchiveRamSwap.return_to_archive_completed) {
                            $mismatches.Add(
                                ("Expected archive_ram_swap.return_to_archive_completed '{0}' but received '{1}'." -f
                                    (ConvertTo-BooleanText -Value ([bool]$expectedArchiveRamSwap.return_to_archive_completed)),
                                    (ConvertTo-BooleanText -Value ([bool]$actualArchiveRamSwap.return_to_archive.returned_to_archive)))
                            )
                        }
                    }
                }

                if ($expectedCEmuHash.enabled) {
                    if ($actualMode -cne "cemu") {
                        $mismatches.Add("Case requires cemu_hash verification, but harness mode was '$actualMode'.")
                    }

                    if (-not $actualCEmuHash.enabled) {
                        $mismatches.Add("Expected cemu_hash verification to be enabled in harness output.")
                    }
                    else {
                        if ($actualCEmuHash.name -cne $expectedCEmuHash.name) {
                            $mismatches.Add("Expected cemu_hash.name '$($expectedCEmuHash.name)' but received '$($actualCEmuHash.name)'.")
                        }
                        if ($actualCEmuHash.start -cne $expectedCEmuHash.start) {
                            $mismatches.Add("Expected cemu_hash.start '$($expectedCEmuHash.start)' but received '$($actualCEmuHash.start)'.")
                        }
                        if ($actualCEmuHash.size -cne $expectedCEmuHash.size) {
                            $mismatches.Add("Expected cemu_hash.size '$($expectedCEmuHash.size)' but received '$($actualCEmuHash.size)'.")
                        }
                        if ([int]$actualCEmuHash.timeout_ms -ne [int]$expectedCEmuHash.timeout_ms) {
                            $mismatches.Add("Expected cemu_hash.timeout_ms '$($expectedCEmuHash.timeout_ms)' but received '$($actualCEmuHash.timeout_ms)'.")
                        }

                        if ($caseCaptureBaselineRequested) {
                            $baselineSampleCrcs = New-Object System.Collections.Generic.List[string]
                            $caseResult.baseline_capture.attempts_executed = 1

                            $firstCaptureParse = Get-ObservedBaselineCrcFromHarnessJson `
                                -HarnessJson $harnessJson `
                                -AttemptLabel "Baseline capture attempt 1"
                            if (-not [bool]$firstCaptureParse.success) {
                                $mismatches.Add([string]$firstCaptureParse.error)
                            }
                            else {
                                $baselineSampleCrcs.Add([string]$firstCaptureParse.crc)
                            }

                            for ($captureAttempt = 2; $captureAttempt -le $BaselineCaptureSampleCount; $captureAttempt++) {
                                if ($mismatches.Count -gt 0) {
                                    break
                                }

                                $retryInvocation = Invoke-HarnessCase `
                                    -HarnessScriptPath $resolvedHarnessScript `
                                    -KeySequence $keys `
                                    -IncludeRomPath $includeRomPath `
                                    -IncludeAutotesterPath $includeAutotesterPath `
                                    -IncludeProgramPath $includeProgramPath `
                                    -IncludeModuleProgramPaths $passModuleProgramPathsToHarness `
                                    -RomPath $RomPath `
                                    -AutotesterPath $AutotesterPath `
                                    -ProgramPath $ProgramPath `
                                    -ModuleProgramPaths $resolvedModuleProgramPaths `
                                    -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
                                    -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes `
                                    -ArchiveRamDispatchModulePath $ArchiveRamDispatchModulePath `
                                    -ArchiveRamSwapRamCapacityBytes $ArchiveRamSwapRamCapacityBytes `
                                    -ArchiveRamFailurePhase $ArchiveRamFailurePhase `
                                    -ForceMock:$ForceMock `
                                    -NoMockFallback:$NoMockFallback `
                                    -CEmuHashExpectation $expectedCEmuHash `
                                    -OverrideExpectedVramCrcs $overrideExpectedVramCrcs

                                $caseResult.baseline_capture.attempts_executed = $captureAttempt

                                if (-not $retryInvocation.json_parse_success) {
                                    $mismatches.Add(
                                        ("Baseline capture attempt {0} failed to parse harness JSON output: {1}" -f $captureAttempt, $retryInvocation.json_parse_error)
                                    )
                                    break
                                }

                                $retryCaptureParse = Get-ObservedBaselineCrcFromHarnessJson `
                                    -HarnessJson $retryInvocation.json_object `
                                    -AttemptLabel ("Baseline capture attempt {0}" -f $captureAttempt)
                                if (-not [bool]$retryCaptureParse.success) {
                                    $mismatches.Add([string]$retryCaptureParse.error)
                                    break
                                }

                                $baselineSampleCrcs.Add([string]$retryCaptureParse.crc)
                            }

                            $caseResult.baseline_capture.observed_crc_samples = $baselineSampleCrcs.ToArray()
                            $frequencyRows = @(Get-CrcFrequencyRows -Samples $baselineSampleCrcs.ToArray())
                            $caseResult.baseline_capture.observed_crc_counts = $frequencyRows

                            if ($mismatches.Count -eq 0) {
                                $consensusSelection = Get-BaselineConsensusSelection `
                                    -Samples $baselineSampleCrcs.ToArray() `
                                    -ConsensusRequired $BaselineCaptureConsensus
                                $caseResult.baseline_capture.consensus_reached = [bool]$consensusSelection.success
                                $caseResult.baseline_capture.observed_crc_counts = @($consensusSelection.frequency_rows)

                                if (-not [bool]$consensusSelection.success) {
                                    $mismatches.Add([string]$consensusSelection.error)
                                }
                                else {
                                    $normalizedCapturedBaselineCrc = [string]$consensusSelection.crc
                                    $caseResult.baseline_capture.captured = $true
                                    $caseResult.baseline_capture.captured_crc = $normalizedCapturedBaselineCrc
                                    $baselineCapturedCases++

                                    Set-ExpectedCaseCEmuHashCrcs `
                                        -ExpectedObject $expectedObject `
                                        -ExpectedCEmuHash $expectedCEmuHash `
                                        -ExpectedCrcs @($normalizedCapturedBaselineCrc)

                                    $expectedCEmuHash.expected_crcs = @($normalizedCapturedBaselineCrc)
                                    $caseResult.expected.cemu_hash.expected_crcs = @($normalizedCapturedBaselineCrc)
                                    $caseResult.expected.cemu_hash.baseline_placeholder = $false
                                    $caseResult.baseline_capture.target_case_updated = $true
                                    $baselineUpdatedCases++
                                }
                            }
                        }
                        elseif (-not (Test-StringArrayEqual -Expected @($expectedCEmuHash.expected_crcs) -Actual @($actualCEmuHash.expected_crcs))) {
                            $mismatches.Add(
                                ("Expected cemu_hash.expected_crcs '{0}' but received '{1}'." -f ((@($expectedCEmuHash.expected_crcs)) -join ","), ((@($actualCEmuHash.expected_crcs)) -join ","))
                            )
                        }
                    }

                    if ($null -eq $actualCEmuHash.matched) {
                        $mismatches.Add("Harness did not report cemu_hash.matched state.")
                    }
                    elseif ((-not $caseCaptureBaselineRequested) -and (-not $actualCEmuHash.matched)) {
                        $mismatches.Add("CEmu hash verification failed (autotest hash assertion mismatch).")
                    }
                }

                if ($caseCaptureBaselineRequested) {
                    # Baseline capture intentionally uses a seed CRC that should mismatch.
                    # Do not enforce harness process exit semantics in this mode.
                }
                elseif ($hasExpectedHarnessSuccess) {
                    if ($actualHarnessSuccess -ne $expectedHarnessSuccess) {
                        $mismatches.Add(
                            ("Expected harness_success '{0}' but received '{1}'." -f (ConvertTo-BooleanText -Value $expectedHarnessSuccess), (ConvertTo-BooleanText -Value $actualHarnessSuccess))
                        )
                    }
                }
                elseif ((-not $actualHarnessSuccess) -and (-not $caseCaptureBaselineRequested)) {
                    $mismatches.Add("Harness exited with non-zero exit code $($harnessInvocation.exit_code).")
                }
            }
            else {
                $mismatches.Add("Failed to parse harness JSON output: $($harnessInvocation.json_parse_error)")
            }
        }
        else {
            $caseResult.harness.command = "<not-run>"
            $caseResult.harness.exit_code = $null
            $caseResult.harness.output_lines = @()
            $caseResult.harness.json_parse_success = $false
            $caseResult.harness.json_parse_error = "Harness invocation skipped due to case schema errors."
        }

        $caseResult.passed = ($mismatches.Count -eq 0)
        $caseResult.mismatches = $mismatches.ToArray()
        $caseResult.duration_ms = [int][Math]::Round(((Get-Date) - $caseStart).TotalMilliseconds)

        $caseResults.Add($caseResult)
    }

    $passedCount = 0
    foreach ($caseResult in $caseResults) {
        if ($caseResult.passed) {
            $passedCount++
        }
    }

    $totalCount = $caseResults.Count
    $failedCount = $totalCount - $passedCount
    $runSuccess = ($failedCount -eq 0)
    $exitCode = 0
    if (-not $runSuccess) {
        $exitCode = 1
    }

    $baselineCaseFileWritten = $false
    if ($captureHashBaselines -and $baselineUpdatedCases -gt 0) {
        $baselineOutputDirectory = Split-Path -Path $resolvedBaselineOutputCaseFile -Parent
        if (-not [string]::IsNullOrWhiteSpace($baselineOutputDirectory) -and -not (Test-Path -LiteralPath $baselineOutputDirectory -PathType Container)) {
            [void](New-Item -Path $baselineOutputDirectory -ItemType Directory -Force)
        }

        $updatedCaseJsonText = $parsedCaseJson | ConvertTo-Json -Depth 100
        Set-Content -LiteralPath $resolvedBaselineOutputCaseFile -Value $updatedCaseJsonText -Encoding UTF8
        $baselineCaseFileWritten = $true
        $resolvedBaselineOutputCaseFile = (Resolve-Path -LiteralPath $resolvedBaselineOutputCaseFile).Path
    }

    $summary = [ordered]@{
        schema_version = 1
        generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        runner = "run-terminal-regression.ps1"
        case_file = $resolvedCaseFile
        harness_script = $resolvedHarnessScript
        options = [ordered]@{
            force_mock = [bool]$ForceMock
            no_mock_fallback = [bool]$NoMockFallback
            include_rom_path = $includeRomPath
            include_autotester_path = $includeAutotesterPath
            include_program_path = $includeProgramPath
            include_module_program_paths = $includeModuleProgramPaths
            rom_path = $RomPath
            autotester_path = $AutotesterPath
            program_path = $ProgramPath
            module_program_paths_source = $moduleProgramPathSource
            module_program_paths = @($resolvedModuleProgramPaths)
            usable_user_ram_limit_bytes = $UsableUserRamLimitBytes
            module_linked_binary_limit_bytes = $ModuleLinkedBinaryLimitBytes
            archive_ram_dispatch_module_path = $ArchiveRamDispatchModulePath
            archive_ram_swap_ram_capacity_bytes = $ArchiveRamSwapRamCapacityBytes
            archive_ram_failure_phase = $ArchiveRamFailurePhase
            capture_cemu_hash_baselines = $captureHashBaselines
            baseline_placeholder = $BaselinePlaceholder
            baseline_seed_crc = $normalizedBaselineSeedCrc
            baseline_capture_sample_count = $BaselineCaptureSampleCount
            baseline_capture_consensus = $BaselineCaptureConsensus
            baseline_output_case_file = $resolvedBaselineOutputCaseFile
        }
        baseline_capture = [ordered]@{
            enabled = $captureHashBaselines
            updated_case_count = $baselineUpdatedCases
            captured_case_count = $baselineCapturedCases
            sample_count = $BaselineCaptureSampleCount
            consensus_required = $BaselineCaptureConsensus
            case_file_written = $baselineCaseFileWritten
            output_case_file = $resolvedBaselineOutputCaseFile
        }
        totals = [ordered]@{
            total = $totalCount
            passed = $passedCount
            failed = $failedCount
        }
        success = $runSuccess
        exit_code = $exitCode
        cases = $caseResults.ToArray()
    }

    $summaryJson = $summary | ConvertTo-Json -Depth 100
    if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
        $summaryDirectory = Split-Path -Path $SummaryPath -Parent
        if (-not [string]::IsNullOrWhiteSpace($summaryDirectory) -and -not (Test-Path -LiteralPath $summaryDirectory -PathType Container)) {
            [void](New-Item -Path $summaryDirectory -ItemType Directory -Force)
        }

        Set-Content -LiteralPath $SummaryPath -Value $summaryJson -Encoding UTF8
    }

    Write-Output "Terminal Regression Runner Summary"
    Write-Output ("Case file: {0}" -f $resolvedCaseFile)
    Write-Output ("Harness script: {0}" -f $resolvedHarnessScript)
    Write-Output ("ForceMock: {0}" -f (ConvertTo-BooleanText -Value $ForceMock.IsPresent))
    Write-Output ("NoMockFallback: {0}" -f (ConvertTo-BooleanText -Value $NoMockFallback.IsPresent))
    Write-Output ("RomPath passthrough: {0}" -f (ConvertTo-BooleanText -Value $includeRomPath))
    Write-Output ("AutotesterPath passthrough: {0}" -f (ConvertTo-BooleanText -Value $includeAutotesterPath))
    Write-Output ("ProgramPath passthrough: {0}" -f (ConvertTo-BooleanText -Value $includeProgramPath))
    Write-Output ("ModuleProgramPaths passthrough: {0}" -f (ConvertTo-BooleanText -Value $includeModuleProgramPaths))
    Write-Output ("ModuleProgramPaths source: {0}" -f $moduleProgramPathSource)
    Write-Output ("ModuleProgramPaths count: {0}" -f $resolvedModuleProgramPaths.Count)
    Write-Output ("UsableUserRamLimitBytes: {0}" -f $UsableUserRamLimitBytes)
    Write-Output ("ModuleLinkedBinaryLimitBytes: {0}" -f $ModuleLinkedBinaryLimitBytes)
    Write-Output ("ArchiveRamDispatchModulePath: {0}" -f $ArchiveRamDispatchModulePath)
    Write-Output ("ArchiveRamSwapRamCapacityBytes: {0}" -f $ArchiveRamSwapRamCapacityBytes)
    Write-Output ("ArchiveRamFailurePhase: {0}" -f $ArchiveRamFailurePhase)
    Write-Output ("CaptureCEmuHashBaselines: {0}" -f (ConvertTo-BooleanText -Value $captureHashBaselines))
    if ($captureHashBaselines) {
        Write-Output ("BaselinePlaceholder: {0}" -f $BaselinePlaceholder)
        Write-Output ("BaselineSeedCrc: {0}" -f $normalizedBaselineSeedCrc)
        Write-Output ("BaselineCaptureSampleCount: {0}" -f $BaselineCaptureSampleCount)
        Write-Output ("BaselineCaptureConsensus: {0}" -f $BaselineCaptureConsensus)
        Write-Output ("BaselineOutputCaseFile: {0}" -f $resolvedBaselineOutputCaseFile)
    }
    Write-Output ""

    foreach ($caseResult in $caseResults) {
        $label = "FAIL"
        if ($caseResult.passed) {
            $label = "PASS"
        }

        Write-Output ("[{0}] {1}: {2}" -f $label, $caseResult.index, $caseResult.name)
        Write-Output ("  keys: {0}" -f $caseResult.keys)
        Write-Output ("  mode: {0}" -f $caseResult.actual.mode)
        Write-Output ("  status: {0}" -f $caseResult.actual.status)
        Write-Output ("  result: {0}" -f $caseResult.actual.result)
        Write-Output ("  harness_exit_code: {0}" -f $caseResult.harness.exit_code)
        if ($caseResult.expected.cemu_hash.enabled -or $caseResult.actual.cemu_hash.enabled) {
            Write-Output ("  cemu_hash_enabled: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.cemu_hash.enabled)))
            Write-Output ("  cemu_hash_name: {0}" -f $caseResult.actual.cemu_hash.name)
            Write-Output ("  cemu_hash_expected_crcs: {0}" -f (@($caseResult.actual.cemu_hash.expected_crcs) -join ","))
            Write-Output ("  cemu_hash_observed_crcs: {0}" -f (@($caseResult.actual.cemu_hash.observed_crcs) -join ","))
            if ($null -eq $caseResult.actual.cemu_hash.matched) {
                Write-Output "  cemu_hash_matched: <none>"
            }
            else {
                Write-Output ("  cemu_hash_matched: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.cemu_hash.matched)))
            }
        }
        if ($caseResult.actual.hardware_constraints.validated) {
            Write-Output ("  hardware_constraints_pass: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.hardware_constraints.pass)))
            Write-Output ("  hardware_constraints_memory_insufficient: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.hardware_constraints.memory_insufficient)))
            Write-Output ("  hardware_constraints_module_size_exceeded: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.hardware_constraints.module_size_exceeded)))
            Write-Output ("  hardware_constraints_errors: {0}" -f (@($caseResult.actual.hardware_constraints.errors) -join " | "))
        }
        if ($caseResult.actual.archive_ram_swap.simulated -or $caseResult.expected.archive_ram_swap.enabled) {
            Write-Output ("  archive_ram_swap_simulated: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.archive_ram_swap.simulated)))
            Write-Output ("  archive_ram_swap_pass: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.archive_ram_swap.pass)))
            Write-Output ("  archive_ram_swap_failure_phase: {0}" -f $caseResult.actual.archive_ram_swap.failure_phase)
            Write-Output ("  archive_ram_swap_dispatch_loaded_to_ram: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.archive_ram_swap.dispatch.loaded_to_ram)))
            Write-Output ("  archive_ram_swap_return_completed: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.actual.archive_ram_swap.return_to_archive.returned_to_archive)))
            Write-Output ("  archive_ram_swap_errors: {0}" -f (@($caseResult.actual.archive_ram_swap.errors) -join " | "))
            Write-Output ("  archive_ram_swap_diagnostics: {0}" -f (@($caseResult.actual.archive_ram_swap.diagnostics) -join " | "))
        }
        if ($caseResult.baseline_capture.requested) {
            Write-Output ("  baseline_capture_requested: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.baseline_capture.requested)))
            Write-Output ("  baseline_capture_attempts_requested: {0}" -f [int]$caseResult.baseline_capture.attempts_requested)
            Write-Output ("  baseline_capture_attempts_executed: {0}" -f [int]$caseResult.baseline_capture.attempts_executed)
            Write-Output ("  baseline_capture_consensus_required: {0}" -f [int]$caseResult.baseline_capture.consensus_required)
            Write-Output ("  baseline_capture_consensus_reached: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.baseline_capture.consensus_reached)))
            Write-Output ("  baseline_capture_captured: {0}" -f (ConvertTo-BooleanText -Value ([bool]$caseResult.baseline_capture.captured)))
            Write-Output ("  baseline_capture_crc: {0}" -f $caseResult.baseline_capture.captured_crc)
            Write-Output ("  baseline_capture_samples: {0}" -f (@($caseResult.baseline_capture.observed_crc_samples) -join ","))
            Write-Output ("  baseline_capture_counts: {0}" -f (Format-CrcFrequencyRows -Rows @($caseResult.baseline_capture.observed_crc_counts)))
        }
        if (-not $caseResult.passed) {
            foreach ($mismatch in $caseResult.mismatches) {
                Write-Output ("  mismatch: {0}" -f $mismatch)
            }
        }
    }

    Write-Output ""
    Write-Output ("Total: {0}" -f $totalCount)
    Write-Output ("Passed: {0}" -f $passedCount)
    Write-Output ("Failed: {0}" -f $failedCount)
    Write-Output ("Success: {0}" -f (ConvertTo-BooleanText -Value $runSuccess))
    if ($captureHashBaselines) {
        Write-Output ("Baseline captured cases: {0}" -f $baselineCapturedCases)
        Write-Output ("Baseline updated cases: {0}" -f $baselineUpdatedCases)
        Write-Output ("Baseline sample count: {0}" -f $BaselineCaptureSampleCount)
        Write-Output ("Baseline consensus required: {0}" -f $BaselineCaptureConsensus)
        Write-Output ("Baseline case file written: {0}" -f (ConvertTo-BooleanText -Value $baselineCaseFileWritten))
    }
    if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
        Write-Output ("SummaryPath: {0}" -f $SummaryPath)
    }
    Write-Output ""
    Write-Output "JSON_SUMMARY_START"
    Write-Output $summaryJson
    Write-Output "JSON_SUMMARY_END"

    exit $exitCode
}
catch {
    Write-Output ("Terminal regression runner failed: {0}" -f $_.Exception.Message)
    exit 2
}
