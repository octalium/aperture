# Combined Windows build-and-package runner invoked by `make windows`.
#
# Runs the full pipeline in a single pwsh session so env vars set by
# setup-deps.ps1 (notably VULKAN_SDK) carry through to meson and the
# MSI build without the Makefile having to thread them.
#
# Usage:
#   PS> .\packaging\windows\make-windows.ps1
#   PS> .\packaging\windows\make-windows.ps1 -BuildDir build -BuildType release

[CmdletBinding()]
param(
    [string]$BuildDir  = 'build',
    [string]$BuildType = 'release'
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repoRoot

& packaging\windows\setup-deps.ps1
if ($LASTEXITCODE -ne 0) {
    throw "setup-deps failed (exit $LASTEXITCODE)"
}

# extend LIB so meson's cc.find_library('vulkan-1') resolves at link time.
if ($env:VULKAN_SDK) {
    $env:LIB = "$env:VULKAN_SDK\Lib;$env:LIB"
}

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'dep\vcpkg' }
$pcPath    = Join-Path $vcpkgRoot 'installed\x64-windows\lib\pkgconfig'

meson setup $BuildDir --buildtype=$BuildType --pkg-config-path $pcPath
if ($LASTEXITCODE -ne 0) { throw "meson setup failed" }

meson compile -C $BuildDir
if ($LASTEXITCODE -ne 0) { throw "meson compile failed" }

& packaging\windows\build-msi.ps1 -BuildDir $BuildDir
if ($LASTEXITCODE -ne 0) { throw "build-msi failed" }
