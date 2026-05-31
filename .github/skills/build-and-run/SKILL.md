---
name: build-and-run
description: Build aperture from source and launch it for development or manual verification. Use this when implementing a feature/fix that needs a clean compile + interactive smoke test, or before opening a PR that touches runtime behavior. Covers Linux distro deps, meson setup, compile, and run; calls out the env vars worth knowing.
---

Aperture is a meson + ninja project. Most dependencies are vendored from source (git submodules under `dep/<name>/upstream/`, a couple of wrapdb wraps, and two in-tree copies); only a small set come from the system. The two things that bite people: forgetting to init the submodules, and missing system-side deps.

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

Everything else is vendored under `dep/` and built from source — no system install needed: most deps are **git submodules** (`cimgui`, `lcms2`, `cJSON`, `tomlc99`, `nativefiledialog`, `glfw`, `libraw`, `libjpeg-turbo`, `vulkan-headers`, `mbedtls`), `libpng`/`libtiff` are wrapdb wraps, and `blake3`/`sqlite3` are in-tree copies. Several `-dev` packages listed above (glfw, libraw, sqlite, libjpeg, libtiff, libpng) are now supplied from source and aren't strictly required as system packages — installing them is harmless, meson uses the vendored copies regardless. See `dep/README.md` for the full vendoring policy.

Required meson version: **>=1.3.0** (see `meson.build` line 11). If your distro ships an older meson, install via `pip install --user meson` or use a venv.

## 2. Fetch vendored sources + configure + build

The vendored deps are git submodules — init them once per clone (a fresh
checkout without this fails configure with `Include dir
upstream/include does not exist`):

```
git submodule update --init --recursive
```

Then:

```
meson setup build --buildtype=debug
meson compile -C build
```

For a release build use `--buildtype=release` instead. The first configure builds the vendored deps from source (one-time, takes a few minutes); subsequent builds reuse them.

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

- `SOURCE_DATE_EPOCH=<unix_ts>` — pin the AppStream `<release date>` to a specific timestamp at build configure time (per #383). Useful for reproducible packaging builds; ignore for development.
- `VK_LOADER_DEBUG=all` — Vulkan loader verbose, set in the runtime environment. Use only when debugging GPU-init failures.

Aperture's logger (`src/core/log.h`, `src/core/log.c`) currently has no level filtering — every `AP_INFO` / `AP_WARN` / `AP_ERROR` writes unconditionally; the `ap_log_level` value only picks stdout vs stderr and triggers exit on `AP_FATAL`. To quiet a specific subsystem during debugging, comment out call sites or guard them locally. A configurable level threshold would be a separate feature.

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

- `Include dir upstream/include does not exist` (or any missing `dep/<name>/upstream` path) — submodules not initialized. Run `git submodule update --init --recursive`.
- `Native dependency 'X' not found` — missing system dep. Reread step 1.
- `meson version is too old` — install a newer meson per step 1.
- `cimgui` build error involving `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` — a known cimgui quirk; the submodule pin in `dep/cimgui/upstream` should build cleanly. Re-pin (`git -C dep/cimgui/upstream checkout <sha>`) only with a deliberate test.
- Vulkan validation layer errors at runtime that don't reproduce in release — likely a debug-build-only assertion. Read the validation message carefully before chasing.

---
*If something in this skill looks wrong, the source code is authoritative. Verify against the current tree and update this skill in the same PR.*
