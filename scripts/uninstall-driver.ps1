[CmdletBinding()]
param()

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

if (-not (Test-IsAdministrator)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $PSCommandPath))
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$devCon = Join-Path $projectRoot "packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\tools\10.0.26100.0\x64\devcon.exe"
$certificatePath = Join-Path $projectRoot "artifacts\cert\TabletDisplayDriverTest.cer"
$deviceIds = Get-TabletDisplayDeviceIds
$publishedNames = @()

foreach ($deviceId in $deviceIds) {
    $signedDriver = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue |
        Where-Object DeviceID -eq $deviceId |
        Select-Object -First 1
    if ($signedDriver.InfName) {
        $publishedNames += $signedDriver.InfName
    }
}

if (Test-Path -LiteralPath $devCon) {
    & $devCon remove "Root\TabletDisplayDriver"
}

foreach ($publishedName in ($publishedNames | Select-Object -Unique)) {
    pnputil.exe /delete-driver $publishedName /uninstall /force
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "PnPUtil could not delete $publishedName."
    }
}

if (Test-Path -LiteralPath $certificatePath) {
    $certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certificatePath)
    foreach ($storePath in @("Cert:\LocalMachine\Root", "Cert:\LocalMachine\TrustedPublisher")) {
        Get-ChildItem $storePath |
            Where-Object Thumbprint -eq $certificate.Thumbprint |
            Remove-Item -Force
    }
}

Write-Output "TabletDisplay driver removed."
