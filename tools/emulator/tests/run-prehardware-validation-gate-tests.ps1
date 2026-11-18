Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:GateScriptPath = Join-Path $script:RepoRoot "tools\emulator\run-prehardware-validation-gate.ps1"
if (-not (Test-Path -LiteralPath $script:GateScriptPath -PathType Leaf)) {
    Write-Error "Pre-hardware validation gate script was not found at '$script:GateScriptPath'."
    exit 2
}

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Expected,

        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message`nExpected: $Expected`nActual: $Actual"
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Needle,

        [Parameter(Mandatory = $true)]
        [string]$Haystack,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Haystack.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Message`nExpected to find: $Needle`nActual text: $Haystack"
    }
}

function Invoke-TestCase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Body
    )

    $script:TestTotal++
    try {
        & $Body
        $script:TestPassed++
        Write-Host "[PASS] $Name"
    }
    catch {
        $script:TestFailed++
        Write-Host "[FAIL] $Name"
        Write-Host $_.Exception.Message
    }
}

function New-TestDirectory {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("prehardware-gate-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BinaryFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [int]$SizeBytes
    )

    $buffer = New-Object byte[] $SizeBytes
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

function New-JsonCaseFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $path = Join-Path $Directory $Name
    $payload = @{
        schema_version = 1
        cases = @(
            @{
                name = "fake-case"
                keys = "2,+,3,enter"
                expect = @{
                    result = "5"
                    status = "ok"
                    error = ""
                    ignored_keys = @()
                }
            }
        )
    }
    $payload | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}

function New-FakeHarnessScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,

        [Parameter(Mandatory = $true)]
        [int]$ExitCode,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $path = Join-Path $Directory ("fake-harness-{0}.ps1" -f $Name)
    $scriptContent = @(
        "param(",
        "    [string]`$Keys = '''',",
        "    [string]`$Format = 'json',",
        "    [string]`$RomPath = '''',",
        "    [string]`$AutotesterPath = '''',",
        "    [string]`$ProgramPath = '''',",
        "    [string[]]`$ModuleProgramPaths = @(),",
        "    [int]`$UsableUserRamLimitBytes = 65536,",
        "    [int]`$ModuleLinkedBinaryLimitBytes = 51200,",
        "    [string[]]`$ExpectedVramCrcs = @(),",
        "    [string]`$CEmuHashName = '''',",
        "    [string]`$CEmuHashStart = '''',",
        "    [string]`$CEmuHashSize = '''',",
        "    [int]`$CEmuHashTimeoutMs = 8000,",
        "    [switch]`$ForceMock,",
        "    [switch]`$NoMockFallback,",
        "    [string]`$OutputPath = ''''",
        ")",
        "`$expandedModuleProgramPaths = New-Object System.Collections.Generic.List[string]",
        "foreach (`$moduleProgramPath in @(`$ModuleProgramPaths)) {",
        "    foreach (`$segment in ([string]`$moduleProgramPath -split ';', 0, [System.StringSplitOptions]::None)) {",
        "        `$trimmedSegment = ([string]`$segment).Trim()",
        "        if (-not [string]::IsNullOrWhiteSpace(`$trimmedSegment)) {",
        "            `$expandedModuleProgramPaths.Add(`$trimmedSegment)",
        "        }",
        "    }",
        "}",
        "`$payload = [ordered]@{",
        "    schema_version = 1",
        "    mode = 'mock'",
        "    success = ($ExitCode -eq 0)",
        "    input = [ordered]@{ keys = `$Keys }",
        "    program_path = `$ProgramPath",
        "    module_program_paths = @(`$ModuleProgramPaths)",
        "    module_program_paths_expanded = `$expandedModuleProgramPaths.ToArray()",
        "    usable_user_ram_limit_bytes = `$UsableUserRamLimitBytes",
        "    module_linked_binary_limit_bytes = `$ModuleLinkedBinaryLimitBytes",
        "    errors = @()",
        "}",
        "if ($ExitCode -ne 0) {",
        "    `$payload.errors += 'fake harness failure'",
        "}",
        "`$rendered = `$payload | ConvertTo-Json -Depth 10",
        "if (-not [string]::IsNullOrWhiteSpace(`$OutputPath)) {",
        "    `$outputDir = Split-Path -Path `$OutputPath -Parent",
        "    if (-not [string]::IsNullOrWhiteSpace(`$outputDir) -and -not (Test-Path -LiteralPath `$outputDir -PathType Container)) {",
        "        [void](New-Item -Path `$outputDir -ItemType Directory -Force)",
        "    }",
        "    Set-Content -LiteralPath `$OutputPath -Value `$rendered -Encoding UTF8",
        "}",
        "Write-Output `$rendered",
        "exit $ExitCode"
    )
    Set-Content -LiteralPath $path -Value $scriptContent -Encoding UTF8
    return $path
}

