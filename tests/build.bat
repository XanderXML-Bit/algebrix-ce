@echo off
setlocal
set "VCVARS="
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
        if exist "%%~I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~I\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    > build_output.txt echo BUILD FAILED
    >> build_output.txt echo Could not locate vcvars64.bat
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    > build_output.txt echo BUILD FAILED
    >> build_output.txt echo Failed to initialize MSVC environment from "%VCVARS%"
    exit /b 1
)
cd /d "C:\Users\user\src\algebrix-ce\tests"
del /q test_runner.exe 2>nul
cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /I..\src /Imock /DDESKTOP_BUILD /DCE_PROC_ALG=1 /DCE_PROC_CALC=1 /DCE_PROC_STAT=1 /DCE_PROC_FIN=1 /DCE_PROC_MATRIX=1 /DCE_PROC_CMPLX=1 /DCE_PROC_GRAPH=1 /Fe:test_runner.exe ..\src\memory.c ..\src\token.c ..\src\ast.c ..\src\parser.c ..\src\eval.c ..\src\deriv.c ..\src\simplify.c ..\src\steps.c ..\src\integrate.c ..\src\limits.c ..\src\series.c ..\src\algebra.c ..\src\stats.c ..\src\matrix.c ..\src\polar.c ..\src\ui.c ..\src\list.c ..\src\complex.c ..\src\prob.c ..\src\finance.c ..\src\stattest.c ..\src\macros.c ..\src\graph.c ..\src\mathprint.c ..\src\split\classify.c ..\src\split\astwire.c ..\src\split\ipc.c ..\src\split\module_runtime.c ..\src\split\module_forward.c ..\src\split\processor.c ..\src\solver.c ..\src\history.c mock\graphx_mock.c mock\keypadc_mock.c mock\tice_mock.c mock\fileioc_mock.c test_main.c > build_output.txt 2>&1
if errorlevel 1 (
    echo BUILD FAILED >> build_output.txt
) else (
    echo BUILD OK >> build_output.txt
    .\test_runner.exe >> build_output.txt 2>&1
)
endlocal
