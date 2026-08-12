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

$devCon = Join-Path $PSScriptRoot "devcon.exe"
$certificatePath = Join-Path $PSScriptRoot "TabletDisplayDriverTest.cer"
$deviceIds = Get-TabletDisplayDeviceIds
$publishedNames = @()
foreach ($deviceId in $deviceIds) {
    $driver = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue |
        Where-Object DeviceID -eq $deviceId |
        Select-Object -First 1
    if ($driver.InfName) { $publishedNames += $driver.InfName }
}
if (Test-Path -LiteralPath $devCon) {
    & $devCon remove "Root\TabletDisplayDriver"
}
foreach ($name in ($publishedNames | Select-Object -Unique)) {
    & pnputil.exe /delete-driver $name /uninstall /force
    if ($LASTEXITCODE -ne 0) { Write-Warning "PnPUtil could not delete $name." }
}
if (Test-Path -LiteralPath $certificatePath) {
    $certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certificatePath)
    foreach ($store in @("Cert:\LocalMachine\Root", "Cert:\LocalMachine\TrustedPublisher")) {
        Get-ChildItem $store |
            Where-Object Thumbprint -eq $certificate.Thumbprint |
            Remove-Item -Force
    }
}
Write-Output "TabletDisplay driver and its local test certificate were removed."
