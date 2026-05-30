# Builds the aperture Windows MSI installer from a completed meson
# build tree + the vcpkg install root that holds lensfun and its glib
# chain.
#
# Pipeline:
#   1. Stage payload — aperture.exe + every runtime DLL — into a
#      temporary directory (out/msi-stage by default).
#   2. Render the LICENSE into a minimal RTF the WiX UI extension can
#      embed in the wizard.
#   3. Invoke `wix build` (WiX v4) against pkg\windows\wix\aperture.wxs
#      with the staging dir as a bindpath, producing the .msi.
#
# Output:
#   out\aperture-<version>-windows-x64.msi
#
# vulkan-1.dll is NOT staged — modern GPU drivers ship it in System32;
# bundling our own would shadow the driver-supplied loader.
#
# Usage:
#   PS> .\pkg\windows\build-msi.ps1
#   PS> .\pkg\windows\build-msi.ps1 -Version 0.1.0 -BuildDir build -VcpkgInstalled C:\vcpkg\installed\x64-windows

[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$Version,
    [string]$VcpkgInstalled,
    [string]$Triplet = 'x64-windows',
    [string]$OutDir = 'out',
    [string]$Manufacturer = 'octalium'
)

$ErrorActionPreference = 'Stop'

function Write-Step($msg) {
    Write-Host "[build-msi] $msg" -ForegroundColor Cyan
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repoRoot

if (-not $Version) {
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
    throw "missing $vcpkgBin; run pkg\windows\setup-deps.ps1 first"
}

# WiX v4 ships as a dotnet tool; verify before doing payload work so
# the failure surface is obvious.
$wix = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wix) {
    throw "wix CLI not found on PATH; run 'dotnet tool install --global wix'"
}

Write-Step "version:        $Version"
Write-Step "build dir:      $BuildDir"
Write-Step "vcpkg installed:$VcpkgInstalled"

$stageRoot = Join-Path $OutDir 'msi-stage'
if (Test-Path $stageRoot) {
    Remove-Item -Recurse -Force $stageRoot
}
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null

Write-Step "staging $exe"
Copy-Item -Force $exe $stageRoot

Write-Step "staging vcpkg runtime DLLs"
Get-ChildItem -Path $vcpkgBin -Filter '*.dll' | ForEach-Object {
    Copy-Item -Force $_.FullName $stageRoot
}

Write-Step "staging meson-built DLLs (if any)"
Get-ChildItem -Path $BuildDir -Filter '*.dll' -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -Force $_.FullName $stageRoot
}

# WiX UI's WixUILicenseRtf needs an RTF, not plain text. The MIT
# license is short enough to wrap inline in a minimal RTF document
# so we don't have to keep a separate hand-authored .rtf in tree.
#
# Written under $OutDir, NOT $stageRoot: wix harvests the whole staging
# tree via the .wxs `Files Include="!(bindpath.Stage)\**"`, so an RTF
# placed there would ship as an installed payload file. wix reads it by
# absolute path (-d LicenseRtf=...), so its location is otherwise free.
Write-Step "rendering LICENSE as RTF"
$licensePlain = Get-Content (Join-Path $repoRoot 'LICENSE') -Raw
$licenseEscaped = $licensePlain -replace '\\','\\' -replace '\{','\{' -replace '\}','\}'
# single pass: `\r?\n` matches CRLF and bare LF without re-matching the
# newline the replacement itself emits (a CRLF-then-LF chain would emit \par\par).
$licenseEscaped = $licenseEscaped -replace "`r?`n","\par`n"
$licenseRtf = "{\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Segoe UI;}}\fs18`n$licenseEscaped`n}"
$licenseRtfPath = Join-Path $OutDir '_license.rtf'
Set-Content -Path $licenseRtfPath -Value $licenseRtf -Encoding ascii -NoNewline

$wxs = Join-Path $repoRoot 'pkg\windows\wix\aperture.wxs'
$msi = Join-Path $OutDir "aperture-$Version-windows-x64.msi"
if (Test-Path $msi) { Remove-Item -Force $msi }

Write-Step "wix build -> $msi"
& wix build `
    -arch x64 `
    -ext WixToolset.UI.wixext `
    -bindpath "Stage=$stageRoot" `
    -d "ApertureVersion=$Version" `
    -d "ApertureManufacturer=$Manufacturer" `
    -d "LicenseRtf=$licenseRtfPath" `
    -out $msi `
    $wxs
if ($LASTEXITCODE -ne 0) {
    throw "wix build failed (exit $LASTEXITCODE)"
}

# the license RTF was a bind-time UI asset, not installer payload —
# drop it now that wix has consumed it.
Remove-Item -Force $licenseRtfPath -ErrorAction SilentlyContinue

Write-Step "done: $msi"
Write-Host ""
Write-Host "ARTIFACT=$msi"