function New-FakeRegressionScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,

        [Parameter(Mandatory = $true)]
        [int]$ExitCode,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $passedCount = 0
    $failedCount = 1
    if ($ExitCode -eq 0) {
        $passedCount = 1
        $failedCount = 0
    }

    $path = Join-Path $Directory ("fake-regression-{0}.ps1" -f $Name)
    $scriptContent = @(
        "param(",
        "    [Parameter(Mandatory = `$true)]",
        "    [string]`$CaseFile,",
        "    [string]`$SummaryPath = '''',",
        "    [string]`$RomPath = '''',",
        "    [string]`$AutotesterPath = '''',",
        "    [string]`$ProgramPath = '''',",
        "    [string[]]`$ModuleProgramPaths = @(),",
        "    [int]`$UsableUserRamLimitBytes = 65536,",
        "    [int]`$ModuleLinkedBinaryLimitBytes = 51200,",
        "    [string]`$ArchiveRamDispatchModulePath = '''',",
        "    [int]`$ArchiveRamSwapRamCapacityBytes = 65536,",
        "    [string]`$ArchiveRamFailurePhase = 'none',",
        "    [switch]`$ForceMock,",
        "    [switch]`$NoMockFallback",
        ")",
        "if (-not [string]::IsNullOrWhiteSpace(`$SummaryPath)) {",
        "    `$summaryDirectory = Split-Path -Path `$SummaryPath -Parent",
        "    if (-not [string]::IsNullOrWhiteSpace(`$summaryDirectory) -and -not (Test-Path -LiteralPath `$summaryDirectory -PathType Container)) {",
        "        [void](New-Item -Path `$summaryDirectory -ItemType Directory -Force)",
        "    }",
        "    `$summary = [ordered]@{",
        "        schema_version = 1",
        "        success = ($ExitCode -eq 0)",
        "        exit_code = $ExitCode",
        "        case_file = `$CaseFile",
        "        totals = [ordered]@{ total = 1; passed = $passedCount; failed = $failedCount }",
        "    }",
        "    `$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8",
        "}",
        "Write-Output ('fake regression case file: ' + `$CaseFile)",
        "exit $ExitCode"
    )
    Set-Content -LiteralPath $path -Value $scriptContent -Encoding UTF8
    return $path
}

function Invoke-PrehardwareGate {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdditionalArgs
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required for pre-hardware gate tests."
    }

    $powershellPath = $powershellCommand.Source
    $captured = @()
    $exitCode = 1
    $invocationError = ""
    $outputPath = ""

    $finalArgs = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:GateScriptPath
    )
    foreach ($arg in @($AdditionalArgs)) {
        $finalArgs += [string]$arg
    }

    $outputPathIndex = [Array]::IndexOf($finalArgs, "-OutputPath")
    if ($outputPathIndex -ge 0 -and ($outputPathIndex + 1) -lt $finalArgs.Length) {
        $outputPath = [string]$finalArgs[$outputPathIndex + 1]
    }

    try {
        $captured = & $powershellPath @finalArgs 2>&1
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
        $outputLines.Add("invocation error: $invocationError")
    }

    $summary = $null
    if (-not [string]::IsNullOrWhiteSpace($outputPath) -and (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        $summary = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = ($outputLines.ToArray() -join "`n")
        output_path = $outputPath
        summary = $summary
    }
}

