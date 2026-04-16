@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SOLUTION=%SCRIPT_DIR%DoRun.sln"
set "CONFIG=Release"
set "PLATFORM=x64"
set "MSBUILD_EXE="

if /I "%~1"=="Debug" set "CONFIG=Debug"
if /I "%~1"=="Release" set "CONFIG=Release"
if not "%~2"=="" set "PLATFORM=%~2"

call "%SCRIPT_DIR%build.bat" %CONFIG% %PLATFORM%
if errorlevel 1 exit /b 1

exit /b 0
