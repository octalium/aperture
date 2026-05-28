#!/usr/bin/env bash
# Refresh dep/sqlite3/sqlite3.{c,h} from sqlite.org's official amalgamation.
#
# Usage:
#   scripts/refresh-sqlite3.sh <version>
# Example:
#   scripts/refresh-sqlite3.sh 3.53.1
#
# Picks the autoconf-amalgamation tarball for the year matching the
# release. SQLite hosts each release at sqlite.org/<year>/, so the year
# is derived from the release announcements. Override by passing
# <version> <year> if the inference is wrong.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <version> [<year>]" >&2
  echo "example: $0 3.53.1" >&2
  exit 1
fi

version=$1
year=${2:-2026}

# sqlite encodes its version into the tarball name as XXYYZZAA:
# major*1000000 + minor*10000 + patch*100 + sub. e.g. 3.53.1 -> 3530100.
IFS='.' read -r major minor patch sub <<<"$version"
sub=${sub:-0}
encoded=$(printf '%d%02d%02d%02d' "$major" "$minor" "$patch" "$sub")

url="https://sqlite.org/${year}/sqlite-autoconf-${encoded}.tar.gz"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "fetching $url"
curl -fsSL -o "$tmp/sqlite.tar.gz" "$url"

echo "extracting"
tar -C "$tmp" -xzf "$tmp/sqlite.tar.gz"

src="$tmp/sqlite-autoconf-${encoded}"
dest="$(cd "$(dirname "$0")/.." && pwd)/dep/sqlite3"

install -m 0644 "$src/sqlite3.c" "$dest/sqlite3.c"
install -m 0644 "$src/sqlite3.h" "$dest/sqlite3.h"

echo "refreshed dep/sqlite3/ to $version"
echo "remember to bump the project() version in dep/sqlite3/meson.build"
