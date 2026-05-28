# Assembles a portable Windows distribution zip from a completed meson
# build tree and the vcpkg install root used to satisfy lensfun (and
# its glib chain).
#
# Output layout (inside the zip):
#   aperture-<version>-windows-x64/
#     aperture.exe
#     *.dll                       (lensfun, glib-2.0, intl, iconv, ...)
#     LICENSE
#     README.txt                  (short pointer back to GitHub releases)
#
# The .exe loads `vulkan-1.dll` from the user's GPU-driver-installed
# location at runtime; we deliberately do not bundle it. If a future
# release decides to ship a fallback loader, drop it next to the .exe.
#
# Usage:
#   PS> .\packaging\windows\build-zip.ps1 -Version 0.1.0
#   PS> .\packaging\windows\build-zip.ps1 -BuildDir build -VcpkgInstalled C:\vcpkg\installed\x64-windows
#
# Defaults try to mirror what setup-deps.ps1 produces and what `make
# build` puts on disk.

[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$Version,
    [string]$VcpkgInstalled,
    [string]$Triplet = 'x64-windows',
    [string]$OutDir = 'out'
)

$ErrorActionPreference = 'Stop'

function Write-Step($msg) {
    Write-Host "[build-zip] $msg" -ForegroundColor Cyan
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repoRoot

if (-not $Version) {
    # parse from root meson.build so the script stays in sync with the
    # canonical project version. format: project('aperture', ..., version: '0.1.0', ...)
    $mesonBuild = Get-Content (Join-Path $repoRoot 'meson.build') -Raw
    if ($mesonBuild -match "version:\s*'([^']+)'") {
        $Version = $Matches[1]
    } else {
        throw "could not derive version from meson.build; pass -Version explicitly"
    }
}

if (-not $VcpkgInstalled) {
    if ($env:VCPKG_ROOT) {
        $VcpkgInstalled = Join-Path $env:VCPKG_ROOT "installed\$Triplet"
    } else {
        $VcpkgInstalled = Join-Path $repoRoot "dep\vcpkg\installed\$Triplet"
    }
}

$exe = Join-Path $BuildDir 'aperture.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe; run 'meson compile -C $BuildDir' first"
}

$vcpkgBin = Join-Path $VcpkgInstalled 'bin'
if (-not (Test-Path $vcpkgBin)) {
    throw "missing $vcpkgBin; run packaging\windows\setup-deps.ps1 first"
}

Write-Step "version:        $Version"
Write-Step "build dir:      $BuildDir"
Write-Step "vcpkg installed:$VcpkgInstalled"

$stageRoot = Join-Path $OutDir "aperture-$Version-windows-x64"
if (Test-Path $stageRoot) {
    Remove-Item -Recurse -Force $stageRoot
}
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null

Write-Step "staging $exe -> $stageRoot"
Copy-Item -Force $exe $stageRoot

# every .dll in vcpkg's bin directory ships alongside the .exe.
# vcpkg's installed/<triplet>/bin only contains runtime DLLs for the
# requested packages (lensfun) and their transitive deps (glib chain),
# so this picks up exactly what we need without an allowlist that
# would rot every time the dep tree shifts.
Write-Step "staging vcpkg runtime DLLs"
Get-ChildItem -Path $vcpkgBin -Filter '*.dll' | ForEach-Object {
    Copy-Item -Force $_.FullName $stageRoot
}

# any DLL that meson built into the build tree (rare under the current
# submodule layout — most subprojects link static — but future deps
# may produce DLLs and we want them shipped).
Write-Step "staging meson-built DLLs (if any)"
Get-ChildItem -Path $BuildDir -Filter '*.dll' -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -Force $_.FullName $stageRoot
}

Copy-Item -Force (Join-Path $repoRoot 'LICENSE') $stageRoot

$readme = @"
aperture $Version (portable, Windows x64)

Requirements:
  - Windows 10 1809 or newer (UCRT in-box)
  - A Vulkan-capable GPU + up-to-date GPU driver (ships vulkan-1.dll)

Usage:
  Unzip anywhere, then run aperture.exe. The application stores
  library + config under %APPDATA%\aperture.

Source, issues, newer releases:
  https://github.com/octalium/aperture
"@
Set-Content -Path (Join-Path $stageRoot 'README.txt') -Value $readme -Encoding utf8

$zip = Join-Path $OutDir "aperture-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Write-Step "compressing -> $zip"
Compress-Archive -Path "$stageRoot\*" -DestinationPath $zip -CompressionLevel Optimal

Write-Step "done: $zip"
Write-Host ""
Write-Host "ARTIFACT=$zip"
