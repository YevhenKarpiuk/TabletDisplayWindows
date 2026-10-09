@echo off
setlocal

set "CONFIGURATION=%~1"
if not defined CONFIGURATION set "CONFIGURATION=Release"

call "%~dp0build.cmd" "%CONFIGURATION%"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0sign-driver.cmd" "%CONFIGURATION%"
if errorlevel 1 exit /b %errorlevel%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install-driver.ps1" -Configuration "%CONFIGURATION%"
exit /b %errorlevel%
