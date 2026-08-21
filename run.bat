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
set "testerExe=%BUILD%/tester.exe"
for /f "tokens=*" %%A in ('powershell -Command "(Start-Process '%testerExe%' -PassThru).Id"') do set "PID=%%A"

echo Launched test process at PID = %PID%
"%BUILD%/main.exe" --pid %PID%
if errorlevel 1 goto :fail

:success
endlocal
exit /b 0

:fail
echo.
echo Failed
endlocal
exit /b 1
