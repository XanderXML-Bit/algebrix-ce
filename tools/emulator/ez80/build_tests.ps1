Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-BuildWarning {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    Write-Host ("[WARN] {0}" -f $Message) -ForegroundColor Yellow
}

function Resolve-LatestDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ParentPath
    )

    if (-not (Test-Path -LiteralPath $ParentPath -PathType Container)) {
        return $null
    }

    $children = @(Get-ChildItem -LiteralPath $ParentPath -Directory | Sort-Object -Property Name -Descending)
    if ($children.Length -eq 0) {
        return $null
    }

    return $children[0].FullName
}

function Resolve-ZigCompiler {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $zigCommand = Get-Command zig.exe -ErrorAction SilentlyContinue
    if ($zigCommand) {
        return $zigCommand.Source
    }

    $toolsRoot = Join-Path $RepoRoot ".tools"
    if (-not (Test-Path -LiteralPath $toolsRoot -PathType Container)) {
        return $null
    }

    $zigPath = Get-ChildItem -LiteralPath $toolsRoot -Recurse -Filter "zig.exe" -File -ErrorAction SilentlyContinue |
        Sort-Object -Property FullName |
        Select-Object -First 1
    if ($zigPath) {
        return $zigPath.FullName
    }

    return $null
}

function Resolve-MsvcCompiler {
    $msvcRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
    $windowsKitIncludeRoot = "C:\Program Files (x86)\Windows Kits\10\Include"
    $windowsKitLibRoot = "C:\Program Files (x86)\Windows Kits\10\Lib"

    $msvcVersionDir = Resolve-LatestDirectory -ParentPath $msvcRoot
    $windowsKitVersionDir = Resolve-LatestDirectory -ParentPath $windowsKitIncludeRoot
    if ($null -eq $msvcVersionDir -or $null -eq $windowsKitVersionDir) {
        return $null
    }

    $windowsKitVersionName = Split-Path -Path $windowsKitVersionDir -Leaf
    $windowsKitLibVersionDir = Join-Path $windowsKitLibRoot $windowsKitVersionName
    $clPath = Join-Path $msvcVersionDir "bin\Hostx64\x64\cl.exe"
    $msvcInclude = Join-Path $msvcVersionDir "include"
    $msvcLib = Join-Path $msvcVersionDir "lib\x64"
    $ucrtInclude = Join-Path $windowsKitVersionDir "ucrt"
    $sharedInclude = Join-Path $windowsKitVersionDir "shared"
    $umInclude = Join-Path $windowsKitVersionDir "um"
    $ucrtLib = Join-Path $windowsKitLibVersionDir "ucrt\x64"
    $umLib = Join-Path $windowsKitLibVersionDir "um\x64"

    $requiredPaths = @(
        $clPath,
        $msvcInclude,
        $msvcLib,
        $ucrtInclude,
        $sharedInclude,
        $umInclude,
        $ucrtLib,
        $umLib
    )
    foreach ($requiredPath in $requiredPaths) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            return $null
        }
    }

    $env:PATH = (Split-Path -Path $clPath -Parent) + ";" + $env:PATH
    $env:INCLUDE = ($msvcInclude, $ucrtInclude, $sharedInclude, $umInclude -join ";")
    $env:LIB = ($msvcLib, $ucrtLib, $umLib -join ";")
    return $clPath
}

function Resolve-NativeCompiler {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $msvcCompiler = Resolve-MsvcCompiler
    if ($msvcCompiler) {
        return @{
            Flavor = "msvc"
            Command = $msvcCompiler
            PrefixArgs = @()
            Display = $msvcCompiler
        }
    }

    foreach ($commandName in @("gcc", "clang")) {
        $resolved = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($resolved) {
            return @{
                Flavor = "posix"
                Command = $resolved.Source
                PrefixArgs = @()
                Display = $resolved.Source
            }
        }
    }

    $zigCompiler = Resolve-ZigCompiler -RepoRoot $RepoRoot
    if ($zigCompiler) {
        return @{
            Flavor = "posix"
            Command = $zigCompiler
            PrefixArgs = @("cc")
            Display = "$zigCompiler cc"
        }
    }

    throw "No supported native compiler found. Install MSVC, gcc/clang, or place Zig under .tools."
}

