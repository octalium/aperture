# aperture

An opinionated raw photo processor. C + Vulkan + Dear ImGui.

A focused, non-destructive editor for photographers who find existing FOSS
raw processors workflow-wrong or architecturally compromised. Narrow scope
by design — feature parity with mature tools is explicitly not the goal.

See [SPEC.md](SPEC.md) for design, scope, and stack rationale.

## Status

Pre-v0. Specification only.

## Install (Linux)

Aperture builds with [Meson](https://mesonbuild.com/) + [Ninja](https://ninja-build.org/)
and ships a `meson install` target that lays down a desktop entry, hicolor
icon, AppStream metainfo, and MIME associations for the raw formats it
reads (CR2, CR3, NEF, ARW, RAF, DNG, ORF, RW2, PEF, SRW).

### Distro dependencies

System packages providing the dependencies pulled in via `pkg-config`. The
remaining deps (lcms2, libpng, libtiff, cimgui, tomlc99, blake3,
nativefiledialog) are vendored as meson wraps and built from source.

**Debian / Ubuntu**

```
sudo apt install build-essential meson ninja-build pkg-config \
    glslc libvulkan-dev libglfw3-dev libraw-dev liblensfun-dev \
    libsqlite3-dev libjpeg-dev desktop-file-utils shared-mime-info \
    appstream
```

**Fedora**

```
sudo dnf install gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
    glslc vulkan-headers vulkan-loader-devel glfw-devel LibRaw-devel \
    lensfun-devel sqlite-devel libjpeg-turbo-devel desktop-file-utils \
    shared-mime-info appstream
```

**Arch**

```
sudo pacman -S base-devel meson ninja pkgconf shaderc vulkan-headers \
    vulkan-icd-loader glfw libraw lensfun sqlite libjpeg-turbo \
    desktop-file-utils shared-mime-info appstream
```

### Build and install

System-wide (`/usr/local`):

```
meson setup build --buildtype=release
meson compile -C build
sudo meson install -C build
```

Per-user, no root required:

```
meson setup build --buildtype=release --prefix="$HOME/.local"
meson compile -C build
meson install -C build
```

When installing into `$HOME/.local`, make sure `$HOME/.local/bin` is on
your `PATH` and that `$HOME/.local/share` is in `$XDG_DATA_DIRS` so the
desktop entry, icon, and MIME types are picked up.

### Desktop / icon / MIME cache refresh

A non-staged `meson install` runs the refresh commands for you. For
staged installs (`DESTDIR=…`), or after manually copying files into
place, refresh the caches yourself:

```
update-desktop-database "$PREFIX/share/applications"
gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor"
update-mime-database "$PREFIX/share/mime"
```

User data (library registry, imgui layout) lives under
`$XDG_DATA_HOME/aperture` (default `~/.local/share/aperture`) regardless
of install prefix.
