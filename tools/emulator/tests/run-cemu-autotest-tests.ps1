Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:WrapperPath = Join-Path (Split-Path -Path $PSScriptRoot -Parent) "run-cemu-autotest.ps1"
if (-not (Test-Path -LiteralPath $script:WrapperPath -PathType Leaf)) {
    Write-Error "CEmu autotest wrapper was not found at '$script:WrapperPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("cemu-wrapper-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
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

function New-TemplateFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $template = [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "CALCENG"
            isASM = $true
        }
        sequence = @("action|launch")
        hashes = [ordered]@{}
    }

    $template | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-FakeAutotester {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = @(
        "@echo off",
        "echo fake autotester config: %~1",
        "exit /b 0"
    )

    Set-Content -LiteralPath $Path -Value $content -Encoding ASCII
}

function Invoke-Wrapper {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdditionalArgs
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required for wrapper tests."
    }

    $powershellPath = $powershellCommand.Source
    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:WrapperPath
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
        $outputLines.Add("wrapper invocation error: $invocationError")
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = ($outputLines.ToArray() -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Wrapper writes launcher and split modules into transfer_files" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "test.rom"
        $programPath = Join-Path $tempDir "CALCENG.8xp"
        $moduleAPath = Join-Path $tempDir "CEALG.8xp"
        $moduleBPath = Join-Path $tempDir "CECALC.8xp"
        $autotesterPath = Join-Path $tempDir "autotester.cmd"
        $templatePath = Join-Path $tempDir "template.json"
        $outputConfigPath = Join-Path $tempDir "generated.json"
        $modulePathFile = Join-Path $tempDir "modules.txt"

        New-BinaryFile -Path $romPath -SizeBytes 4096
        New-BinaryFile -Path $programPath -SizeBytes 8192
        New-BinaryFile -Path $moduleAPath -SizeBytes 16384
        New-BinaryFile -Path $moduleBPath -SizeBytes 24576
        New-FakeAutotester -Path $autotesterPath
        New-TemplateFile -Path $templatePath
        Set-Content -LiteralPath $modulePathFile -Value @($moduleAPath, $moduleBPath) -Encoding UTF8

        $run = Invoke-Wrapper -AdditionalArgs @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-ProgramPath", $programPath,
            "-ModuleProgramPathsFile", $modulePathFile,
            "-TemplatePath", $templatePath,
            "-OutputConfigPath", $outputConfigPath
        )

        if ([int]$run.exit_code -ne 0) {
            throw "Wrapper should succeed with explicit launcher and module inputs.`nOutput:`n$($run.output_text)"
        }

        Assert-True -Condition (Test-Path -LiteralPath $outputConfigPath -PathType Leaf) -Message "Generated AutoTester config should exist."
        $outputBytes = [System.IO.File]::ReadAllBytes($outputConfigPath)
        Assert-True -Condition ($outputBytes.Length -gt 0) -Message "Generated AutoTester config should not be empty."
        Assert-True -Condition (-not ($outputBytes.Length -ge 3 -and $outputBytes[0] -eq 0xEF -and $outputBytes[1] -eq 0xBB -and $outputBytes[2] -eq 0xBF)) -Message "Generated AutoTester config should be written without a UTF-8 BOM so upstream autotester.exe can parse it."
        $config = Get-Content -LiteralPath $outputConfigPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected (Resolve-Path -LiteralPath $romPath).ProviderPath -Actual ([string]$config.rom) -Message "Wrapper should resolve the ROM path into the generated config."
        Assert-Equal -Expected "CALCENG" -Actual ([string]$config.target.name) -Message "Wrapper should derive target name from the launcher artifact."
        Assert-Equal -Expected 3 -Actual ([int]@($config.transfer_files).Count) -Message "Wrapper should transfer the launcher plus both split modules."
        Assert-Contains -Needle (Resolve-Path -LiteralPath $programPath).ProviderPath -Haystack (@($config.transfer_files) -join "|") -Message "Launcher should be included in transfer_files."
        Assert-Contains -Needle (Resolve-Path -LiteralPath $moduleAPath).ProviderPath -Haystack (@($config.transfer_files) -join "|") -Message "First split module should be included in transfer_files."
        Assert-Contains -Needle (Resolve-Path -LiteralPath $moduleBPath).ProviderPath -Haystack (@($config.transfer_files) -join "|") -Message "Second split module should be included in transfer_files."
        Assert-Contains -Needle "Modules transferred: 2" -Haystack $run.output_text -Message "Wrapper output should report the split-module transfer count."
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
