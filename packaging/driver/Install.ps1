[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]$identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TabletDisplayDeviceIds {
    $root = "HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY"
    if (-not (Test-Path -LiteralPath $root)) { return @() }
    return @(
        Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue |
            Where-Object {
                $properties = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
                @($properties.HardwareID) -contains "Root\TabletDisplayDriver"
            } |
            ForEach-Object { "ROOT\DISPLAY\$($_.PSChildName)" }
    )
}

if (-not (Test-IsAdministrator)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $PSCommandPath))
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$inf = Join-Path $PSScriptRoot "TabletDisplayDriver.inf"
$catalog = Join-Path $PSScriptRoot "TabletDisplayDriver.cat"
$certificate = Join-Path $PSScriptRoot "TabletDisplayDriverTest.cer"
$devCon = Join-Path $PSScriptRoot "devcon.exe"
foreach ($path in @($inf, $catalog, $certificate, $devCon, (Join-Path $PSScriptRoot "TabletDisplayDriver.dll"))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required file not found: $path" }
}
if ($null -eq (Get-AuthenticodeSignature -LiteralPath $catalog).SignerCertificate) {
    throw "Driver catalog is not signed."
}

Import-Certificate -FilePath $certificate -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $certificate -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
if ((Get-TabletDisplayDeviceIds).Count -eq 0) {
    & $devCon install $inf "Root\TabletDisplayDriver"
} else {
    & $devCon update $inf "Root\TabletDisplayDriver"
}
if ($LASTEXITCODE -notin @(0, 1)) { throw "DevCon failed with exit code $LASTEXITCODE." }
Start-Sleep -Seconds 2
$device = Get-PnpDevice -InstanceId "ROOT\DISPLAY\*" -ErrorAction SilentlyContinue |
    Where-Object InstanceId -in (Get-TabletDisplayDeviceIds) |
    Select-Object -First 1
if (-not $device) { throw "TabletDisplay device was not created." }
Write-Output "TabletDisplay driver installed. Device: $($device.InstanceId); status: $($device.Status)."
if ($device.Status -ne "OK") {
    Write-Warning "The package is installed but the device is not started. A restart may be required."
    exit 2
}
exit 0
