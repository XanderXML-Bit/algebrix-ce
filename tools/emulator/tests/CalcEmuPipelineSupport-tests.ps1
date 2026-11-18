Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:SourceModulePath = (Resolve-Path (Join-Path $PSScriptRoot "..\CalcEmuPipelineSupport.psm1")).ProviderPath
if (-not (Test-Path -LiteralPath $script:SourceModulePath -PathType Leaf)) {
    Write-Error "CalcEmuPipelineSupport module was not found at '$script:SourceModulePath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("pipeline-support-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BinaryFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$SizeBytes
    )

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }

    $buffer = New-Object byte[] $SizeBytes
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

function New-IsolatedModulePath {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath
    )

    $moduleDirectory = Join-Path $RootPath "tools\emulator"
    [void](New-Item -ItemType Directory -Path $moduleDirectory -Force)
    $modulePath = Join-Path $moduleDirectory "CalcEmuPipelineSupport.psm1"
    Copy-Item -LiteralPath $script:SourceModulePath -Destination $modulePath -Force
    return $modulePath
}

function Invoke-WithIsolatedModule {
    param(
        [Parameter(Mandatory = $true)][string]$ModulePath,
        [Parameter(Mandatory = $true)][scriptblock]$ScriptBlock
    )

    $module = Import-Module -Name $ModulePath -Force -PassThru
    try {
        & $ScriptBlock
    }
    finally {
        if ($null -ne $module) {
            Remove-Module -ModuleInfo $module -Force -ErrorAction SilentlyContinue
        }
    }
}

try {
    Invoke-TestCase -Name "Resolve-CalcEmuRomPath honors explicit RomPath when repo default is absent" -Body {
        $tempRoot = New-TestDirectory
        $modulePath = New-IsolatedModulePath -RootPath $tempRoot
        $explicitRomPath = Join-Path $tempRoot "external\Custom.rom"
        New-BinaryFile -Path $explicitRomPath -SizeBytes 4096

        Invoke-WithIsolatedModule -ModulePath $modulePath -ScriptBlock {
            $result = Resolve-CalcEmuRomPath -RomPath $explicitRomPath
            Assert-Equal -Expected (Resolve-Path -LiteralPath $explicitRomPath).ProviderPath -Actual ([string]$result.path) -Message "Explicit ROM path should resolve directly."
            Assert-Equal -Expected "explicit" -Actual ([string]$result.source) -Message "Explicit ROM path should be marked as explicit."
            Assert-Equal -Expected ([System.IO.Path]::GetFullPath((Join-Path $tempRoot "rom\Rom.rom"))) -Actual ([string]$result.canonical_path) -Message "Canonical path should still point at the repo-default ROM location."
        }
    }

    Invoke-TestCase -Name "Resolve-CalcEmuRomPath falls back to repo default when RomPath is omitted" -Body {
        $tempRoot = New-TestDirectory
        $modulePath = New-IsolatedModulePath -RootPath $tempRoot
        $defaultRomPath = Join-Path $tempRoot "rom\Rom.rom"
        New-BinaryFile -Path $defaultRomPath -SizeBytes 4194304

        Invoke-WithIsolatedModule -ModulePath $modulePath -ScriptBlock {
            $result = Resolve-CalcEmuRomPath
            $resolvedDefault = (Resolve-Path -LiteralPath $defaultRomPath).ProviderPath
            Assert-Equal -Expected $resolvedDefault -Actual ([string]$result.path) -Message "Default ROM should resolve when RomPath is omitted."
            Assert-Equal -Expected $resolvedDefault -Actual ([string]$result.canonical_path) -Message "Canonical path should match the repo-default ROM."
            Assert-Equal -Expected "repo_default" -Actual ([string]$result.source) -Message "Default fallback should be marked as repo_default."
        }
    }

    Invoke-TestCase -Name "Resolve-CalcEmuRomPath reports a clear error when neither explicit nor default ROM exists" -Body {
        $tempRoot = New-TestDirectory
        $modulePath = New-IsolatedModulePath -RootPath $tempRoot
        $missingExplicitRom = Join-Path $tempRoot "external\Missing.rom"
        $capturedError = Invoke-WithIsolatedModule -ModulePath $modulePath -ScriptBlock {
            try {
                Resolve-CalcEmuRomPath -RomPath $missingExplicitRom
                throw "Resolve-CalcEmuRomPath should fail when both explicit and default ROM paths are missing."
            }
            catch {
                $_.Exception.Message
            }
        }

        Assert-Contains -Needle ([System.IO.Path]::GetFullPath($missingExplicitRom)) -Haystack $capturedError -Message "Missing-ROM error should report the explicit path that failed."
        Assert-Contains -Needle "Supply -RomPath" -Haystack $capturedError -Message "Missing-ROM error should tell the caller how to recover."
        Assert-Contains -Needle ([System.IO.Path]::GetFullPath((Join-Path $tempRoot "rom\Rom.rom"))) -Haystack $capturedError -Message "Missing-ROM error should report the repo-default fallback path."
    }

    Invoke-TestCase -Name "Resolve-CalcEmuPipelinePath returns a Windows-readable path for the repo-default arTIfiCE asset" -Body {
        $tempRoot = New-TestDirectory
        $modulePath = New-IsolatedModulePath -RootPath $tempRoot
        $repoAssetPath = Join-Path (Split-Path -Path $script:SourceModulePath -Parent) "assets\artifice-v2.8xp"

        Assert-True -Condition (Test-Path -LiteralPath $repoAssetPath -PathType Leaf) -Message "The repo-default arTIfiCE asset should exist for this regression test."

        $resolvedPath = Invoke-WithIsolatedModule -ModulePath $modulePath -ScriptBlock {
            Resolve-CalcEmuPipelinePath -PathValue $repoAssetPath
        }

        $hash = $null
        try {
            $hash = (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256 -ErrorAction Stop).Hash
        }
        catch {
            throw ("Resolved path must be readable by Windows file APIs. Path: {0}`nError: {1}" -f $resolvedPath, $_.Exception.Message)
        }

        Assert-Equal -Expected 64 -Actual ([string]$hash).Length -Message "Hashing the resolved repo-default asset should succeed and produce a 64-character SHA-256 digest."
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
