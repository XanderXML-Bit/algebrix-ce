@echo off
setlocal
set "MSVC_GCC_ARGS=%*"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0msvc-gcc-compat.ps1"
set "WRAP_EXIT=%ERRORLEVEL%"
endlocal & exit /b %WRAP_EXIT%
