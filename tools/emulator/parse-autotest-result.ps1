[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

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

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }

    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $enc)
}

function Get-HashDefinitions {
    param([string]$ConfigPath)

    $definitions = New-Object System.Collections.Generic.List[object]
    if ([string]::IsNullOrWhiteSpace($ConfigPath) -or -not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
        return $definitions.ToArray()
    }

    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    foreach ($prop in @($config.hashes.PSObject.Properties | Where-Object { $_.MemberType -eq "NoteProperty" })) {
        $definitions.Add([ordered]@{
            hash_name = [string]$prop.Name
            description = [string]$prop.Value.description
            expected_crcs = [string[]]@($prop.Value.expected_CRCs)
        })
    }

    return $definitions.ToArray()
}

function Get-VerifyStageFromSummary {
    param([object]$Summary)

    if ($Summary.PSObject.Properties.Name -contains "verify_stage" -and $null -ne $Summary.verify_stage) {
        return $Summary.verify_stage
    }

    if ($Summary.PSObject.Properties.Name -contains "runner_summary" -and
        $null -ne $Summary.runner_summary -and
        $Summary.runner_summary.PSObject.Properties.Name -contains "verify_stage") {
        return $Summary.runner_summary.verify_stage
    }

    throw "Input JSON does not contain a verify_stage payload."
}

function Normalize-HashResultRow {
    param(
        [Parameter(Mandatory = $true)][object]$Row,
        [string]$FallbackDescription = "",
        [string[]]$FallbackExpectedCrcs = @()
    )

    $observedCrc = if ($Row.PSObject.Properties.Name -contains "observed_crc") {
        [string]$Row.observed_crc
    } else {
        ""
    }
    $status = if ($Row.PSObject.Properties.Name -contains "status") {
        [string]$Row.status
    } else {
        "missing"
    }
    $statusReason = if ($Row.PSObject.Properties.Name -contains "status_reason" -and
        -not [string]::IsNullOrWhiteSpace([string]$Row.status_reason)) {
        [string]$Row.status_reason
    }
    elseif ($status -eq "fail") {
        if ([string]::IsNullOrWhiteSpace($observedCrc)) { "reported_failure_without_hash_line" } else { "crc_mismatch" }
    }
    elseif ($status -eq "pass") {
        if ([string]::IsNullOrWhiteSpace($observedCrc)) { "reported_pass_without_observed_crc" } else { "reported_pass_with_observed_crc" }
    }
    else {
        "not_reported"
    }
    $observedCrcSource = if ($Row.PSObject.Properties.Name -contains "observed_crc_source" -and
        -not [string]::IsNullOrWhiteSpace([string]$Row.observed_crc_source)) {
        [string]$Row.observed_crc_source
    }
    elseif ([string]::IsNullOrWhiteSpace($observedCrc)) {
        "not_reported"
    }
    else {
        "reported_crc"
    }
    $observedCrcInferred = if ($Row.PSObject.Properties.Name -contains "observed_crc_inferred") {
        [bool]$Row.observed_crc_inferred
    }
    else {
        $statusReason -in @(
            "reported_failure_without_hash_line",
            "reported_pass_budget_without_observed_crc"
        )
    }
    $description = if ($Row.PSObject.Properties.Name -contains "description" -and
        -not [string]::IsNullOrWhiteSpace([string]$Row.description)) {
        [string]$Row.description
    } else {
        $FallbackDescription
    }
    $expectedCrcs = if ($Row.PSObject.Properties.Name -contains "expected_crcs" -and $null -ne $Row.expected_crcs) {
        [string[]]@($Row.expected_crcs)
    } else {
        [string[]]@($FallbackExpectedCrcs)
    }

    return [ordered]@{
        hash_name = [string]$Row.hash_name
        description = $description
        expected_crcs = $expectedCrcs
        observed_crc = $observedCrc
        observed_crc_source = $observedCrcSource
        observed_crc_inferred = $observedCrcInferred
        status = $status
        status_reason = $statusReason
    }
}

