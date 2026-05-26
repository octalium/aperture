#define _GNU_SOURCE

#include "modals.h"

#include "library/library.h"
#include "output/export.h"
#include "ui/file_dialog.h"
#include "ui/modal_kbd.h"
#include "update/settings.h"

#include <stdio.h>
#include <string.h>

void draw_preferences_modal(ap_app *app)
{
    if (app->preferences_modal) {
        igOpenPopup_Str("Preferences", 0);
        app->preferences_modal = false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoCollapse;
    if (!igBeginPopupModal("Preferences", NULL, flags)) return;

    ap_quick_export_settings *q = ap_app_quick_export_settings(app);
    ap_library               *lib = ap_app_library(app);

    igSeparatorText("Quick Export");

    {
        // order must match ap_export_format in output/export.h
        static const char *const format_items[] = { "JPEG", "TIFF", "PNG" };
        igText("Format:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(120.0f);
        igCombo_Str_arr("##qe_format", &q->format, format_items, 3, -1);
    }

    {
        bool jpeg = (q->format == AP_EXPORT_FORMAT_JPEG);
        if (!jpeg) igBeginDisabled(true);
        igText("Quality:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(200.0f);
        igSliderInt("##qe_quality", &q->jpeg_quality, 1, 100, "%d", 0);
        if (!jpeg) igEndDisabled();
    }

    {
        char placeholder[AP_EXPORT_DEST_LEN];
        const char *root = lib ? ap_library_root(lib) : NULL;
        if (root && root[0]) {
            snprintf(placeholder, sizeof(placeholder), "%s/export", root);
        } else {
            snprintf(placeholder, sizeof(placeholder),
                     "<library>/export  (open a library to see the default)");
        }

        igText("Destination:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(320.0f);
        igInputTextWithHint("##qe_dest", placeholder, q->destination,
                            sizeof(q->destination), 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        if (igButton("Browse...", (ImVec2_c){ 0.0f, 0.0f })) {
            char picked[AP_EXPORT_DEST_LEN];
            const char *seed = q->destination[0] ? q->destination : root;
            if (ap_file_dialog_pick_folder(picked, sizeof(picked), seed)) {
                snprintf(q->destination, sizeof(q->destination), "%s", picked);
            }
        }
        if (q->destination[0]) {
            igSameLine(0.0f, 4.0f);
            if (igButton("Use default", (ImVec2_c){ 0.0f, 0.0f })) {
                q->destination[0] = '\0';
            }
        }
    }

    ap_update_settings *u = ap_app_update_settings(app);

    igSeparatorText("Updates");

    igCheckbox("Check for updates on launch", &u->check_on_launch);

    igText("Manifest URL:");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(320.0f);
    igInputTextWithHint("##update_manifest_url",
                        "(default release manifest)",
                        u->manifest_url, sizeof(u->manifest_url),
                        0, NULL, NULL);

    igSeparator();

    if (igButton("Save", (ImVec2_c){ 120.0f, 0.0f })
        || ap_modal_enter_pressed()) {
        ap_quick_export_save(q);
        ap_update_settings_save(u);
        igCloseCurrentPopup();
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })
        || ap_modal_esc_pressed()) {
        // Reload from the store so any unsaved edits do not leak into
        // the next open.
        ap_quick_export_load(q);
        ap_update_settings_load(u);
        igCloseCurrentPopup();
    }

    igEndPopup();
}
