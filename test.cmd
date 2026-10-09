@echo off
setlocal

call "%~dp0build.cmd" Debug
if errorlevel 1 exit /b %errorlevel%

"%~dp0artifacts\x64\Debug\tests\TabletDisplayProtocolTests.exe" "%~dp0protocol\golden"
exit /b %errorlevel%
