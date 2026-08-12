@echo off
setlocal

set "CONFIGURATION=%~1"
if not defined CONFIGURATION set "CONFIGURATION=Release"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\sign-driver.ps1" -Configuration "%CONFIGURATION%"
exit /b %errorlevel%
