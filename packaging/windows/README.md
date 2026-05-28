# Windows packaging

Native MSVC + meson build, packaged as a WiX v4 MSI installer.
Lensfun (and its glib chain) come from vcpkg; every other dep is
built from source via the submoduled `dep/` tree. `vulkan-1.dll` is
**not** bundled — modern GPU drivers ship it in `System32`.

## Prerequisites

- Windows 10 1809 or newer (host and target).
- Visual Studio 2022 with the **Desktop development with C++** workload
  (Build Tools alone also works).
- Python 3.10+ on `PATH`, and `pip install meson ninja`.
- Git (for `git clone` of vcpkg and the dep submodules).
- WiX Toolset v4: `dotnet tool install --global wix` then
  `wix extension add --global WixToolset.UI.wixext`.

CI mirrors the same setup on `windows-2022` runners.

## Bootstrap

From a Developer PowerShell for VS 2022 (so `cl.exe` is on `PATH`):

```powershell
.\packaging\windows\setup-deps.ps1
```

Re-running is safe — vcpkg is incremental. The script prints
`VCPKG_ROOT`, `VCPKG_TOOLCHAIN`, and `VCPKG_INSTALLED` lines so the
next step knows where to find them.

By default vcpkg lands under `dep\vcpkg\`. Override with `-VcpkgRoot`
or by exporting `$env:VCPKG_ROOT` first.

## Build

```powershell
$env:PKG_CONFIG_PATH = "$env:VCPKG_ROOT\installed\x64-windows\lib\pkgconfig"
meson setup build --buildtype=release `
    --pkg-config-path "$env:PKG_CONFIG_PATH"
meson compile -C build
```

The `PKG_CONFIG_PATH` plumbing makes meson's `dependency('lensfun')`
resolve against the vcpkg-installed `.pc` files. Every other
`dependency()` and `subproject()` call resolves against the in-tree
`dep/` submodules without further configuration.

## Package

```powershell
.\packaging\windows\build-msi.ps1
```

Produces `out\aperture-<version>-windows-x64.msi`. The installer
delivers `aperture.exe` and every runtime DLL into
`%ProgramFiles%\aperture\`, creates a **Start Menu → aperture**
shortcut, and registers an Add/Remove Programs entry with help
links pointing back to the GitHub repo. Uninstall removes
everything the installer placed.

## WiX source

The MSI is authored in [`wix/aperture.wxs`](wix/aperture.wxs):

- Single `Package` (per-machine, x64).
- `MajorUpgrade` matched on a fixed `UpgradeCode` GUID so a newer
  MSI silently replaces an older install. **Never change that
  GUID.**
- `WixUI_Minimal` wizard (welcome → license → install → finish);
  license RTF is rendered at build time from the project's `LICENSE`
  file by `build-msi.ps1`.
- File harvest via `<Files Include="!(bindpath.Stage)\**" ...>` —
  the .wxs is generic; `build-msi.ps1` decides what ends up in the
  staging dir.

## Make

`make windows` (root `Makefile`) is a thin wrapper that runs
`setup-deps.ps1` + `meson setup`/`meson compile` + `build-msi.ps1`
in one shot. It refuses to run on non-Windows hosts;
cross-compilation from Linux is out of scope for now and tracked
under #434.
