@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "EXTERNAL_ROOT=%ROOT%\external"
set "CLANG=%ROOT%\tools\clang\bin\clang.exe"
set "MINILUA=%BUILD%\minilua.exe"
set "DYNASM_LUA=%EXTERNAL_ROOT%\LuaJIT\dynasm\dynasm.lua"
set "DYNASM_INJECTION_C=%BUILD%\injection_dynasm.c"

if not exist "%BUILD%" mkdir "%BUILD%"

:: was originally planning on building something on dynamorio but i instead wanted to build my own instrumentation engine to learn... so this is unsed
:: call "tools/download_dynamorio.bat"
set DynamorioArgs= -I%EXTERNAL_ROOT%/DynamoRIO-Windows-%DR_VER%/include -L%EXTERNAL_ROOT%/DynamoRIO-Windows-%DR_VER%/lib64 -L%EXTERNAL_ROOT%/DynamoRIO-Windows-%DR_VER%/lib64/release -Wl,/NODEFAULTLIB:dynamorio.lib -ldrinjectlib -ldynamorio

:build
if not exist "%DYNASM_LUA%" (
    echo [build] Missing LuaJIT DynASM checkout at "%EXTERNAL_ROOT%\LuaJIT".
    echo [build] Run: git submodule update --init external/LuaJIT
    goto :fail
)

if not exist "%MINILUA%" (
    "%CLANG%" "%EXTERNAL_ROOT%\LuaJIT\src\host\minilua.c" -o "%MINILUA%" -O2 -D_CRT_SECURE_NO_WARNINGS
    if errorlevel 1 goto :fail
)

"%MINILUA%" "%DYNASM_LUA%" -L -I "%EXTERNAL_ROOT%\LuaJIT\dynasm" -o "%DYNASM_INJECTION_C%" "%ROOT%\injection.c"
if errorlevel 1 goto :fail

set ZydisArgs= -I%EXTERNAL_ROOT%/zydis/dependencies/zycore/include -I%EXTERNAL_ROOT%/zydis/include/Zydis -I%EXTERNAL_ROOT%/zydis/include -L%EXTERNAL_ROOT%/zydis/build/RelWithDebInfo -lZydis -L%EXTERNAL_ROOT%/zydis/build/zycore/RelWithDebInfo -lZycore -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD
set DynasmArgs= -I%ROOT% -I%EXTERNAL_ROOT%/LuaJIT/dynasm

"%CLANG%" main.c -o main.exe -g
if errorlevel 1 goto :fail

"%CLANG%" "%DYNASM_INJECTION_C%" -o injection.dll -g -shared %ZydisArgs% %DynasmArgs%
if errorlevel 1 goto :fail

"%CLANG%" tester.c -o tester.exe -g -luser32
if errorlevel 1 goto :fail

echo [build] Success.
endlocal
exit /b 0

:fail
echo.
echo [build] BUILD FAILED
endlocal
exit /b 1
