param(
    [Parameter(Mandatory = $false)]
    [ValidateSet("strict", "minimal")]
    [string]$Mode = "strict",

    [Parameter(Mandatory = $false)]
    [switch]$EnableCEmuChecks,

    [Parameter(Mandatory = $false)]
    [switch]$SkipDesktopStage,

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
    [ValidateSet("none", "dispatch_load", "return_to_archive", "archive_full")]
    [string]$ArchiveRamFailurePhase = "none",

    [Parameter(Mandatory = $false)]
    [string]$HarnessKeys = "2,+,3,enter",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$HarnessScriptPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RegressionScriptPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RegressionCaseFileStrict = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$CEmuRegressionCaseFileStrict = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RegressionCaseFileMinimal = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$RegressionCaseFileEdgeStrict = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$HardwareConstraintCaseFile = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$DesktopRunnerPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$DesktopCommand = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyCollection()]
    [string[]]$CEmuHarnessExpectedVramCrcs = @("B9B5D5E0"),

    [Parameter(Mandatory = $false)]
    [string]$CEmuHarnessHashName = "basic_addition",

    [Parameter(Mandatory = $false)]
    [string]$CEmuHarnessHashStart = "vram_start",

    [Parameter(Mandatory = $false)]
    [string]$CEmuHarnessHashSize = "vram_16_size",

    [Parameter(Mandatory = $false)]
    [ValidateRange(1, 120000)]
    [int]$CEmuHarnessHashTimeoutMs = 8000,

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:MaxCapturedOutputLines = 400
$script:MaxOutputLineLength = 600

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

function ConvertTo-DisplayArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ($Value -match "[\s`"]") {
        return ('"{0}"' -f $Value.Replace('"', '\"'))
    }

    return $Value
}

function ConvertTo-DisplayCommandLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Arguments
    )

    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add((ConvertTo-DisplayArgument -Value $ExecutablePath))
    foreach ($argument in @($Arguments)) {
        $argumentText = [string]$argument
        $parts.Add((ConvertTo-DisplayArgument -Value $argumentText))
    }

    return ($parts.ToArray() -join " ")
}

function Ensure-DirectoryExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        [void](New-Item -Path $Path -ItemType Directory -Force)
    }
}

function ConvertTo-NormalizedPathList {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [AllowNull()]
        [string[]]$Values = @()
    )

    $rows = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    foreach ($raw in @($Values)) {
        if ($null -eq $raw) {
            continue
        }

        foreach ($part in ([string]$raw -split ";")) {
            $trimmed = $part.Trim()
            if ([string]::IsNullOrWhiteSpace($trimmed)) {
                continue
            }

            $key = $trimmed.ToLowerInvariant()
            if ($seen.ContainsKey($key)) {
                continue
            }

            $seen[$key] = $true
            $rows.Add($trimmed)
        }
    }

    return $rows.ToArray()
}

function Resolve-EmulatorBuildArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ExplicitProgramPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ExplicitModuleProgramPaths = @()
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]

    $resolvedProgramPath = ""
    $programSource = "auto"

    $officialArtifacts = $null
    try {
        $officialArtifacts = Resolve-CalcEmuOfficialSplitSuiteArtifacts -RepoRoot $RepoRoot
    }
    catch {
        $errors.Add($_.Exception.Message)
    }

    if ($null -ne $officialArtifacts) {
        foreach ($artifactError in @($officialArtifacts.errors)) {
            $errors.Add([string]$artifactError)
        }
        foreach ($artifactWarning in @($officialArtifacts.warnings)) {
            $warnings.Add([string]$artifactWarning)
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitProgramPath)) {
        $programSource = "explicit"
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

    $resolvedModulePaths = New-Object System.Collections.Generic.List[string]
    $moduleSource = "auto"

    $explicitModules = @(ConvertTo-NormalizedPathList -Values $ExplicitModuleProgramPaths)
    if ($explicitModules.Count -gt 0) {
        $moduleSource = "explicit"

        foreach ($entry in $explicitModules) {
            if (-not (Test-Path -LiteralPath $entry -PathType Leaf)) {
                $errors.Add("Module program path '$entry' does not exist.")
                continue
            }

            $resolvedModulePaths.Add((Resolve-Path -LiteralPath $entry).ProviderPath)
        }
    }
    elseif ($null -ne $officialArtifacts) {
        $moduleSource = [string]$officialArtifacts.source
        foreach ($artifactPath in @($officialArtifacts.module_paths)) {
            $resolvedModulePaths.Add([string]$artifactPath)
        }
    }

    if ([string]::IsNullOrWhiteSpace($resolvedProgramPath)) {
        $warnings.Add("No program artifact was discovered. Provide -ProgramPath to enforce hardware-constraint checks.")
    }

    if ($resolvedModulePaths.Count -eq 0) {
        $warnings.Add("No module artifacts were discovered. Provide -ModuleProgramPaths to enforce module size checks.")
    }

    return [ordered]@{
        program_path = $resolvedProgramPath
        program_source = $programSource
        module_paths = $resolvedModulePaths.ToArray()
        module_source = $moduleSource
        errors = $errors.ToArray()
        warnings = $warnings.ToArray()
    }
}

