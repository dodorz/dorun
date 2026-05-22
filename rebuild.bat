@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SOLUTION=%SCRIPT_DIR%DoRun.sln"
set "CONFIG=Release"
set "PLATFORM=x64"
set "MSBUILD_EXE="
set "BUILD_ALL=1"

if /I "%~1"=="Debug" (
    set "CONFIG=Debug"
    set "BUILD_ALL="
)
if /I "%~1"=="Release" (
    set "CONFIG=Release"
    set "BUILD_ALL="
)
if /I "%~1"=="StaticRelease" (
    set "CONFIG=StaticRelease"
    set "BUILD_ALL="
)
if not "%BUILD_ALL%"=="" (
    if not "%~1"=="" set "BUILD_ALL="
)
if not "%~2"=="" set "PLATFORM=%~2"

call :find_msbuild
if errorlevel 1 goto :fail

call :stop_running_process
if errorlevel 1 goto :fail

echo Using MSBuild: %MSBUILD_EXE%
if not "%BUILD_ALL%"=="" (
    call :rebuild_config Release
    if errorlevel 1 goto :fail
    call :rebuild_config Debug
    if errorlevel 1 goto :fail
    call :rebuild_config StaticRelease
    if errorlevel 1 goto :fail
) else (
    call :rebuild_config %CONFIG%
    if errorlevel 1 goto :fail
)

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

:rebuild_config
set "CURRENT_CONFIG=%~1"
echo Rebuilding %SOLUTION% /p:Configuration=%CURRENT_CONFIG% /p:Platform=%PLATFORM%
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\invoke_msbuild.ps1" -Msbuild "%MSBUILD_EXE%" -Solution "%SOLUTION%" -Configuration "%CURRENT_CONFIG%" -Platform "%PLATFORM%" -Targets Clean,Build
exit /b %errorlevel%

:stop_running_process
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$processes = Get-Process -Name 'DoRun' -ErrorAction SilentlyContinue; " ^
    "if (-not $processes) { exit 0 }; " ^
    "Write-Host 'Detected running process: DoRun.exe'; " ^
    "$processes | Stop-Process -Force -ErrorAction Stop; " ^
    "Write-Host 'Stopped DoRun.exe';"
if errorlevel 1 (
    echo Failed to stop DoRun.exe.
    exit /b 1
)
exit /b 0

:fail
echo.
echo Rebuild failed.
exit /b 1
