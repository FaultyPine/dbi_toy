@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "NOB=%ROOT%\build.exe"
set "CLANG=%ROOT%\tools\clang\bin\clang.exe"

"%CLANG%" -o "%NOB%" "%ROOT%\build.c"
if errorlevel 1 goto :fail

"%NOB%" %*
if errorlevel 1 goto :fail

endlocal
exit /b 0

:fail
echo.
echo [build] BUILD FAILED
endlocal
exit /b 1