function Invoke-ExternalCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$WorkingDirectory = ""
    )

    $captured = @()
    $exitCode = 1
    $invocationError = ""
    $startTime = Get-Date

    $commandDisplay = ConvertTo-DisplayCommandLine -ExecutablePath $ExecutablePath -Arguments $Arguments

    try {
        if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            $captured = & $ExecutablePath @Arguments 2>&1
        }
        else {
            Push-Location -LiteralPath $WorkingDirectory
            try {
                $captured = & $ExecutablePath @Arguments 2>&1
            }
            finally {
                Pop-Location
            }
        }

        if ($null -ne $LASTEXITCODE) {
            $exitCode = [int]$LASTEXITCODE
        }
        else {
            $exitCode = 0
        }
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    $outputLines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($captured)) {
        if ($null -ne $entry) {
            $outputText = [string]$entry
            if ($outputText.Length -gt $script:MaxOutputLineLength) {
                $outputText = $outputText.Substring(0, $script:MaxOutputLineLength) + "... <line truncated>"
            }
            $outputLines.Add($outputText)
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($invocationError)) {
        $outputLines.Add("Invocation error: $invocationError")
    }

    if ($outputLines.Count -gt $script:MaxCapturedOutputLines) {
        $headCount = [int][Math]::Floor(($script:MaxCapturedOutputLines - 1) / 2)
        $tailCount = $script:MaxCapturedOutputLines - $headCount - 1
        $allLines = $outputLines.ToArray()
        $truncatedLines = New-Object System.Collections.Generic.List[string]

        for ($index = 0; $index -lt $headCount; $index++) {
            $truncatedLines.Add([string]$allLines[$index])
        }

        $truncatedLines.Add(
            ("... output truncated: {0} total lines captured, limited to {1} ..." -f $allLines.Length, $script:MaxCapturedOutputLines)
        )

        $tailStart = $allLines.Length - $tailCount
        for ($index = $tailStart; $index -lt $allLines.Length; $index++) {
            $truncatedLines.Add([string]$allLines[$index])
        }

        $outputLines = $truncatedLines
    }

    $durationMs = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)

    return [ordered]@{
        command = $commandDisplay
        working_directory = $WorkingDirectory
        exit_code = $exitCode
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
        duration_ms = $durationMs
    }
}

function Invoke-PowerShellScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PowerShellPath,

        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ScriptArguments = @()
    )

    if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
        throw "Script path '$ScriptPath' does not exist."
    }

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $resolvedScriptPath
    )

    foreach ($scriptArgument in @($ScriptArguments)) {
        $arguments += [string]$scriptArgument
    }

    return Invoke-ExternalCommand -ExecutablePath $PowerShellPath -Arguments $arguments
}

function Invoke-PowerShellCommandString {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PowerShellPath,

        [Parameter(Mandatory = $true)]
        [string]$CommandText
    )

    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        $CommandText
    )

    return Invoke-ExternalCommand -ExecutablePath $PowerShellPath -Arguments $arguments
}

function New-StageRecord {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Description,

        [Parameter(Mandatory = $true)]
        [bool]$Required
    )

    return [ordered]@{
        name = $Name
        description = $Description
        required = $Required
        status = "pending"
        success = $null
        skipped = $false
        exit_code = $null
        started_at_utc = ""
        completed_at_utc = ""
        duration_ms = 0
        command = ""
        working_directory = ""
        output_lines = @()
        artifacts = @()
        notes = @()
        errors = @()
    }
}

function Complete-StageFromInvocation {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Stage,

        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Invocation,

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [AllowNull()]
        [object[]]$Artifacts = @()
    )

    $Stage.command = [string]$Invocation.command
    $Stage.working_directory = [string]$Invocation.working_directory
    $Stage.output_lines = @($Invocation.output_lines)
    $Stage.exit_code = [int]$Invocation.exit_code
    $Stage.duration_ms = [int]$Invocation.duration_ms
    $Stage.artifacts = @($Artifacts)
    $Stage.completed_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    $Stage.success = ($Stage.exit_code -eq 0)
    if ($Stage.success) {
        $Stage.status = "pass"
    }
    else {
        $Stage.status = "fail"
        $Stage.errors = @("Command exited with code $($Stage.exit_code).")
    }
}

function Complete-StageAsSkipped {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Stage,

        [Parameter(Mandatory = $true)]
        [string]$Reason
    )

    $Stage.status = "skip"
    $Stage.success = $true
    $Stage.skipped = $true
    $Stage.exit_code = 0
    $Stage.duration_ms = 0
    $Stage.notes = @($Reason)
    $Stage.completed_at_utc = (Get-Date).ToUniversalTime().ToString("o")
}

