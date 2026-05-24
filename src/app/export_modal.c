#define _GNU_SOURCE

#include "modals.h"

#include "library/library.h"
#include "output/export.h"
#include "photo/photo.h"
#include "ui/file_dialog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Which set of photos to export.
typedef enum {
    APPLY_TO_PHOTO     = 0,
    APPLY_TO_SELECTION = 1,
} export_apply_to;

// Per-modal state that survives between frames while the popup is open.
static export_apply_to g_apply_to   = APPLY_TO_PHOTO;
static int64_t         g_preset_id  = 0;
static char            g_save_name[AP_EXPORT_PRESET_NAME_LEN] = {0};
static char            g_status[256] = {0};

// Write the file stem (basename, no extension) of `path` into `out`.
// Falls back to "photo" when `path` is NULL.
static void path_stem(const char *path, char *out, size_t len)
{
    if (!path) { snprintf(out, len, "photo"); return; }
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    snprintf(out, len, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

// Lowest selected grid cell, or -1 if no selection.
static int first_selected_grid_index(const ap_app *app)
{
    if (!app || !app->grid || !app->grid_map) return -1;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (ap_grid_is_selected(app->grid, c)) return c;
    }
    return -1;
}

void draw_export_modal(ap_app *app)
{
    if (app->export_modal) {
        igOpenPopup_Str("Export", 0);
        app->export_modal = false;
        g_status[0] = '\0';
        // Default apply_to based on context.
        bool have_photo     = (app->photo != NULL);
        bool have_selection = (app->library && app->grid &&
                               ap_grid_selection_count(app->grid) > 0);
        if (app->mode == AP_MODE_LIBRARY && have_selection) {
            g_apply_to = APPLY_TO_SELECTION;
        } else {
            g_apply_to = APPLY_TO_PHOTO;
        }
        (void)have_photo;
        (void)have_selection;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoCollapse;
    if (!igBeginPopupModal("Export", NULL, flags)) return;

    ap_export_settings *s = ap_app_export_settings(app);
    ap_library         *lib   = ap_app_library(app);
    ap_photo           *photo = ap_app_photo(app);

    bool have_photo     = (photo != NULL);
    bool have_selection = (lib && app->grid &&
                           ap_grid_selection_count(app->grid) > 0);
    int  sel_count      = have_selection ? ap_app_grid_selection_count(app) : 0;

    // Presets row.
    if (lib) {
        ap_export_preset list[AP_EXPORT_PRESETS_MAX];
        int np = ap_export_preset_list(lib, list, AP_EXPORT_PRESETS_MAX);
        if (np < 0) np = 0;

        igText("Preset:");
        igSameLine(0.0f, -1.0f);

        const char *cur_name = "Custom";
        for (int i = 0; i < np; i++) {
            if (list[i].id == g_preset_id) { cur_name = list[i].name; break; }
        }

        igSetNextItemWidth(180.0f);
        if (igBeginCombo("##preset", cur_name, 0)) {
            bool is_custom = (g_preset_id == 0);
            if (igSelectable_Bool("Custom", is_custom, 0,
                                  (ImVec2_c){ 0.0f, 0.0f })) {
                g_preset_id = 0;
            }
            for (int i = 0; i < np; i++) {
                bool sel = (list[i].id == g_preset_id);
                if (igSelectable_Bool(list[i].name, sel, 0,
                                      (ImVec2_c){ 0.0f, 0.0f })) {
                    g_preset_id = list[i].id;
                    ap_export_preset p;
                    if (ap_export_preset_load(lib, g_preset_id, &p) == 0)
                        *s = p.settings;
                }
            }
            igEndCombo();
        }

        igSameLine(0.0f, 8.0f);
        igSetNextItemWidth(140.0f);
        igInputText("##save_name", g_save_name, sizeof(g_save_name),
                    0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        bool can_save = g_save_name[0] != '\0';
        if (!can_save) igBeginDisabled(true);
        if (igButton("Save preset", (ImVec2_c){ 0.0f, 0.0f })) {
            if (ap_export_preset_save(lib, g_save_name, s) == 0) {
                snprintf(g_status, sizeof(g_status),
                         "Preset \"%s\" saved.", g_save_name);
                g_save_name[0] = '\0';
                // Re-select the preset we just saved.
                ap_export_preset list2[AP_EXPORT_PRESETS_MAX];
                int np2 = ap_export_preset_list(lib, list2, AP_EXPORT_PRESETS_MAX);
                for (int i = 0; i < np2; i++) {
                    if (strcmp(list2[i].name, g_save_name) == 0) {
                        g_preset_id = list2[i].id; break;
                    }
                }
            } else {
                snprintf(g_status, sizeof(g_status), "Save failed.");
            }
        }
        if (!can_save) igEndDisabled();

        if (g_preset_id > 0) {
            igSameLine(0.0f, 4.0f);
            if (igButton("Delete preset", (ImVec2_c){ 0.0f, 0.0f })) {
                if (ap_export_preset_delete(lib, g_preset_id) == 0) {
                    snprintf(g_status, sizeof(g_status), "Preset deleted.");
                    g_preset_id = 0;
                } else {
                    snprintf(g_status, sizeof(g_status), "Delete failed.");
                }
            }
        }

        igSeparator();
    }

    // Apply-to row.
    {
        bool photo_disabled     = !have_photo;
        bool selection_disabled = !have_selection;

        igText("Apply to:");
        igSameLine(0.0f, -1.0f);

        if (photo_disabled) igBeginDisabled(true);
        if (igRadioButton_IntPtr("Open photo", (int *)&g_apply_to,
                                 APPLY_TO_PHOTO) && photo_disabled) {
            g_apply_to = APPLY_TO_SELECTION;
        }
        if (photo_disabled) igEndDisabled();

        igSameLine(0.0f, 16.0f);

        if (selection_disabled) igBeginDisabled(true);
        char sel_label[64];
        if (sel_count > 0) {
            snprintf(sel_label, sizeof(sel_label),
                     "Library selection (%d)", sel_count);
        } else {
            snprintf(sel_label, sizeof(sel_label), "Library selection");
        }
        if (igRadioButton_IntPtr(sel_label, (int *)&g_apply_to,
                                 APPLY_TO_SELECTION) && selection_disabled) {
            g_apply_to = APPLY_TO_PHOTO;
        }
        if (selection_disabled) igEndDisabled();
    }

    igSeparator();

    // Format.
    {
        static const char *const format_items[] = { "JPEG", "TIFF", "PNG" };
        igText("Format:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(120.0f);
        igCombo_Str_arr("##format", &s->format, format_items, 3, -1);
    }

    // Format-specific controls.
    switch (s->format) {
    case AP_EXPORT_FORMAT_JPEG:
        igText("Quality:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(200.0f);
        igSliderInt("##jpeg_q", &s->jpeg_quality, 1, 100, "%d", 0);
        break;

    case AP_EXPORT_FORMAT_PNG: {
        static const char *const depth_items[] = { "8-bit", "16-bit" };
        igText("Bit depth:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(120.0f);
        igCombo_Str_arr("##png_depth", &s->png_depth, depth_items, 2, -1);
        break;
    }

    case AP_EXPORT_FORMAT_TIFF: {
        static const char *const depth_items[] = {
            "8-bit integer", "16-bit integer",
        };
        static const char *const compress_items[] = {
            "None", "LZW", "Deflate",
        };
        igText("Bit depth:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(160.0f);
        igCombo_Str_arr("##tiff_depth", &s->tiff_depth, depth_items, 2, -1);

        igText("Compress:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(160.0f);
        igCombo_Str_arr("##tiff_compress", &s->tiff_compress,
                        compress_items, 3, -1);
        break;
    }

    default:
        break;
    }

    igSeparator();

    // Naming.
    {
        igText("Naming:");
        igSameLine(0.0f, -1.0f);
        igRadioButton_IntPtr("Keep original", &s->naming,
                             AP_EXPORT_NAME_KEEP);
        igSameLine(0.0f, 12.0f);
        igRadioButton_IntPtr("Pattern", &s->naming,
                             AP_EXPORT_NAME_PATTERN);
        if (s->naming == AP_EXPORT_NAME_PATTERN) {
            igSameLine(0.0f, 8.0f);
            igSetNextItemWidth(200.0f);
            igInputText("##pattern", s->pattern, sizeof(s->pattern),
                        0, NULL, NULL);
            igSameLine(0.0f, 4.0f);
            igTextDisabled("tokens: {ORIG} {YYYY} {MM} {DD} {HH} {MIN} {SEC} {SEQ}");
        }
    }

    igSeparator();

    // Destination.
    {
        igText("Destination:");
        igSameLine(0.0f, -1.0f);

        bool have_lib = (lib != NULL);

        igRadioButton_IntPtr("Beside source", &s->destination,
                             AP_EXPORT_DEST_BESIDE);
        igSameLine(0.0f, 12.0f);
        if (!have_lib) igBeginDisabled(true);
        igRadioButton_IntPtr("Library subfolder", &s->destination,
                             AP_EXPORT_DEST_SUBDIR);
        if (!have_lib) igEndDisabled();
        igSameLine(0.0f, 12.0f);
        igRadioButton_IntPtr("Custom folder", &s->destination,
                             AP_EXPORT_DEST_CUSTOM);

        if (s->destination == AP_EXPORT_DEST_SUBDIR) {
            igText("Subfolder:");
            igSameLine(0.0f, -1.0f);
            igSetNextItemWidth(280.0f);
            igInputText("##dest_subdir", s->dest_subdir,
                        sizeof(s->dest_subdir), 0, NULL, NULL);
        } else if (s->destination == AP_EXPORT_DEST_CUSTOM) {
            igText("Folder:");
            igSameLine(0.0f, -1.0f);
            igSetNextItemWidth(280.0f);
            igInputText("##dest_dir", s->dest_dir, sizeof(s->dest_dir),
                        0, NULL, NULL);
            igSameLine(0.0f, 4.0f);
            if (igButton("Browse...", (ImVec2_c){ 0.0f, 0.0f })) {
                char picked[AP_EXPORT_DEST_LEN];
                const char *seed = s->dest_dir[0] ? s->dest_dir : NULL;
                if (ap_file_dialog_pick_folder(picked, sizeof(picked), seed)) {
                    snprintf(s->dest_dir, sizeof(s->dest_dir), "%s", picked);
                }
            }
        }

        igText("On conflict:");
        igSameLine(0.0f, -1.0f);
        static const char *const collide_items[] = {
            "Skip", "Overwrite", "Auto-suffix",
        };
        igSetNextItemWidth(140.0f);
        igCombo_Str_arr("##collision", &s->collision, collide_items, 3, -1);
    }

    igSeparator();

    // Output preview line.
    {
        // Pick the preview source: the open photo, or — when targeting the
        // library selection — the first selected grid photo. With no
        // selection, fall back to a placeholder stem rather than the open
        // photo (which would mislead about what export will actually run).
        char        sel_abs[AP_EXPORT_DEST_LEN] = {0};
        const char *src = NULL;
        char        stem[256];
        if (g_apply_to == APPLY_TO_SELECTION) {
            int idx = first_selected_grid_index(app);
            int mapped = (idx >= 0) ? app->grid_map[idx] : -1;
            if (mapped >= 0 && lib &&
                ap_library_photo_absolute_path(lib, mapped, sel_abs,
                                               sizeof(sel_abs)) == 0) {
                src = sel_abs;
                path_stem(src, stem, sizeof(stem));
            } else {
                snprintf(stem, sizeof(stem), "<selected-photo>");
            }
        } else {
            src = photo ? ap_photo_path(photo) : NULL;
            path_stem(src, stem, sizeof(stem));
        }

        char out_stem[512];
        ap_export_format_stem(s, stem, time(NULL), 1,
                              out_stem, sizeof(out_stem));
        const char *ext = ap_export_format_extension(s->format);

        char dir[AP_EXPORT_DEST_LEN];
        const char *root = lib ? ap_library_root(lib) : NULL;
        bool dir_ok = (ap_export_resolve_dir(s, src, root,
                                             dir, sizeof(dir)) == 0);

        igTextDisabled("Output preview:");
        if (dir_ok) {
            igTextWrapped("%s.%s  →  %s/", out_stem, ext, dir);
        } else {
            igTextWrapped("%s.%s  →  (set a valid destination)",
                          out_stem, ext);
        }
    }

    if (g_status[0]) {
        igTextDisabled("%s", g_status);
    }

    igSeparator();

    // Export / Cancel buttons.
    {
        bool dest_ok = (s->destination != AP_EXPORT_DEST_CUSTOM
                        || s->dest_dir[0] != '\0')
                    && (s->destination != AP_EXPORT_DEST_SUBDIR
                        || lib != NULL);
        bool can_export_photo     = dest_ok && have_photo
                                    && g_apply_to == APPLY_TO_PHOTO;
        bool can_export_selection = dest_ok && have_selection
                                    && g_apply_to == APPLY_TO_SELECTION;
        bool can_export = can_export_photo || can_export_selection;

        if (!can_export) igBeginDisabled(true);
        if (igButton("Export", (ImVec2_c){ 120.0f, 0.0f })) {
            if (g_apply_to == APPLY_TO_PHOTO && have_photo) {
                int rc = ap_app_run_export(app);
                if (rc < 0) {
                    snprintf(g_status, sizeof(g_status),
                             "Export failed — see the log.");
                } else if (rc == 1) {
                    snprintf(g_status, sizeof(g_status),
                             "Skipped — file already exists.");
                } else {
                    ap_export_settings_save(lib, s);
                    igCloseCurrentPopup();
                }
            } else if (g_apply_to == APPLY_TO_SELECTION && have_selection) {
                int queued = 0, skipped = 0;
                int rc = ap_app_batch_export_selection(app, s,
                                                       &queued, &skipped);
                if (rc < 0) {
                    snprintf(g_status, sizeof(g_status),
                             "Batch export failed — see the log.");
                } else {
                    ap_export_settings_save(lib, s);
                    snprintf(g_status, sizeof(g_status),
                             "Queued %d, skipped %d.", queued, skipped);
                    igCloseCurrentPopup();
                }
            }
        }
        if (!can_export) igEndDisabled();

        igSameLine(0.0f, -1.0f);
        if (igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })) {
            igCloseCurrentPopup();
        }
    }

    igEndPopup();
}
