@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\enable-test-signing.ps1"
exit /b %errorlevel%
