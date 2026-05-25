# aperture

An opinionated raw photo processor. C + Vulkan + Dear ImGui.

A focused, non-destructive editor for photographers who find existing FOSS
raw processors workflow-wrong or architecturally compromised. Narrow scope
by design — feature parity with mature tools is explicitly not the goal.

aperture's job is library management, culling, grading, and export.
Anything that needs pixel-level retouching belongs in a dedicated editor
afterward.

See [SPEC.md](SPEC.md) for design, scope, and stack rationale.

## Status

Pre-v0. Implementation is underway on `dev`; `main` carries the
specification only. No tagged releases yet.

## Install (Linux)

Aperture builds with [Meson](https://mesonbuild.com/) + [Ninja](https://ninja-build.org/)
under a thin Makefile wrapper. The install drops a desktop entry, hicolor
icon, AppStream metainfo, and MIME associations for the raw formats it
reads (CR2, CR3, NEF, ARW, RAF, DNG, ORF, RW2, PEF, SRW).

### 1. Distro dependencies

System packages providing the dependencies pulled in via `pkg-config`. The
remaining deps (lcms2, libpng, libtiff, cimgui, tomlc99, blake3,
nativefiledialog) are vendored as meson wraps and built from source.

**Debian / Ubuntu**

```
sudo apt install build-essential meson ninja-build pkg-config \
    glslc libvulkan-dev vulkan-validationlayers libglfw3-dev \
    libraw-dev liblensfun-dev libsqlite3-dev libjpeg-dev \
    libtiff-dev libpng-dev desktop-file-utils shared-mime-info \
    appstream
```

**Fedora**

```
sudo dnf install gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
    glslc vulkan-headers vulkan-loader-devel vulkan-validation-layers \
    glfw-devel LibRaw-devel lensfun-devel sqlite-devel \
    libjpeg-turbo-devel libtiff-devel libpng-devel \
    desktop-file-utils shared-mime-info appstream
```

**Arch**

```
sudo pacman -S base-devel meson ninja pkgconf shaderc vulkan-headers \
    vulkan-icd-loader vulkan-validation-layers glfw libraw lensfun \
    sqlite libjpeg-turbo libtiff libpng desktop-file-utils \
    shared-mime-info appstream
```

### 2. Build and install

System-wide (`/usr/local`):

```
make install
```

Per-user (`$HOME/.local`), no root required:

```
make install-user
```

Or just build without installing and run from the build dir:

```
make run
```

`make` on its own does an incremental debug build. `make release` produces
a release build in a parallel `build-release/` directory. `make clean`
removes the build dirs.

When installing into `$HOME/.local`, make sure `$HOME/.local/bin` is on
your `PATH` and `$HOME/.local/share` is in `$XDG_DATA_DIRS` so the
desktop entry, icon, and MIME types are picked up.

### Desktop / icon / MIME cache refresh

A non-staged `make install` runs the refresh commands for you. For
staged installs (`DESTDIR=…`), or after manually copying files into
place, refresh the caches yourself:

```
update-desktop-database "$PREFIX/share/applications"
gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor"
update-mime-database "$PREFIX/share/mime"
```

User data (library registry) lives under `$XDG_DATA_HOME/aperture`
(default `~/.local/share/aperture`) and UI configuration (imgui layout)
under `$XDG_CONFIG_HOME/aperture` (default `~/.config/aperture`),
regardless of install prefix.

### Manual meson

The `Makefile` is a thin wrapper. For non-default flags (cross-compile,
sanitizers, custom prefix, custom build dir), drive meson directly:

```
meson setup build --buildtype=release --prefix=/opt/aperture
meson compile -C build
meson install -C build
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the dev loop, branch policy,
and commit convention.

## License

[MIT](LICENSE).