function Complete-StageAsConfigurationFailure {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Stage,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $Stage.status = "fail"
    $Stage.success = $false
    $Stage.skipped = $false
    $Stage.exit_code = 1
    $Stage.duration_ms = 0
    $Stage.errors = @($Message)
    $Stage.output_lines = @($Message)
    $Stage.completed_at_utc = (Get-Date).ToUniversalTime().ToString("o")
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$outputsDirectory = Join-Path $repoRoot "tests\emulator\outputs"
Ensure-DirectoryExists -Path $outputsDirectory

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $outputsDirectory "prehardware-validation-gate.summary.json"
}
else {
    $outputDirectory = Split-Path -Path $OutputPath -Parent
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
        Ensure-DirectoryExists -Path $outputDirectory
    }
}

if ([string]::IsNullOrWhiteSpace($HarnessScriptPath)) {
    $HarnessScriptPath = Join-Path $repoRoot "tools\emulator\run-terminal-harness.ps1"
}
if ([string]::IsNullOrWhiteSpace($RegressionScriptPath)) {
    $RegressionScriptPath = Join-Path $repoRoot "tools\emulator\run-terminal-regression.ps1"
}
if ([string]::IsNullOrWhiteSpace($RegressionCaseFileStrict)) {
    $RegressionCaseFileStrict = Join-Path $repoRoot "tests\emulator\terminal-regression.pass6.cases.json"
}
if ([string]::IsNullOrWhiteSpace($CEmuRegressionCaseFileStrict)) {
    $CEmuRegressionCaseFileStrict = Join-Path $repoRoot "tests\emulator\terminal-regression.pass8.baselined.cases.json"
}
if ([string]::IsNullOrWhiteSpace($RegressionCaseFileMinimal)) {
    $RegressionCaseFileMinimal = Join-Path $repoRoot "tests\emulator\terminal-regression.pass2.cases.json"
}
if ([string]::IsNullOrWhiteSpace($RegressionCaseFileEdgeStrict)) {
    $RegressionCaseFileEdgeStrict = Join-Path $repoRoot "tests\emulator\terminal-regression.pass9.cases.json"
}
if ([string]::IsNullOrWhiteSpace($HardwareConstraintCaseFile)) {
    $HardwareConstraintCaseFile = Join-Path $repoRoot "tests\emulator\terminal-regression.pass2.cases.json"
}
if ([string]::IsNullOrWhiteSpace($DesktopRunnerPath)) {
    $DesktopRunnerPath = Join-Path $repoRoot "tests\test_runner.exe"
}

$harnessOutputPath = Join-Path $outputsDirectory "prehardware-validation-gate.harness-output.json"
$regressionSummaryPath = Join-Path $outputsDirectory "prehardware-validation-gate.regression-summary.json"
$regressionEdgeSummaryPath = Join-Path $outputsDirectory "prehardware-validation-gate.regression-edge-summary.json"
$hardwareConstraintSummaryPath = Join-Path $outputsDirectory "prehardware-validation-gate.hardware-constraints-summary.json"
$cemuHarnessOutputPath = Join-Path $outputsDirectory "prehardware-validation-gate.cemu-harness-output.json"
$cemuRegressionSummaryPath = Join-Path $outputsDirectory "prehardware-validation-gate.cemu-regression-summary.json"

$powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
if ($null -eq $powershellCommand) {
    Write-Output "Pre-hardware gate failed: powershell.exe was not found."
    exit 2
}
$powershellPath = $powershellCommand.Source

