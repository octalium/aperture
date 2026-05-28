# Windows packaging

Native MSVC + meson build. Lensfun (and its glib chain) come from
vcpkg; every other dep is built from source via the submodule overlay
under `dep/`.

## Prerequisites

- Windows 10 1809 or newer (host and target).
- Visual Studio 2022 with the **Desktop development with C++** workload
  (Build Tools alone also works).
- Python 3.10+ on `PATH`, and `pip install meson ninja`.
- Git (for `git clone` of vcpkg).

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
.\packaging\windows\build-zip.ps1
```

Produces `out\aperture-<version>-windows-x64.zip` containing
`aperture.exe`, all runtime DLLs, the LICENSE, and a short README.
`vulkan-1.dll` is **not** bundled — it lives in the user's GPU driver
install. If a release ever needs to ship the LunarG loader as a
fallback, drop it alongside the `.exe` and the script will pick it up
from the build dir.

## Make

`make windows` (root `Makefile`) is a thin wrapper that runs `meson
compile` + `build-zip.ps1` in one shot. It refuses to run on non-Windows
hosts; cross-compilation from Linux is out of scope for now and tracked
under #434.
