@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "EXTERNAL_ROOT=%ROOT%\external"

:: was originally planning on building something on dynamorio but i instead wanted to build my own instrumentation engine to learn... so this is unsed
:: call "tools/download_dynamorio.bat"
set DynamorioArgs= -I%EXTERNAL_ROOT%/DynamoRIO-Windows-%DR_VER%/include -L%EXTERNAL_ROOT%/DynamoRIO-Windows-%DR_VER%/lib64 -L%EXTERNAL_ROOT%/DynamoRIO-Windows-%DR_VER%/lib64/release -Wl,/NODEFAULTLIB:dynamorio.lib -ldrinjectlib -ldynamorio

:build
set ZydisArgs= -I%EXTERNAL_ROOT%/zydis/dependencies/zycore/include -I%EXTERNAL_ROOT%/zydis/include/Zydis -I%EXTERNAL_ROOT%/zydis/include -L%EXTERNAL_ROOT%/zydis/build/RelWithDebInfo -lZydis -L%EXTERNAL_ROOT%/zydis/build/zycore/RelWithDebInfo -lZycore -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD
"tools/clang/bin/clang.exe" main.c -o main.exe -g
if errorlevel 1 goto :fail

"tools/clang/bin/clang.exe" injection.c -o injection.dll -g -shared %ZydisArgs%
if errorlevel 1 goto :fail

"tools/clang/bin/clang.exe" tester.c -o tester.exe -g -luser32
if errorlevel 1 goto :fail

echo [build] Success.
endlocal
exit /b 0

:fail
echo.
echo [build] BUILD FAILED
endlocal
exit /b 1
