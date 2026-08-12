[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

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
public static class TabletDisplayCheckStatus {
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

$deviceIds = Get-TabletDisplayDeviceIds
if ($deviceIds.Count -eq 0) {
    Write-Output "TabletDisplay driver: not installed"
    exit 1
}

 $healthy = $false
foreach ($deviceId in $deviceIds) {
    $devNodeStatus = [uint32]0
    $problemCode = [uint32]0
    $result = [TabletDisplayCheckStatus]::Get($deviceId, [ref]$devNodeStatus, [ref]$problemCode)
    $started = $result -eq 0 -and ($devNodeStatus -band 0x00000008) -ne 0
    Write-Output "TabletDisplay driver: $(if ($started) { 'Started' } else { 'Not started' })"
    Write-Output "InstanceId: $deviceId"
    Write-Output "ProblemCode: $problemCode"
    if ($started -and $problemCode -eq 0) {
        $healthy = $true
    }
}

if ($healthy) {
    exit 0
}
exit 2
