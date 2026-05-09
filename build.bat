@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SOLUTION=%SCRIPT_DIR%DoRun.sln"
set "PROFILE=StaticRelease"
set "CONFIG=StaticRelease"
set "PLATFORM=x64"
set "MSBUILD_EXE="
set "PROCESS_NAME=DoRun.exe"

if /I "%~1"=="Debug" (
    set "PROFILE=Debug"
    set "CONFIG=Debug"
)
if /I "%~1"=="Release" (
    set "PROFILE=Release"
    set "CONFIG=Release"
)
if /I "%~1"=="StaticRelease" (
    set "PROFILE=StaticRelease"
    set "CONFIG=StaticRelease"
)
if not "%~2"=="" set "PLATFORM=%~2"

call :find_msbuild
if errorlevel 1 goto :fail

call :stop_running_process
if errorlevel 1 goto :fail

echo Using MSBuild: %MSBUILD_EXE%
echo Building %PROFILE% (%CONFIG% / %PLATFORM%) from %SOLUTION%
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\invoke_msbuild.ps1" -Msbuild "%MSBUILD_EXE%" -Solution "%SOLUTION%" -Configuration "%CONFIG%" -Platform "%PLATFORM%"
if errorlevel 1 goto :fail

echo.
echo Build succeeded.
exit /b 0

:find_msbuild
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe`) do (
        set "MSBUILD_EXE=%%I"
        goto :msbuild_found
    )
)

if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" (
    set "MSBUILD_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    goto :msbuild_found
)

if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\17\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" (
    set "MSBUILD_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\17\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    goto :msbuild_found
)

where MSBuild.exe >nul 2>nul
if not errorlevel 1 (
    for /f "delims=" %%I in ('where MSBuild.exe') do (
        set "MSBUILD_EXE=%%I"
        goto :msbuild_found
    )
)

echo MSBuild.exe was not found.
echo Install Visual Studio Build Tools with MSBuild support, then try again.
exit /b 1

:msbuild_found
exit /b 0

:stop_running_process
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$processes = Get-Process -Name 'DoRun' -ErrorAction SilentlyContinue; " ^
    "if (-not $processes) { exit 0 }; " ^
    "Write-Host 'Detected running process: DoRun.exe'; " ^
    "$processes | Stop-Process -Force -ErrorAction Stop; " ^
    "Write-Host 'Stopped DoRun.exe';"
if errorlevel 1 (
    echo Failed to stop %PROCESS_NAME%.
    exit /b 1
)
exit /b 0

:fail
echo.
echo Build failed.
exit /b 1
