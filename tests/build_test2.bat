@echo off
setlocal

set "MSVC_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808"
set "WINSDK_DIR=C:\Program Files (x86)\Windows Kits\10"
set "WINSDK_VER=10.0.22621.0"

set "PATH=%MSVC_DIR%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC_DIR%\include;%WINSDK_DIR%\Include\%WINSDK_VER%\ucrt"
set "LIB=%MSVC_DIR%\lib\x64;%WINSDK_DIR%\Lib\%WINSDK_VER%\ucrt\x64;%WINSDK_DIR%\Lib\%WINSDK_VER%\um\x64"

cd /d "%~dp0"
cl.exe /nologo /W3 /Od /D_CRT_SECURE_NO_WARNINGS /DDESKTOP_BUILD /I..\src /Imock /Fe:test_runner.exe test_main.c ..\src\memory.c ..\src\token.c ..\src\ast.c ..\src\parser.c ..\src\eval.c ..\src\deriv.c ..\src\simplify.c ..\src\steps.c ..\src\integrate.c ..\src\limits.c ..\src\series.c ..\src\algebra.c ..\src\stats.c ..\src\matrix.c ..\src\polar.c ..\src\ui.c ..\src\complex.c ..\src\list.c ..\src\stattest.c ..\src\prob.c ..\src\finance.c ..\src\macros.c ..\src\graph.c mock\graphx_mock.c mock\keypadc_mock.c mock\tice_mock.c mock\fileioc_mock.c
if %ERRORLEVEL% EQU 0 (
    echo BUILD SUCCESS
    test_runner.exe
) else (
    echo BUILD FAILED
    exit /b 1
)
