Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\check-artifice-asset.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "Asset helper was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("check-artifice-asset-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BinaryFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$SizeBytes
    )

    $buffer = New-Object byte[] $SizeBytes
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

function Invoke-Runner {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:RunnerPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = (@($captured | ForEach-Object { [string]$_ }) -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Found asset copies to canonical repo cache path" -Body {
        $tempDir = New-TestDirectory
        $searchDir = Join-Path $tempDir "search"
        $outputDir = Join-Path $tempDir "output"
        $destinationPath = Join-Path $outputDir "artifice-v2.8xp"
        $assetPath = Join-Path $searchDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $searchDir -Force)
        New-BinaryFile -Path $assetPath -SizeBytes 4096

        $run = Invoke-Runner -Arguments @(
            "-SearchRoots", $searchDir,
            "-DestinationPath", $destinationPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Helper should succeed when an arTIfiCE asset is present."
        Assert-Contains -Needle "FOUND $assetPath" -Haystack $run.output_text -Message "Helper should print FOUND and the source path."
        Assert-True -Condition (Test-Path -LiteralPath $destinationPath -PathType Leaf) -Message "Helper should copy the asset into the destination path."
        Assert-Equal -Expected 4096 -Actual ([int](Get-Item -LiteralPath $destinationPath).Length) -Message "Copied asset size should match the source."
    }

    Invoke-TestCase -Name "Missing asset prints download instructions" -Body {
        $tempDir = New-TestDirectory
        $searchDir = Join-Path $tempDir "search"
        $destinationPath = Join-Path $tempDir "artifice-v2.8xp"

        [void](New-Item -ItemType Directory -Path $searchDir -Force)

        $run = Invoke-Runner -Arguments @(
            "-SearchRoots", $searchDir,
            "-DestinationPath", $destinationPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Helper should fail when no arTIfiCE asset is present."
        Assert-Contains -Needle "MISSING" -Haystack $run.output_text -Message "Helper should print the missing marker."
        Assert-Contains -Needle "https://github.com/YvanTT/arTIfiCE/releases" -Haystack $run.output_text -Message "Helper should print the maintained upstream release URL."
        Assert-True -Condition (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) -Message "Helper should not create a destination file when nothing was found."
    }

    Invoke-TestCase -Name "Multiple candidates selects first alphabetically" -Body {
        $tempDir = New-TestDirectory
        $searchDir = Join-Path $tempDir "search"
        $outputDir = Join-Path $tempDir "output"
        $destinationPath = Join-Path $outputDir "artifice-v2.8xp"
        [void](New-Item -ItemType Directory -Path $searchDir -Force)
        New-BinaryFile -Path (Join-Path $searchDir "arTIfiCE_v2.0.8xp") -SizeBytes 1111
        New-BinaryFile -Path (Join-Path $searchDir "arTIfiCE_v2.1.8xp") -SizeBytes 2222

        $run = Invoke-Runner -Arguments @(
            "-SearchRoots", $searchDir,
            "-DestinationPath", $destinationPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Helper should succeed when multiple candidates exist."
        Assert-Contains -Needle "arTIfiCE_v2.0.8xp" -Haystack $run.output_text -Message "The first alphabetical candidate should be selected."
        Assert-True -Condition (Test-Path -LiteralPath $destinationPath -PathType Leaf) -Message "The destination file should have been written."
        Assert-Equal -Expected 1111 -Actual ([int](Get-Item -LiteralPath $destinationPath).Length) -Message "The smaller first-alphabetical file should be the one copied."
    }

    Invoke-TestCase -Name "Case-insensitive filename match finds ARTIFICE in all caps" -Body {
        $tempDir = New-TestDirectory
        $searchDir = Join-Path $tempDir "search"
        $destinationPath = Join-Path $tempDir "artifice-v2.8xp"
        [void](New-Item -ItemType Directory -Path $searchDir -Force)
        New-BinaryFile -Path (Join-Path $searchDir "ARTIFICE_JAILBREAK.8xp") -SizeBytes 4096

        $run = Invoke-Runner -Arguments @(
            "-SearchRoots", $searchDir,
            "-DestinationPath", $destinationPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Helper should succeed with all-uppercase filename."
        Assert-Contains -Needle "ARTIFICE_JAILBREAK.8xp" -Haystack $run.output_text -Message "All-caps filename should be found by the case-insensitive scan."
    }

    Invoke-TestCase -Name "Destination parent directory is created when missing" -Body {
        $tempDir = New-TestDirectory
        $searchDir = Join-Path $tempDir "search"
        $nestedBase = Join-Path $tempDir "deeply\nested\path"
        $nestedDestination = Join-Path $nestedBase "artifice-v2.8xp"
        [void](New-Item -ItemType Directory -Path $searchDir -Force)
        New-BinaryFile -Path (Join-Path $searchDir "arTIfiCE_v2.8xp") -SizeBytes 512

        $run = Invoke-Runner -Arguments @(
            "-SearchRoots", $searchDir,
            "-DestinationPath", $nestedDestination
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Helper should succeed even when the destination parent does not exist."
        Assert-True -Condition (Test-Path -LiteralPath $nestedDestination -PathType Leaf) -Message "Helper should create intermediate parent directories and write the file."
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
