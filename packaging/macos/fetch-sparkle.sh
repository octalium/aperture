#!/usr/bin/env bash
# Fetch and stage Sparkle.framework for embedding into Aperture.app.
#
# Sparkle ships pre-built as a .tar.xz containing Sparkle.framework
# (plus tools: generate_keys, sign_update, generate_appcast). We extract
# the framework to a cached directory and print its absolute path so
# build-app.sh can copy it into the .app bundle.
#
# Inputs (env vars, all optional):
#   SPARKLE_VERSION  release tag to fetch (default: pinned below)
#   SPARKLE_CACHE    cache dir for the extracted tarball
#                    (default: packaging/macos/.sparkle-cache)
#
# Output (stdout, single line):
#   absolute path to the extracted Sparkle.framework directory
#
# The tools/ directory inside the same cache (sign_update,
# generate_keys, generate_appcast) is what the release workflow uses
# during the signing step.
#
# Requires: curl, tar with xz support.

set -euo pipefail

SPARKLE_VERSION=${SPARKLE_VERSION:-2.9.2}

here=$(cd "$(dirname "$0")" && pwd)
SPARKLE_CACHE=${SPARKLE_CACHE:-$here/.sparkle-cache}

mkdir -p "$SPARKLE_CACHE"

tarball="$SPARKLE_CACHE/Sparkle-${SPARKLE_VERSION}.tar.xz"
extract_dir="$SPARKLE_CACHE/Sparkle-${SPARKLE_VERSION}"
framework_dir="$extract_dir/Sparkle.framework"

if [ ! -d "$framework_dir" ]; then
    if [ ! -f "$tarball" ]; then
        url="https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
        echo "==> fetching $url" >&2
        curl -fsSL --retry 3 -o "$tarball.part" "$url"
        mv "$tarball.part" "$tarball"
    fi
    echo "==> extracting Sparkle ${SPARKLE_VERSION}" >&2
    rm -rf "$extract_dir"
    mkdir -p "$extract_dir"
    tar -xJf "$tarball" -C "$extract_dir"
    if [ ! -d "$framework_dir" ]; then
        echo "Sparkle tarball did not contain Sparkle.framework at $framework_dir" >&2
        exit 1
    fi
fi

echo "$framework_dir"
