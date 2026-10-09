param(
    [string]$PackageSource = "C:\tools\wdk\10.0.26100.6584\packages"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$packagesRoot = Join-Path $projectRoot "packages"

Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path $packagesRoot | Out-Null

$packages = @(
    @{
        File = "Microsoft.Windows.SDK.CPP.10.0.26100.6584.nupkg"
        Directory = "Microsoft.Windows.SDK.CPP.10.0.26100.6584"
        Marker = "build\native\Microsoft.Windows.SDK.cpp.props"
    },
    @{
        File = "Microsoft.Windows.SDK.cpp.x64.10.0.26100.6584.nupkg"
        Directory = "Microsoft.Windows.SDK.CPP.x64.10.0.26100.6584"
        Marker = "build\native\Microsoft.Windows.SDK.cpp.x64.props"
    },
    @{
        File = "Microsoft.Windows.WDK.x64.10.0.26100.6584.nupkg"
        Directory = "Microsoft.Windows.WDK.x64.10.0.26100.6584"
        Marker = "build\native\Microsoft.Windows.WDK.x64.props"
    }
)

foreach ($package in $packages) {
    $source = Join-Path $PackageSource $package.File
    $destination = Join-Path $packagesRoot $package.Directory
    $marker = Join-Path $destination $package.Marker

    if (Test-Path -LiteralPath $marker) {
        Write-Host "Using $($package.Directory)"
        continue
    }
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required portable package not found: $source"
    }
    if (Test-Path -LiteralPath $destination) {
        throw "Incomplete package directory exists: $destination"
    }

    Write-Host "Expanding $($package.File)"
    [System.IO.Compression.ZipFile]::ExtractToDirectory($source, $destination)
}

Write-Host "Portable WDK packages are ready."
