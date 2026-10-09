@echo off
setlocal

call "%~dp0build.cmd" Release
if errorlevel 1 exit /b %errorlevel%
call "%~dp0sign-driver.cmd" Release
if errorlevel 1 exit /b %errorlevel%
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package-release.ps1"
exit /b %errorlevel%
