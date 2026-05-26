#!/usr/bin/env bash
# Generate a single-item Sparkle appcast.xml for a .dmg release artifact.
#
# Inputs (env vars):
#   DMG_PATH         path to the signed .dmg (required)
#   VERSION          short version string for sparkle:shortVersionString
#                    (required; the same value baked into Info.plist)
#   BUNDLE_VERSION   build number for sparkle:version (default: $VERSION)
#   DMG_URL          public download URL for the .dmg (required; e.g.
#                    https://github.com/octalium/aperture/releases/download/v0.2.0/aperture-0.2.0-arm64.dmg)
#   RELEASE_NOTES_URL  optional URL for sparkle:releaseNotesLink
#   MIN_SYSTEM       minimum macOS version (default: 11.0, matches Info.plist)
#   SPARKLE_PRIVATE_KEY  base64-encoded EdDSA private key (required;
#                        consumed via stdin by sign_update)
#   SIGN_UPDATE      override sign_update binary (default: discovered from
#                    packaging/macos/.sparkle-cache/Sparkle-*/bin/sign_update)
#   APPCAST_OUT      output path (default: $PWD/appcast.xml)
#
# Output: an appcast.xml at $APPCAST_OUT, ready to upload alongside the
# .dmg to the GitHub Release. The "latest"-tagged release's appcast.xml
# is what SUFeedURL in Info.plist points at.

set -euo pipefail

: "${DMG_PATH:?DMG_PATH is required}"
: "${VERSION:?VERSION is required}"
: "${DMG_URL:?DMG_URL is required}"
: "${SPARKLE_PRIVATE_KEY:?SPARKLE_PRIVATE_KEY is required}"

BUNDLE_VERSION=${BUNDLE_VERSION:-$VERSION}
MIN_SYSTEM=${MIN_SYSTEM:-11.0}
APPCAST_OUT=${APPCAST_OUT:-$PWD/appcast.xml}

if [ ! -f "$DMG_PATH" ]; then
    echo "missing .dmg at $DMG_PATH" >&2
    exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
if [ -z "${SIGN_UPDATE:-}" ]; then
    # Discover sign_update from the same Sparkle cache used by
    # fetch-sparkle.sh. The tarball ships it under bin/.
    cache_root="$here/.sparkle-cache"
    SIGN_UPDATE=$(find "$cache_root" -type f -name sign_update -perm -u+x 2>/dev/null | head -n 1 || true)
fi
if [ -z "${SIGN_UPDATE:-}" ] || [ ! -x "$SIGN_UPDATE" ]; then
    echo "sign_update binary not found; set SIGN_UPDATE or pre-fetch via fetch-sparkle.sh" >&2
    exit 1
fi

echo "==> signing $DMG_PATH with $SIGN_UPDATE" >&2
# sign_update reads the base64-encoded private key from stdin when
# --ed-key-file is set to '-', and prints exactly:
#   sparkle:edSignature="<sig>" length="<bytes>"
sig_line=$(printf '%s' "$SPARKLE_PRIVATE_KEY" | "$SIGN_UPDATE" --ed-key-file - "$DMG_PATH")

# RFC 822 pubDate; LC_TIME=C avoids locale-localised weekday names.
pub_date=$(LC_TIME=C date -u "+%a, %d %b %Y %H:%M:%S +0000")

release_notes_xml=""
if [ -n "${RELEASE_NOTES_URL:-}" ]; then
    release_notes_xml="        <sparkle:releaseNotesLink>${RELEASE_NOTES_URL}</sparkle:releaseNotesLink>"
fi

cat >"$APPCAST_OUT" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle"
     xmlns:dc="http://purl.org/dc/elements/1.1/"
     version="2.0">
  <channel>
    <title>Aperture</title>
    <link>https://github.com/octalium/aperture</link>
    <description>Most recent updates to Aperture</description>
    <language>en</language>
    <item>
      <title>Aperture ${VERSION}</title>
      <pubDate>${pub_date}</pubDate>
      <sparkle:version>${BUNDLE_VERSION}</sparkle:version>
      <sparkle:shortVersionString>${VERSION}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>${MIN_SYSTEM}</sparkle:minimumSystemVersion>
${release_notes_xml}
      <enclosure url="${DMG_URL}"
                 type="application/octet-stream"
                 ${sig_line} />
    </item>
  </channel>
</rss>
XML

echo "==> wrote $APPCAST_OUT" >&2
