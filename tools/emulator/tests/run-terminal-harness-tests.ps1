Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$modulePath = Join-Path (Split-Path -Path $PSScriptRoot -Parent) "CalcEmuTerminalHarness.psm1"
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    Write-Error "Harness module was not found at '$modulePath'."
    exit 2
}

$artifactModulePath = Join-Path (Split-Path -Path $PSScriptRoot -Parent) "CalcEmuSplitSuiteArtifacts.psm1"
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    Write-Error "Artifact module was not found at '$artifactModulePath'."
    exit 2
}

$script:HarnessScriptPath = Join-Path (Split-Path -Path $PSScriptRoot -Parent) "run-terminal-harness.ps1"
if (-not (Test-Path -LiteralPath $script:HarnessScriptPath -PathType Leaf)) {
    Write-Error "Harness script was not found at '$script:HarnessScriptPath'."
    exit 2
}

Import-Module -Name $modulePath -Force
Import-Module -Name $artifactModulePath -Force

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

function Test-ManifestArtifactsUnavailable {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ManifestArtifacts
    )

    $errors = @($ManifestArtifacts.errors)
    return ($errors.Count -gt 0) -and (@($errors | Where-Object {
        ([string]$_).IndexOf("Could not resolve manifest module", [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    }).Count -eq $errors.Count)
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("calcemu-harness-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
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
        [string]$HashFailureCrc = ""
    )

    $scriptPath = Join-Path $Directory ("fake-autotester-{0}.ps1" -f $Label)
    $scriptContent = @(
        "param([string]`$ConfigPath)",
        "`$ErrorActionPreference = `"Stop`"",
        "if ([string]::IsNullOrWhiteSpace(`$ConfigPath)) {",
        "    Write-Output `"fake-$Label missing-config`"",
        "    exit 31",
        "}",
        "if (-not (Test-Path -LiteralPath `$ConfigPath -PathType Leaf)) {",
        "    Write-Output `"fake-$Label config-not-found`"",
        "    exit 32",
        "}",
        "Write-Output (`"fake-$Label config=`" + `$ConfigPath)"
    )

    if (-not [string]::IsNullOrWhiteSpace($HashFailureName) -and -not [string]::IsNullOrWhiteSpace($HashFailureCrc)) {
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

function Invoke-HarnessScript {
    param(
        [Parameter(Mandatory = $false)]
        [string[]]$AdditionalArgs = @()
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required for harness script tests."
    }

    $powershellPath = $powershellCommand.Source
    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:HarnessScriptPath,
        "-Keys",
        "2,+,3,enter",
        "-Format",
        "json"
    )

    foreach ($argument in @($AdditionalArgs)) {
        $arguments += [string]$argument
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

    $outputText = ($outputLines.ToArray() -join "`n")
    $parsedJson = $null
    if (-not [string]::IsNullOrWhiteSpace($outputText)) {
        $parsedJson = $outputText | ConvertFrom-Json
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = $outputText
        json = $parsedJson
    }
}

