#include "panels.h"

#include "app/app.h"
#include "output/export.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Export-mode Naming panel: chooses whether the output keeps the
// source filename or is renamed by a token pattern, and shows a live
// preview of the resulting filename for the open photo.

static void source_stem(const ap_photo *photo, char *out, size_t out_len)
{
    const char *path = photo ? ap_photo_path(photo) : NULL;
    if (!path) {
        snprintf(out, out_len, "photo");
        return;
    }
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    snprintf(out, out_len, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

static void export_naming_draw(ap_app *app)
{
    if (!app) return;
    ap_export_settings *s = ap_app_export_settings(app);
    if (!s) return;

    if (!igBegin("Naming##export", NULL, 0)) {
        igEnd();
        return;
    }

    igRadioButton_IntPtr("Keep original name", &s->naming,
                         AP_EXPORT_NAME_KEEP);
    igRadioButton_IntPtr("Rename by pattern", &s->naming,
                         AP_EXPORT_NAME_PATTERN);

    if (s->naming == AP_EXPORT_NAME_PATTERN) {
        igSpacing();
        igText("Pattern");
        igSetNextItemWidth(-1.0f);
        igInputText("##pattern", s->pattern, sizeof(s->pattern),
                    0, NULL, NULL);
        igTextDisabled("tokens: {ORIG} {YYYY} {MM} {DD} "
                       "{HH} {MIN} {SEC} {SEQ}");
    }

    igSeparator();

    // Live preview of the output filename for the open photo.
    ap_photo *photo = ap_app_photo(app);
    char stem[256];
    source_stem(photo, stem, sizeof(stem));

    char out_stem[512];
    ap_export_format_stem(s, stem, time(NULL), 1,
                          out_stem, sizeof(out_stem));
    const char *ext = ap_export_format_extension(s->format);

    igText("Output filename");
    igTextWrapped("%s.%s", out_stem, ext);

    igEnd();
}

const ap_panel panel_export_naming = {
    .name = "export_naming",
    .mode = AP_MODE_EXPORT,
    .draw = export_naming_draw,
};
