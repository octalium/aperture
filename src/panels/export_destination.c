#include "panels.h"

#include "app/app.h"
#include "library/library.h"
#include "output/export.h"
#include "photo/photo.h"
#include "ui/file_dialog.h"

#include "cimgui.h"

#include <stdio.h>

// Export-mode Destination panel: chooses where the exported file
// lands, how filename collisions are handled, and carries the Export
// action button. The resolved output directory is previewed live.

static void export_destination_draw(ap_app *app)
{
    if (!app) return;
    ap_export_settings *s = ap_app_export_settings(app);
    if (!s) return;

    if (!igBegin("Destination##export", NULL, 0)) {
        igEnd();
        return;
    }

    ap_library *lib   = ap_app_library(app);
    ap_photo   *photo = ap_app_photo(app);

    igText("Export to");
    igRadioButton_IntPtr("Beside the source file", &s->destination,
                         AP_EXPORT_DEST_BESIDE);
    // The library subdir option only makes sense with a library open.
    bool have_lib = (lib != NULL);
    if (!have_lib) igBeginDisabled(true);
    igRadioButton_IntPtr("Library subfolder", &s->destination,
                         AP_EXPORT_DEST_SUBDIR);
    if (!have_lib) igEndDisabled();
    igRadioButton_IntPtr("Custom folder", &s->destination,
                         AP_EXPORT_DEST_CUSTOM);

    igSpacing();

    if (s->destination == AP_EXPORT_DEST_SUBDIR) {
        igText("Subfolder name");
        igSetNextItemWidth(-1.0f);
        igInputText("##dest_subdir", s->dest_subdir,
                    sizeof(s->dest_subdir), 0, NULL, NULL);
        igTextDisabled("relative to the library root");
    } else if (s->destination == AP_EXPORT_DEST_CUSTOM) {
        igText("Folder");
        igTextDisabled("%s", s->dest_dir[0] ? s->dest_dir
                                            : "(none chosen)");
        if (igButton("Choose Folder...", (ImVec2_c){ 0.0f, 0.0f })) {
            char picked[AP_EXPORT_DEST_LEN];
            const char *seed = s->dest_dir[0] ? s->dest_dir : NULL;
            if (ap_file_dialog_pick_folder(picked, sizeof(picked), seed)) {
                snprintf(s->dest_dir, sizeof(s->dest_dir), "%s", picked);
            }
        }
    }

    igSeparator();

    igText("On name collision");
    static const char *const collide_items[] = {
        "Overwrite", "Auto-suffix", "Skip",
    };
    igSetNextItemWidth(-1.0f);
    igCombo_Str_arr("##collision", &s->collision, collide_items, 3, -1);

    igSeparator();

    // Resolved output directory preview for the open photo.
    igText("Resolves to");
    char dir[AP_EXPORT_DEST_LEN];
    const char *src = photo ? ap_photo_path(photo) : NULL;
    if (ap_export_resolve_dir(s, src,
                              lib ? ap_library_root(lib) : NULL,
                              dir, sizeof(dir)) == 0) {
        igTextWrapped("%s/", dir);
    } else {
        igTextDisabled("(set a valid destination)");
    }

    igSeparator();

    bool can_export = (photo != NULL) &&
        (s->destination != AP_EXPORT_DEST_CUSTOM || s->dest_dir[0] != '\0') &&
        (s->destination != AP_EXPORT_DEST_SUBDIR || lib != NULL);
    if (!can_export) igBeginDisabled(true);
    if (igButton("Export", (ImVec2_c){ -1.0f, 0.0f })) {
        ap_app_run_export(app);
    }
    if (!can_export) igEndDisabled();
    igTextDisabled("the result lands as a toast in the corner");

    igEnd();
}

const ap_panel panel_export_destination = {
    .name = "export_destination",
    .mode = AP_MODE_EXPORT,
    .draw = export_destination_draw,
};