try {
    Invoke-TestCase -Name "Normalizes simple key aliases" -Body {
        $normalized = ConvertTo-CalcEmuKeySequence -KeySequence "2,+,3,enter"
        Assert-Equal -Expected @("KEY_2", "KEY_ADD", "KEY_3", "KEY_ENTER") -Actual $normalized -Message "Normalized keys did not match expected values."
    }

    Invoke-TestCase -Name "Normalizes canonical and mixed-case tokens" -Body {
        $normalized = ConvertTo-CalcEmuKeySequence -KeySequence " key_2 , Add , KEY_3 , Return "
        Assert-Equal -Expected @("KEY_2", "KEY_ADD", "KEY_3", "KEY_ENTER") -Actual $normalized -Message "Mixed-case normalization failed."
    }

    Invoke-TestCase -Name "Rejects unknown tokens with actionable error" -Body {
        $capturedError = ""
        try {
            [void](ConvertTo-CalcEmuKeySequence -KeySequence "2,banana,3")
            throw "Expected parse failure but parser returned successfully."
        }
        catch {
            $capturedError = $_.Exception.Message
        }

        Assert-Contains -Needle "Unknown key token 'banana'" -Haystack $capturedError -Message "Unknown key error message was not descriptive."
    }

    Invoke-TestCase -Name "Rejects unknown canonical KEY tokens with actionable error" -Body {
        $capturedError = ""
        try {
            [void](ConvertTo-CalcEmuKeySequence -KeySequence "KEY_FAKE,2,+,3,ENTER")
            throw "Expected parse failure for unknown canonical key token, but parser returned successfully."
        }
        catch {
            $capturedError = $_.Exception.Message
        }

        Assert-Contains -Needle "Unknown key token 'KEY_FAKE'" -Haystack $capturedError -Message "Unknown canonical key error message was not descriptive."
    }

    Invoke-TestCase -Name "Mock screen evaluates arithmetic with enter" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "2,+,3,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "2+3" -Actual $screen.expression -Message "Expression buffer mismatch."
        Assert-Equal -Expected "ok" -Actual $screen.status -Message "Status should be ok after valid evaluation."
        Assert-Equal -Expected "5" -Actual $screen.result -Message "Result mismatch for 2+3."
    }

    Invoke-TestCase -Name "Mock screen handles precedence and parentheses" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "(,4,+,5,),*,3,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "(4+5)*3" -Actual $screen.expression -Message "Expression with parentheses mismatch."
        Assert-Equal -Expected "27" -Actual $screen.result -Message "Result mismatch for (4+5)*3."
    }

    Invoke-TestCase -Name "Mock screen enforces exponent precedence over unary negation" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "neg,2,^,2,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "-2^2" -Actual $screen.expression -Message "Expression mismatch for unary-exponent precedence case."
        Assert-Equal -Expected "-4" -Actual $screen.result -Message "Unary negation should apply after exponentiation for -2^2."
    }

    Invoke-TestCase -Name "Mock screen keeps explicit parenthesized negative base semantics" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "(,neg,2,),^,2,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "(-2)^2" -Actual $screen.expression -Message "Expression mismatch for explicit parenthesized negative base."
        Assert-Equal -Expected "4" -Actual $screen.result -Message "(-2)^2 should evaluate to 4."
    }

    Invoke-TestCase -Name "Mock screen supports negative exponents" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "2,^,neg,3,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "2^-3" -Actual $screen.expression -Message "Expression mismatch for negative exponent case."
        Assert-Equal -Expected "0.125" -Actual $screen.result -Message "2^-3 should evaluate to 0.125."
    }

    Invoke-TestCase -Name "Mock screen surfaces divide-by-zero errors" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "1,/,0,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "error" -Actual $screen.status -Message "Division by zero should produce error status."
        Assert-Contains -Needle "Division by zero" -Haystack $screen.error -Message "Division by zero error should be explicit."
    }

    Invoke-TestCase -Name "Mock screen surfaces non-finite overflow errors" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "9,^,9,^,9,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "error" -Actual $screen.status -Message "Overflowing exponentiation should produce error status."
        Assert-Contains -Needle "non-finite" -Haystack $screen.error -Message "Overflowing exponentiation should report a non-finite evaluation error."
    }

    Invoke-TestCase -Name "Editing after evaluation error clears stale screen error state" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "1,/,0,enter,1"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "1/01" -Actual $screen.expression -Message "Expression should keep edited text after typing post-error."
        Assert-Equal -Expected "pending" -Actual $screen.status -Message "Typing after an evaluation error should return the screen to pending edit mode."
        Assert-Equal -Expected "" -Actual $screen.result -Message "Pending edit mode should clear stale result text."
        Assert-Equal -Expected "" -Actual $screen.error -Message "Pending edit mode should clear stale error text."
    }

    Invoke-TestCase -Name "Mock screen rejects malformed decimal tokens" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "1,.,.,2,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "error" -Actual $screen.status -Message "Malformed decimal should produce error status."
        Assert-Contains -Needle "Invalid number" -Haystack $screen.error -Message "Malformed decimal error should mention invalid number parsing."
    }

    Invoke-TestCase -Name "Mock screen reports mismatched parentheses" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "(,1,+,2,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "error" -Actual $screen.status -Message "Mismatched parentheses should produce error status."
        Assert-Contains -Needle "Mismatched parenthesis" -Haystack $screen.error -Message "Mismatched parenthesis error should be explicit."
    }

    Invoke-TestCase -Name "Clear key resets expression state" -Body {
        $keys = ConvertTo-CalcEmuKeySequence -KeySequence "2,+,3,clear,6,*,7,enter"
        $screen = Get-CalcEmuMockScreenState -NormalizedKeys $keys
        Assert-Equal -Expected "6*7" -Actual $screen.expression -Message "Clear did not reset expression buffer."
        Assert-Equal -Expected "42" -Actual $screen.result -Message "Post-clear expression result mismatch."
    }

    Invoke-TestCase -Name "Harness falls back to mock when CEmu inputs are missing" -Body {
        $result = Invoke-CalcEmuTerminalHarness -KeySequence "2,+,3,enter" -RomPath "" -AutotesterPath "" -ProgramPath ""
        Assert-Equal -Expected "mock" -Actual $result.mode -Message "Harness should use mock mode when CEmu inputs are unavailable."
        Assert-True -Condition $result.success -Message "Mock fallback should return success."
        Assert-True -Condition $result.fallback.used -Message "Fallback flag should be set."
        Assert-True -Condition ($result.fallback.reasons.Count -gt 0) -Message "Fallback reasons should be included."
    }

    Invoke-TestCase -Name "Harness reports failure when fallback is disabled" -Body {
        $result = Invoke-CalcEmuTerminalHarness -KeySequence "2,+,3,enter" -RomPath "" -AutotesterPath "" -ProgramPath "" -NoMockFallback
        Assert-Equal -Expected "error" -Actual $result.mode -Message "Mode should be error when fallback is disabled."
        Assert-True -Condition (-not $result.success) -Message "Result should be unsuccessful when fallback is disabled."
        Assert-True -Condition ($result.errors.Count -gt 0) -Message "Errors should describe missing CEmu requirements."
    }

    Invoke-TestCase -Name "Harness executes existing wrapper path in cemu mode with fake autotester success" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 0 -Label "success"

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath $romPath `
            -AutotesterPath $autotesterPath `
            -ProgramPath $programPath

        Assert-Equal -Expected "cemu" -Actual $result.mode -Message "Mode should be cemu when dependencies are available."
        Assert-True -Condition $result.autotest.attempted -Message "Autotest should have been attempted."
        Assert-Equal -Expected 0 -Actual ([int]$result.autotest.exit_code) -Message "Autotest exit code should be 0."
        Assert-True -Condition $result.success -Message "Success should be true when fake autotester exits 0."
        Assert-True -Condition (Test-Path -LiteralPath $result.autotest.generated_config_path -PathType Leaf) -Message "Generated autotest config path should exist."
    }

    Invoke-TestCase -Name "Harness propagates non-zero autotester exit code in cemu mode" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 7 -Label "failure"

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath $romPath `
            -AutotesterPath $autotesterPath `
            -ProgramPath $programPath

        Assert-Equal -Expected "cemu" -Actual $result.mode -Message "Mode should remain cemu when autotester fails."
        Assert-True -Condition (-not $result.success) -Message "Result should be unsuccessful when autotester exits non-zero."
        Assert-Equal -Expected 7 -Actual ([int]$result.autotest.exit_code) -Message "Autotester exit code should be propagated."
        Assert-True -Condition ($result.errors.Count -gt 0) -Message "Errors should include autotest failure details."
    }

    Invoke-TestCase -Name "Harness enables CEmu hash assertion and injects hash config" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 0 -Label "hash-enabled"

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath $romPath `
            -AutotesterPath $autotesterPath `
            -ProgramPath $programPath `
            -ExpectedVramCrcs @("0x1a2b3c4d", "1A2B3C4D", "DEADBEEF") `
            -CEmuHashName "result_hash" `
            -CEmuHashTimeoutMs 9000

        Assert-Equal -Expected "cemu" -Actual $result.mode -Message "Mode should be cemu when hash assertions are requested and dependencies are available."
        Assert-True -Condition $result.autotest.hash_assertion.enabled -Message "Hash assertion should be enabled."
        Assert-Equal -Expected @("1A2B3C4D", "DEADBEEF") -Actual $result.autotest.hash_assertion.expected_crcs -Message "CRC list should be normalized and deduplicated."
        Assert-Equal -Expected "result_hash" -Actual $result.autotest.hash_assertion.name -Message "Hash name mismatch."
        Assert-Equal -Expected 9000 -Actual ([int]$result.autotest.hash_assertion.timeout_ms) -Message "Hash timeout mismatch."
        Assert-True -Condition $result.autotest.hash_assertion.matched -Message "Hash assertion should report matched when autotester exits 0."

        Assert-True -Condition (Test-Path -LiteralPath $result.autotest.generated_config_path -PathType Leaf) -Message "Generated autotest config path should exist."
        $generatedConfig = Get-Content -LiteralPath $result.autotest.generated_config_path -Raw | ConvertFrom-Json
        Assert-True -Condition ($generatedConfig.sequence -contains "hash|result_hash") -Message "Generated autotest sequence must include hash command."
        Assert-Equal -Expected "vram_start" -Actual ([string]$generatedConfig.hashes.result_hash.start) -Message "Generated hash start mismatch."
        Assert-Equal -Expected "vram_16_size" -Actual ([string]$generatedConfig.hashes.result_hash.size) -Message "Generated hash size mismatch."
        Assert-Equal -Expected 9000 -Actual ([int]$generatedConfig.hashes.result_hash.timeout) -Message "Generated hash timeout mismatch."
        Assert-Equal -Expected @("1A2B3C4D", "DEADBEEF") -Actual @($generatedConfig.hashes.result_hash.expected_CRCs) -Message "Generated expected CRCs mismatch."
    }

    Invoke-TestCase -Name "Harness reports hash assertion mismatch when autotester exits non-zero" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript `
            -Directory $tempDir `
            -ExitCode 9 `
            -Label "hash-fail" `
            -HashFailureName "terminal_vram" `
            -HashFailureCrc "1a2b3c4d"

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath $romPath `
            -AutotesterPath $autotesterPath `
            -ProgramPath $programPath `
            -ExpectedVramCrcs @("ABCDEF12")

        Assert-Equal -Expected "cemu" -Actual $result.mode -Message "Mode should remain cemu when hash assertion fails."
        Assert-True -Condition (-not $result.success) -Message "Harness should fail when CEmu hash assertion fails."
        Assert-True -Condition ($result.autotest.hash_assertion.enabled) -Message "Hash assertion should remain enabled."
        Assert-Equal -Expected $false -Actual ([bool]$result.autotest.hash_assertion.matched) -Message "Hash assertion matched flag should be false when autotester exits non-zero."
        Assert-Equal -Expected @("1A2B3C4D") -Actual @($result.autotest.hash_assertion.observed_crcs) -Message "Observed CRC list should include parsed autotester mismatch CRC."
        Assert-Equal -Expected "1A2B3C4D" -Actual ([string]$result.autotest.hash_assertion.last_observed_crc) -Message "Last observed CRC should match parsed value."
    }

    Invoke-TestCase -Name "Harness emits explicit diagnostics when hash mismatch CRC is not parseable" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 9 -Label "hash-fail-no-observed"

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath $romPath `
            -AutotesterPath $autotesterPath `
            -ProgramPath $programPath `
            -ExpectedVramCrcs @("ABCDEF12")

        Assert-Equal -Expected "cemu" -Actual $result.mode -Message "Mode should remain cemu when hash assertion fails without parseable CRC."
        Assert-True -Condition (-not $result.success) -Message "Harness should fail when CEmu hash assertion fails."
        Assert-Equal -Expected 0 -Actual ([int]$result.autotest.hash_assertion.observed_crcs.Count) -Message "Observed CRC list should be empty when autotester output lacks parseable CRC data."
        Assert-Contains -Needle "no observed CRC could be parsed" -Haystack ($result.errors -join " | ") -Message "Failure diagnostics should include unparseable CRC guidance."
    }

    Invoke-TestCase -Name "Harness fails when hash assertion is requested but cemu mode is unavailable" -Body {
        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath "" `
            -AutotesterPath "" `
            -ProgramPath "" `
            -ExpectedVramCrcs @("AABBCCDD")

        Assert-Equal -Expected "mock" -Actual $result.mode -Message "Mode should remain mock without CEmu inputs."
        Assert-True -Condition (-not $result.success) -Message "Harness should fail when hash assertion is requested but cemu mode did not run."
        Assert-True -Condition ($result.errors.Count -gt 0) -Message "Errors should explain hash assertion requirements."
        Assert-Contains -Needle "hash assertion was requested" -Haystack ($result.errors -join " | ") -Message "Hash assertion failure reason should be explicit."
    }

    Invoke-TestCase -Name "Harness rejects invalid hash CRC input" -Body {
        $capturedError = ""
        try {
            [void](Invoke-CalcEmuTerminalHarness -KeySequence "2,+,3,enter" -ExpectedVramCrcs @("invalid-crc"))
            throw "Expected CRC validation failure but invocation succeeded."
        }
        catch {
            $capturedError = $_.Exception.Message
        }

        Assert-Contains -Needle "Invalid CRC value" -Haystack $capturedError -Message "CRC validation error message should be explicit."
    }

    Invoke-TestCase -Name "ForceMock overrides available CEmu configuration" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        Set-Content -LiteralPath $romPath -Value "rom-bytes" -Encoding UTF8
        Set-Content -LiteralPath $programPath -Value "program-bytes" -Encoding UTF8
        $autotesterPath = New-FakeAutotesterScript -Directory $tempDir -ExitCode 0 -Label "forcemock"

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -RomPath $romPath `
            -AutotesterPath $autotesterPath `
            -ProgramPath $programPath `
            -ForceMock

        Assert-Equal -Expected "mock" -Actual $result.mode -Message "ForceMock should force mock mode."
        Assert-True -Condition $result.success -Message "Forced mock mode should succeed."
        Assert-True -Condition ($result.fallback.reasons.Count -gt 0) -Message "Fallback reasons should include ForceMock."
        Assert-True -Condition (-not $result.autotest.attempted) -Message "Autotest should not run in ForceMock mode."
    }

    Invoke-TestCase -Name "Harness enforces usable user RAM limit with Memory insufficient error" -Body {
        $tempDir = New-TestDirectory
        $oversizedProgramPath = Join-Path $tempDir "OVERSIZE.8xp"
        New-BinaryFile -Path $oversizedProgramPath -SizeBytes 70000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ProgramPath $oversizedProgramPath

        Assert-Equal -Expected $false -Actual ([bool]$result.success) -Message "Harness should fail when program exceeds usable user RAM limit."
        Assert-Equal -Expected $true -Actual ([bool]$result.hardware_constraints.validated) -Message "Hardware constraints should be marked as validated."
        Assert-Equal -Expected $true -Actual ([bool]$result.hardware_constraints.memory_insufficient) -Message "Memory-insufficient flag should be set for oversized program."
        Assert-Contains -Needle "Memory is insufficient to perform the calculation." -Haystack ($result.errors -join " | ") -Message "Failure should include hardware-faithful memory-insufficient text."
    }

    Invoke-TestCase -Name "Harness enforces per-module linked binary size limit" -Body {
        $tempDir = New-TestDirectory
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $oversizedModulePath = Join-Path $tempDir "CEBIG.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $oversizedModulePath -SizeBytes 53000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ProgramPath $programPath `
            -ModuleProgramPaths @($oversizedModulePath)

        Assert-Equal -Expected $false -Actual ([bool]$result.success) -Message "Harness should fail when any module exceeds linked binary limit."
        Assert-Equal -Expected $true -Actual ([bool]$result.hardware_constraints.module_size_exceeded) -Message "Module-size-exceeded flag should be set."
        Assert-Contains -Needle "Module linked binary exceeds limit" -Haystack ($result.errors -join " | ") -Message "Failure should include actionable module size guidance."
    }

    Invoke-TestCase -Name "Harness accepts multi-program module set within limits" -Body {
        $tempDir = New-TestDirectory
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $moduleAPath -SizeBytes 48000
        New-BinaryFile -Path $moduleBPath -SizeBytes 50000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ProgramPath $programPath `
            -ModuleProgramPaths @($moduleAPath, $moduleBPath)

        Assert-Equal -Expected $true -Actual ([bool]$result.success) -Message "Harness should pass when program and modules satisfy hardware limits."
        Assert-Equal -Expected $true -Actual ([bool]$result.hardware_constraints.validated) -Message "Hardware constraints should report validated state."
        Assert-Equal -Expected $true -Actual ([bool]$result.hardware_constraints.pass) -Message "Hardware constraints should pass for compliant modules."
        Assert-Equal -Expected 2 -Actual ([int]$result.hardware_constraints.modules.checked_count) -Message "Both modules should be validated."
    }

    Invoke-TestCase -Name "Harness script expands semicolon-delimited module paths for validation and archive swap" -Body {
        $tempDir = New-TestDirectory
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        New-BinaryFile -Path $programPath -SizeBytes 4096
        New-BinaryFile -Path $moduleAPath -SizeBytes 12000
        New-BinaryFile -Path $moduleBPath -SizeBytes 18000

        $run = Invoke-HarnessScript -AdditionalArgs @(
            "-ForceMock",
            "-ProgramPath", $programPath,
            "-ModuleProgramPaths", ("{0};{1}" -f $moduleAPath, $moduleBPath),
            "-ArchiveRamDispatchModulePath", $moduleAPath,
            "-ArchiveRamSwapRamCapacityBytes", "65536"
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Harness script should succeed when semicolon-delimited modules are valid."
        Assert-True -Condition ($null -ne $run.json) -Message "Harness script should emit parseable JSON."
        Assert-Equal -Expected 2 -Actual ([int]$run.json.hardware_constraints.modules.checked_count) -Message "Harness script should expand both module paths for hardware validation."
        Assert-Equal -Expected 2 -Actual ([int]$run.json.archive_ram_swap.modules.requested_count) -Message "Harness script should reuse both expanded module paths for archive swap simulation."
        Assert-Equal -Expected 2 -Actual ([int]$run.json.archive_ram_swap.modules.dispatch_events) -Message "Archive swap should dispatch both expanded module paths once."
    }

    Invoke-TestCase -Name "Manifest transfer-suite resolver prefers staged hardware-ready artifacts over stale bin copies" -Body {
        $tempDir = New-TestDirectory
        $manifestDir = Join-Path $tempDir "tests\emulator"
        $stagedDir = Join-Path $tempDir "deploy\hardware-ready"
        $binDir = Join-Path $tempDir "bin\calc_lim_cont"
        [void](New-Item -ItemType Directory -Path $manifestDir -Force)
        [void](New-Item -ItemType Directory -Path $stagedDir -Force)
        [void](New-Item -ItemType Directory -Path $binDir -Force)

        $stagedProgramPath = Join-Path $stagedDir "CALCENG.8xp"
        $stagedModulePath = Join-Path $stagedDir "CECLIC.8xp"
        $staleProgramPath = Join-Path $binDir "CALCENG.8xp"
        $staleModulePath = Join-Path $binDir "CECLIC.8xp"
        New-BinaryFile -Path $stagedProgramPath -SizeBytes 4096
        New-BinaryFile -Path $stagedModulePath -SizeBytes 12345
        New-BinaryFile -Path $staleProgramPath -SizeBytes 2048
        New-BinaryFile -Path $staleModulePath -SizeBytes 8192

        $manifestPath = Join-Path $manifestDir "manifest.json"
        $manifestPayload = [ordered]@{
            version = "test-manifest"
            modules = @(
                [ordered]@{ name = "CALCENG.8xp"; size_bytes = 4096 },
                [ordered]@{ name = "CECLIC.8xp"; size_bytes = 12345 }
            )
        }
        $manifestPayload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

        $artifacts = Get-CalcEmuTransferManifestArtifacts -RepoRoot $tempDir -ManifestPath $manifestPath

        Assert-Equal -Expected @() -Actual @($artifacts.errors) -Message "Manifest transfer-suite resolver should not report errors when staged artifacts satisfy the manifest."
        Assert-Equal -Expected @() -Actual @($artifacts.warnings) -Message "Manifest transfer-suite resolver should not fall back to stale bin copies when exact staged matches exist."
        Assert-Equal -Expected (Resolve-Path -LiteralPath $stagedProgramPath).ProviderPath -Actual ([string]$artifacts.program_path) -Message "Program path should resolve to the staged hardware-ready launcher artifact."
        Assert-Equal -Expected @((Resolve-Path -LiteralPath $stagedModulePath).ProviderPath) -Actual @($artifacts.module_paths) -Message "Module paths should resolve to staged hardware-ready support artifacts."
    }

    Invoke-TestCase -Name "Manifest transfer-suite resolver prefers staged artifacts over equal-size bin copies without warning" -Body {
        $tempDir = New-TestDirectory
        $manifestDir = Join-Path $tempDir "tests\emulator"
        $stagedDir = Join-Path $tempDir "deploy\hardware-ready"
        $binDir = Join-Path $tempDir "bin\calc_lim_cont"
        [void](New-Item -ItemType Directory -Path $manifestDir -Force)
        [void](New-Item -ItemType Directory -Path $stagedDir -Force)
        [void](New-Item -ItemType Directory -Path $binDir -Force)

        $stagedProgramPath = Join-Path $stagedDir "CALCENG.8xp"
        $stagedModulePath = Join-Path $stagedDir "CECLIC.8xp"
        New-BinaryFile -Path $stagedProgramPath -SizeBytes 4096
        New-BinaryFile -Path $stagedModulePath -SizeBytes 12345
        New-BinaryFile -Path (Join-Path $binDir "CALCENG.8xp") -SizeBytes 4096
        New-BinaryFile -Path (Join-Path $binDir "CECLIC.8xp") -SizeBytes 12345

        $manifestPath = Join-Path $manifestDir "manifest.json"
        $manifestPayload = [ordered]@{
            version = "test-manifest"
            modules = @(
                [ordered]@{ name = "CALCENG.8xp"; size_bytes = 4096 },
                [ordered]@{ name = "CECLIC.8xp"; size_bytes = 12345 }
            )
        }
        $manifestPayload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

        $artifacts = Get-CalcEmuTransferManifestArtifacts -RepoRoot $tempDir -ManifestPath $manifestPath

        Assert-Equal -Expected @() -Actual @($artifacts.errors) -Message "Resolver should not report errors when staged and bin copies both match the manifest."
        Assert-Equal -Expected @() -Actual @($artifacts.warnings) -Message "Staged priority over an equal-size bin copy is intended and must not warn (warnings break JSON stdout callers)."
        Assert-Equal -Expected (Resolve-Path -LiteralPath $stagedProgramPath).ProviderPath -Actual ([string]$artifacts.program_path) -Message "Program path should resolve to the staged launcher when bin holds an equal-size copy."
        Assert-Equal -Expected @((Resolve-Path -LiteralPath $stagedModulePath).ProviderPath) -Actual @($artifacts.module_paths) -Message "Module paths should resolve to staged artifacts when bin holds equal-size copies."
    }

    Invoke-TestCase -Name "Manifest transfer-suite resolver still warns when equal-priority exact matches tie" -Body {
        $tempDir = New-TestDirectory
        $manifestDir = Join-Path $tempDir "tests\emulator"
        $stagedDir = Join-Path $tempDir "deploy\hardware-ready"
        $binDirA = Join-Path $tempDir "bin\calc_lim_cont"
        $binDirB = Join-Path $tempDir "bin\calc_lim_copy"
        foreach ($dir in @($manifestDir, $stagedDir, $binDirA, $binDirB)) {
            [void](New-Item -ItemType Directory -Path $dir -Force)
        }

        New-BinaryFile -Path (Join-Path $stagedDir "CALCENG.8xp") -SizeBytes 4096
        New-BinaryFile -Path (Join-Path $binDirA "CECLIC.8xp") -SizeBytes 12345
        New-BinaryFile -Path (Join-Path $binDirB "CECLIC.8xp") -SizeBytes 12345

        $manifestPath = Join-Path $manifestDir "manifest.json"
        $manifestPayload = [ordered]@{
            version = "test-manifest"
            modules = @(
                [ordered]@{ name = "CALCENG.8xp"; size_bytes = 4096 },
                [ordered]@{ name = "CECLIC.8xp"; size_bytes = 12345 }
            )
        }
        $manifestPayload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

        $artifacts = Get-CalcEmuTransferManifestArtifacts -RepoRoot $tempDir -ManifestPath $manifestPath

        Assert-Equal -Expected @() -Actual @($artifacts.errors) -Message "Resolver should still resolve a module when equal-priority copies tie."
        Assert-Equal -Expected 1 -Actual @($artifacts.warnings).Count -Message "Equal-priority exact matches are a real ambiguity and must warn once."
        Assert-Contains -Needle "Multiple exact matches found for 'CECLIC.8xp'" -Haystack (@($artifacts.warnings) -join " | ") -Message "Tie warning should name the ambiguous module."
    }

    Invoke-TestCase -Name "Harness script resolves manifest transfer-suite module paths for package-presence stress" -Body {
        $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
        $manifestArtifacts = Get-CalcEmuTransferManifestArtifacts -RepoRoot $repoRoot
        if (Test-ManifestArtifactsUnavailable -ManifestArtifacts $manifestArtifacts) {
            Write-Host "[SKIP] Manifest-backed package stress requires prebuilt split-suite artifacts under bin\\ or deploy\\hardware-ready."
            return
        }
        Assert-Equal -Expected @() -Actual @($manifestArtifacts.errors) -Message "Manifest-backed artifact resolution should not report errors."
        Assert-True -Condition (@($manifestArtifacts.module_paths).Count -gt 0) -Message "Manifest-backed artifact resolution should discover support modules."

        $dispatchModulePath = Get-CalcEmuLargestArtifactPath -CandidatePaths @($manifestArtifacts.module_paths)
        $run = Invoke-HarnessScript -AdditionalArgs @(
            "-ForceMock",
            "-ModuleProgramPathSource", "manifest_transfer_suite",
            "-ArchiveRamDispatchModulePath", $dispatchModulePath,
            "-ArchiveRamRapidSwapCycles", "2"
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Harness script should succeed when manifest transfer-suite modules complete rapid archive swaps."
        Assert-True -Condition ($null -ne $run.json) -Message "Harness script should emit parseable JSON for manifest transfer-suite stress runs."
        Assert-Equal -Expected @($manifestArtifacts.module_paths).Count -Actual ([int]$run.json.hardware_constraints.modules.checked_count) -Message "Harness script should validate every manifest-backed support module."
        Assert-Equal -Expected @($manifestArtifacts.module_paths).Count -Actual ([int]$run.json.archive_ram_swap.modules.requested_count) -Message "Archive swap should target every manifest-backed support module."
        Assert-Equal -Expected (@($manifestArtifacts.module_paths).Count * 2) -Actual ([int]$run.json.archive_ram_swap.modules.dispatch_events) -Message "Archive swap should dispatch each manifest-backed support module once per cycle."
        Assert-Equal -Expected (@($manifestArtifacts.module_paths).Count * 2) -Actual ([int]$run.json.archive_ram_swap.modules.return_events) -Message "Archive swap should return each manifest-backed support module once per cycle."
    }

    Invoke-TestCase -Name "Harness script expands semicolon-delimited expected VRAM CRCs" -Body {
        $run = Invoke-HarnessScript -AdditionalArgs @(
            "-ForceMock",
            "-ExpectedVramCrcs", "0x1a2b3c4d;DEADBEEF"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Harness script should fail in mock mode when CEmu hash assertions are requested."
        Assert-True -Condition ($null -ne $run.json) -Message "Harness script should still emit parseable JSON for hash assertion failures."
        Assert-Equal -Expected @("1A2B3C4D", "DEADBEEF") -Actual @($run.json.autotest.hash_assertion.expected_crcs) -Message "Harness script should expand and normalize semicolon-delimited CRC inputs."
        Assert-Contains -Needle "did not run in cemu mode" -Haystack (@($run.json.errors) -join " | ") -Message "Hash assertion failure should explain why mock mode cannot satisfy it."
    }

    Invoke-TestCase -Name "Harness simulates archive dispatch load and RAM return success" -Body {
        $tempDir = New-TestDirectory
        $modulePath = Join-Path $tempDir "CEALG.8xp"
        New-BinaryFile -Path $modulePath -SizeBytes 24000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamDispatchModulePath $modulePath `
            -ArchiveRamSwapRamCapacityBytes 65536

        Assert-Equal -Expected $true -Actual ([bool]$result.success) -Message "Harness should pass when archive dispatch and return both succeed."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.simulated) -Message "Archive swap simulation should be marked as active."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.pass) -Message "Archive swap simulation should pass."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.dispatch.loaded_to_ram) -Message "Dispatch should load module into RAM."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.return_to_archive.returned_to_archive) -Message "Exit flow should return module to archive."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.state.module_in_archive) -Message "Module should end in archive after successful return."
        Assert-Equal -Expected $false -Actual ([bool]$result.archive_ram_swap.state.module_in_ram) -Message "Module should not remain in RAM after successful return."
    }

    Invoke-TestCase -Name "Harness defaults archive swap module set from ModuleProgramPaths when dispatch path is provided" -Body {
        $tempDir = New-TestDirectory
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        New-BinaryFile -Path $moduleAPath -SizeBytes 12000
        New-BinaryFile -Path $moduleBPath -SizeBytes 18000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ModuleProgramPaths @($moduleAPath, $moduleBPath) `
            -ArchiveRamDispatchModulePath $moduleAPath `
            -ArchiveRamSwapRamCapacityBytes 65536

        Assert-Equal -Expected $true -Actual ([bool]$result.success) -Message "Harness should pass when ModuleProgramPaths are reused as archive-swap inputs."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.simulated) -Message "Archive swap should run when a dispatch path is provided."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.pass) -Message "Archive swap should pass for compliant ModuleProgramPaths."
        Assert-Equal -Expected 2 -Actual ([int]$result.archive_ram_swap.modules.requested_count) -Message "Archive swap should reuse all ModuleProgramPaths."
        Assert-Equal -Expected 2 -Actual ([int]$result.archive_ram_swap.modules.dispatch_events) -Message "Archive swap should dispatch every ModuleProgramPath once."
        Assert-Equal -Expected 2 -Actual ([int]$result.archive_ram_swap.modules.return_events) -Message "Archive swap should return every ModuleProgramPath once."
    }

    Invoke-TestCase -Name "Harness fails archive swap when dispatch cannot load module into RAM capacity" -Body {
        $tempDir = New-TestDirectory
        $modulePath = Join-Path $tempDir "CELARGE.8xp"
        New-BinaryFile -Path $modulePath -SizeBytes 70000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamDispatchModulePath $modulePath `
            -ArchiveRamSwapRamCapacityBytes 65536

        Assert-Equal -Expected $false -Actual ([bool]$result.success) -Message "Harness should fail when archive dispatch cannot fit module in swap RAM."
        Assert-Equal -Expected $false -Actual ([bool]$result.archive_ram_swap.pass) -Message "Archive swap pass flag should be false for dispatch-capacity failure."
        Assert-Equal -Expected "dispatch_load" -Actual ([string]$result.archive_ram_swap.failure_phase) -Message "Failure phase should be dispatch_load."
        Assert-Equal -Expected $false -Actual ([bool]$result.archive_ram_swap.dispatch.loaded_to_ram) -Message "Dispatch should not report loaded_to_ram when capacity fails."
        Assert-Contains -Needle "swap RAM capacity" -Haystack ($result.archive_ram_swap.errors -join " | ") -Message "Failure diagnostics should explain RAM-capacity mismatch."
    }

    Invoke-TestCase -Name "Harness fails archive swap when RAM-to-archive return is forced to fail" -Body {
        $tempDir = New-TestDirectory
        $modulePath = Join-Path $tempDir "CERET.8xp"
        New-BinaryFile -Path $modulePath -SizeBytes 16000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamDispatchModulePath $modulePath `
            -ArchiveRamFailurePhase "return_to_archive"

        Assert-Equal -Expected $false -Actual ([bool]$result.success) -Message "Harness should fail when return-to-archive fails."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.dispatch.loaded_to_ram) -Message "Dispatch should still load into RAM before return failure."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.return_to_archive.attempted) -Message "Return phase should be attempted after successful dispatch."
        Assert-Equal -Expected $false -Actual ([bool]$result.archive_ram_swap.return_to_archive.returned_to_archive) -Message "Return phase should report failure."
        Assert-Equal -Expected "return_to_archive" -Actual ([string]$result.archive_ram_swap.failure_phase) -Message "Failure phase should be return_to_archive."
        Assert-Contains -Needle "RAM->archive return failed" -Haystack ($result.archive_ram_swap.errors -join " | ") -Message "Failure diagnostics should identify return-to-archive failure."
    }

    Invoke-TestCase -Name "Harness validates rapid multi-module archive swaps without corruption" -Body {
        $tempDir = New-TestDirectory
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        $moduleCPath = Join-Path $tempDir "CESTPRB.8xp"
        New-BinaryFile -Path $moduleAPath -SizeBytes 12000
        New-BinaryFile -Path $moduleBPath -SizeBytes 18000
        New-BinaryFile -Path $moduleCPath -SizeBytes 24000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamModulePaths @($moduleAPath, $moduleBPath, $moduleCPath) `
            -ArchiveRamRapidSwapCycles 4 `
            -ArchiveRamSwapRamCapacityBytes 65536

        Assert-Equal -Expected $true -Actual ([bool]$result.success) -Message "Rapid multi-module swap simulation should pass when all modules fit and return cleanly."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.pass) -Message "Archive swap pass flag should be true for successful rapid multi-module swapping."
        Assert-Equal -Expected 3 -Actual ([int]$result.archive_ram_swap.modules.requested_count) -Message "Archive swap should report all requested modules."
        Assert-Equal -Expected 12 -Actual ([int]$result.archive_ram_swap.modules.dispatch_events) -Message "Dispatch events should equal module count multiplied by rapid swap cycles."
        Assert-Equal -Expected 12 -Actual ([int]$result.archive_ram_swap.modules.return_events) -Message "Return events should equal dispatch events for successful swaps."
        Assert-Equal -Expected $false -Actual ([bool]$result.archive_ram_swap.modules.corruption_detected) -Message "No corruption should be detected in successful rapid swaps."
        Assert-Equal -Expected $false -Actual ([bool]$result.archive_ram_swap.modules.partial_load_detected) -Message "No partial loads should be detected in successful rapid swaps."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.state.module_in_archive) -Message "All modules should end in archive after successful rapid swaps."
    }

    Invoke-TestCase -Name "Harness detects partial-load edge case and flags corruption risk" -Body {
        $tempDir = New-TestDirectory
        $modulePath = Join-Path $tempDir "CEPART.8xp"
        New-BinaryFile -Path $modulePath -SizeBytes 12000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamDispatchModulePath $modulePath `
            -ArchiveRamSimulatePartialLoad

        Assert-Equal -Expected $false -Actual ([bool]$result.success) -Message "Harness should fail when partial-load simulation is requested."
        Assert-Equal -Expected "partial_load" -Actual ([string]$result.archive_ram_swap.failure_phase) -Message "Partial-load simulation should report failure_phase=partial_load."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.modules.partial_load_detected) -Message "Partial-load edge case should be recorded."
        Assert-Equal -Expected $true -Actual ([bool]$result.archive_ram_swap.modules.corruption_detected) -Message "Partial-load edge case should trigger corruption detection."
        Assert-Contains -Needle "partial load detected" -Haystack ($result.archive_ram_swap.errors -join " | ") -Message "Partial-load diagnostics should include explicit failure reason."
    }

    Invoke-TestCase -Name "Harness detects memory-pressure edge case during archive dispatch" -Body {
        $tempDir = New-TestDirectory
        $modulePath = Join-Path $tempDir "CEMEMP.8xp"
        New-BinaryFile -Path $modulePath -SizeBytes 8000

        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamDispatchModulePath $modulePath `
            -ArchiveRamFailurePhase "memory_pressure" `
            -ArchiveRamReservedRamBytes 64000

        Assert-Equal -Expected $false -Actual ([bool]$result.success) -Message "Harness should fail when memory-pressure simulation is requested."
        Assert-Equal -Expected "memory_pressure" -Actual ([string]$result.archive_ram_swap.failure_phase) -Message "Memory-pressure simulation should report failure_phase=memory_pressure."
        Assert-Equal -Expected 0 -Actual ([int]$result.archive_ram_swap.modules.effective_ram_capacity_bytes) -Message "Effective RAM capacity should collapse to zero for forced memory-pressure simulation."
        Assert-Contains -Needle "effective swap RAM capacity is 0" -Haystack ($result.archive_ram_swap.errors -join " | ") -Message "Memory-pressure diagnostics should describe effective capacity exhaustion."
    }

    Invoke-TestCase -Name "Text formatter emits archive swap diagnostics fields" -Body {
        $tempDir = New-TestDirectory
        $modulePath = Join-Path $tempDir "CETEXT.8xp"
        New-BinaryFile -Path $modulePath -SizeBytes 12000
        $result = Invoke-CalcEmuTerminalHarness `
            -KeySequence "2,+,3,enter" `
            -ForceMock `
            -ArchiveRamDispatchModulePath $modulePath
        $text = Format-CalcEmuHarnessTextOutput -Result $result
        Assert-Contains -Needle "archive_ram_swap_simulated=true" -Haystack $text -Message "Text output should include archive swap simulation state."
        Assert-Contains -Needle "archive_ram_swap_dispatch_loaded_to_ram=true" -Haystack $text -Message "Text output should include dispatch load state."
        Assert-Contains -Needle "archive_ram_swap_return_completed=true" -Haystack $text -Message "Text output should include return completion state."
        Assert-Contains -Needle "archive_ram_swap_module_count=1" -Haystack $text -Message "Text output should include archive module count."
        Assert-Contains -Needle "archive_ram_swap_corruption_detected=false" -Haystack $text -Message "Text output should include corruption detection state."
    }

    Invoke-TestCase -Name "Text formatter emits deterministic key-value output" -Body {
        $result = Invoke-CalcEmuTerminalHarness -KeySequence "2,+,3,enter" -ForceMock
        $text = Format-CalcEmuHarnessTextOutput -Result $result
        Assert-Contains -Needle "mode=mock" -Haystack $text -Message "Text output should include mode."
        Assert-Contains -Needle "keys_normalized=KEY_2,KEY_ADD,KEY_3,KEY_ENTER" -Haystack $text -Message "Text output should include normalized keys."
        Assert-Contains -Needle "screen_result=5" -Haystack $text -Message "Text output should include result."
        Assert-Contains -Needle "autotest_hash_enabled=false" -Haystack $text -Message "Text output should include hash assertion enabled state."
        Assert-Contains -Needle "hardware_constraints_validated=false" -Haystack $text -Message "Text output should include hardware constraint validation state."
    }

    Invoke-TestCase -Name "JSON formatter returns parseable JSON object" -Body {
        $result = Invoke-CalcEmuTerminalHarness -KeySequence "2,+,3,enter" -ForceMock
        $json = ConvertTo-CalcEmuHarnessJson -Result $result
        $parsed = $json | ConvertFrom-Json
        Assert-Equal -Expected "mock" -Actual $parsed.mode -Message "Parsed JSON mode mismatch."
        Assert-Equal -Expected 1 -Actual ([int]$parsed.schema_version) -Message "Schema version mismatch."
        Assert-Equal -Expected "5" -Actual $parsed.screen.result -Message "Parsed JSON result mismatch."
        Assert-Equal -Expected $false -Actual ([bool]$parsed.archive_ram_swap.simulated) -Message "Parsed JSON should include archive swap object with default simulated=false."
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