try {
    Invoke-TestCase -Name "Gate succeeds in minimal mode when all required stages pass" -Body {
        $tempDir = New-TestDirectory
        $outputPath = Join-Path $tempDir "gate-summary-success.json"
        $harnessPath = New-FakeHarnessScript -Directory $tempDir -ExitCode 0 -Name "success"
        $regressionPath = New-FakeRegressionScript -Directory $tempDir -ExitCode 0 -Name "success"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $moduleAPath -SizeBytes 30000
        New-BinaryFile -Path $moduleBPath -SizeBytes 32000
        $caseMinimal = New-JsonCaseFile -Directory $tempDir -Name "minimal-cases.json"
        $caseStrict = New-JsonCaseFile -Directory $tempDir -Name "strict-cases.json"
        $caseCEmuStrict = New-JsonCaseFile -Directory $tempDir -Name "cemu-strict-cases.json"
        $caseEdge = New-JsonCaseFile -Directory $tempDir -Name "edge-cases.json"
        $caseHardware = New-JsonCaseFile -Directory $tempDir -Name "hardware-cases.json"

        $run = Invoke-PrehardwareGate -AdditionalArgs @(
            "-Mode", "minimal",
            "-SkipDesktopStage",
            "-HarnessScriptPath", $harnessPath,
            "-RegressionScriptPath", $regressionPath,
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", ("{0};{1}" -f $moduleAPath, $moduleBPath),
            "-RegressionCaseFileMinimal", $caseMinimal,
            "-RegressionCaseFileStrict", $caseStrict,
            "-RegressionCaseFileEdgeStrict", $caseEdge,
            "-HardwareConstraintCaseFile", $caseHardware,
            "-OutputPath", $outputPath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "Gate should succeed when all required minimal stages pass.`nOutput:`n$($run.output_text)"
        }
        Assert-True -Condition ($null -ne $run.summary) -Message "Gate summary JSON should exist."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.success) -Message "Gate summary success should be true."
        Assert-Equal -Expected "pass" -Actual ([string]($run.summary.stages | Where-Object { $_.name -eq "emulator_harness_mock" }).status) -Message "Harness stage should pass."
        Assert-Equal -Expected "pass" -Actual ([string]($run.summary.stages | Where-Object { $_.name -eq "emulator_regression" }).status) -Message "Regression stage should pass."
        Assert-Equal -Expected "pass" -Actual ([string]($run.summary.stages | Where-Object { $_.name -eq "hardware_constraints" }).status) -Message "Hardware constraint stage should pass."
    }

    Invoke-TestCase -Name "Gate propagates required-stage failure with non-zero exit" -Body {
        $tempDir = New-TestDirectory
        $outputPath = Join-Path $tempDir "gate-summary-fail.json"
        $harnessPath = New-FakeHarnessScript -Directory $tempDir -ExitCode 9 -Name "harness-fail"
        $regressionPath = New-FakeRegressionScript -Directory $tempDir -ExitCode 0 -Name "regression-pass"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $modulePath = Join-Path $tempDir "CEALG.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $modulePath -SizeBytes 20000
        $caseMinimal = New-JsonCaseFile -Directory $tempDir -Name "minimal-cases.json"
        $caseStrict = New-JsonCaseFile -Directory $tempDir -Name "strict-cases.json"
        $caseCEmuStrict = New-JsonCaseFile -Directory $tempDir -Name "cemu-strict-cases.json"
        $caseEdge = New-JsonCaseFile -Directory $tempDir -Name "edge-cases.json"
        $caseHardware = New-JsonCaseFile -Directory $tempDir -Name "hardware-cases.json"

        $run = Invoke-PrehardwareGate -AdditionalArgs @(
            "-Mode", "minimal",
            "-SkipDesktopStage",
            "-HarnessScriptPath", $harnessPath,
            "-RegressionScriptPath", $regressionPath,
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", $modulePath,
            "-RegressionCaseFileMinimal", $caseMinimal,
            "-RegressionCaseFileStrict", $caseStrict,
            "-RegressionCaseFileEdgeStrict", $caseEdge,
            "-HardwareConstraintCaseFile", $caseHardware,
            "-OutputPath", $outputPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Gate should return non-zero when required stage fails."
        Assert-True -Condition ($null -ne $run.summary) -Message "Failure run should still write summary JSON."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.success) -Message "Summary success should be false on required-stage failure."
        $harnessStage = $run.summary.stages | Where-Object { $_.name -eq "emulator_harness_mock" }
        Assert-Equal -Expected "fail" -Actual ([string]$harnessStage.status) -Message "Harness stage should be marked fail."
        Assert-Equal -Expected 9 -Actual ([int]$harnessStage.exit_code) -Message "Harness stage exit code should be propagated."
        Assert-True -Condition ([int]$run.summary.totals.required_failed -ge 1) -Message "Required failed count should be >= 1."
    }

    Invoke-TestCase -Name "Gate summary artifact contract includes required fields and stage records" -Body {
        $tempDir = New-TestDirectory
        $outputPath = Join-Path $tempDir "gate-summary-contract.json"
        $harnessPath = New-FakeHarnessScript -Directory $tempDir -ExitCode 0 -Name "contract"
        $regressionPath = New-FakeRegressionScript -Directory $tempDir -ExitCode 0 -Name "contract"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $modulePath = Join-Path $tempDir "CEALG.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $modulePath -SizeBytes 20000
        $caseMinimal = New-JsonCaseFile -Directory $tempDir -Name "minimal-cases.json"
        $caseStrict = New-JsonCaseFile -Directory $tempDir -Name "strict-cases.json"
        $caseEdge = New-JsonCaseFile -Directory $tempDir -Name "edge-cases.json"
        $caseHardware = New-JsonCaseFile -Directory $tempDir -Name "hardware-cases.json"

        $run = Invoke-PrehardwareGate -AdditionalArgs @(
            "-Mode", "minimal",
            "-SkipDesktopStage",
            "-HarnessScriptPath", $harnessPath,
            "-RegressionScriptPath", $regressionPath,
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", $modulePath,
            "-RegressionCaseFileMinimal", $caseMinimal,
            "-RegressionCaseFileStrict", $caseStrict,
            "-RegressionCaseFileEdgeStrict", $caseEdge,
            "-HardwareConstraintCaseFile", $caseHardware,
            "-OutputPath", $outputPath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "Contract run should succeed.`nOutput:`n$($run.output_text)"
        }
        Assert-True -Condition (Test-Path -LiteralPath $outputPath -PathType Leaf) -Message "Output summary artifact should exist."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.schema_version) -Message "Schema version should be 1."
        Assert-Equal -Expected "run-prehardware-validation-gate.ps1" -Actual ([string]$run.summary.runner) -Message "Runner field mismatch."
        Assert-True -Condition ($run.summary.PSObject.Properties.Name -contains "totals") -Message "Summary should include totals object."
        Assert-True -Condition ($run.summary.PSObject.Properties.Name -contains "stages") -Message "Summary should include stages."
        Assert-True -Condition ($run.summary.stages.Count -ge 5) -Message "Summary should include stage records."
        $firstStage = $run.summary.stages[0]
        Assert-True -Condition ($firstStage.PSObject.Properties.Name -contains "name") -Message "Stage contract should include name."
        Assert-True -Condition ($firstStage.PSObject.Properties.Name -contains "status") -Message "Stage contract should include status."
        Assert-True -Condition ($firstStage.PSObject.Properties.Name -contains "required") -Message "Stage contract should include required."
        Assert-True -Condition ($firstStage.PSObject.Properties.Name -contains "output_lines") -Message "Stage contract should include output_lines."
    }

    Invoke-TestCase -Name "Gate skips optional CEmu stages when inputs exist but checks are disabled" -Body {
        $tempDir = New-TestDirectory
        $outputPath = Join-Path $tempDir "gate-summary-optional-skip.json"
        $harnessPath = New-FakeHarnessScript -Directory $tempDir -ExitCode 0 -Name "optional"
        $regressionPath = New-FakeRegressionScript -Directory $tempDir -ExitCode 0 -Name "optional"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $modulePath = Join-Path $tempDir "CEALG.8xp"
        $romPath = Join-Path $tempDir "test.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $modulePath -SizeBytes 20000
        New-BinaryFile -Path $romPath -SizeBytes 1024
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        $caseMinimal = New-JsonCaseFile -Directory $tempDir -Name "minimal-cases.json"
        $caseStrict = New-JsonCaseFile -Directory $tempDir -Name "strict-cases.json"
        $caseCEmuStrict = New-JsonCaseFile -Directory $tempDir -Name "cemu-strict-cases.json"
        $caseEdge = New-JsonCaseFile -Directory $tempDir -Name "edge-cases.json"
        $caseHardware = New-JsonCaseFile -Directory $tempDir -Name "hardware-cases.json"

        $run = Invoke-PrehardwareGate -AdditionalArgs @(
            "-Mode", "minimal",
            "-SkipDesktopStage",
            "-HarnessScriptPath", $harnessPath,
            "-RegressionScriptPath", $regressionPath,
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", $modulePath,
            "-RegressionCaseFileMinimal", $caseMinimal,
            "-RegressionCaseFileStrict", $caseStrict,
            "-CEmuRegressionCaseFileStrict", $caseCEmuStrict,
            "-RegressionCaseFileEdgeStrict", $caseEdge,
            "-HardwareConstraintCaseFile", $caseHardware,
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-OutputPath", $outputPath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "Gate should pass when optional CEmu checks are disabled.`nOutput:`n$($run.output_text)"
        }
        $cemuHarnessStage = $run.summary.stages | Where-Object { $_.name -eq "emulator_harness_cemu" }
        $cemuRegressionStage = $run.summary.stages | Where-Object { $_.name -eq "emulator_regression_cemu" }
        Assert-Equal -Expected "skip" -Actual ([string]$cemuHarnessStage.status) -Message "CEmu harness stage should be skipped."
        Assert-Equal -Expected "skip" -Actual ([string]$cemuRegressionStage.status) -Message "CEmu regression stage should be skipped."
        Assert-Contains -Needle "disabled" -Haystack ([string]($cemuHarnessStage.notes -join " | ")) -Message "Skip reason should mention disabled checks."
    }

    Invoke-TestCase -Name "Gate uses strict CEmu case file and forwards module artifacts to the CEmu harness" -Body {
        $tempDir = New-TestDirectory
        $outputPath = Join-Path $tempDir "gate-summary-cemu-strict.json"
        $harnessPath = New-FakeHarnessScript -Directory $tempDir -ExitCode 0 -Name "cemu-strict"
        $regressionPath = New-FakeRegressionScript -Directory $tempDir -ExitCode 0 -Name "cemu-strict"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        $romPath = Join-Path $tempDir "test.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $moduleAPath -SizeBytes 30000
        New-BinaryFile -Path $moduleBPath -SizeBytes 32000
        New-BinaryFile -Path $romPath -SizeBytes 1024
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        $caseMinimal = New-JsonCaseFile -Directory $tempDir -Name "minimal-cases.json"
        $caseStrict = New-JsonCaseFile -Directory $tempDir -Name "strict-cases.json"
        $caseCEmuStrict = New-JsonCaseFile -Directory $tempDir -Name "cemu-strict-cases.json"
        $caseEdge = New-JsonCaseFile -Directory $tempDir -Name "edge-cases.json"
        $caseHardware = New-JsonCaseFile -Directory $tempDir -Name "hardware-cases.json"

        $run = Invoke-PrehardwareGate -AdditionalArgs @(
            "-Mode", "minimal",
            "-SkipDesktopStage",
            "-EnableCEmuChecks",
            "-HarnessScriptPath", $harnessPath,
            "-RegressionScriptPath", $regressionPath,
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", ("{0};{1}" -f $moduleAPath, $moduleBPath),
            "-RegressionCaseFileMinimal", $caseMinimal,
            "-RegressionCaseFileStrict", $caseStrict,
            "-CEmuRegressionCaseFileStrict", $caseCEmuStrict,
            "-RegressionCaseFileEdgeStrict", $caseEdge,
            "-HardwareConstraintCaseFile", $caseHardware,
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-OutputPath", $outputPath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "Gate should succeed when strict CEmu stages are satisfied.`nOutput:`n$($run.output_text)"
        }

        $cemuHarnessStage = $run.summary.stages | Where-Object { $_.name -eq "emulator_harness_cemu" }
        $cemuRegressionStage = $run.summary.stages | Where-Object { $_.name -eq "emulator_regression_cemu" }
        Assert-Equal -Expected "pass" -Actual ([string]$cemuHarnessStage.status) -Message "CEmu harness stage should pass."
        Assert-Equal -Expected "pass" -Actual ([string]$cemuRegressionStage.status) -Message "CEmu regression stage should pass."

        $cemuHarnessArtifactPath = [string](($cemuHarnessStage.artifacts | Select-Object -First 1).path)
        Assert-True -Condition (Test-Path -LiteralPath $cemuHarnessArtifactPath -PathType Leaf) -Message "CEmu harness artifact should exist."
        $cemuHarnessArtifact = Get-Content -LiteralPath $cemuHarnessArtifactPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $programPath -Actual ([string]$cemuHarnessArtifact.program_path) -Message "CEmu harness should receive the resolved launcher path."
        Assert-Equal -Expected 2 -Actual ([int]@($cemuHarnessArtifact.module_program_paths_expanded).Count) -Message "CEmu harness should receive both split-suite module paths."
        Assert-Contains -Needle $moduleAPath -Haystack (@($cemuHarnessArtifact.module_program_paths_expanded) -join "|") -Message "CEmu harness should receive CEALG."
        Assert-Contains -Needle $moduleBPath -Haystack (@($cemuHarnessArtifact.module_program_paths_expanded) -join "|") -Message "CEmu harness should receive CECALC."

        $cemuRegressionArtifactPath = [string](($cemuRegressionStage.artifacts | Select-Object -First 1).path)
        Assert-True -Condition (Test-Path -LiteralPath $cemuRegressionArtifactPath -PathType Leaf) -Message "CEmu regression artifact should exist."
        $cemuRegressionArtifact = Get-Content -LiteralPath $cemuRegressionArtifactPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected (Resolve-Path -LiteralPath $caseCEmuStrict).ProviderPath -Actual ([string]$cemuRegressionArtifact.case_file) -Message "CEmu regression stage should use the dedicated CEmu strict case file."
    }
}
finally {
    foreach ($directory in $script:CreatedTempDirectories) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ""
Write-Host "Total tests: $script:TestTotal"
Write-Host "Passed: $script:TestPassed"
Write-Host "Failed: $script:TestFailed"

if ($script:TestFailed -gt 0) {
    exit 1
}

exit 0
