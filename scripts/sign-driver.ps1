[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$packageDir = Join-Path $projectRoot "artifacts\x64\$Configuration\driver\TabletDisplayDriver"
$catalogPath = Join-Path $packageDir "TabletDisplayDriver.cat"
$certificateDir = Join-Path $projectRoot "artifacts\cert"
$certificatePath = Join-Path $certificateDir "TabletDisplayDriverTest.cer"
$signTool = Join-Path $projectRoot "packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584\c\bin\10.0.26100.0\x64\signtool.exe"
$certificateSubject = "CN=TabletDisplay Driver Test Certificate"
$hostPaths = @(
    (Join-Path $projectRoot "artifacts\x64\$Configuration\host\TabletDisplay.exe"),
    (Join-Path $projectRoot "artifacts\x64\$Configuration\host\TabletDisplayHost.exe")
)

if (-not (Test-Path -LiteralPath $catalogPath)) {
    throw "Driver catalog not found. Run build.cmd $Configuration first."
}
if (-not (Test-Path -LiteralPath $signTool)) {
    throw "SignTool not found. Run scripts\restore-wdk.ps1 first."
}

$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object {
        $_.Subject -eq $certificateSubject -and
        $_.HasPrivateKey -and
        $_.NotAfter -gt (Get-Date).AddDays(30)
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if ($null -eq $certificate) {
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $certificateSubject `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(5)
}

New-Item -ItemType Directory -Force -Path $certificateDir | Out-Null
Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null

& $signTool sign /v /fd SHA256 /sha1 $certificate.Thumbprint /s My $catalogPath
if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE."
}

foreach ($hostPath in $hostPaths) {
    if (-not (Test-Path -LiteralPath $hostPath)) { throw "Host executable not found: $hostPath" }
    & $signTool sign /v /fd SHA256 /sha1 $certificate.Thumbprint /s My $hostPath
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed for $hostPath with exit code $LASTEXITCODE." }
}

$signature = Get-AuthenticodeSignature -LiteralPath $catalogPath
if ($null -eq $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
    throw "The catalog signature does not match the generated test certificate."
}

Write-Output "Signed catalog: $catalogPath"
Write-Output "Certificate: $certificatePath"
Write-Output "Thumbprint: $($certificate.Thumbprint)"
Write-Output "Signed host executables: $($hostPaths -join ', ')"
