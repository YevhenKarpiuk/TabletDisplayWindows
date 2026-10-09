@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\check-driver.ps1"
exit /b %errorlevel%
