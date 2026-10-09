[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]$identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdministrator)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $PSCommandPath))
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$secureBoot = (Get-ItemProperty `
    -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State" `
    -ErrorAction SilentlyContinue).UEFISecureBootEnabled
if ($secureBoot -eq 1) {
    Write-Error "Secure Boot is enabled. Disable it in UEFI before enabling TESTSIGNING. Check BitLocker recovery-key availability first."
    exit 2
}

bcdedit.exe /set testsigning on
if ($LASTEXITCODE -ne 0) {
    throw "BCDEdit failed with exit code $LASTEXITCODE."
}

Write-Output "TESTSIGNING is enabled. Restart Windows to apply it."
