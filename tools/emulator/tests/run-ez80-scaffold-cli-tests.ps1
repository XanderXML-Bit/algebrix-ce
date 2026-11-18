Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:CliPath = Join-Path $script:RepoRoot "tools\emulator\ez80\run-ez80-scaffold-cli.ps1"
if (-not (Test-Path -LiteralPath $script:CliPath -PathType Leaf)) {
    Write-Error "eZ80 scaffold CLI was not found at '$script:CliPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("ez80-scaffold-cli-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function Invoke-ScaffoldCli {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RomPath,

        [Parameter(Mandatory = $true)]
        [ValidateSet("json", "text")]
        [string]$Format,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$OutputPath = ""
    )

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "powershell.exe is required for scaffold CLI tests."
    }

    $powershellPath = $powershellCommand.Source
    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:CliPath,
        "-Command",
        "probe-rom",
        "-RomPath",
        $RomPath,
        "-Format",
        $Format
    )
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $arguments += @(
            "-OutputPath",
            $OutputPath
        )
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
        $outputLines.Add("invocation error: $invocationError")
    }

    $outputText = $outputLines.ToArray() -join "`n"
    $parsedJson = $null
    if ($Format -eq "json" -and -not [string]::IsNullOrWhiteSpace($outputText)) {
        try {
            $parsedJson = $outputText | ConvertFrom-Json
        }
        catch {
            $parsedJson = $null
        }
    }

    return [ordered]@{
        exit_code = $exitCode
        output_text = $outputText
        output_lines = $outputLines.ToArray()
        json = $parsedJson
    }
}

