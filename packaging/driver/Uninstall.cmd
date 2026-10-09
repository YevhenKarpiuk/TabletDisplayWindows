@echo off
setlocal
title TabletDisplay Driver Uninstaller
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
echo Exit code: %RESULT%
pause
exit /b %RESULT%
