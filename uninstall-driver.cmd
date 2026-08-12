@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\uninstall-driver.ps1"
exit /b %errorlevel%
