#include "panels.h"

#include "library/library.h"
#include "photo/metadata.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Bulk Metadata window: a per-photo Metadata pane fan-out for the
// library grid's current selection. Each field is an editable text
// input; Apply writes every non-empty field as a user-override on
// each selected photo's sidecar. Empty fields are skipped, so the
// user can fill in just the ones they want to fan out.
//
// Independent buffers from the per-photo Metadata pane - this is a
// scratch pad, not a view onto any single photo's values.

static char     g_buffers[AP_META_FIELD_COUNT][AP_META_VALUE_LEN];
static char     g_status[128] = {0};

static void clear_buffers(void)
{
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) g_buffers[i][0] = '\0';
}

static void library_metadata_draw(ap_app *app)
{
    ap_library *lib = ap_app_library(app);
    if (!lib) return;

    if (!igBegin("Bulk Metadata", NULL, 0)) {
        igEnd();
        return;
    }

    igTextDisabled("fill any fields you want to fan out; blanks are skipped");
    igSeparator();

    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        ap_meta_field f = (ap_meta_field)i;
        igPushID_Int(i);

        igText("%s", ap_meta_field_label(f));
        float avail = igGetContentRegionAvail().x;
        if (avail < 80.0f) avail = 80.0f;
        igSetNextItemWidth(avail);
        igInputText("##value", g_buffers[i], AP_META_VALUE_LEN,
                    0, NULL, NULL);
        igPopID();
    }

    igSeparator();

    bool any = false;
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        if (g_buffers[i][0]) { any = true; break; }
    }

    if (!any) igBeginDisabled(true);
    if (igButton("Apply to selection", (ImVec2_c){ 180.0f, 0.0f })) {
        ap_photo_metadata patch;
        bool              patch_set[AP_META_FIELD_COUNT] = {0};
        ap_photo_metadata_clear(&patch);
        int filled = 0;
        for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
            if (!g_buffers[i][0]) continue;
            ap_photo_metadata_set(&patch, (ap_meta_field)i, g_buffers[i]);
            patch_set[i] = true;
            filled++;
        }
        int wrote = ap_app_apply_metadata_to_selection(app, &patch, patch_set);
        if (wrote < 0) {
            snprintf(g_status, sizeof(g_status),
                     "Bulk apply failed (no library / grid).");
        } else if (wrote == 0) {
            snprintf(g_status, sizeof(g_status),
                     "Nothing applied: no photos selected.");
        } else {
            snprintf(g_status, sizeof(g_status),
                     "Applied %d field%s to %d photo%s.",
                     filled, filled == 1 ? "" : "s",
                     wrote,  wrote  == 1 ? "" : "s");
        }
    }
    if (!any) igEndDisabled();

    igSameLine(0.0f, -1.0f);
    if (igButton("Clear", (ImVec2_c){ 80.0f, 0.0f })) {
        clear_buffers();
        g_status[0] = '\0';
    }

    if (g_status[0]) {
        igTextDisabled("%s", g_status);
    }

    igEnd();
}

const ap_panel panel_library_metadata = {
    .name = "library_metadata",
    .mode = AP_MODE_LIBRARY,
    .draw = library_metadata_draw,
};
