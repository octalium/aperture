#!/usr/bin/env bash
# Build an aperture AppImage from a release-configured meson tree.
#
# Inputs (env vars, all optional unless noted):
#   BUILD_DIR        meson build dir (default: build)
#   STAGE_DIR        staging prefix for the AppDir contents (default: $BUILD_DIR/AppDir)
#   APPIMAGE_OUT     destination .AppImage path (default: $PWD/aperture-<version>-x86_64.AppImage)
#   ARCH             target arch tag (default: x86_64)
#   LINUXDEPLOY      override linuxdeploy binary (default: linuxdeploy)
#   APPIMAGETOOL     override appimagetool binary (default: appimagetool — embedded in linuxdeploy via --output appimage)
#   VERSION          version string baked into the AppImage filename (default: project version from meson)
#   UPDATE_INFO      AppImageUpdate update-info string (default: gh-releases-zsync|octalium|aperture|latest|aperture-*x86_64.AppImage.zsync)
#
# Output:
#   $APPIMAGE_OUT and (when zsyncmake is available) $APPIMAGE_OUT.zsync.
#
# linuxdeploy bakes the UPDATE_INFO string into the AppImage runtime
# header at build time so AppImageUpdate (issue #418) can discover
# the latest release without parsing release metadata at runtime.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)

BUILD_DIR=${BUILD_DIR:-build}
STAGE_DIR=${STAGE_DIR:-$BUILD_DIR/AppDir}
ARCH=${ARCH:-x86_64}
LINUXDEPLOY=${LINUXDEPLOY:-linuxdeploy}
UPDATE_INFO=${UPDATE_INFO:-"gh-releases-zsync|octalium|aperture|latest|aperture-*${ARCH}.AppImage.zsync"}

if [ ! -d "$BUILD_DIR" ]; then
    echo "build dir '$BUILD_DIR' not found — run meson setup + compile first" >&2
    exit 1
fi

# meson introspection: pull the project version so the artifact filename
# matches the build it came from. Falls back to "dev" if introspection
# fails (e.g. older meson, but >=1.3.0 is required by the project).
if [ -z "${VERSION:-}" ]; then
    VERSION=$(meson introspect "$BUILD_DIR" --projectinfo | python3 -c 'import json,sys; print(json.load(sys.stdin).get("version","dev"))')
fi

APPIMAGE_OUT=${APPIMAGE_OUT:-$repo_root/aperture-${VERSION}-${ARCH}.AppImage}

echo "==> staging install into $STAGE_DIR"
rm -rf "$STAGE_DIR"
stage_abs=$(cd "$(dirname "$STAGE_DIR")" 2>/dev/null && pwd)/$(basename "$STAGE_DIR")
mkdir -p "$stage_abs"
DESTDIR="$stage_abs" meson install -C "$BUILD_DIR" --quiet

# meson install lays things under $DESTDIR$prefix; linuxdeploy wants the
# AppDir as a self-contained tree rooted at /usr. Move into place when
# the prefix wasn't /usr, otherwise leave alone.
prefix=$(meson introspect "$BUILD_DIR" --buildoptions | python3 -c 'import json,sys; print(next(o["value"] for o in json.load(sys.stdin) if o["name"]=="prefix"))')
if [ "$prefix" != "/usr" ]; then
    # promote the prefixed tree to /usr inside the AppDir
    if [ -d "$STAGE_DIR$prefix" ]; then
        mkdir -p "$STAGE_DIR/usr"
        cp -a "$STAGE_DIR$prefix/." "$STAGE_DIR/usr/"
        rm -rf "${STAGE_DIR:?}$prefix"
    fi
fi

# linuxdeploy expects desktop + icon at the AppDir root in addition to
# /usr/share. it bootstraps those copies itself when pointed at them.
desktop_file="$STAGE_DIR/usr/share/applications/io.github.octalium.aperture.desktop"
icon_file_svg="$STAGE_DIR/usr/share/icons/hicolor/scalable/apps/io.github.octalium.aperture.svg"
icon_file_256="$STAGE_DIR/usr/share/icons/hicolor/256x256/apps/io.github.octalium.aperture.png"

if [ ! -f "$desktop_file" ]; then
    echo "missing desktop file at $desktop_file" >&2
    exit 1
fi

# linuxdeploy resolves Exec= literally; the installed entry uses an
# absolute /usr/bin/aperture path, which it can't locate inside the
# AppDir. Strip the path so it matches the bare binary name.
sed -i 's|^Exec=/.*/aperture|Exec=aperture|' "$desktop_file"

# prefer the rasterized 256px icon for the AppImage thumbnail since the
# .DirIcon embedded by linuxdeploy must be a PNG; the SVG is kept inside
# /usr/share for desktops that prefer scalable.
icon_for_appimage=$icon_file_256
if [ ! -f "$icon_for_appimage" ] && [ -f "$icon_file_svg" ]; then
    icon_for_appimage=$icon_file_svg
fi

echo "==> running linuxdeploy"
cd "$repo_root"
export UPDATE_INFORMATION=$UPDATE_INFO
export VERSION
export OUTPUT
OUTPUT=$(basename "$APPIMAGE_OUT")

# pass --executable explicitly: the staged desktop file has an absolute
# Exec=/usr/bin/aperture path, and linuxdeploy's resolver only handles
# bare executable names. Pointing at the binary inside the AppDir tells
# it which file to deploy without needing to rewrite the desktop entry.
aperture_bin="$STAGE_DIR/usr/bin/aperture"
if [ ! -x "$aperture_bin" ]; then
    echo "missing aperture binary at $aperture_bin" >&2
    exit 1
fi

"$LINUXDEPLOY" \
    --appdir "$STAGE_DIR" \
    --executable "$aperture_bin" \
    --desktop-file "$desktop_file" \
    --icon-file "$icon_for_appimage" \
    --output appimage

# linuxdeploy writes the .AppImage (and .zsync if zsyncmake is on PATH)
# into the current working directory using $OUTPUT as the basename.
produced="$repo_root/$OUTPUT"
if [ "$produced" != "$APPIMAGE_OUT" ]; then
    mv "$produced" "$APPIMAGE_OUT"
    if [ -f "$produced.zsync" ]; then
        mv "$produced.zsync" "$APPIMAGE_OUT.zsync"
    fi
fi

echo "==> wrote $APPIMAGE_OUT"
if [ -f "$APPIMAGE_OUT.zsync" ]; then
    echo "==> wrote $APPIMAGE_OUT.zsync"
else
    echo "note: zsyncmake not found on PATH; .zsync sidecar not generated" >&2
fi
