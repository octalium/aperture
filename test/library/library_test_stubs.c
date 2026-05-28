// Stubs for symbols the library references but the library test
// doesn't exercise — keeps the test binary off the heavy GPU /
// raw / encoder dependency surface.

// ap_raw_is_raw_path is supplied as a static inline by io/raw_exts.h
// (pulled in transitively via io/raw.h), so the test sees the same
// extension list as production with no manual mirroring.

#include "io/raw.h"
#include "output/export.h"
#include "photo/thumbnail.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
