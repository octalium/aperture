#include "panels.h"

#include "app/app.h"
#include "library/library.h"
#include "output/export.h"

#include "cimgui.h"

#include <stdio.h>

// Export-mode Batch panel: export the library grid's multi-selection
// through the current export settings. Shows a live count of the
// selected photos and a progress summary after each run.

static char g_status[256] = {0};

static void export_batch_draw(ap_app *app)
{
    if (!app) return;
    ap_export_settings *s = ap_app_export_settings(app);
    if (!s) return;

    if (!igBegin("Batch##export", NULL, 0)) {
        igEnd();
        return;
    }

    ap_library *lib = ap_app_library(app);
    int sel = ap_app_grid_selection_count(app);

    igTextDisabled("export the grid selection through the current settings");
    igSeparator();

    if (!lib) {
        igTextDisabled("(no library open)");
        igEnd();
        return;
    }

    if (sel == 0) {
        igTextDisabled("(no photos selected in the grid)");
    } else {
        igText("%d photo%s selected", sel, sel == 1 ? "" : "s");
    }

    igSpacing();

    bool can_batch = sel > 0 &&
        (s->destination != AP_EXPORT_DEST_CUSTOM || s->dest_dir[0] != '\0') &&
        (s->destination != AP_EXPORT_DEST_SUBDIR || lib != NULL);

    if (!can_batch) igBeginDisabled(true);
    if (igButton("Export Selection", (ImVec2_c){ -1.0f, 0.0f })) {
        int queued  = 0;
        int skipped = 0;
        int rc = ap_app_batch_export_selection(app, s, &queued, &skipped);
        if (rc < 0) {
            snprintf(g_status, sizeof(g_status),
                     "Batch export failed - check the log.");
        } else if (queued == 0 && skipped == 0) {
            snprintf(g_status, sizeof(g_status),
                     "Nothing exported (no selectable photos).");
        } else {
            snprintf(g_status, sizeof(g_status),
                     "Queued %d, skipped %d.", queued, skipped);
        }
        ap_export_settings_save(lib, s);
    }
    if (!can_batch) igEndDisabled();

    if (g_status[0]) {
        igSeparator();
        igTextDisabled("%s", g_status);
    }

    igEnd();
}

const ap_panel panel_export_batch = {
    .name = "export_batch",
    .mode = AP_MODE_EXPORT,
    .draw = export_batch_draw,
};
