$ErrorActionPreference = 'Stop'

function Resolve-VcVarsPath {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $fallback = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $fallback) {
        return $fallback
    }

    throw 'Could not locate vcvars64.bat'
}

function Import-VcVarsEnvironment {
    param([string]$VcVarsPath)

    $cmdOutput = & cmd.exe /c """$VcVarsPath"" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize MSVC environment from '$VcVarsPath'"
    }

    foreach ($line in $cmdOutput) {
        $separatorIndex = $line.IndexOf('=')
        if ($separatorIndex -lt 1) {
            continue
        }
        $name = $line.Substring(0, $separatorIndex)
        $value = $line.Substring($separatorIndex + 1)
        Set-Item -Path ("Env:{0}" -f $name) -Value $value
    }
}

Import-VcVarsEnvironment -VcVarsPath (Resolve-VcVarsPath)
Set-Location "C:\Users\user\src\algebrix-ce\tests"
$defines = @(
    "/D_CRT_SECURE_NO_WARNINGS",
    "/DDESKTOP_BUILD",
    "/DCE_PROC_ALG=1",
    "/DCE_PROC_CALC=1",
    "/DCE_PROC_STAT=1",
    "/DCE_PROC_FIN=1",
    "/DCE_PROC_MATRIX=1",
    "/DCE_PROC_CMPLX=1",
    "/DCE_PROC_GRAPH=1"
)

$sources = @(
    "..\src\memory.c",
    "..\src\token.c",
    "..\src\ast.c",
    "..\src\parser.c",
    "..\src\eval.c",
    "..\src\deriv.c",
    "..\src\simplify.c",
    "..\src\steps.c",
    "..\src\integrate.c",
    "..\src\limits.c",
    "..\src\series.c",
    "..\src\algebra.c",
    "..\src\stats.c",
    "..\src\matrix.c",
    "..\src\polar.c",
    "..\src\ui.c",
    "..\src\list.c",
    "..\src\complex.c",
    "..\src\prob.c",
    "..\src\finance.c",
    "..\src\stattest.c",
    "..\src\macros.c",
    "..\src\graph.c",
    "..\src\mathprint.c",
    "..\src\mathprint_template.c",
    "..\src\split\classify.c",
    "..\src\split\astwire.c",
    "..\src\split\ipc.c",
    "..\src\split\module_runtime.c",
    "..\src\split\module_forward.c",
    "..\src\split\processor.c",
    "..\src\split\disambig.c",
    "..\src\solver.c",
    "..\src\history.c",
    "..\src\dsolve.c",
    "..\src\vector.c",
    "..\src\laplace.c",
    "..\src\units.c",
    "..\src\userfunc.c",
    "..\src\bookmark.c",
    ".\mock\graphx_mock.c",
    ".\mock\keypadc_mock.c",
    ".\mock\tice_mock.c",
    ".\mock\fileioc_mock.c"
)

cl /W3 /Od /I..\src /Imock /nologo @defines test_main.c @sources /Fetest_runner.exe /link
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force -Path "..\bin" | Out-Null
cl /W3 /Od /I..\src /Imock /nologo @defines ..\tools\desktop\calcengine_cli.c @sources /Fe..\bin\calcengine.exe /link
