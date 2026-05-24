---
name: build-and-run
description: Build aperture from source and launch it for development or manual verification. Use this when implementing a feature/fix that needs a clean compile + interactive smoke test, or before opening a PR that touches runtime behavior. Covers Linux distro deps, meson setup, compile, and run; calls out the env vars worth knowing.
---

Aperture is a meson + ninja project with several vendored wraps. The build is straightforward; the one thing that bites people is missing system-side deps.

## 1. Install system deps (once per machine)

System packages provide the non-vendored deps via pkg-config. Choose the line for your distro — these are the canonical lists from the README:

**Debian / Ubuntu:**
```
sudo apt install build-essential meson ninja-build pkg-config \
    glslc libvulkan-dev vulkan-validationlayers libglfw3-dev \
    libraw-dev liblensfun-dev libsqlite3-dev libjpeg-dev \
    libtiff-dev libpng-dev desktop-file-utils shared-mime-info \
    appstream
```

**Fedora:**
```
sudo dnf install gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
    glslc vulkan-headers vulkan-loader-devel vulkan-validation-layers \
    glfw-devel LibRaw-devel lensfun-devel sqlite-devel \
    libjpeg-turbo-devel libtiff-devel libpng-devel \
    desktop-file-utils shared-mime-info appstream
```

**Arch:**
```
sudo pacman -S base-devel meson ninja pkgconf shaderc vulkan-headers \
    vulkan-icd-loader vulkan-validation-layers glfw libraw lensfun \
    sqlite libjpeg-turbo libtiff libpng desktop-file-utils \
    shared-mime-info appstream
```

The remaining deps (`blake3`, `cimgui`, `lcms2`, `libpng`, `libtiff`, `nativefiledialog`, `tomlc99`) are vendored as meson wraps under `dep/*.wrap` and built from source — no system install needed for those.

Required meson version: **>=1.3.0** (see `meson.build` line 11). If your distro ships an older meson, install via `pip install --user meson` or use a venv.

## 2. Configure + build

```
meson setup build --buildtype=debug
meson compile -C build
```

For a release build use `--buildtype=release` instead. The first configure clones + builds the wraps (one-time, takes a few minutes); subsequent builds reuse them.

Useful flags:
- `meson setup --reconfigure build` — re-run configure after a `meson.build` change without nuking the build dir
- `meson configure build -Doption=value` — change build options without re-running setup
- `meson compile -C build --verbose` — see full compile commands when debugging build failures

If you need `compile_commands.json` for clangd / your editor's LSP, it's auto-generated at `build/compile_commands.json`. Symlink it to repo root if your tool wants it there.

## 3. Run

```
./build/aperture
```

That's it for the happy path. The binary opens against the last library it had open (or empty state on first run); use `File → Open Library` to point it somewhere.

For manual smoke tests, opening a small test library with a handful of RAW files is faster than opening a real photo library. Keep one around at e.g. `~/aperture-test-lib/` with 5-10 photos covering the formats you care about (CR2, NEF, ARW, DNG cover most cases).

## 4. Useful env vars

- `AP_LOG_LEVEL=debug` (or `info` / `warn` / `error`) — controls log verbosity. Debug surfaces import/render/io diagnostics that are silent otherwise.
- `VK_LOADER_DEBUG=all` — Vulkan loader verbose. Use only when debugging GPU-init failures.
- `SOURCE_DATE_EPOCH=<unix_ts>` — pin the AppStream `<release date>` to a specific timestamp at build configure time (per #383). Useful for reproducible packaging builds; ignore for development.

## 5. Install (optional, for manual install-target testing)

```
sudo meson install -C build
```

Installs to the system prefix (typically `/usr/local`). For a user-local install:

```
meson setup build --buildtype=release --prefix="$HOME/.local"
meson compile -C build
meson install -C build   # no sudo
```

`meson install --destdir=/tmp/stage` is useful for verifying the install layout without touching the real filesystem.

## Common failures

- `Native dependency 'X' not found` — missing system dep. Reread step 1.
- `meson version is too old` — install a newer meson per step 1.
- `cimgui` build error involving `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` — recurring quirk of the cimgui wrap. See the `roll-cimgui-wrap` skill if a roll is in scope; otherwise the existing pin should be fine.
- Vulkan validation layer errors at runtime that don't reproduce in release — set `AP_LOG_LEVEL=info` and check whether the issue is build-config-dependent before chasing it.
