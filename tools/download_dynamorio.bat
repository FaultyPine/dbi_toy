@echo off
setlocal

:: expected to be called by build.bat
set "ROOT=%CD%"

:: Ensure DynamoRIO is present. The release has large binaries, so it's not included directy in the repo
set "DR_VER=11.2.0"
set "DR_DIR=%ROOT%\external\DynamoRIO-Windows-%DR_VER%"
set "DR_ZIP=%DR_DIR%.zip"
set "DR_URL=https://github.com/DynamoRIO/dynamorio/releases/download/release_%DR_VER%/DynamoRIO-Windows-%DR_VER%.zip"

if not exist "%DR_DIR%\cmake\DynamoRIOConfig.cmake" (
    echo [build] DynamoRIO not found.  Downloading ^(~700 MB^) ...
    curl -L --progress-bar -o "%DR_ZIP%" "%DR_URL%"
    if errorlevel 1 (
        echo [build] Download failed.
        if exist "%DR_ZIP%" del /f /q "%DR_ZIP%"
        goto :fail
    )
    mkdir "%DR_DIR%"
    echo [build] Extracting ...
    tar -xf "%DR_ZIP%" -C "%EXTERNAL_ROOT%"
    if errorlevel 1 (
        echo [build] Extraction failed.
        del /f /q "%DR_ZIP%"
        goto :fail
    )
    del /f /q "%DR_ZIP%"
    echo [build] DynamoRIO ready.
    echo.
)


endlocal