function New-HashResultsFromVerifyStage {
    param(
        [object]$VerifyStage,
        [string]$BaselinedPath
    )

    if ($VerifyStage.PSObject.Properties.Name -contains "hash_results" -and $null -ne $VerifyStage.hash_results) {
        $normalizedRows = New-Object System.Collections.Generic.List[object]
        foreach ($row in @($VerifyStage.hash_results)) {
            if ($null -eq $row) {
                continue
            }
            $normalizedRows.Add((Normalize-HashResultRow -Row $row))
        }
        return $normalizedRows.ToArray()
    }

    $definitions = Get-HashDefinitions -ConfigPath $BaselinedPath
    if ($definitions.Count -eq 0) {
        return @()
    }

    $mismatchMap = @{}
    foreach ($mismatch in @($VerifyStage.mismatches)) {
        $mismatchMap[[string]$mismatch.hash_name] = $mismatch
    }

    $attemptedCount = if ($VerifyStage.PSObject.Properties.Name -contains "attempted_count") {
        [int]$VerifyStage.attempted_count
    }
    else {
        if ([bool]$VerifyStage.success) { $definitions.Count } else { -1 }
    }
    $passedBudget = if ($VerifyStage.PSObject.Properties.Name -contains "pass_count") {
        [int]$VerifyStage.pass_count
    }
    else {
        if ([bool]$VerifyStage.success) { $definitions.Count } else { 0 }
    }

    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($definition in @($definitions)) {
        $hashName = [string]$definition.hash_name
        if ($mismatchMap.ContainsKey($hashName)) {
            $rows.Add((Normalize-HashResultRow -Row ([ordered]@{
                hash_name = $hashName
                description = [string]$definition.description
                expected_crcs = [string[]]@($definition.expected_crcs)
                observed_crc = [string]$mismatchMap[$hashName].observed_crc
                status = "fail"
            })))
        }
        elseif ($attemptedCount -eq $definitions.Count -or $passedBudget -gt 0) {
            $rows.Add((Normalize-HashResultRow -Row ([ordered]@{
                hash_name = $hashName
                description = [string]$definition.description
                expected_crcs = [string[]]@($definition.expected_crcs)
                observed_crc = ""
                status = "pass"
            })))
            if ($attemptedCount -ne $definitions.Count -and $passedBudget -gt 0) {
                $passedBudget--
            }
        }
        else {
            $rows.Add((Normalize-HashResultRow -Row ([ordered]@{
                hash_name = $hashName
                description = [string]$definition.description
                expected_crcs = [string[]]@($definition.expected_crcs)
                observed_crc = ""
                status = "missing"
            })))
        }
    }

    return $rows.ToArray()
}

$resolvedInputPath = Resolve-CalcEmuPipelinePath -PathValue $InputPath
if (-not (Test-Path -LiteralPath $resolvedInputPath -PathType Leaf)) {
    throw "Input JSON '$resolvedInputPath' does not exist."
}

$summary = Get-Content -LiteralPath $resolvedInputPath -Raw | ConvertFrom-Json
$verifyStage = Get-VerifyStageFromSummary -Summary $summary
$baselinedPath = ""
if ($verifyStage.PSObject.Properties.Name -contains "baselined_path") {
    $baselinedPath = [string]$verifyStage.baselined_path
}
elseif ($summary.PSObject.Properties.Name -contains "baselined_path") {
    $baselinedPath = [string]$summary.baselined_path
}

$hashResults = New-HashResultsFromVerifyStage -VerifyStage $verifyStage -BaselinedPath $baselinedPath
$passRows = @($hashResults | Where-Object { $_.status -eq "pass" })
$failRows = @($hashResults | Where-Object { $_.status -eq "fail" })
$missingRows = @($hashResults | Where-Object { $_.status -eq "missing" })

$result = [ordered]@{
    schema_version = 1
    input_path = $resolvedInputPath
    baselined_path = $baselinedPath
    success = ($failRows.Count -eq 0 -and $missingRows.Count -eq 0)
    total_hash_count = $hashResults.Count
    total_pass_count = $passRows.Count
    total_fail_count = $failRows.Count
    total_missing_count = $missingRows.Count
    missing_hashes = @($missingRows | ForEach-Object { [string]$_.hash_name })
    hash_results = $hashResults
}

Write-Host "CalcEngine AutoTester Result Summary"
Write-Host ("Input          : {0}" -f $resolvedInputPath)
if (-not [string]::IsNullOrWhiteSpace($baselinedPath)) {
    Write-Host ("Baseline       : {0}" -f $baselinedPath)
}
Write-Host ("Hashes         : {0}" -f $result.total_hash_count)
Write-Host ("Passed         : {0}" -f $result.total_pass_count)
Write-Host ("Failed         : {0}" -f $result.total_fail_count)
Write-Host ("Missing        : {0}" -f $result.total_missing_count)

if ($failRows.Count -gt 0) {
    Write-Host ""
    Write-Host "Failing hashes:"
    foreach ($row in $failRows) {
        $expectedText = if (@($row.expected_crcs).Count -gt 0) { @($row.expected_crcs) -join ", " } else { "<unknown>" }
        Write-Host ("  - {0}: expected [{1}] got {2}" -f [string]$row.hash_name, $expectedText, [string]$row.observed_crc)
    }
}

if ($missingRows.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing hashes:"
    foreach ($row in $missingRows) {
        Write-Host ("  - {0}" -f [string]$row.hash_name)
    }
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $resolvedOutputPath = Resolve-CalcEmuPipelinePath -PathValue $OutputPath
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($result | ConvertTo-Json -Depth 12)
    Write-Host ""
    Write-Host ("JSON summary   : {0}" -f $resolvedOutputPath)
}

exit $(if ($result.success) { 0 } else { 1 })
