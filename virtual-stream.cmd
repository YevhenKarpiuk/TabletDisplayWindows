@echo off
setlocal

set "HOST=%~dp0artifacts\x64\Debug\host\TabletDisplayHost.exe"
if not exist "%HOST%" (
    call "%~dp0build.cmd" Debug
    if errorlevel 1 exit /b %errorlevel%
)

"%SystemRoot%\System32\DisplaySwitch.exe" /extend
timeout /t 2 /nobreak >nul
"%HOST%" --virtual-stream %*
exit /b %errorlevel%
