# Bootstraps the Windows toolchain dependencies that aperture's build
# can't supply from its own submodules. Idempotent: re-running picks
# up where a previous run stopped without re-fetching what's already
# installed.
#
# Scope:
#   - vcpkg (cloned + bootstrapped under $env:VCPKG_ROOT, default
#     $repo\dep\vcpkg) if not already present.
#   - vcpkg install lensfun (pulls in glib + intl + iconv chain).
#
# Out of scope, intentionally:
#   - MSVC / Visual Studio Build Tools. CI runners ship those; local
#     contributors install Visual Studio 2022 (Community is fine) with
#     the "Desktop development with C++" workload. The README + the
#     CONTRIBUTING Windows section document this.
#   - meson / ninja / python. The CI workflow installs them via pip /
#     the runner's bundled chocolatey; local devs install Python +
#     `pip install meson ninja` themselves.
#   - Vulkan SDK. We rely on `vulkan-1.dll` shipping with the user's
#     GPU driver, and on the vendored vulkan-headers submodule under
#     dep/vulkan-headers (added in #523) for compile-time headers.
#
# Usage:
#   PS> .\packaging\windows\setup-deps.ps1
#   PS> .\packaging\windows\setup-deps.ps1 -VcpkgRoot C:\src\vcpkg
#
# Exits non-zero on any failure so CI surfaces breakage.

[CmdletBinding()]
param(
    [string]$VcpkgRoot,
    [string]$Triplet = 'x64-windows'
)

$ErrorActionPreference = 'Stop'

function Write-Step($msg) {
    Write-Host "[setup-deps] $msg" -ForegroundColor Cyan
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

if (-not $VcpkgRoot) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = Join-Path $repoRoot 'dep\vcpkg'
    }
}

Write-Step "vcpkg root: $VcpkgRoot"
Write-Step "triplet:    $Triplet"

if (-not (Test-Path $VcpkgRoot)) {
    Write-Step "cloning vcpkg into $VcpkgRoot"
    git clone --depth=1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$bootstrap = Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'
$vcpkgExe  = Join-Path $VcpkgRoot 'vcpkg.exe'

if (-not (Test-Path $vcpkgExe)) {
    Write-Step "bootstrapping vcpkg"
    & cmd /c "`"$bootstrap`" -disableMetrics"
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg bootstrap failed (exit $LASTEXITCODE)"
    }
}

Write-Step "installing lensfun ($Triplet)"
& $vcpkgExe install "lensfun:$Triplet" --recurse
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg install lensfun failed (exit $LASTEXITCODE)"
}

# Surface the toolchain path that callers (meson, CMake) need to pass
# so they pick up vcpkg's installed packages without any manual prefix
# fiddling. Printed last so a CI step can grep it.
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
Write-Host ""
Write-Host "VCPKG_ROOT=$VcpkgRoot"
Write-Host "VCPKG_TOOLCHAIN=$toolchain"
Write-Host "VCPKG_INSTALLED=$VcpkgRoot\installed\$Triplet"
