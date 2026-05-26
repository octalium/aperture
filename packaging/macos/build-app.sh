#!/usr/bin/env bash
# Build Aperture.app from a release-configured meson tree on macOS.
#
# Inputs (env vars, all optional unless noted):
#   BUILD_DIR          meson build dir (default: build)
#   STAGE_DIR          staging prefix for meson install (default: $BUILD_DIR/stage)
#   APP_OUT            destination .app path (default: $PWD/Aperture.app)
#   VERSION            version string (default: project version from meson)
#   DYLIBBUNDLER       override dylibbundler binary (default: dylibbundler)
#   SOURCE_DATE_EPOCH  when set, every file inside $APP_OUT is touched
#                      to this epoch for reproducible mtimes.
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

cp "$aperture_bin" "$APP_OUT/Contents/MacOS/aperture-bin"
cp "$info_plist"   "$APP_OUT/Contents/Info.plist"

# CFBundleExecutable points at "aperture", which is a shell launcher
# that sets VK_ICD_FILENAMES so the bundled Vulkan loader discovers the
# bundled MoltenVK ICD (instead of searching brew's prefix or failing
# outright). The real ELF binary is "aperture-bin" sitting next to it.
cat > "$APP_OUT/Contents/MacOS/aperture" <<'LAUNCHER'
#!/bin/sh
here=$(cd "$(dirname "$0")" && pwd)
export VK_ICD_FILENAMES="$here/../Resources/vulkan/icd.d/MoltenVK_icd.json"
exec "$here/aperture-bin" "$@"
LAUNCHER
chmod +x "$APP_OUT/Contents/MacOS/aperture"

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
# the destination Frameworks dir, -x is the binary to walk. The runner is
# Apple Silicon (macos-14) so brew lives under /opt/homebrew; -s adds it
# to the search path so dylibbundler picks up sibling deps
# (e.g. lensfun -> liblensfun.1.dylib).
"$DYLIBBUNDLER" \
    -of -cd -b \
    -x "$APP_OUT/Contents/MacOS/aperture-bin" \
    -d "$APP_OUT/Contents/Frameworks" \
    -p "@executable_path/../Frameworks/" \
    -s /opt/homebrew/lib

# Sparkle.framework: fetched on demand by fetch-sparkle.sh (also
# invoked at meson configure time). cp -a preserves the versioned
# framework symlinks; codesign and the dyld loader both require the
# canonical Versions/A/Sparkle layout.
echo "==> staging Sparkle.framework"
sparkle_framework=$("$here/fetch-sparkle.sh")
if [ ! -d "$sparkle_framework" ]; then
    echo "fetch-sparkle.sh did not produce a framework at $sparkle_framework" >&2
    exit 1
fi
rm -rf "$APP_OUT/Contents/Frameworks/Sparkle.framework"
cp -a "$sparkle_framework" "$APP_OUT/Contents/Frameworks/Sparkle.framework"

# MoltenVK ships an ICD JSON (MoltenVK_icd.json) plus the libMoltenVK
# dylib. The Vulkan loader dlopens MoltenVK at runtime via the JSON's
# library_path; dylibbundler only follows direct link-time deps so it
# won't pick the dylib up on its own. Copy both: the dylib into
# Frameworks/ and the JSON into Resources/vulkan/icd.d/ with a relative
# library_path that points back at the bundled dylib.
moltenvk_icd=""
moltenvk_dylib=""
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
for candidate in \
    /opt/homebrew/lib/libMoltenVK.dylib \
    /opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib \
    /usr/local/lib/libMoltenVK.dylib
do
    if [ -f "$candidate" ]; then
        moltenvk_dylib=$candidate
        break
    fi
done

if [ -z "$moltenvk_dylib" ]; then
    echo "error: libMoltenVK.dylib not found; brew install molten-vk on the build host" >&2
    exit 1
fi
if [ -z "$moltenvk_icd" ]; then
    echo "error: MoltenVK_icd.json not found; the molten-vk brew formula should ship it" >&2
    exit 1
fi
cp "$moltenvk_dylib" "$APP_OUT/Contents/Frameworks/libMoltenVK.dylib"
mkdir -p "$APP_OUT/Contents/Resources/vulkan/icd.d"
cp "$moltenvk_icd" "$APP_OUT/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
# rewrite library_path to the bundled MoltenVK dylib. The path is
# resolved relative to the JSON file (Contents/Resources/vulkan/
# icd.d/), so it climbs three directories before descending into
# Frameworks/.
python3 - "$APP_OUT/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as f:
    data = json.load(f)
data.setdefault('ICD', {})['library_path'] = '../../../Frameworks/libMoltenVK.dylib'
with open(path, 'w') as f:
    json.dump(data, f, indent=4)
PY

# pin every file in the bundle to SOURCE_DATE_EPOCH for reproducible
# mtimes (matches the compile step). Skipped silently when the env var
# isn't set so the developer dev loop stays untouched.
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    echo "==> pinning bundle mtimes to SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    find "$APP_OUT" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
fi

echo "==> wrote $APP_OUT (version $VERSION)"