function Invoke-NativeCompile {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Compiler,

        [Parameter(Mandatory = $true)]
        [string[]]$IncludeDirs,

        [string[]]$Defines = @(),

        [Parameter(Mandatory = $true)]
        [string]$OutputPath,

        [Parameter(Mandatory = $true)]
        [string[]]$Sources,

        [switch]$LinkMath
    )

    $args = @()
    $args += $Compiler.PrefixArgs

    if ($Compiler.Flavor -eq "msvc") {
        $args += @("/nologo", "/W3", "/Od", "/D_CRT_SECURE_NO_WARNINGS")
        foreach ($define in $Defines) {
            $args += "/D$define"
        }
        foreach ($includeDir in $IncludeDirs) {
            $args += "/I$includeDir"
        }
        $args += "/Fe:$OutputPath"
        $args += $Sources
    } else {
        $args += @("-Wall", "-Wextra", "-g", "-O0", "-std=c99", "-D_CRT_SECURE_NO_WARNINGS")
        foreach ($define in $Defines) {
            $args += "-D$define"
        }
        foreach ($includeDir in $IncludeDirs) {
            $args += "-I$includeDir"
        }
        $args += $Sources
        if ($LinkMath) {
            $args += "-lm"
        }
        $args += @("-o", $OutputPath)
    }

    & $Compiler.Command @args
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed for '$OutputPath' with exit code $LASTEXITCODE."
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$runtimeOutputPath = Join-Path $PSScriptRoot "tests\ez80_test_runner.exe"
$cliOutputPath = Join-Path $PSScriptRoot "ez80_cli.exe"

$compiler = $null
try {
    if ($env:CALCENGINE_FORCE_NO_NATIVE_BUILD -eq "1") {
        throw "CALCENGINE_FORCE_NO_NATIVE_BUILD=1"
    }

    $compiler = Resolve-NativeCompiler -RepoRoot $repoRoot
}
catch {
    $missingArtifacts = @()
    foreach ($artifactPath in @($runtimeOutputPath, $cliOutputPath)) {
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            $missingArtifacts += $artifactPath
        }
    }

    if ($missingArtifacts.Count -eq 0) {
        Write-BuildWarning ("build:ez80 reusing existing native artifacts because no supported compiler is available. {0}" -f $_.Exception.Message)
    }
    else {
        Write-BuildWarning ("build:ez80 skipped because no supported compiler is available and the following artifacts are absent: {0}" -f ($missingArtifacts -join ", "))
    }

    exit 0
}

$runtimeIncludes = @(
    "include",
    "src",
    "../../../src",
    "../../../tests/mock"
)
$runtimeSources = @(
    "tests/ez80_stub_runtime_tests.c",
    "src/ez80_emulator_stub.c",
    "src/ez80_decode_stub.c",
    "src/ez80_agent.c",
    "src/ez80_rom_stub.c",
    "src/ez80_8xp.c",
    "../../../src/memory.c",
    "../../../src/token.c",
    "../../../src/ast.c",
    "../../../src/parser.c",
    "../../../src/eval.c",
    "../../../src/deriv.c",
    "../../../src/simplify.c",
    "../../../src/steps.c",
    "../../../src/integrate.c",
    "../../../src/limits.c",
    "../../../src/series.c",
    "../../../src/algebra.c",
    "../../../src/stats.c",
    "../../../src/matrix.c",
    "../../../src/polar.c",
    "../../../src/ui.c",
    "../../../src/list.c",
    "../../../src/complex.c",
    "../../../src/prob.c",
    "../../../src/finance.c",
    "../../../src/stattest.c",
    "../../../src/macros.c",
    "../../../src/graph.c",
    "../../../src/solver.c",
    "../../../src/history.c",
    "../../../src/mathprint.c",
    "../../../src/mathprint_template.c",
    "../../../src/dsolve.c",
    "../../../src/vector.c",
    "../../../src/laplace.c",
    "../../../src/units.c",
    "../../../src/userfunc.c",
    "../../../src/bookmark.c",
    "../../../src/split/classify.c",
    "../../../src/split/astwire.c",
    "../../../src/split/ipc.c",
    "../../../src/split/module_runtime.c",
    "../../../src/split/module_forward.c",
    "../../../src/split/processor.c",
    "../../../src/split/disambig.c",
    "../../../tests/mock/graphx_mock.c",
    "../../../tests/mock/keypadc_mock.c",
    "../../../tests/mock/tice_mock.c",
    "../../../tests/mock/fileioc_mock.c"
)

$cliIncludes = @(
    "include",
    "src"
)
$cliSources = @(
    "src/ez80_cli.c",
    "src/ez80_agent.c",
    "src/ez80_emulator_stub.c",
    "src/ez80_decode_stub.c",
    "src/ez80_rom_stub.c",
    "src/ez80_8xp.c"
)

Push-Location $PSScriptRoot
try {
    Write-Host "Compiler: $($compiler.Display)"

    Invoke-NativeCompile -Compiler $compiler `
        -IncludeDirs $runtimeIncludes `
        -OutputPath $runtimeOutputPath `
        -Sources $runtimeSources `
        -LinkMath
    Write-Host "TEST BUILD OK" -ForegroundColor Green

    Invoke-NativeCompile -Compiler $compiler `
        -IncludeDirs $cliIncludes `
        -OutputPath $cliOutputPath `
        -Sources $cliSources
    Write-Host "CLI BUILD OK" -ForegroundColor Green
}
catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}

exit 0
