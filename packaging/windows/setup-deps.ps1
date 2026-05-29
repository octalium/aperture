# Bootstraps the Windows toolchain dependencies that aperture's build
# can't supply from its own submodules. Idempotent: re-running picks
# up where a previous run stopped without re-fetching what's already
# installed.
#
# Scope:
#   - vcpkg (cloned + bootstrapped under $env:VCPKG_ROOT, default
#     $repo\dep\vcpkg) if not already present.
#   - vcpkg install lensfun (pulls in glib + intl + iconv chain).
#   - LunarG Vulkan SDK (silent install) if VULKAN_SDK is not already
#     set. Supplies `vulkan-1.lib` at $env:VULKAN_SDK\Lib for MSVC's
#     `cc.find_library('vulkan-1')` at link time. The runtime DLL
#     comes from the user's GPU driver; the SDK is dev-only.
#
# Out of scope, intentionally:
#   - MSVC / Visual Studio Build Tools. CI runners ship those; local
#     contributors install Visual Studio 2022 (Community is fine) with
#     the "Desktop development with C++" workload. The README + the
#     CONTRIBUTING Windows section document this.
#   - meson / ninja / python. The CI workflow installs them via pip /
#     the runner's bundled chocolatey; local devs install Python +
#     `pip install meson ninja` themselves.
#
# Usage:
#   PS> .\packaging\windows\setup-deps.ps1
#   PS> .\packaging\windows\setup-deps.ps1 -VcpkgRoot C:\src\vcpkg
#   PS> .\packaging\windows\setup-deps.ps1 -SkipVulkanSdk     # CI uses humbletim/install-vulkan-sdk instead
#
# Exits non-zero on any failure so CI surfaces breakage.

[CmdletBinding()]
param(
    [string]$VcpkgRoot,
    [string]$Triplet      = 'x64-windows',
    [string]$VulkanSdkVersion = '1.4.309.0',
    [switch]$SkipVulkanSdk
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

if (-not $SkipVulkanSdk) {
    if ($env:VULKAN_SDK -and (Test-Path (Join-Path $env:VULKAN_SDK 'Lib\vulkan-1.lib'))) {
        Write-Step "Vulkan SDK already present at $env:VULKAN_SDK"
    } else {
        # silent installer; lands at C:\VulkanSDK\<version>\ by default.
        $sdkInstaller = Join-Path $env:TEMP "VulkanSDK-$VulkanSdkVersion-Installer.exe"
        $sdkUrl       = "https://sdk.lunarg.com/sdk/download/$VulkanSdkVersion/windows/VulkanSDK-$VulkanSdkVersion-Installer.exe"
        $sdkInstallDir = "C:\VulkanSDK\$VulkanSdkVersion"

        if (-not (Test-Path (Join-Path $sdkInstallDir 'Lib\vulkan-1.lib'))) {
            Write-Step "downloading Vulkan SDK $VulkanSdkVersion"
            Invoke-WebRequest -Uri $sdkUrl -OutFile $sdkInstaller -UseBasicParsing

            Write-Step "installing Vulkan SDK silently to $sdkInstallDir"
            & $sdkInstaller --root $sdkInstallDir --accept-licenses --default-answer --confirm-command install
            if ($LASTEXITCODE -ne 0) {
                throw "Vulkan SDK installer failed (exit $LASTEXITCODE)"
            }
        }

        $env:VULKAN_SDK = $sdkInstallDir
        Write-Step "VULKAN_SDK=$env:VULKAN_SDK"
    }
}

# Surface the toolchain path that callers (meson, CMake) need to pass
# so they pick up vcpkg's installed packages without any manual prefix
# fiddling. Printed last so a CI step can grep it.
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
Write-Host ""
Write-Host "VCPKG_ROOT=$VcpkgRoot"
Write-Host "VCPKG_TOOLCHAIN=$toolchain"
Write-Host "VCPKG_INSTALLED=$VcpkgRoot\installed\$Triplet"
if ($env:VULKAN_SDK) {
    Write-Host "VULKAN_SDK=$env:VULKAN_SDK"
}
