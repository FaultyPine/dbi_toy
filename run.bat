@echo off
setlocal

set "testerExe=tester.exe"
for /f "tokens=*" %%A in ('powershell -Command "(Start-Process '%testerExe%' -PassThru).Id"') do set "PID=%%A"

echo Launched test process at PID = %PID%
main.exe --pid %PID%

endlocal
exit /b 0

:fail
echo.
echo Failed
endlocal
exit /b 1
