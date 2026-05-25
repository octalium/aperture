#!/usr/bin/env bash
# Build Aperture.app from a release-configured meson tree on macOS.
#
# Inputs (env vars, all optional unless noted):
#   BUILD_DIR     meson build dir (default: build)
#   STAGE_DIR     staging prefix for meson install (default: $BUILD_DIR/stage)
#   APP_OUT       destination .app path (default: $PWD/Aperture.app)
#   VERSION       version string (default: project version from meson)
#   DYLIBBUNDLER  override dylibbundler binary (default: dylibbundler)
#
# Output:
#   $APP_OUT, a self-contained Aperture.app with brew-sourced dylibs
#   (libraw, lensfun, libjpeg-turbo, glfw, MoltenVK, ...) rewritten to
#   resolve under @executable_path/../Frameworks/.
#
# Requires: macOS, brew install dylibbundler, brew install molten-vk.

set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "build-app.sh requires macOS (got $(uname -s))" >&2
    exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)

BUILD_DIR=${BUILD_DIR:-build}
STAGE_DIR=${STAGE_DIR:-$BUILD_DIR/stage}
DYLIBBUNDLER=${DYLIBBUNDLER:-dylibbundler}

if [ ! -d "$BUILD_DIR" ]; then
    echo "build dir '$BUILD_DIR' not found - run meson setup + compile first" >&2
    exit 1
fi

if [ -z "${VERSION:-}" ]; then
    VERSION=$(meson introspect "$BUILD_DIR" --projectinfo \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("version","dev"))')
fi

APP_OUT=${APP_OUT:-$repo_root/Aperture.app}

echo "==> staging install into $STAGE_DIR"
rm -rf "$STAGE_DIR"
stage_abs=$(cd "$(dirname "$STAGE_DIR")" 2>/dev/null && pwd)/$(basename "$STAGE_DIR")
mkdir -p "$stage_abs"
DESTDIR="$stage_abs" meson install -C "$BUILD_DIR" --quiet

prefix=$(meson introspect "$BUILD_DIR" --buildoptions \
    | python3 -c 'import json,sys; print(next(o["value"] for o in json.load(sys.stdin) if o["name"]=="prefix"))')
staged_prefix="$STAGE_DIR$prefix"

aperture_bin="$staged_prefix/bin/aperture"
info_plist="$staged_prefix/share/aperture/macos/Info.plist"

if [ ! -x "$aperture_bin" ]; then
    echo "missing aperture binary at $aperture_bin" >&2
    exit 1
fi
if [ ! -f "$info_plist" ]; then
    echo "missing staged Info.plist at $info_plist (packaging/macos/meson.build not invoked?)" >&2
    exit 1
fi

echo "==> laying out $APP_OUT"
rm -rf "$APP_OUT"
mkdir -p "$APP_OUT/Contents/MacOS"
mkdir -p "$APP_OUT/Contents/Frameworks"
mkdir -p "$APP_OUT/Contents/Resources"

cp "$aperture_bin" "$APP_OUT/Contents/MacOS/aperture"
cp "$info_plist"   "$APP_OUT/Contents/Info.plist"

# generate aperture.icns from the existing rasterized linux icon. iconutil
# wants an .iconset directory with the standard size + @2x variants; we
# only have a single 256px master so the iconset is sparse, but iconutil
# accepts that and macOS falls back to the closest match.
iconset=$(mktemp -d)
src_icon="$repo_root/packaging/linux/icons/hicolor/256x256/apps/io.github.octalium.aperture.png"
if [ -f "$src_icon" ]; then
    cp "$src_icon" "$iconset/icon_256x256.png"
    sips -z 128 128 "$src_icon" --out "$iconset/icon_128x128.png" >/dev/null
    sips -z 64  64  "$src_icon" --out "$iconset/icon_64x64.png"   >/dev/null
    sips -z 32  32  "$src_icon" --out "$iconset/icon_32x32.png"   >/dev/null
    sips -z 16  16  "$src_icon" --out "$iconset/icon_16x16.png"   >/dev/null
    mv "$iconset" "${iconset}.iconset"
    iconutil --convert icns --output "$APP_OUT/Contents/Resources/aperture.icns" "${iconset}.iconset"
    rm -rf "${iconset}.iconset"
else
    echo "note: source icon $src_icon not found; .app will use the system default" >&2
    rm -rf "$iconset"
fi

# preserve AppStream metainfo + MIME xml inside the bundle for tooling
# that scans Resources/ (Sparkle reads metainfo for release notes).
if [ -d "$staged_prefix/share/metainfo" ]; then
    cp -a "$staged_prefix/share/metainfo" "$APP_OUT/Contents/Resources/"
fi

echo "==> bundling dylibs via $DYLIBBUNDLER"
# -of overwrites existing rewrites, -cd copies non-system deps, -b runs
# the bundler, -p sets the rpath embedded into the binary, -d points at
# the destination Frameworks dir, -x is the binary to walk. Brew installs
# under /opt/homebrew on Apple Silicon; -s adds it to the search path
# explicitly so dylibbundler picks up sibling deps (e.g. lensfun ->
# liblensfun.1.dylib).
"$DYLIBBUNDLER" \
    -of -cd -b \
    -x "$APP_OUT/Contents/MacOS/aperture" \
    -d "$APP_OUT/Contents/Frameworks" \
    -p "@executable_path/../Frameworks/" \
    -s /opt/homebrew/lib \
    -s /usr/local/lib

# MoltenVK ships an ICD JSON (MoltenVK_icd.json) that the Vulkan loader
# discovers via VK_ICD_FILENAMES. Bundle it next to the dylib so the
# loader inside the .app finds it without leaking out to brew's prefix.
moltenvk_icd=""
for candidate in \
    /opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json \
    /opt/homebrew/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json \
    /usr/local/share/vulkan/icd.d/MoltenVK_icd.json
do
    if [ -f "$candidate" ]; then
        moltenvk_icd=$candidate
        break
    fi
done

if [ -n "$moltenvk_icd" ]; then
    mkdir -p "$APP_OUT/Contents/Resources/vulkan/icd.d"
    cp "$moltenvk_icd" "$APP_OUT/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
    # rewrite library_path to the bundled MoltenVK dylib so the loader
    # inside the .app resolves it under Frameworks/ rather than brew.
    python3 - "$APP_OUT/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as f:
    data = json.load(f)
data.setdefault('ICD', {})['library_path'] = '../../../Frameworks/libMoltenVK.dylib'
with open(path, 'w') as f:
    json.dump(data, f, indent=4)
PY
else
    echo "warning: MoltenVK_icd.json not found; brew install molten-vk on the build host" >&2
fi

echo "==> wrote $APP_OUT (version $VERSION)"
