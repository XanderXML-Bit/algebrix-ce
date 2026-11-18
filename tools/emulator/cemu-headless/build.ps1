param(
    [string]$BuildDir = "",
    [string]$Config   = "Release",
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# build.ps1 - Build cemu-headless.exe with native CEmu core via MSVC + cmake.
# Usage: .\build.ps1 [-Clean] [-Config Release|Debug] [-BuildDir <path>]

function Write-Step { param([string]$Msg); Write-Host "[build.ps1] $Msg" -ForegroundColor Cyan }
function Write-Fail { param([string]$Msg); Write-Host "[build.ps1] FAIL: $Msg" -ForegroundColor Red }

# Locate Visual Studio via vswhere
$vswherePath = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswherePath)) {
    Write-Fail "vswhere.exe not found at: $vswherePath"
    exit 1
}

$vsInstall = (& $vswherePath -latest -property installationPath 2>&1) | Select-Object -First 1
if (-not $vsInstall -or -not (Test-Path -LiteralPath $vsInstall)) {
    Write-Fail "vswhere could not locate a Visual Studio installation."
    exit 1
}
Write-Step "Visual Studio: $vsInstall"

$vcvarsall = Join-Path $vsInstall "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path -LiteralPath $vcvarsall)) {
    Write-Fail "vcvarsall.bat not found: $vcvarsall"
    exit 1
}

# Locate cmake bundled with VS
$cmakeExe = Join-Path $vsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmakeExe)) {
    $cmakeCmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCmd) {
        $cmakeExe = $cmakeCmd.Source
    } else {
        Write-Fail "cmake.exe not found in VS install or PATH."
        exit 1
    }
}
Write-Step "cmake: $cmakeExe"

# Resolve directories
$scriptDir = Split-Path -Parent $PSCommandPath
if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $scriptDir "build_native"
}

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Step "Cleaning: $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Write a temporary batch file so vcvarsall env is inherited by cmake.
# Paths are quoted to handle spaces (e.g. "Zendren LLC" in the project path).
$tempBat = [System.IO.Path]::ChangeExtension([System.IO.Path]::GetTempFileName(), ".bat")
try {
    $lines = @(
        "@echo off",
        "call `"$vcvarsall`" x64",
        "if %errorlevel% neq 0 exit /b %errorlevel%",
        "",
        "`"$cmakeExe`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=$Config -S `"$scriptDir`" -B `"$BuildDir`"",
        "if %errorlevel% neq 0 exit /b %errorlevel%",
        "",
        "`"$cmakeExe`" --build `"$BuildDir`" --config $Config",
        "if %errorlevel% neq 0 exit /b %errorlevel%",
        "",
        "exit /b 0"
    )
    [System.IO.File]::WriteAllLines($tempBat, $lines, [System.Text.Encoding]::ASCII)

    Write-Step "Configuring and building..."
    cmd.exe /c "`"$tempBat`""
    $buildExitCode = $LASTEXITCODE
} finally {
    if (Test-Path -LiteralPath $tempBat) {
        Remove-Item -LiteralPath $tempBat -Force -ErrorAction SilentlyContinue
    }
}

if ($buildExitCode -ne 0) {
    Write-Fail "Build failed with exit code $buildExitCode"
    exit $buildExitCode
}

# Copy output to script directory
$exeName  = "cemu-headless.exe"
$builtExe = Join-Path $BuildDir $exeName
$destExe  = Join-Path $scriptDir $exeName

if (-not (Test-Path -LiteralPath $builtExe)) {
    Write-Fail "Built executable not found at: $builtExe"
    exit 1
}

Copy-Item -LiteralPath $builtExe -Destination $destExe -Force
$sizeKb = [Math]::Round((Get-Item $destExe).Length / 1KB)
Write-Step "Output: $destExe ($sizeKb KB)"

Write-Host "[build.ps1] BUILD OK" -ForegroundColor Green
exit 0
