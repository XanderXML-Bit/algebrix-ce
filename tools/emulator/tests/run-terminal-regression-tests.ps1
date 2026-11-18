Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-terminal-regression.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "Regression runner script was not found at '$script:RunnerPath'."
    exit 2
}

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]
$script:CreatedFiles = New-Object System.Collections.Generic.List[string]

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

    if ($Expected -is [array] -or $Actual -is [array]) {
        $expectedText = @($Expected) -join ","
        $actualText = @($Actual) -join ","
        if ($expectedText -ne $actualText) {
            throw "$Message`nExpected: $expectedText`nActual: $actualText"
        }

        return
    }

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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("calcemu-regression-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-JsonCaseFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [object]$Payload
    )

    $path = Join-Path $Directory $Name
    $json = $Payload | ConvertTo-Json -Depth 30
    Set-Content -LiteralPath $path -Value $json -Encoding UTF8
    return $path
}

function New-FakeAutotesterScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,

        [Parameter(Mandatory = $true)]
        [int]$ExitCode,

        [Parameter(Mandatory = $true)]
        [string]$Label,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$HashFailureName = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$HashFailureCrc = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$HashFailureCrcSequence = @()
    )

    $scriptPath = Join-Path $Directory ("fake-autotester-{0}.ps1" -f $Label)
    $scriptContent = @(
        "param([string]`$ConfigPath)",
        "`$ErrorActionPreference = `"Stop`"",
        "if ([string]::IsNullOrWhiteSpace(`$ConfigPath)) {",
        "    Write-Output `"fake-$Label missing-config`"",
        "    exit 41",
        "}",
        "if (-not (Test-Path -LiteralPath `$ConfigPath -PathType Leaf)) {",
        "    Write-Output `"fake-$Label config-not-found`"",
        "    exit 42",
        "}",
        "Write-Output (`"fake-$Label config=`" + `$ConfigPath)"
    )
    if (-not [string]::IsNullOrWhiteSpace($HashFailureName) -and ($HashFailureCrcSequence.Count -gt 0)) {
        $sequenceValues = New-Object System.Collections.Generic.List[string]
        foreach ($crcEntry in $HashFailureCrcSequence) {
            $sequenceValues.Add(('"{0}"' -f ([string]$crcEntry).Replace('"', '\"')))
        }

        $scriptContent += "`$sequence = @($($sequenceValues.ToArray() -join ','))"
        $scriptContent += "`$statePath = [System.IO.Path]::ChangeExtension(`$PSCommandPath, `".state.txt`")"
        $scriptContent += "`$attemptIndex = 0"
        $scriptContent += "if (Test-Path -LiteralPath `$statePath -PathType Leaf) {"
        $scriptContent += "    try {"
        $scriptContent += "        `$attemptIndex = [int](Get-Content -LiteralPath `$statePath -Raw)"
        $scriptContent += "    }"
        $scriptContent += "    catch {"
        $scriptContent += "        `$attemptIndex = 0"
        $scriptContent += "    }"
        $scriptContent += "}"
        $scriptContent += "if (`$attemptIndex -lt 0) { `$attemptIndex = 0 }"
        $scriptContent += "`$selectedIndex = `$attemptIndex"
        $scriptContent += "if (`$selectedIndex -ge `$sequence.Count) { `$selectedIndex = `$sequence.Count - 1 }"
        $scriptContent += "`$selectedCrc = [string]`$sequence[`$selectedIndex]"
        $scriptContent += "Set-Content -LiteralPath `$statePath -Value ([string](`$attemptIndex + 1)) -Encoding UTF8"
        $scriptContent += "Write-Output ('[Test failed!] Hash #$HashFailureName (""synthetic"") did not match the expected CRC 00000000 (got ' + `$selectedCrc + ').')"
    }
    elseif (-not [string]::IsNullOrWhiteSpace($HashFailureName) -and -not [string]::IsNullOrWhiteSpace($HashFailureCrc)) {
        $scriptContent += "Write-Output ('[Test failed!] Hash #$HashFailureName (""synthetic"") did not match the expected CRC 00000000 (got $HashFailureCrc).')"
    }
    $scriptContent += "exit $ExitCode"
    Set-Content -LiteralPath $scriptPath -Value $scriptContent -Encoding UTF8
    return $scriptPath
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