try {
    Invoke-TestCase -Name "CLI computes deterministic CRC32 for known vector file" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "known-vector.rom"
        [System.IO.File]::WriteAllBytes($romPath, [System.Text.Encoding]::ASCII.GetBytes("123456789"))

        $run = Invoke-ScaffoldCli -RomPath $romPath -Format "json"

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "CLI should succeed for a valid ROM path."
        Assert-True -Condition ($null -ne $run.json) -Message "CLI JSON output should be parseable."
        Assert-Equal -Expected 1 -Actual ([int]$run.json.schema_version) -Message "Schema version mismatch."
        Assert-Equal -Expected "ok" -Actual ([string]$run.json.status) -Message "Status should be ok."
        Assert-Equal -Expected $true -Actual ([bool]$run.json.success) -Message "Success should be true."
        Assert-Equal -Expected 9 -Actual ([int64]$run.json.rom.file_size_bytes) -Message "File size should match known vector length."
        Assert-Equal -Expected "CBF43926" -Actual ([string]$run.json.rom.crc32) -Message "CRC32 should match canonical IEEE test vector."
        Assert-Equal -Expected $false -Actual ([bool]$run.json.rom.size_matches_expected) -Message "Small test ROM should not match expected 4MB size."
    }

    Invoke-TestCase -Name "CLI flags expected 4MB ROM size accurately" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "size-4mb.rom"
        $buffer = New-Object byte[] 4194304
        [System.IO.File]::WriteAllBytes($romPath, $buffer)

        $run = Invoke-ScaffoldCli -RomPath $romPath -Format "json"

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "CLI should succeed for 4MB test ROM."
        Assert-True -Condition ($null -ne $run.json) -Message "CLI JSON output should be parseable."
        Assert-Equal -Expected 4194304 -Actual ([int64]$run.json.rom.file_size_bytes) -Message "4MB test ROM size mismatch."
        Assert-Equal -Expected $true -Actual ([bool]$run.json.rom.size_matches_expected) -Message "4MB ROM should match expected size."
        Assert-Equal -Expected $false -Actual ([bool]$run.json.rom.header_probe.has_nonzero_prefix) -Message "Zeroed test ROM should not report non-zero header bytes."
    }

    Invoke-TestCase -Name "CLI returns structured error when ROM path is missing" -Body {
        $tempDir = New-TestDirectory
        $missingPath = Join-Path $tempDir "missing.rom"

        $run = Invoke-ScaffoldCli -RomPath $missingPath -Format "json"

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "CLI should return exit code 1 for missing ROM path."
        Assert-True -Condition ($null -ne $run.json) -Message "Error JSON output should be parseable."
        Assert-Equal -Expected "error" -Actual ([string]$run.json.status) -Message "Status should be error for missing ROM path."
        Assert-Equal -Expected $false -Actual ([bool]$run.json.success) -Message "Success should be false for missing ROM path."
        Assert-Contains -Needle "does not exist" -Haystack ([string]$run.json.error) -Message "Error message should mention missing ROM path."
    }

    Invoke-TestCase -Name "CLI text format emits parseable key telemetry" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "known-vector-text.rom"
        [System.IO.File]::WriteAllBytes($romPath, [System.Text.Encoding]::ASCII.GetBytes("123456789"))

        $run = Invoke-ScaffoldCli -RomPath $romPath -Format "text"

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "CLI text mode should succeed for valid ROM path."
        Assert-Contains -Needle "status=ok" -Haystack $run.output_text -Message "Text output should include success status."
        Assert-Contains -Needle "rom_crc32=CBF43926" -Haystack $run.output_text -Message "Text output should include deterministic CRC."
        Assert-Contains -Needle "rom_size_matches_expected=false" -Haystack $run.output_text -Message "Text output should include size flag."
    }

    Invoke-TestCase -Name "CLI sets TI-marker telemetry when header contains TI prefix" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "ti-marker.rom"
        $header = [System.Text.Encoding]::ASCII.GetBytes("TI84CE_ROM_HEADER_SAMPLE")
        [System.IO.File]::WriteAllBytes($romPath, $header)

        $run = Invoke-ScaffoldCli -RomPath $romPath -Format "json"

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "CLI should succeed for TI-marker telemetry test file."
        Assert-True -Condition ($null -ne $run.json) -Message "TI-marker telemetry JSON should parse."
        Assert-Equal -Expected $true -Actual ([bool]$run.json.rom.header_probe.has_nonzero_prefix) -Message "Header with ASCII content should report non-zero prefix."
        Assert-Equal -Expected $true -Actual ([bool]$run.json.rom.header_probe.contains_ti_marker) -Message "Header containing TI marker should set contains_ti_marker."
    }

    Invoke-TestCase -Name "CLI writes rendered payload to output path" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "known-vector-output.rom"
        $outputPath = Join-Path $tempDir "probe-output.txt"
        [System.IO.File]::WriteAllBytes($romPath, [System.Text.Encoding]::ASCII.GetBytes("123456789"))

        $run = Invoke-ScaffoldCli -RomPath $romPath -Format "text" -OutputPath $outputPath

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "CLI should succeed when output path is provided."
        Assert-True -Condition (Test-Path -LiteralPath $outputPath -PathType Leaf) -Message "CLI should create requested output file."
        $outputFileText = Get-Content -LiteralPath $outputPath -Raw
        Assert-Contains -Needle "status=ok" -Haystack $outputFileText -Message "Output file should include success status."
        Assert-Contains -Needle "rom_crc32=CBF43926" -Haystack $outputFileText -Message "Output file should include deterministic CRC."
    }

    Invoke-TestCase -Name "CLI returns structured error for empty RomPath input" -Body {
        $run = Invoke-ScaffoldCli -RomPath "   " -Format "json"

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "CLI should return exit code 1 when RomPath is empty."
        Assert-True -Condition ($null -ne $run.json) -Message "Empty RomPath error JSON should be parseable."
        Assert-Equal -Expected "error" -Actual ([string]$run.json.status) -Message "Empty RomPath should return error status."
        Assert-Equal -Expected $false -Actual ([bool]$run.json.success) -Message "Empty RomPath should set success=false."
        Assert-Contains -Needle "cannot be empty" -Haystack ([string]$run.json.error) -Message "Empty RomPath error should mention empty input."
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
