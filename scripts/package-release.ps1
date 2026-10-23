[CmdletBinding()]
param(
    [string]$Version = "0.3.0"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$distRoot = Join-Path $projectRoot "dist"
$driverName = "TabletDisplayDriver-$Version"
$hostName = "TabletDisplayHostPortable-$Version"
$driverDir = Join-Path $distRoot $driverName
$hostDir = Join-Path $distRoot $hostName
$driverArtifacts = Join-Path $projectRoot "artifacts\x64\Release\driver\TabletDisplayDriver"
$hostArtifacts = Join-Path $projectRoot "artifacts\x64\Release\host"
$certificate = Join-Path $projectRoot "artifacts\cert\TabletDisplayDriverTest.cer"
$devCon = Join-Path $projectRoot "packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\tools\10.0.26100.0\x64\devcon.exe"
$platformTools = "C:\tools\android\sdk\platform-tools"
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

foreach ($path in @(
    (Join-Path $driverArtifacts "TabletDisplayDriver.inf"),
    (Join-Path $driverArtifacts "TabletDisplayDriver.cat"),
    (Join-Path $driverArtifacts "TabletDisplayDriver.dll"),
    (Join-Path $hostArtifacts "TabletDisplay.exe"),
    (Join-Path $hostArtifacts "TabletDisplayHost.exe"),
    $certificate,
    $devCon,
    (Join-Path $platformTools "adb.exe"),
    (Join-Path $platformTools "AdbWinApi.dll"),
    (Join-Path $platformTools "AdbWinUsbApi.dll")
)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required release input not found: $path" }
}

New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
foreach ($directory in @($driverDir, $hostDir)) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

Copy-Item -LiteralPath (Join-Path $driverArtifacts "TabletDisplayDriver.inf") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $driverArtifacts "TabletDisplayDriver.cat") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $driverArtifacts "TabletDisplayDriver.dll") -Destination $driverDir
Copy-Item -LiteralPath $certificate -Destination $driverDir
Copy-Item -LiteralPath $devCon -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\driver\Install.cmd") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\driver\Install.ps1") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\driver\Uninstall.cmd") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\driver\Uninstall.ps1") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\driver\README.txt") -Destination $driverDir
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY_NOTICES.md") -Destination $driverDir

Copy-Item -LiteralPath (Join-Path $hostArtifacts "TabletDisplay.exe") -Destination $hostDir
Copy-Item -LiteralPath (Join-Path $hostArtifacts "TabletDisplayHost.exe") -Destination $hostDir
Copy-Item -LiteralPath (Join-Path $projectRoot "assets\TabletDisplay.png") -Destination $hostDir
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\host\README.txt") -Destination $hostDir
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY_NOTICES.md") -Destination $hostDir
$vsInstall = & $vsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vsInstall) { throw "Visual Studio runtime location was not found." }
$crtDir = Get-ChildItem -LiteralPath (Join-Path $vsInstall "VC\Redist\MSVC") -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $crtDir) { throw "Microsoft VC143 x64 runtime was not found." }
Copy-Item -Path (Join-Path $crtDir "*.dll") -Destination $hostDir
$portableTools = Join-Path $hostDir "platform-tools"
New-Item -ItemType Directory -Force -Path $portableTools | Out-Null
foreach ($name in @("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll", "NOTICE.txt", "source.properties")) {
    $path = Join-Path $platformTools $name
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $portableTools }
}

foreach ($name in @($driverName, $hostName)) {
    $zip = Join-Path $distRoot "$name.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -LiteralPath (Join-Path $distRoot $name) -DestinationPath $zip -CompressionLevel Optimal
}

Write-Output "Driver release: $driverDir"
Write-Output "Host release: $hostDir"
