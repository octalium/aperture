#include "panels.h"

#include "library/library.h"
#include "photo/metadata.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Bulk Metadata window: a per-photo Metadata pane fan-out for the
// library grid's current selection. Each field is an editable text
// input. Three states per field:
//   - non-empty buffer → write that value to every selected photo
//   - g_clear[i] = true (shown with an [x] indicator) → write "" to
//     every selected photo, explicitly clearing the field
//   - buffer empty and not cleared → skip (leave unchanged)
//
// The per-field [x] button is the affordance for explicitly clearing
// a field that cannot otherwise be reached by typing an empty string.
//
// Independent buffers from the per-photo Metadata pane - this is a
// scratch pad, not a view onto any single photo's values.

static char g_buffers[AP_META_FIELD_COUNT][AP_META_VALUE_LEN];
static bool g_clear[AP_META_FIELD_COUNT];
static char g_status[128] = {0};

static void reset_all(void)
{
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        g_buffers[i][0] = '\0';
        g_clear[i]      = false;
    }
}

static void library_metadata_draw(ap_app *app)
{
    ap_library *lib = ap_app_library(app);
    if (!lib) return;

    // ##library disambiguates from the per-photo Metadata window
    // (src/panels/photo_metadata.c), which shares the visible title.
    // The two never coexist (mode-gated), but ImGui would still hash
    // them to the same ID and share size/position/dock state.
    if (!igBegin("Metadata##library", &ap_panel_visible_library_metadata, 0)) {
        igEnd();
        return;
    }

    igTextDisabled("fill fields to set, [x] to clear; blanks are skipped");
    igSeparator();

    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        ap_meta_field f = (ap_meta_field)i;
        igPushID_Int(i);

        igText("%s", ap_meta_field_label(f));

        // Reserve space for the [x] button so the input width stays
        // consistent across rows.
        float btn_w  = igGetFrameHeight();
        float avail  = igGetContentRegionAvail().x - btn_w - igGetStyle()->ItemSpacing.x;
        if (avail < 40.0f) avail = 40.0f;
        igSetNextItemWidth(avail);

        if (g_clear[i]) {
            // Show a greyed-out "(will clear)" placeholder while
            // the clear flag is set; the actual buffer stays empty.
            igBeginDisabled(true);
            char placeholder[AP_META_VALUE_LEN] = "(will clear)";
            igInputText("##value", placeholder, sizeof(placeholder),
                        0, NULL, NULL);
            igEndDisabled();
        } else {
            igInputText("##value", g_buffers[i], AP_META_VALUE_LEN,
                        0, NULL, NULL);
        }

        igSameLine(0.0f, igGetStyle()->ItemSpacing.x);

        // [x] button: when a value is typed, clears the buffer (revert
        // to "skip"). When the buffer is empty, marks the field for an
        // explicit clear (write "" to the sidecar).
        if (g_clear[i]) {
            if (igButton("x##clr", (ImVec2_c){ btn_w, 0.0f })) {
                g_clear[i] = false;
            }
            if (igIsItemHovered(0)) {
                igSetTooltip("Cancel clear for this field");
            }
        } else if (g_buffers[i][0]) {
            if (igButton("x##clr", (ImVec2_c){ btn_w, 0.0f })) {
                g_buffers[i][0] = '\0';
            }
            if (igIsItemHovered(0)) {
                igSetTooltip("Clear this field's value");
            }
        } else {
            if (igButton("x##clr", (ImVec2_c){ btn_w, 0.0f })) {
                g_clear[i] = true;
            }
            if (igIsItemHovered(0)) {
                igSetTooltip("Mark this field for clearing on Apply");
            }
        }

        igPopID();
    }

    igSeparator();

    bool any = false;
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        if (g_buffers[i][0] || g_clear[i]) { any = true; break; }
    }

    if (!any) igBeginDisabled(true);
    if (igButton("Apply to selection", (ImVec2_c){ 180.0f, 0.0f })) {
        ap_photo_metadata patch;
        bool              patch_set[AP_META_FIELD_COUNT] = {0};
        ap_photo_metadata_clear(&patch);
        int filled = 0;
        for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
            if (g_clear[i]) {
                // Explicit clear: write empty string.
                ap_photo_metadata_set(&patch, (ap_meta_field)i, "");
                patch_set[i] = true;
                filled++;
            } else if (g_buffers[i][0]) {
                ap_photo_metadata_set(&patch, (ap_meta_field)i, g_buffers[i]);
                patch_set[i] = true;
                filled++;
            }
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
    if (igButton("Reset", (ImVec2_c){ 80.0f, 0.0f })) {
        reset_all();
        g_status[0] = '\0';
    }

    if (g_status[0]) {
        igTextDisabled("%s", g_status);
    }

    igEnd();
}

const ap_panel panel_library_metadata = {
    .name       = "library_metadata",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_metadata_draw,
    .visible    = &ap_panel_visible_library_metadata,
    .menu_label = "Metadata",
};
