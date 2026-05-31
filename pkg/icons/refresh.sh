#!/bin/sh
# regenerate the vendored hicolor PNGs from the scalable SVG.
# requires rsvg-convert (librsvg2-bin on debian/ubuntu).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_id=io.github.octalium.aperture
svg="$here/hicolor/scalable/apps/$app_id.svg"

for size in 16 32 48 64 128 256 512; do
  out_dir="$here/hicolor/${size}x${size}/apps"
  mkdir -p "$out_dir"
  rsvg-convert -w "$size" -h "$size" -o "$out_dir/$app_id.png" "$svg"
done
