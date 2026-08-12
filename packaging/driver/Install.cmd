@echo off
setlocal
title TabletDisplay Driver Installer
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
echo Exit code: %RESULT%
pause
exit /b %RESULT%