function Invoke-RegressionRunner {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CaseFile,

        [Parameter(Mandatory = $false)]
        [string[]]$AdditionalArgs = @()
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required for runner tests."
    }

    $powershellPath = $powershellCommand.Source
    $summaryPath = Join-Path (Split-Path -Path $CaseFile -Parent) ("summary-{0}.json" -f [System.Guid]::NewGuid().ToString("N"))

    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:RunnerPath,
        "-CaseFile",
        $CaseFile,
        "-SummaryPath",
        $summaryPath
    )

    foreach ($argument in $AdditionalArgs) {
        $arguments += $argument
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
        $outputLines.Add("runner invocation error: $invocationError")
    }

    $summaryObject = $null
    if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
        $summaryObject = (Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json)
    }
    if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
        $script:CreatedFiles.Add($summaryPath)
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = ($outputLines.ToArray() -join "`n")
        summary_path = $summaryPath
        summary = $summaryObject
    }
}

try {
    Invoke-TestCase -Name "Runner passes deterministic mock cases and emits summary JSON markers" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-pass.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "addition"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                    }
                },
                @{
                    name = "clear-multiply"
                    keys = "2,+,3,clear,6,*,7,enter"
                    expect = @{
                        result = "42"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                    }
                },
                @{
                    name = "mode-ignored"
                    keys = "2,mode,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @("KEY_MODE")
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed for passing mock cases."
        Assert-True -Condition ($null -ne $run.summary) -Message "Summary JSON should be created."
        Assert-Equal -Expected 3 -Actual ([int]$run.summary.totals.total) -Message "Unexpected total case count."
        Assert-Equal -Expected 3 -Actual ([int]$run.summary.totals.passed) -Message "Unexpected passed case count."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.totals.failed) -Message "Unexpected failed case count."
        Assert-Contains -Needle "JSON_SUMMARY_START" -Haystack $run.output_text -Message "Runner output should include JSON summary marker."
        Assert-Contains -Needle "JSON_SUMMARY_END" -Haystack $run.output_text -Message "Runner output should include JSON summary marker."
    }

    Invoke-TestCase -Name "Runner enforces usable user RAM limit for oversized .8xp program path" -Body {
        $tempDir = New-TestDirectory
        $oversizedProgramPath = Join-Path $tempDir "CALCENG.8xp"
        New-BinaryFile -Path $oversizedProgramPath -SizeBytes 70000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-memory-limit.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "memory-limit-check"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ProgramPath", $oversizedProgramPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when ProgramPath exceeds usable user RAM limit."
        Assert-Contains -Needle "Memory is insufficient to perform the calculation." -Haystack $run.output_text -Message "Failure output should include hardware-faithful memory-insufficient message."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.hardware_constraints.validated) -Message "Case summary should report validated hardware constraints."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.hardware_constraints.memory_insufficient) -Message "Case summary should report memory-insufficient state."
    }

    Invoke-TestCase -Name "Runner enforces per-module linked binary limit for multi-program architecture" -Body {
        $tempDir = New-TestDirectory
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $oversizedModulePath = Join-Path $tempDir "CEBIG.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $oversizedModulePath -SizeBytes 53000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-module-limit.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "module-limit-check"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", $oversizedModulePath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when any module exceeds linked binary limit."
        Assert-Contains -Needle "Module linked binary exceeds limit" -Haystack $run.output_text -Message "Failure output should describe module linked-binary limit violation."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.hardware_constraints.module_size_exceeded) -Message "Case summary should report module-size-exceeded state."
    }

    Invoke-TestCase -Name "Runner passes when explicit module set is compliant with hardware limits" -Body {
        $tempDir = New-TestDirectory
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $moduleAPath -SizeBytes 49000
        New-BinaryFile -Path $moduleBPath -SizeBytes 50000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-module-compliant.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "module-limit-pass"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", ("{0};{1}" -f $moduleAPath, $moduleBPath)
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should pass when program and module binaries satisfy limits."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.options.include_module_program_paths) -Message "Summary options should indicate module-path passthrough."
        Assert-Equal -Expected "explicit" -Actual ([string]$run.summary.options.module_program_paths_source) -Message "Summary should mark module paths as explicit inputs."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.hardware_constraints.pass) -Message "Case summary should report passing hardware constraints."
    }

    Invoke-TestCase -Name "Runner passes archive swap dispatch and return success path with structured diagnostics" -Body {
        $tempDir = New-TestDirectory
        $dispatchModulePath = Join-Path $tempDir "CEARCH.8xp"
        New-BinaryFile -Path $dispatchModulePath -SizeBytes 22000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-archive-swap-success.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "archive-swap-success"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        archive_ram_swap = @{
                            simulated = $true
                            pass = $true
                            failure_phase = "none"
                            dispatch_loaded_to_ram = $true
                            return_to_archive_completed = $true
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should pass when archive dispatch and return complete successfully."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.simulated) -Message "Summary should mark archive swap as simulated."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.pass) -Message "Summary should mark archive swap as passing."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.dispatch.loaded_to_ram) -Message "Summary should report dispatch loaded_to_ram=true."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.return_to_archive.returned_to_archive) -Message "Summary should report return_to_archive=true."
        Assert-Contains -Needle "archive_ram_swap_pass: true" -Haystack $run.output_text -Message "Text output should expose archive swap pass diagnostics."
    }

    Invoke-TestCase -Name "Runner fails when archive swap dispatch cannot complete due to RAM capacity" -Body {
        $tempDir = New-TestDirectory
        $dispatchModulePath = Join-Path $tempDir "CEARCHFAIL.8xp"
        New-BinaryFile -Path $dispatchModulePath -SizeBytes 70000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-archive-swap-dispatch-fail.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "archive-swap-dispatch-fail"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when archive dispatch cannot fit module in RAM capacity."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.pass) -Message "Summary should report archive swap pass=false."
        Assert-Equal -Expected "dispatch_load" -Actual ([string]$run.summary.cases[0].actual.archive_ram_swap.failure_phase) -Message "Summary should report dispatch_load failure phase."
        Assert-Contains -Needle "Archive<->RAM swap simulation failed" -Haystack $run.output_text -Message "Output should include archive swap failure diagnostics."
    }

    Invoke-TestCase -Name "Runner fails when archive swap return-to-archive cannot complete" -Body {
        $tempDir = New-TestDirectory
        $dispatchModulePath = Join-Path $tempDir "CEARCHRET.8xp"
        New-BinaryFile -Path $dispatchModulePath -SizeBytes 18000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-archive-swap-return-fail.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "archive-swap-return-fail"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath,
            "-ArchiveRamFailurePhase", "return_to_archive"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when return-to-archive is forced to fail."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.dispatch.loaded_to_ram) -Message "Dispatch should still load to RAM before return failure."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.return_to_archive.returned_to_archive) -Message "Return phase should report returned_to_archive=false."
        Assert-Equal -Expected "return_to_archive" -Actual ([string]$run.summary.cases[0].actual.archive_ram_swap.failure_phase) -Message "Summary should report return_to_archive failure phase."
        Assert-Contains -Needle "archive_ram_swap_failure_phase: return_to_archive" -Haystack $run.output_text -Message "Text output should include archive swap failure phase."
    }

    Invoke-TestCase -Name "Runner fails when archive swap detects a partial-load corruption edge case" -Body {
        $tempDir = New-TestDirectory
        $dispatchModulePath = Join-Path $tempDir "CEARCHPART.8xp"
        New-BinaryFile -Path $dispatchModulePath -SizeBytes 18000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-archive-swap-partial-load.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "archive-swap-partial-load"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath,
            "-ArchiveRamFailurePhase", "partial_load"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when archive swap detects a partial-load corruption edge case."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.pass) -Message "Summary should report archive swap pass=false for partial-load corruption."
        Assert-Equal -Expected "partial_load" -Actual ([string]$run.summary.cases[0].actual.archive_ram_swap.failure_phase) -Message "Summary should report partial_load failure phase."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.modules.partial_load_detected) -Message "Summary should report partial-load detection."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.modules.corruption_detected) -Message "Summary should report corruption detection for partial-load failures."
        Assert-Contains -Needle "partial load detected" -Haystack $run.output_text -Message "Output should include partial-load corruption diagnostics."
    }

    Invoke-TestCase -Name "Runner fails when archive swap collapses under memory-pressure simulation" -Body {
        $tempDir = New-TestDirectory
        $dispatchModulePath = Join-Path $tempDir "CEARCHMEM.8xp"
        New-BinaryFile -Path $dispatchModulePath -SizeBytes 8000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-archive-swap-memory-pressure.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "archive-swap-memory-pressure"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath,
            "-ArchiveRamFailurePhase", "memory_pressure",
            "-ArchiveRamSwapRamCapacityBytes", "65536"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when memory-pressure simulation exhausts effective archive swap RAM."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.pass) -Message "Summary should report archive swap pass=false for memory-pressure failures."
        Assert-Equal -Expected "memory_pressure" -Actual ([string]$run.summary.cases[0].actual.archive_ram_swap.failure_phase) -Message "Summary should report memory_pressure failure phase."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.cases[0].actual.archive_ram_swap.modules.effective_ram_capacity_bytes) -Message "Effective RAM capacity should drop to zero during memory-pressure simulation."
        Assert-Contains -Needle "effective swap RAM capacity is 0" -Haystack $run.output_text -Message "Output should include effective-capacity exhaustion diagnostics."
    }

    Invoke-TestCase -Name "Runner fails when archive swap cannot return module because archive is full" -Body {
        $tempDir = New-TestDirectory
        $dispatchModulePath = Join-Path $tempDir "CEARCHFULL.8xp"
        New-BinaryFile -Path $dispatchModulePath -SizeBytes 18000

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-archive-swap-archive-full.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "archive-swap-archive-full"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-ForceMock",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath,
            "-ArchiveRamFailurePhase", "archive_full"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when archive swap cannot re-archive because archive storage is full."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.pass) -Message "Summary should report archive swap pass=false for archive-full failures."
        Assert-Equal -Expected "archive_full" -Actual ([string]$run.summary.cases[0].actual.archive_ram_swap.failure_phase) -Message "Summary should report archive_full failure phase."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.return_to_archive.attempted) -Message "Return phase should still be attempted before archive-full failure is reported."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].actual.archive_ram_swap.return_to_archive.returned_to_archive) -Message "Return phase should report returned_to_archive=false for archive-full failures."
        Assert-Contains -Needle "archive is full" -Haystack $run.output_text -Message "Output should include archive-full diagnostics."
    }

    Invoke-TestCase -Name "Runner fails when expected result does not match" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-fail-result.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "bad-result"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "6"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should return non-zero for mismatched expectations."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Failed case count should be 1."
        Assert-Contains -Needle "Expected result '6'" -Haystack $run.output_text -Message "Failure output should explain result mismatch."
    }

    Invoke-TestCase -Name "Runner rejects cases missing required expected fields" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-missing-fields.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "missing-ignored"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail schema-invalid cases."
        Assert-Contains -Needle "Missing required expected field 'ignored_keys'" -Haystack $run.output_text -Message "Runner should report missing expected fields."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Invalid case should count as failed."
    }

    Invoke-TestCase -Name "Runner supports cemu passthrough and validates mode when fake autotester succeeds" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 0 -Label "passthrough"

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-success"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        mode = "cemu"
                        harness_success = $true
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-NoMockFallback"
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should pass in cemu passthrough mode."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.passed) -Message "Exactly one case should pass."
        Assert-Equal -Expected "cemu" -Actual ([string]$run.summary.cases[0].actual.mode) -Message "Case mode should be cemu."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.cases[0].harness.exit_code) -Message "Harness exit code should be 0."
        Assert-Contains -Needle "NoMockFallback: true" -Haystack $run.output_text -Message "Output should reflect NoMockFallback option."
    }

    Invoke-TestCase -Name "Runner validates cemu_hash expectations in cemu mode" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 0 -Label "cemu-hash-pass"

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-hash-pass.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-hash-pass"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        mode = "cemu"
                        harness_success = $true
                        cemu_hash = @{
                            name = "addition_result"
                            expected_crcs = @("0xA1B2C3D4")
                            start = "vram_start"
                            size = "vram_16_size"
                            timeout_ms = 7500
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-NoMockFallback"
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should pass when cemu_hash expectations are met."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.passed) -Message "Exactly one cemu_hash case should pass."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.cemu_hash.enabled) -Message "Actual cemu_hash.enabled should be true."
        Assert-Equal -Expected "addition_result" -Actual ([string]$run.summary.cases[0].actual.cemu_hash.name) -Message "Actual cemu_hash.name mismatch."
        Assert-Equal -Expected @("A1B2C3D4") -Actual @($run.summary.cases[0].actual.cemu_hash.expected_crcs) -Message "Actual cemu_hash.expected_crcs mismatch."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].actual.cemu_hash.matched) -Message "Actual cemu_hash.matched should be true."
    }

    Invoke-TestCase -Name "Runner fails cemu_hash case when harness does not run in cemu mode" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-hash-force-mock.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-hash-force-mock"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        cemu_hash = @{
                            expected_crcs = @("AABBCCDD")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when cemu_hash is requested but cemu mode is unavailable."
        Assert-Contains -Needle "requires cemu_hash verification" -Haystack $run.output_text -Message "Failure output should mention cemu_hash verification mode requirement."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Case should be reported as failed."
    }

    Invoke-TestCase -Name "Runner rejects invalid cemu_hash CRC schema" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-hash-invalid-crc.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-hash-invalid-crc"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        cemu_hash = @{
                            expected_crcs = @("NOT_HEX")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail invalid cemu_hash CRC schema."
        Assert-Contains -Needle "contains invalid CRC" -Haystack $run.output_text -Message "Failure output should explain CRC format requirements."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Invalid schema case should fail."
    }

    Invoke-TestCase -Name "Runner requires baseline capture mode when placeholder CRC is present" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-hash-placeholder.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-hash-placeholder"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        cemu_hash = @{
                            expected_crcs = @("__BASELINE_REQUIRED__")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail placeholder CRC cases when baseline capture mode is disabled."
        Assert-Contains -Needle "Re-run with -CaptureCEmuHashBaselines" -Haystack $run.output_text -Message "Failure output should explain how to capture strict hash baselines."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Placeholder case should fail without capture mode."
    }

    Invoke-TestCase -Name "Runner captures baseline CRC and writes updated case file" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $updatedCasePath = Join-Path $tempDir "cases-cemu-baselined.json"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript `
            -Directory $tempDir `
            -ExitCode 1 `
            -Label "cemu-baseline-capture" `
            -HashFailureName "baseline_add" `
            -HashFailureCrc "00abcdef"

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-baseline-capture.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-baseline-capture"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        mode = "cemu"
                        harness_success = $true
                        cemu_hash = @{
                            name = "baseline_add"
                            expected_crcs = @("__BASELINE_REQUIRED__")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-NoMockFallback",
            "-CaptureCEmuHashBaselines",
            "-BaselineOutputCaseFile", $updatedCasePath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should pass when baseline capture mode records a CRC."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.baseline_capture.updated_case_count) -Message "Exactly one case should be updated from baseline capture."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.baseline_capture.captured_case_count) -Message "Exactly one case should report captured baseline CRC."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.baseline_capture.case_file_written) -Message "Runner should write an updated case file in baseline capture mode."
        Assert-Equal -Expected @("00ABCDEF") -Actual @($run.summary.cases[0].actual.cemu_hash.observed_crcs) -Message "Captured observed CRC should be reflected in summary."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].baseline_capture.captured) -Message "Case summary should mark baseline capture as captured."
        Assert-Equal -Expected "00ABCDEF" -Actual ([string]$run.summary.cases[0].baseline_capture.captured_crc) -Message "Captured CRC should be normalized in case summary."
        Assert-True -Condition (Test-Path -LiteralPath $updatedCasePath -PathType Leaf) -Message "Updated case file should be written."

        $script:CreatedFiles.Add($updatedCasePath)
        $updatedCaseJson = Get-Content -LiteralPath $updatedCasePath -Raw | ConvertFrom-Json
        Assert-Equal -Expected @("00ABCDEF") -Actual @($updatedCaseJson.cases[0].expect.cemu_hash.expected_crcs) -Message "Updated case file should replace placeholder with captured CRC."
    }

    Invoke-TestCase -Name "Runner baseline capture supports multi-attempt CRC consensus" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $updatedCasePath = Join-Path $tempDir "cases-cemu-baseline-consensus.json"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript `
            -Directory $tempDir `
            -ExitCode 1 `
            -Label "cemu-baseline-consensus" `
            -HashFailureName "baseline_consensus" `
            -HashFailureCrcSequence @("00aaa111", "00bbb222", "00aaa111")

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-baseline-consensus-input.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-baseline-consensus"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        mode = "cemu"
                        cemu_hash = @{
                            name = "baseline_consensus"
                            expected_crcs = @("__BASELINE_REQUIRED__")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-NoMockFallback",
            "-CaptureCEmuHashBaselines",
            "-BaselineCaptureSampleCount", "3",
            "-BaselineCaptureConsensus", "2",
            "-BaselineOutputCaseFile", $updatedCasePath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should pass when baseline capture reaches CRC consensus."
        Assert-Equal -Expected 3 -Actual ([int]$run.summary.baseline_capture.sample_count) -Message "Summary should include configured baseline sample count."
        Assert-Equal -Expected 2 -Actual ([int]$run.summary.baseline_capture.consensus_required) -Message "Summary should include configured baseline consensus requirement."
        Assert-Equal -Expected 3 -Actual ([int]$run.summary.cases[0].baseline_capture.attempts_executed) -Message "Case should execute all baseline capture sample attempts."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].baseline_capture.consensus_reached) -Message "Case should report successful baseline consensus."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.cases[0].baseline_capture.captured) -Message "Case should report captured baseline CRC after consensus."
        Assert-Equal -Expected "00AAA111" -Actual ([string]$run.summary.cases[0].baseline_capture.captured_crc) -Message "Consensus-selected baseline CRC should be normalized."
        Assert-Equal -Expected @("00AAA111", "00BBB222", "00AAA111") -Actual @($run.summary.cases[0].baseline_capture.observed_crc_samples) -Message "Observed baseline samples should retain capture order."
        Assert-Equal -Expected 2 -Actual ([int]$run.summary.cases[0].baseline_capture.observed_crc_counts[0].count) -Message "Top CRC frequency should reflect consensus count."
        Assert-Equal -Expected "00AAA111" -Actual ([string]$run.summary.cases[0].baseline_capture.observed_crc_counts[0].crc) -Message "Top CRC frequency entry should match consensus CRC."
        Assert-True -Condition (Test-Path -LiteralPath $updatedCasePath -PathType Leaf) -Message "Consensus baseline capture should write updated case file."

        $script:CreatedFiles.Add($updatedCasePath)
        $updatedCaseJson = Get-Content -LiteralPath $updatedCasePath -Raw | ConvertFrom-Json
        Assert-Equal -Expected @("00AAA111") -Actual @($updatedCaseJson.cases[0].expect.cemu_hash.expected_crcs) -Message "Updated case file should use consensus CRC."
    }

    Invoke-TestCase -Name "Runner fails baseline capture when CRC consensus is not reached" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript `
            -Directory $tempDir `
            -ExitCode 1 `
            -Label "cemu-baseline-no-consensus" `
            -HashFailureName "baseline_no_consensus" `
            -HashFailureCrcSequence @("11111111", "22222222", "33333333")

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-baseline-no-consensus.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-baseline-no-consensus"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        mode = "cemu"
                        cemu_hash = @{
                            name = "baseline_no_consensus"
                            expected_crcs = @("__BASELINE_REQUIRED__")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-NoMockFallback",
            "-CaptureCEmuHashBaselines",
            "-BaselineCaptureSampleCount", "3",
            "-BaselineCaptureConsensus", "2"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when baseline CRC samples do not reach consensus."
        Assert-Contains -Needle "did not reach consensus >= 2" -Haystack $run.output_text -Message "Failure output should include CRC consensus diagnostics."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].baseline_capture.consensus_reached) -Message "Case should report consensus_reached=false."
        Assert-Equal -Expected $false -Actual ([bool]$run.summary.cases[0].baseline_capture.captured) -Message "Case should not mark baseline as captured when consensus fails."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.baseline_capture.updated_case_count) -Message "No baseline cases should be updated on consensus failure."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Consensus failure should fail the case."
    }

    Invoke-TestCase -Name "Runner rejects baseline consensus higher than sample count" -Body {
        $tempDir = New-TestDirectory
        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-baseline-options-guard.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "addition"
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

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-CaptureCEmuHashBaselines",
            "-BaselineCaptureSampleCount", "2",
            "-BaselineCaptureConsensus", "3"
        )

        Assert-Equal -Expected 2 -Actual ([int]$run.exit_code) -Message "Runner should fail fast with exit code 2 when consensus exceeds sample count."
        Assert-Contains -Needle "cannot be greater than BaselineCaptureSampleCount" -Haystack $run.output_text -Message "Runner should explain invalid baseline consensus configuration."
    }

    Invoke-TestCase -Name "Runner fails baseline capture when observed CRC is unavailable" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 1 -Label "cemu-baseline-no-observed"

        $caseFile = New-JsonCaseFile -Directory $tempDir -Name "cases-cemu-baseline-no-observed.json" -Payload @{
            schema_version = 1
            cases = @(
                @{
                    name = "cemu-baseline-no-observed"
                    keys = "2,+,3,enter"
                    expect = @{
                        result = "5"
                        status = "ok"
                        error = ""
                        ignored_keys = @()
                        mode = "cemu"
                        cemu_hash = @{
                            expected_crcs = @("__BASELINE_REQUIRED__")
                        }
                    }
                }
            )
        }

        $run = Invoke-RegressionRunner -CaseFile $caseFile -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-NoMockFallback",
            "-CaptureCEmuHashBaselines"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when baseline capture mode cannot parse an observed CRC."
        Assert-Contains -Needle "did not report any observed CRC values" -Haystack $run.output_text -Message "Failure output should explain baseline capture parsing failure."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.baseline_capture.updated_case_count) -Message "No cases should be updated when capture fails."
        Assert-Equal -Expected 1 -Actual ([int]$run.summary.totals.failed) -Message "Case should be reported as failed when baseline capture is incomplete."
    }

    Invoke-TestCase -Name "Canonical pass2 case file executes successfully in force-mock mode" -Body {
        $canonicalCaseFile = Join-Path $script:RepoRoot "tests\emulator\terminal-regression.pass2.cases.json"
        Assert-True -Condition (Test-Path -LiteralPath $canonicalCaseFile -PathType Leaf) -Message "Canonical case file is missing."

        $run = Invoke-RegressionRunner -CaseFile $canonicalCaseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Canonical pass2 case file should pass in mock mode."
        Assert-Equal -Expected 5 -Actual ([int]$run.summary.totals.total) -Message "Canonical case count mismatch."
        Assert-Equal -Expected 5 -Actual ([int]$run.summary.totals.passed) -Message "Canonical cases should all pass in mock mode."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.totals.failed) -Message "Canonical file should have no failed cases."
    }

    Invoke-TestCase -Name "Canonical pass6 comprehensive case file executes all 36 cases in force-mock mode" -Body {
        $canonicalCaseFile = Join-Path $script:RepoRoot "tests\emulator\terminal-regression.pass6.cases.json"
        Assert-True -Condition (Test-Path -LiteralPath $canonicalCaseFile -PathType Leaf) -Message "Canonical pass6 case file is missing."

        $run = Invoke-RegressionRunner -CaseFile $canonicalCaseFile -AdditionalArgs @("-ForceMock")
        if ([string]$run.summary.options.module_program_paths_source -eq "none") {
            Write-Host "[SKIP] Canonical pass6 official split-suite validation requires prebuilt split-suite artifacts under bin\\."
            return
        }

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Canonical pass6 case file should pass in mock mode."
        Assert-Equal -Expected 36 -Actual ([int]$run.summary.totals.total) -Message "Pass6 should contain exactly 36 cases."
        Assert-Equal -Expected 36 -Actual ([int]$run.summary.totals.passed) -Message "All 36 pass6 cases should pass in mock mode."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.totals.failed) -Message "Pass6 should have no failed cases."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.success) -Message "Pass6 summary success should be true."
        Assert-Equal -Expected "official_split_suite" -Actual ([string]$run.summary.options.module_program_paths_source) -Message "Pass6 should validate the official split-suite artifact set."
        Assert-Equal -Expected 34 -Actual ([int]$run.summary.options.module_program_paths.Count) -Message "Pass6 should validate all 34 non-launcher split-suite artifacts."
        Assert-Equal -Expected 0 -Actual ([int](@($run.summary.options.module_program_paths | Where-Object { $_ -like "*\\bin\\_exp_*" }).Count)) -Message "Pass6 auto-discovery should ignore experimental bin artifacts."
        Assert-Contains -Needle "bin\algsolve_adv\CEASLA.8xp" -Haystack (@($run.summary.options.module_program_paths) -join "|") -Message "Pass6 official artifact set should include CEASLA."
        Assert-Contains -Needle "bin\calc_lim_adv\CECLIA.8xp" -Haystack (@($run.summary.options.module_program_paths) -join "|") -Message "Pass6 official artifact set should include CECLIA."

        $caseNames = @($run.summary.cases | ForEach-Object { $_.name })
        Assert-Contains -Needle "addition: 2+3" -Haystack ($caseNames -join "|") -Message "Pass6 should include basic addition case."
        Assert-Contains -Needle "precedence: 2+3*4" -Haystack ($caseNames -join "|") -Message "Pass6 should include operator precedence case."
        Assert-Contains -Needle "nested parens" -Haystack ($caseNames -join "|") -Message "Pass6 should include nested parentheses case."
        Assert-Contains -Needle "division by zero" -Haystack ($caseNames -join "|") -Message "Pass6 should include error condition case."
        Assert-Contains -Needle "ignored keys" -Haystack ($caseNames -join "|") -Message "Pass6 should include ignored-keys case."
        Assert-Contains -Needle "pending: no enter" -Haystack ($caseNames -join "|") -Message "Pass6 should include pending-state case."
        Assert-Contains -Needle "alias normalization" -Haystack ($caseNames -join "|") -Message "Pass6 should include alias normalization case."
        Assert-Contains -Needle "complex expression" -Haystack ($caseNames -join "|") -Message "Pass6 should include complex multi-operator case."

        $errorCase = $run.summary.cases | Where-Object { $_.name -like "*division by zero*" }
        Assert-Equal -Expected "error" -Actual ([string]$errorCase.actual.status) -Message "Division-by-zero case should return error status."
        Assert-Equal -Expected "" -Actual ([string]$errorCase.actual.result) -Message "Division-by-zero case should return empty result."

        $pendingCase = $run.summary.cases | Where-Object { $_.name -like "*no enter pressed*" }
        Assert-Equal -Expected "pending" -Actual ([string]$pendingCase.actual.status) -Message "No-enter case should return pending status."
    }

    Invoke-TestCase -Name "Canonical pass7 edge-case file executes unary precedence and overflow diagnostics in force-mock mode" -Body {
        $canonicalCaseFile = Join-Path $script:RepoRoot "tests\emulator\terminal-regression.pass7.cases.json"
        Assert-True -Condition (Test-Path -LiteralPath $canonicalCaseFile -PathType Leaf) -Message "Canonical pass7 case file is missing."

        $run = Invoke-RegressionRunner -CaseFile $canonicalCaseFile -AdditionalArgs @("-ForceMock")

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Canonical pass7 case file should pass in mock mode."
        Assert-Equal -Expected 11 -Actual ([int]$run.summary.totals.total) -Message "Pass7 should contain exactly 11 cases."
        Assert-Equal -Expected 11 -Actual ([int]$run.summary.totals.passed) -Message "All pass7 cases should pass in mock mode."
        Assert-Equal -Expected 0 -Actual ([int]$run.summary.totals.failed) -Message "Pass7 should have no failed cases."
        Assert-Equal -Expected $true -Actual ([bool]$run.summary.success) -Message "Pass7 summary success should be true."

        $unaryPrecedenceCase = $run.summary.cases | Where-Object { $_.name -like "unary precedence*" }
        Assert-Equal -Expected "-4" -Actual ([string]$unaryPrecedenceCase.actual.result) -Message "Unary precedence case should return -4."

        $negativeExponentCase = $run.summary.cases | Where-Object { $_.name -like "negative exponent*" }
        Assert-Equal -Expected "0.125" -Actual ([string]$negativeExponentCase.actual.result) -Message "Negative exponent case should return 0.125."

        $overflowCase = $run.summary.cases | Where-Object { $_.name -like "non-finite overflow*" }
        Assert-Equal -Expected "error" -Actual ([string]$overflowCase.actual.status) -Message "Overflow case should return error status."
        Assert-Contains -Needle "non-finite" -Haystack ([string]$overflowCase.actual.error) -Message "Overflow case should report non-finite error."
    }
}
finally {
    foreach ($filePath in $script:CreatedFiles) {
        if (Test-Path -LiteralPath $filePath -PathType Leaf) {
            Remove-Item -LiteralPath $filePath -Force -ErrorAction SilentlyContinue
        }
    }

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
