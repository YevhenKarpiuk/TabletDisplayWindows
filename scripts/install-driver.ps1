[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]$identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TabletDisplayDeviceIds {
    $enumRoot = "HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY"
    if (-not (Test-Path -LiteralPath $enumRoot)) {
        return @()
    }
    return @(
        Get-ChildItem -LiteralPath $enumRoot -ErrorAction SilentlyContinue |
            Where-Object {
                $properties = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
                @($properties.HardwareID) -contains "Root\TabletDisplayDriver"
            } |
            ForEach-Object { "ROOT\DISPLAY\$($_.PSChildName)" }
    )
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class TabletDisplayDeviceStatus {
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern int CM_Locate_DevNodeW(out uint deviceInstance, string deviceId, uint flags);
    [DllImport("cfgmgr32.dll")]
    private static extern int CM_Get_DevNode_Status(out uint status, out uint problem, uint deviceInstance, uint flags);
    public static int Get(string deviceId, out uint status, out uint problem) {
        status = 0; problem = 0; uint deviceInstance;
        int result = CM_Locate_DevNodeW(out deviceInstance, deviceId, 0);
        return result == 0 ? CM_Get_DevNode_Status(out status, out problem, deviceInstance, 0) : result;
    }
}
"@

if (-not (Test-IsAdministrator)) {
    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", ('"{0}"' -f $PSCommandPath),
        "-Configuration", $Configuration
    )
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$packageDir = Join-Path $projectRoot "artifacts\x64\$Configuration\driver\TabletDisplayDriver"
$infPath = Join-Path $packageDir "TabletDisplayDriver.inf"
$catalogPath = Join-Path $packageDir "TabletDisplayDriver.cat"
$certificatePath = Join-Path $projectRoot "artifacts\cert\TabletDisplayDriverTest.cer"
$devCon = Join-Path $projectRoot "packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\tools\10.0.26100.0\x64\devcon.exe"
$hardwareId = "Root\TabletDisplayDriver"

foreach ($requiredPath in @($infPath, $catalogPath, $certificatePath, $devCon)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required file not found: $requiredPath"
    }
}

$signature = Get-AuthenticodeSignature -LiteralPath $catalogPath
if ($null -eq $signature.SignerCertificate) {
    throw "Driver catalog is not signed. Run sign-driver.cmd $Configuration first."
}

Import-Certificate -FilePath $certificatePath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $certificatePath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

$existingDeviceIds = Get-TabletDisplayDeviceIds
if ($existingDeviceIds.Count -eq 0) {
    & $devCon install $infPath $hardwareId
} else {
    & $devCon update $infPath $hardwareId
}
$devConExitCode = $LASTEXITCODE
if ($devConExitCode -notin @(0, 1)) {
    throw "DevCon failed with exit code $devConExitCode."
}

Start-Sleep -Seconds 2
$deviceId = Get-TabletDisplayDeviceIds | Select-Object -First 1
if (-not $deviceId) {
    throw "The TabletDisplay root device was not created."
}

$devNodeStatus = [uint32]0
$problemCode = [uint32]0
$statusResult = [TabletDisplayDeviceStatus]::Get($deviceId, [ref]$devNodeStatus, [ref]$problemCode)
if ($statusResult -ne 0) {
    throw "CM_Get_DevNode_Status failed with code $statusResult for $deviceId."
}
$started = ($devNodeStatus -band 0x00000008) -ne 0

Write-Output "InstanceId: $deviceId"
Write-Output "Status: $(if ($started) { 'Started' } else { 'Not started' })"
Write-Output "ProblemCode: $problemCode"
if ($devConExitCode -eq 1) {
    Write-Output "RestartRequired: True"
}
if (-not $started -or $problemCode -ne 0) {
    $secureBoot = (Get-ItemProperty `
        -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State" `
        -ErrorAction SilentlyContinue).UEFISecureBootEnabled
    Write-Warning "The driver package is installed but the device did not start. SecureBoot=$secureBoot."
    Write-Warning "If ProblemCode is 52, disable Secure Boot, enable TESTSIGNING, and reboot."
    exit 2
}

Write-Output "TabletDisplay driver installed and started."
exit 0
