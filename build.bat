@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "NOB=%ROOT%\build.exe"
set "CLANG=%ROOT%\tools\clang\bin\clang.exe"

rem Use Visual Studio's bundled CMake when it is not on PATH.
where cmake >nul 2>&1
if errorlevel 1 (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
        for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do set "PATH=%%~dpI;%PATH%"
    )
)

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
