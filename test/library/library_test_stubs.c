// Stubs for symbols the library references but the library test
// doesn't exercise — keeps the test binary off the heavy GPU /
// raw / encoder dependency surface.

#define _GNU_SOURCE

#include "io/raw.h"
#include "output/export.h"
#include "photo/thumbnail.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// only raw-extension check is reachable from library.c during scan.
// mirrors src/io/raw.c so the test sees the same set of extensions.
bool ap_raw_is_raw_path(const char *path)
{
    static const char *const RAW_EXTENSIONS[] = {
        ".nef", ".cr2", ".cr3", ".raf", ".arw",
        ".dng", ".orf", ".rw2", ".pef", ".srw",
        NULL,
    };
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    for (int i = 0; RAW_EXTENSIONS[i]; i++) {
        if (strcasecmp(dot, RAW_EXTENSIONS[i]) == 0) return true;
    }
    return false;
}

// library does not actually allocate thumbnails in these tests — every
// thumbs[] slot stays NULL. ap_thumbnail_destroy(NULL) is a no-op.
void ap_thumbnail_destroy(ap_thumbnail *t)
{
    (void)t;
}

// export_preset_list reads settings then overlays the blob. tests in
// this binary never list presets, so a zero-fill default is enough.
void ap_export_settings_load(const ap_library *lib, ap_export_settings *out)
{
    (void)lib;
    if (out) memset(out, 0, sizeof(*out));
}
