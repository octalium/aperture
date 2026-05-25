#!/usr/bin/env bash
# Package Aperture.app into a distributable .dmg.
#
# Inputs (env vars, all optional unless noted):
#   APP_IN     source .app (default: $PWD/Aperture.app)
#   DMG_OUT    destination .dmg path (default: $PWD/aperture-<version>-arm64.dmg)
#   VERSION    version string baked into the .dmg filename and volume
#              label (default: derived from APP_IN's Info.plist
#              CFBundleShortVersionString)
#   ARCH       arch tag for the filename (default: arm64)
#
# Output:
#   $DMG_OUT.
#
# Tries create-dmg first (nicer drag-to-Applications layout). Falls back
# to hdiutil on a plain folder when create-dmg is not installed.

set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "build-dmg.sh requires macOS (got $(uname -s))" >&2
    exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)

APP_IN=${APP_IN:-$repo_root/Aperture.app}
ARCH=${ARCH:-arm64}

if [ ! -d "$APP_IN" ]; then
    echo "missing .app at $APP_IN - run build-app.sh first" >&2
    exit 1
fi

if [ -z "${VERSION:-}" ]; then
    plist="$APP_IN/Contents/Info.plist"
    if [ -f "$plist" ]; then
        VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist" 2>/dev/null || echo dev)
    else
        VERSION=dev
    fi
fi

DMG_OUT=${DMG_OUT:-$repo_root/aperture-${VERSION}-${ARCH}.dmg}
VOL_NAME="Aperture ${VERSION}"

rm -f "$DMG_OUT"

if command -v create-dmg >/dev/null 2>&1; then
    echo "==> packaging via create-dmg"
    # create-dmg returns 2 when it skips code-signing (we're unsigned by
    # design); treat that exit code as success. Background image is
    # intentionally omitted - no design asset exists yet, so we accept
    # create-dmg's default plain-white volume window.
    set +e
    create-dmg \
        --volname "$VOL_NAME" \
        --window-pos 200 120 \
        --window-size 600 320 \
        --icon-size 100 \
        --icon "Aperture.app" 175 120 \
        --hide-extension "Aperture.app" \
        --app-drop-link 425 120 \
        --no-internet-enable \
        "$DMG_OUT" \
        "$APP_IN"
    rc=$?
    set -e
    if [ $rc -ne 0 ] && [ $rc -ne 2 ]; then
        echo "create-dmg failed with exit code $rc" >&2
        exit $rc
    fi
else
    echo "==> create-dmg not installed; falling back to hdiutil"
    stage=$(mktemp -d)
    trap 'rm -rf "$stage"' EXIT
    cp -a "$APP_IN" "$stage/"
    ln -s /Applications "$stage/Applications"
    hdiutil create \
        -volname "$VOL_NAME" \
        -srcfolder "$stage" \
        -ov \
        -format UDZO \
        "$DMG_OUT"
fi

echo "==> wrote $DMG_OUT"