$artifactResolution = Resolve-EmulatorBuildArtifacts `
    -RepoRoot $repoRoot `
    -ExplicitProgramPath $ProgramPath `
    -ExplicitModuleProgramPaths $ModuleProgramPaths

$resolvedProgramPath = [string]$artifactResolution.program_path
$resolvedModuleProgramPaths = @($artifactResolution.module_paths)

$resolvedArchiveRamDispatchModulePath = $ArchiveRamDispatchModulePath
if ([string]::IsNullOrWhiteSpace($resolvedArchiveRamDispatchModulePath) -and $resolvedModuleProgramPaths.Count -gt 0) {
    $resolvedArchiveRamDispatchModulePath = Get-CalcEmuLargestArtifactPath -CandidatePaths $resolvedModuleProgramPaths
}
elseif (-not [string]::IsNullOrWhiteSpace($resolvedArchiveRamDispatchModulePath) -and (Test-Path -LiteralPath $resolvedArchiveRamDispatchModulePath -PathType Leaf)) {
    $resolvedArchiveRamDispatchModulePath = (Resolve-Path -LiteralPath $resolvedArchiveRamDispatchModulePath).Path
}

$stages = New-Object System.Collections.Generic.List[object]
$fatalErrors = New-Object System.Collections.Generic.List[string]

try {
    $isStrictMode = ($Mode -eq "strict")
    $isMinimalMode = ($Mode -eq "minimal")

    $desktopRequired = ($isStrictMode -and -not $SkipDesktopStage.IsPresent)
    $desktopStage = New-StageRecord `
        -Name "desktop_unit" `
        -Description "Desktop unit/integration regression stage." `
        -Required $desktopRequired
    $desktopStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")

    $runDesktopStage = $desktopRequired -or `
        (-not [string]::IsNullOrWhiteSpace($DesktopCommand)) -or `
        (-not [string]::IsNullOrWhiteSpace($DesktopRunnerPath))

    if ($SkipDesktopStage.IsPresent) {
        Complete-StageAsSkipped -Stage $desktopStage -Reason "Desktop stage skipped by -SkipDesktopStage."
    }
    elseif ($isMinimalMode -and [string]::IsNullOrWhiteSpace($DesktopCommand) -and [string]::IsNullOrWhiteSpace($DesktopRunnerPath)) {
        Complete-StageAsSkipped -Stage $desktopStage -Reason "Minimal mode without explicit desktop command/runner."
    }
    elseif (-not $runDesktopStage) {
        Complete-StageAsSkipped -Stage $desktopStage -Reason "Desktop stage was not requested."
    }
    else {
        $desktopInvocation = $null

        if (-not [string]::IsNullOrWhiteSpace($DesktopCommand)) {
            $desktopInvocation = Invoke-PowerShellCommandString -PowerShellPath $powershellPath -CommandText $DesktopCommand
            Complete-StageFromInvocation -Stage $desktopStage -Invocation $desktopInvocation
        }
        else {
            if (Test-Path -LiteralPath $DesktopRunnerPath -PathType Leaf) {
                $resolvedDesktopRunnerPath = (Resolve-Path -LiteralPath $DesktopRunnerPath).Path
                if ([System.IO.Path]::GetExtension($resolvedDesktopRunnerPath).Equals(".ps1", [System.StringComparison]::OrdinalIgnoreCase)) {
                    $desktopInvocation = Invoke-PowerShellScript -PowerShellPath $powershellPath -ScriptPath $resolvedDesktopRunnerPath
                }
                else {
                    $desktopWorkingDirectory = Split-Path -Path $resolvedDesktopRunnerPath -Parent
                    $desktopInvocation = Invoke-ExternalCommand -ExecutablePath $resolvedDesktopRunnerPath -WorkingDirectory $desktopWorkingDirectory
                }

                Complete-StageFromInvocation -Stage $desktopStage -Invocation $desktopInvocation
            }
            else {
                $makeCommand = Get-Command -Name "make" -ErrorAction SilentlyContinue
                $testsDirectory = Join-Path $repoRoot "tests"
                if ($null -ne $makeCommand -and (Test-Path -LiteralPath (Join-Path $testsDirectory "Makefile") -PathType Leaf)) {
                    $desktopInvocation = Invoke-ExternalCommand `
                        -ExecutablePath $makeCommand.Source `
                        -Arguments @("run") `
                        -WorkingDirectory $testsDirectory
                    Complete-StageFromInvocation -Stage $desktopStage -Invocation $desktopInvocation
                }
                elseif ($desktopRequired) {
                    Complete-StageAsConfigurationFailure `
                        -Stage $desktopStage `
                        -Message "Desktop runner '$DesktopRunnerPath' was not found and no 'make' command is available for tests/Makefile."
                }
                else {
                    Complete-StageAsSkipped `
                        -Stage $desktopStage `
                        -Reason "Desktop runner '$DesktopRunnerPath' was not found and desktop stage is optional in minimal mode."
                }
            }
        }
    }
    $stages.Add($desktopStage)

    $harnessStage = New-StageRecord `
        -Name "emulator_harness_mock" `
        -Description "Terminal harness deterministic mock-mode validation." `
        -Required $true
    $harnessStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    if (-not (Test-Path -LiteralPath $HarnessScriptPath -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $harnessStage -Message "Harness script '$HarnessScriptPath' does not exist."
    }
    else {
        $harnessArguments = @(
            "-Keys", $HarnessKeys,
            "-ForceMock",
            "-Format", "json",
            "-OutputPath", $harnessOutputPath
        )
        $harnessInvocation = Invoke-PowerShellScript `
            -PowerShellPath $powershellPath `
            -ScriptPath $HarnessScriptPath `
            -ScriptArguments $harnessArguments

        Complete-StageFromInvocation `
            -Stage $harnessStage `
            -Invocation $harnessInvocation `
            -Artifacts @(
                [ordered]@{
                    name = "harness_output"
                    path = $harnessOutputPath
                }
            )
    }
    $stages.Add($harnessStage)

    $regressionCaseFile = $RegressionCaseFileStrict
    if ($isMinimalMode) {
        $regressionCaseFile = $RegressionCaseFileMinimal
    }

    $regressionStage = New-StageRecord `
        -Name "emulator_regression" `
        -Description "Terminal regression suite execution." `
        -Required $true
    $regressionStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    if (-not (Test-Path -LiteralPath $RegressionScriptPath -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $regressionStage -Message "Regression script '$RegressionScriptPath' does not exist."
    }
    elseif (-not (Test-Path -LiteralPath $regressionCaseFile -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $regressionStage -Message "Regression case file '$regressionCaseFile' does not exist."
    }
    else {
        $regressionArguments = @(
            "-CaseFile", (Resolve-Path -LiteralPath $regressionCaseFile).Path,
            "-ForceMock",
            "-SummaryPath", $regressionSummaryPath
        )
        $regressionInvocation = Invoke-PowerShellScript `
            -PowerShellPath $powershellPath `
            -ScriptPath $RegressionScriptPath `
            -ScriptArguments $regressionArguments
        Complete-StageFromInvocation `
            -Stage $regressionStage `
            -Invocation $regressionInvocation `
            -Artifacts @(
                [ordered]@{
                    name = "regression_summary"
                    path = $regressionSummaryPath
                }
            )
    }
    $stages.Add($regressionStage)

    $edgeRequired = $isStrictMode
    $edgeStage = New-StageRecord `
        -Name "emulator_regression_edge" `
        -Description "Edge-case regression suite execution (strict mode)." `
        -Required $edgeRequired
    $edgeStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    if ($isMinimalMode) {
        Complete-StageAsSkipped -Stage $edgeStage -Reason "Edge-case suite is strict-mode only."
    }
    elseif (-not (Test-Path -LiteralPath $RegressionScriptPath -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $edgeStage -Message "Regression script '$RegressionScriptPath' does not exist."
    }
    elseif (-not (Test-Path -LiteralPath $RegressionCaseFileEdgeStrict -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $edgeStage -Message "Edge-case file '$RegressionCaseFileEdgeStrict' does not exist."
    }
    else {
        $edgeArguments = @(
            "-CaseFile", (Resolve-Path -LiteralPath $RegressionCaseFileEdgeStrict).Path,
            "-ForceMock",
            "-SummaryPath", $regressionEdgeSummaryPath
        )
        $edgeInvocation = Invoke-PowerShellScript `
            -PowerShellPath $powershellPath `
            -ScriptPath $RegressionScriptPath `
            -ScriptArguments $edgeArguments
        Complete-StageFromInvocation `
            -Stage $edgeStage `
            -Invocation $edgeInvocation `
            -Artifacts @(
                [ordered]@{
                    name = "edge_regression_summary"
                    path = $regressionEdgeSummaryPath
                }
            )
    }
    $stages.Add($edgeStage)

    $hardwareRequired = $isStrictMode
    $hardwareStage = New-StageRecord `
        -Name "hardware_constraints" `
        -Description "Hardware-faithful size and archive/RAM constraints via regression runner." `
        -Required $hardwareRequired
    $hardwareStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")

    $artifactConfigurationErrors = New-Object System.Collections.Generic.List[string]
    foreach ($artifactError in @($artifactResolution.errors)) {
        $artifactConfigurationErrors.Add([string]$artifactError)
    }

    if ($artifactConfigurationErrors.Count -gt 0) {
        Complete-StageAsConfigurationFailure `
            -Stage $hardwareStage `
            -Message ($artifactConfigurationErrors.ToArray() -join " | ")
    }
    elseif ([string]::IsNullOrWhiteSpace($resolvedProgramPath) -or $resolvedModuleProgramPaths.Count -eq 0) {
        if ($hardwareRequired) {
            $missingMessage = "Hardware-constraint stage requires program and module artifacts. Provide -ProgramPath and -ModuleProgramPaths or build split outputs under bin/."
            Complete-StageAsConfigurationFailure -Stage $hardwareStage -Message $missingMessage
        }
        else {
            Complete-StageAsSkipped `
                -Stage $hardwareStage `
                -Reason "Minimal mode without complete program/module artifacts for hardware-constraint validation."
        }
    }
    elseif (-not (Test-Path -LiteralPath $RegressionScriptPath -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $hardwareStage -Message "Regression script '$RegressionScriptPath' does not exist."
    }
    elseif (-not (Test-Path -LiteralPath $HardwareConstraintCaseFile -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $hardwareStage -Message "Hardware-constraint case file '$HardwareConstraintCaseFile' does not exist."
    }
    else {
        $hardwareArguments = New-Object System.Collections.Generic.List[string]
        $hardwareArguments.Add("-CaseFile")
        $hardwareArguments.Add((Resolve-Path -LiteralPath $HardwareConstraintCaseFile).Path)
        $hardwareArguments.Add("-ForceMock")
        $hardwareArguments.Add("-ProgramPath")
        $hardwareArguments.Add($resolvedProgramPath)
        if ($resolvedModuleProgramPaths.Count -gt 0) {
            $hardwareArguments.Add("-ModuleProgramPaths")
            $hardwareArguments.Add(($resolvedModuleProgramPaths -join ";"))
        }
        $hardwareArguments.Add("-UsableUserRamLimitBytes")
        $hardwareArguments.Add([string]$UsableUserRamLimitBytes)
        $hardwareArguments.Add("-ModuleLinkedBinaryLimitBytes")
        $hardwareArguments.Add([string]$ModuleLinkedBinaryLimitBytes)
        if (-not [string]::IsNullOrWhiteSpace($resolvedArchiveRamDispatchModulePath)) {
            $hardwareArguments.Add("-ArchiveRamDispatchModulePath")
            $hardwareArguments.Add($resolvedArchiveRamDispatchModulePath)
        }
        $hardwareArguments.Add("-ArchiveRamSwapRamCapacityBytes")
        $hardwareArguments.Add([string]$ArchiveRamSwapRamCapacityBytes)
        $hardwareArguments.Add("-ArchiveRamFailurePhase")
        $hardwareArguments.Add($ArchiveRamFailurePhase)
        $hardwareArguments.Add("-SummaryPath")
        $hardwareArguments.Add($hardwareConstraintSummaryPath)

        $hardwareInvocation = Invoke-PowerShellScript `
            -PowerShellPath $powershellPath `
            -ScriptPath $RegressionScriptPath `
            -ScriptArguments $hardwareArguments.ToArray()

        Complete-StageFromInvocation `
            -Stage $hardwareStage `
            -Invocation $hardwareInvocation `
            -Artifacts @(
                [ordered]@{
                    name = "hardware_constraints_summary"
                    path = $hardwareConstraintSummaryPath
                }
            )
        $hardwareStage.notes = @(
            ("program_path={0}" -f $resolvedProgramPath),
            ("module_count={0}" -f $resolvedModuleProgramPaths.Count),
            ("archive_dispatch_path={0}" -f $resolvedArchiveRamDispatchModulePath)
        )
    }
    $stages.Add($hardwareStage)

    $cemuRequired = $EnableCEmuChecks.IsPresent
    $cemuHarnessStage = New-StageRecord `
        -Name "emulator_harness_cemu" `
        -Description "Optional strict CEmu harness verification." `
        -Required $cemuRequired
    $cemuHarnessStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")

    if (-not $EnableCEmuChecks.IsPresent) {
        Complete-StageAsSkipped `
            -Stage $cemuHarnessStage `
            -Reason "CEmu checks disabled. Use -EnableCEmuChecks to require ROM/autotester validation."
    }
    else {
        $cemuConfigErrors = New-Object System.Collections.Generic.List[string]
        $resolvedCemuRomPath = ""
        $resolvedCemuAutotesterPath = ""

        try {
            $resolvedCemuRomPath = [string](Resolve-CalcEmuRomPath -RomPath $RomPath).path
        }
        catch {
            $cemuConfigErrors.Add($_.Exception.Message)
        }

        try {
            $resolvedCemuAutotesterPath = [string](Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath).path
        }
        catch {
            $cemuConfigErrors.Add($_.Exception.Message)
        }

        if ([string]::IsNullOrWhiteSpace($resolvedProgramPath)) {
            $cemuConfigErrors.Add("ProgramPath could not be resolved for CEmu checks.")
        }

        if (-not (Test-Path -LiteralPath $HarnessScriptPath -PathType Leaf)) {
            $cemuConfigErrors.Add("Harness script '$HarnessScriptPath' does not exist.")
        }

        if ($cemuConfigErrors.Count -gt 0) {
            Complete-StageAsConfigurationFailure -Stage $cemuHarnessStage -Message ($cemuConfigErrors.ToArray() -join " | ")
        }
        else {
            $cemuHarnessArguments = @(
                "-Keys", $HarnessKeys,
                "-Format", "json",
                "-NoMockFallback",
                "-RomPath", $resolvedCemuRomPath,
                "-AutotesterPath", $resolvedCemuAutotesterPath,
                "-ProgramPath", $resolvedProgramPath,
                "-UsableUserRamLimitBytes", [string]$UsableUserRamLimitBytes,
                "-ModuleLinkedBinaryLimitBytes", [string]$ModuleLinkedBinaryLimitBytes,
                "-ExpectedVramCrcs", ($CEmuHarnessExpectedVramCrcs -join ";"),
                "-CEmuHashName", $CEmuHarnessHashName,
                "-CEmuHashStart", $CEmuHarnessHashStart,
                "-CEmuHashSize", $CEmuHarnessHashSize,
                "-CEmuHashTimeoutMs", [string]$CEmuHarnessHashTimeoutMs,
                "-OutputPath", $cemuHarnessOutputPath
            )
            if ($resolvedModuleProgramPaths.Count -gt 0) {
                $cemuHarnessArguments += @(
                    "-ModuleProgramPaths",
                    ($resolvedModuleProgramPaths -join ";")
                )
            }

            $cemuHarnessInvocation = Invoke-PowerShellScript `
                -PowerShellPath $powershellPath `
                -ScriptPath $HarnessScriptPath `
                -ScriptArguments $cemuHarnessArguments

            Complete-StageFromInvocation `
                -Stage $cemuHarnessStage `
                -Invocation $cemuHarnessInvocation `
                -Artifacts @(
                    [ordered]@{
                        name = "cemu_harness_output"
                        path = $cemuHarnessOutputPath
                    }
                )
        }
    }
    $stages.Add($cemuHarnessStage)

    $cemuRegressionStage = New-StageRecord `
        -Name "emulator_regression_cemu" `
        -Description "Optional strict CEmu regression run." `
        -Required $cemuRequired
    $cemuRegressionStage.started_at_utc = (Get-Date).ToUniversalTime().ToString("o")

    if (-not $EnableCEmuChecks.IsPresent) {
        Complete-StageAsSkipped -Stage $cemuRegressionStage -Reason "CEmu checks disabled."
    }
    elseif (-not $cemuHarnessStage.success) {
        Complete-StageAsSkipped -Stage $cemuRegressionStage -Reason "CEmu harness stage failed; skipping CEmu regression stage."
    }
    elseif (-not (Test-Path -LiteralPath $RegressionScriptPath -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $cemuRegressionStage -Message "Regression script '$RegressionScriptPath' does not exist."
    }
    elseif (-not (Test-Path -LiteralPath $CEmuRegressionCaseFileStrict -PathType Leaf)) {
        Complete-StageAsConfigurationFailure -Stage $cemuRegressionStage -Message "CEmu regression case file '$CEmuRegressionCaseFileStrict' does not exist."
    }
    else {
        $cemuRegressionArguments = New-Object System.Collections.Generic.List[string]
        $cemuRegressionArguments.Add("-CaseFile")
        $cemuRegressionArguments.Add((Resolve-Path -LiteralPath $CEmuRegressionCaseFileStrict).ProviderPath)
        $cemuRegressionArguments.Add("-NoMockFallback")
        $cemuRegressionArguments.Add("-RomPath")
        $cemuRegressionArguments.Add($resolvedCemuRomPath)
        $cemuRegressionArguments.Add("-AutotesterPath")
        $cemuRegressionArguments.Add($resolvedCemuAutotesterPath)
        $cemuRegressionArguments.Add("-ProgramPath")
        $cemuRegressionArguments.Add($resolvedProgramPath)
        if ($resolvedModuleProgramPaths.Count -gt 0) {
            $cemuRegressionArguments.Add("-ModuleProgramPaths")
            $cemuRegressionArguments.Add(($resolvedModuleProgramPaths -join ";"))
        }
        $cemuRegressionArguments.Add("-UsableUserRamLimitBytes")
        $cemuRegressionArguments.Add([string]$UsableUserRamLimitBytes)
        $cemuRegressionArguments.Add("-ModuleLinkedBinaryLimitBytes")
        $cemuRegressionArguments.Add([string]$ModuleLinkedBinaryLimitBytes)
        $cemuRegressionArguments.Add("-SummaryPath")
        $cemuRegressionArguments.Add($cemuRegressionSummaryPath)

        $cemuRegressionInvocation = Invoke-PowerShellScript `
            -PowerShellPath $powershellPath `
            -ScriptPath $RegressionScriptPath `
            -ScriptArguments $cemuRegressionArguments.ToArray()

        Complete-StageFromInvocation `
            -Stage $cemuRegressionStage `
            -Invocation $cemuRegressionInvocation `
            -Artifacts @(
                [ordered]@{
                    name = "cemu_regression_summary"
                    path = $cemuRegressionSummaryPath
                }
            )
    }
    $stages.Add($cemuRegressionStage)
}
catch {
    $fatalErrors.Add($_.Exception.Message)
}

$stageArray = @($stages.ToArray())

$totalCount = $stageArray.Count
$passedCount = 0
$failedCount = 0
$skippedCount = 0
$requiredFailedCount = 0

foreach ($stageRow in $stageArray) {
    if ([string]$stageRow.status -eq "skip") {
        $skippedCount++
        continue
    }

    if ([string]$stageRow.status -eq "pass") {
        $passedCount++
    }
    else {
        $failedCount++
        if ([bool]$stageRow.required) {
            $requiredFailedCount++
        }
    }
}

$runSuccess = ($requiredFailedCount -eq 0 -and $fatalErrors.Count -eq 0)
$exitCode = 0
if (-not $runSuccess) {
    $exitCode = 1
}
if ($fatalErrors.Count -gt 0) {
    $exitCode = 2
}

$summary = [ordered]@{
    schema_version = 1
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    runner = "run-prehardware-validation-gate.ps1"
    repo_root = $repoRoot
    mode = $Mode
    success = $runSuccess
    exit_code = $exitCode
    output_path = $OutputPath
    options = [ordered]@{
        enable_cemu_checks = [bool]$EnableCEmuChecks
        skip_desktop_stage = [bool]$SkipDesktopStage
        rom_path = $RomPath
        autotester_path = $AutotesterPath
        harness_keys = $HarnessKeys
        harness_script_path = $HarnessScriptPath
        regression_script_path = $RegressionScriptPath
        regression_case_file_strict = $RegressionCaseFileStrict
        cemu_regression_case_file_strict = $CEmuRegressionCaseFileStrict
        regression_case_file_minimal = $RegressionCaseFileMinimal
        regression_case_file_edge_strict = $RegressionCaseFileEdgeStrict
        hardware_constraint_case_file = $HardwareConstraintCaseFile
        desktop_runner_path = $DesktopRunnerPath
        desktop_command = $DesktopCommand
        cemu_harness_expected_vram_crcs = @($CEmuHarnessExpectedVramCrcs)
        cemu_harness_hash_name = $CEmuHarnessHashName
        cemu_harness_hash_start = $CEmuHarnessHashStart
        cemu_harness_hash_size = $CEmuHarnessHashSize
        cemu_harness_hash_timeout_ms = $CEmuHarnessHashTimeoutMs
        program_path_input = $ProgramPath
        module_program_paths_input = @($ModuleProgramPaths)
        resolved_program_path = $resolvedProgramPath
        resolved_module_program_paths = @($resolvedModuleProgramPaths)
        resolved_archive_dispatch_module_path = $resolvedArchiveRamDispatchModulePath
        usable_user_ram_limit_bytes = $UsableUserRamLimitBytes
        module_linked_binary_limit_bytes = $ModuleLinkedBinaryLimitBytes
        archive_ram_swap_ram_capacity_bytes = $ArchiveRamSwapRamCapacityBytes
        archive_ram_failure_phase = $ArchiveRamFailurePhase
        artifact_resolution = $artifactResolution
    }
    totals = [ordered]@{
        total = $totalCount
        passed = $passedCount
        failed = $failedCount
        skipped = $skippedCount
        required_failed = $requiredFailedCount
    }
    fatal_errors = $fatalErrors.ToArray()
    stages = $stageArray
}

$summaryJson = $summary | ConvertTo-Json -Depth 100
Set-Content -LiteralPath $OutputPath -Value $summaryJson -Encoding UTF8

Write-Output "Pre-hardware Validation Gate Summary"
Write-Output ("Mode: {0}" -f $Mode)
Write-Output ("EnableCEmuChecks: {0}" -f (ConvertTo-BooleanText -Value ([bool]$EnableCEmuChecks)))
Write-Output ("SkipDesktopStage: {0}" -f (ConvertTo-BooleanText -Value ([bool]$SkipDesktopStage)))
Write-Output ("Resolved ProgramPath: {0}" -f $resolvedProgramPath)
Write-Output ("Resolved ModuleProgramPaths: {0}" -f $resolvedModuleProgramPaths.Count)
Write-Output ""

foreach ($stageRow in $stageArray) {
    if ($stageRow.status -eq "pass") {
        Write-Output ("[PASS] {0} ({1} ms)" -f $stageRow.name, $stageRow.duration_ms)
    }
    elseif ($stageRow.status -eq "skip") {
        Write-Output ("[SKIP] {0}" -f $stageRow.name)
    }
    else {
        Write-Output ("[FAIL] {0} (exit={1})" -f $stageRow.name, $stageRow.exit_code)
    }

    if (-not [string]::IsNullOrWhiteSpace([string]$stageRow.command)) {
        Write-Output ("  command: {0}" -f $stageRow.command)
    }
    foreach ($note in @($stageRow.notes)) {
        Write-Output ("  note: {0}" -f $note)
    }
    foreach ($errorText in @($stageRow.errors)) {
        Write-Output ("  error: {0}" -f $errorText)
    }
}

Write-Output ""
Write-Output ("Total: {0}" -f $totalCount)
Write-Output ("Passed: {0}" -f $passedCount)
Write-Output ("Failed: {0}" -f $failedCount)
Write-Output ("Skipped: {0}" -f $skippedCount)
Write-Output ("RequiredFailed: {0}" -f $requiredFailedCount)
Write-Output ("Success: {0}" -f (ConvertTo-BooleanText -Value $runSuccess))
Write-Output ("SummaryPath: {0}" -f $OutputPath)
Write-Output ""
Write-Output "JSON_SUMMARY_START"
Write-Output $summaryJson
Write-Output "JSON_SUMMARY_END"

exit $exitCode
