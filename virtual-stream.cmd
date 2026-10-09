@echo off
setlocal

set "HOST=%~dp0artifacts\x64\Release\host\TabletDisplayHost.exe"
if not exist "%HOST%" (
    call "%~dp0build.cmd" Release
    if errorlevel 1 exit /b %errorlevel%
)

"%SystemRoot%\System32\DisplaySwitch.exe" /extend
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "Start-Sleep -Seconds 2"
"%HOST%" --virtual-stream %*
exit /b %errorlevel%
