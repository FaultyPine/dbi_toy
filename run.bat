@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"

if "%~1"=="" (
    goto :testerRun
) else (
    "%BUILD%/main.exe" %*
    if errorlevel 1 goto :fail
    goto :success
)

:testerRun
"%BUILD%/main.exe" --run "%BUILD%/tester.exe"
if errorlevel 1 goto :fail

:success
endlocal
exit /b 0

:fail
echo.
echo Failed
endlocal
exit /b 1
