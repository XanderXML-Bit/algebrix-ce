Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:SetupScriptPath = Join-Path $script:RepoRoot "scripts\setup-cemu.ps1"
if (-not (Test-Path -LiteralPath $script:SetupScriptPath -PathType Leaf)) {
    Write-Error "CEmu setup script was not found at '$script:SetupScriptPath'."
    exit 2
}

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)][AllowNull()][object]$Expected,
        [Parameter(Mandatory = $true)][AllowNull()][object]$Actual,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message`nExpected: $Expected`nActual: $Actual"
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Needle,
        [Parameter(Mandatory = $true)][string]$Haystack,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Haystack.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Message`nExpected to find: $Needle`nActual text: $Haystack"
    }
}

function Invoke-TestCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("setup-cemu-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-FakeBootRunner {
    param([Parameter(Mandatory = $true)][string]$Path)

    $content = @"
param(
    [string]`$OutputPath = ''
)

`$summary = [ordered]@{
    success = `$true
    exit_code = 0
}

if (-not [string]::IsNullOrWhiteSpace(`$OutputPath)) {
    `$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath `$OutputPath -Encoding UTF8
}

Write-Host 'fake boot runner complete'
exit 0
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function Invoke-SetupScript {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:SetupScriptPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = (@($captured | ForEach-Object { [string]$_ }) -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Setup succeeds with an existing local install and explicit boot runner" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "Rom.rom"
        $installRoot = Join-Path $tempDir "CEmu\v2.0"
        $cemuPath = Join-Path $installRoot "CEmu.exe"
        $autotesterPath = Join-Path $installRoot "autotester.exe"
        $summaryPath = Join-Path $tempDir "summary.json"
        $bootSummaryPath = Join-Path $tempDir "boot.json"
        $bootRunnerPath = Join-Path $tempDir "boot-runner.ps1"

        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 16))
        [void](New-Item -ItemType Directory -Path $installRoot -Force)
        Set-Content -LiteralPath $cemuPath -Value "fake gui" -Encoding ASCII
        Set-Content -LiteralPath $autotesterPath -Value "fake autotester" -Encoding ASCII
        New-FakeBootRunner -Path $bootRunnerPath

        $run = Invoke-SetupScript -Arguments @(
            "-RomPath", $romPath,
            "-InstallRoot", $installRoot,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", $autotesterPath,
            "-SkipGuiDownload",
            "-OutputPath", $summaryPath,
            "-BootVerifyRunnerPath", $bootRunnerPath,
            "-BootVerifySummaryPath", $bootSummaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Setup should pass when explicit GUI and autotester paths already exist."
        Assert-Contains -Needle "Overall         : PASS" -Haystack $run.output_text -Message "Console output should report PASS."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected $true -Actual ([bool]$summary.install.autotester_ready) -Message "Autotester should be marked ready."
        Assert-Equal -Expected $true -Actual ([bool]$summary.boot_verification.success) -Message "Boot verification should pass."
    }

    Invoke-TestCase -Name "Setup records manual autotester build steps when autotester is missing" -Body {
        $tempDir = New-TestDirectory
        $romPath = Join-Path $tempDir "Rom.rom"
        $installRoot = Join-Path $tempDir "CEmu\v2.0"
        $cemuPath = Join-Path $installRoot "CEmu.exe"
        $summaryPath = Join-Path $tempDir "summary.json"

        [System.IO.File]::WriteAllBytes($romPath, (New-Object byte[] 16))
        [void](New-Item -ItemType Directory -Path $installRoot -Force)
        Set-Content -LiteralPath $cemuPath -Value "fake gui" -Encoding ASCII

        $run = Invoke-SetupScript -Arguments @(
            "-RomPath", $romPath,
            "-InstallRoot", $installRoot,
            "-CEmuPath", $cemuPath,
            "-AutotesterPath", (Join-Path $tempDir "missing-autotester.exe"),
            "-RequireExplicitAutotesterPath",
            "-SkipGuiDownload",
            "-SkipBootVerification",
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Setup should fail when autotester cannot be resolved."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary should report failure."
        Assert-Equal -Expected $false -Actual ([bool]$summary.install.autotester_ready) -Message "Autotester should be marked unavailable."
        Assert-True -Condition (@($summary.manual_steps).Count -gt 0) -Message "Manual build steps should be recorded when autotester is missing."
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
