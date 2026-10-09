@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "CONFIGURATION=%~1"
if not defined CONFIGURATION set "CONFIGURATION=Debug"
set "PROJECT_ROOT=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_ROOT%scripts\restore-wdk.ps1"
if errorlevel 1 exit /b %errorlevel%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer vswhere.exe was not found.
    exit /b 1
)

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSINSTALL=%%I"
set "MSBUILD=!VSINSTALL!\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "!MSBUILD!" (
    echo x64 MSBuild.exe was not found.
    exit /b 1
)

set "WDK_PACKAGE=%PROJECT_ROOT%packages\Microsoft.Windows.WDK.x64.10.0.26100.6584"
set "SDK_PACKAGE=%PROJECT_ROOT%packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584"
set "CLEANPATH=%PATH%"
set "PATH="
set "Path="
set "PATH=%WDK_PACKAGE%\c\bin\10.0.26100.0\x64;%WDK_PACKAGE%\c\bin\10.0.26100.0\x86;%WDK_PACKAGE%\c\tools\10.0.26100.0\x64;%WDK_PACKAGE%\c\build\10.0.26100.0\bin;%WDK_PACKAGE%\c\build\10.0.26100.0\bin\x64;%SDK_PACKAGE%\c\bin\10.0.26100.0\x64;%CLEANPATH%"

"!MSBUILD!" "%PROJECT_ROOT%TabletDisplayWindows.sln" /m:1 /nr:false /t:Build /p:Configuration=%CONFIGURATION% /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:SignMode=Off
exit /b %errorlevel%
