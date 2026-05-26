# aperture

An opinionated raw photo processor. C + Vulkan + Dear ImGui.

A focused, non-destructive editor for photographers who find existing FOSS
raw processors workflow-wrong or architecturally compromised. Narrow scope
by design — feature parity with mature tools is explicitly not the goal.

aperture's job is library management, culling, grading, and export.
Anything that needs pixel-level retouching belongs in a dedicated editor
afterward.

## Status

Pre-v0. Implementation is underway on `dev`; `main` carries the
specification only. No tagged releases yet.

## Install (Linux)

Once tagged releases ship, the easiest paths will be:

- **Flatpak** (primary channel, via Flathub once submission lands).
- **AppImage** (single-file fallback, attached to each GitHub Release).

Both artifacts are produced by `.github/workflows/release-linux.yml` on
`v*` tag pushes. The packaging sources live under `packaging/` — see
`packaging/flatpak/FLATHUB.md` for submission notes.

## Install (macOS)

An unsigned arm64 `.dmg` (containing `Aperture.app`) is attached to each
GitHub Release by `.github/workflows/release-macos.yml`. macOS 11 Big
Sur or newer; Apple Silicon only. Because the build is not Developer-ID
signed, the first launch needs a right-click → Open to bypass the
Gatekeeper "unidentified developer" warning. See
`packaging/macos/README.md` for build-from-source instructions.

For development or distros where the packaged artifacts aren't an option,
aperture builds from source with [Meson](https://mesonbuild.com/) +
[Ninja](https://ninja-build.org/) and ships a `meson install` target that
lays down a desktop entry, hicolor icon, AppStream metainfo, and MIME
associations for the raw formats it reads (CR2, CR3, NEF, ARW, RAF, DNG,
ORF, RW2, PEF, SRW).

### Distro dependencies

System packages providing the dependencies pulled in via `pkg-config`. The
remaining deps (lcms2, libpng, libtiff, cimgui, tomlc99, blake3, cJSON,
nativefiledialog) are vendored as meson wraps and built from source.

**Debian / Ubuntu**

```
sudo apt install build-essential meson ninja-build pkg-config \
    glslc libvulkan-dev vulkan-validationlayers libglfw3-dev \
    libraw-dev liblensfun-dev libsqlite3-dev libjpeg-dev \
    libtiff-dev libpng-dev libcurl4-openssl-dev \
    desktop-file-utils shared-mime-info appstream
```

**Fedora**

```
sudo dnf install gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
    glslc vulkan-headers vulkan-loader-devel vulkan-validation-layers \
    glfw-devel LibRaw-devel lensfun-devel sqlite-devel \
    libjpeg-turbo-devel libtiff-devel libpng-devel libcurl-devel \
    desktop-file-utils shared-mime-info appstream
```

**Arch**

```
sudo pacman -S base-devel meson ninja pkgconf shaderc vulkan-headers \
    vulkan-icd-loader vulkan-validation-layers glfw libraw lensfun \
    sqlite libjpeg-turbo libtiff libpng curl desktop-file-utils \
    shared-mime-info appstream
```

### Build and install

System-wide (`/usr/local`):

```
make build
sudo make install
```

Equivalent direct meson invocation:

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

User data (library registry) lives under `$XDG_DATA_HOME/aperture`
(default `~/.local/share/aperture`) and UI configuration (imgui layout)
under `$XDG_CONFIG_HOME/aperture` (default `~/.config/aperture`),
regardless of install prefix.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the dev loop, branch policy,
and commit convention.

## License

[MIT](LICENSE).
