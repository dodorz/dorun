@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "CONFIG=Release"
set "PLATFORM=x64"
set "FORCE_REBUILD=0"

if /I "%~1"=="Debug" set "CONFIG=Debug"
if /I "%~1"=="Release" set "CONFIG=Release"
if not "%~2"=="" if /I not "%~2"=="/rebuild" if /I not "%~2"=="-rebuild" set "PLATFORM=%~2"

if /I "%~1"=="/rebuild" set "FORCE_REBUILD=1"
if /I "%~1"=="-rebuild" set "FORCE_REBUILD=1"
if /I "%~2"=="/rebuild" set "FORCE_REBUILD=1"
if /I "%~2"=="-rebuild" set "FORCE_REBUILD=1"
if /I "%~3"=="/rebuild" set "FORCE_REBUILD=1"
if /I "%~3"=="-rebuild" set "FORCE_REBUILD=1"

set "TARGET_EXE=%SCRIPT_DIR%build\%CONFIG%\DoRun.exe"

if "%FORCE_REBUILD%"=="1" (
    call "%SCRIPT_DIR%rebuild.bat" %CONFIG% %PLATFORM%
    if errorlevel 1 exit /b 1
) else (
    if not exist "%TARGET_EXE%" (
        echo Build output not found. Rebuilding first.
        call "%SCRIPT_DIR%rebuild.bat" %CONFIG% %PLATFORM%
        if errorlevel 1 exit /b 1
    )
)

if not exist "%TARGET_EXE%" (
    echo Build output not found: %TARGET_EXE%
    exit /b 1
)

echo Running %TARGET_EXE%
start "" "%TARGET_EXE%"
exit /b 0
