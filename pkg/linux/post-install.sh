#!/bin/sh
# post-install refresh of desktop, icon, and MIME caches.
# skipped during staged installs (DESTDIR set) — those caches are
# refreshed by the packaging tooling on the target system, not here.

set -e

if [ -n "${DESTDIR:-}" ]; then
    exit 0
fi

datadir="${MESON_INSTALL_PREFIX:-/usr/local}/share"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q "${datadir}/applications" || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -qtf "${datadir}/icons/hicolor" || true
fi

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "${datadir}/mime" || true
fi
