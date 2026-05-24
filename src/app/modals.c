#define _GNU_SOURCE

#include "modals.h"

#include <string.h>

// Last path component of `path`, or "" if `path` is NULL / empty.
static const char *path_leaf(const char *path)
{
    if (!path || !*path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void draw_import_modal(ap_app *app)
{
    if (app->import_modal) {
        igOpenPopup_Str("Import Photos", 0);
        app->import_modal = false;
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoCollapse;
    if (!igBeginPopupModal("Import Photos", NULL, flags)) return;

    if (!app->library) {
        igText("No library open.");
        if (igButton("Close", (ImVec2_c){ 120.0f, 0.0f })) {
            igCloseCurrentPopup();
        }
        igEndPopup();
        return;
    }

    ap_import_settings *s = &app->import_settings;

    // Source section.
    igSeparatorText("Source");
    igText("Folder:");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(280.0f);
    igInputText("##source_path", app->import_source,
                sizeof(app->import_source), 0, NULL, NULL);
    igSameLine(0.0f, 4.0f);
    if (igButton("Browse...", (ImVec2_c){ 0.0f, 0.0f })) {
        const char *seed = app->import_source[0] ? app->import_source : NULL;
        ap_file_dialog_pick_folder(app->import_source,
                                   sizeof(app->import_source), seed);
    }

    // Destination section.
    igSeparatorText("Destination");
    igText("Library:");
    igSameLine(0.0f, -1.0f);
    igTextDisabled("%s", ap_library_root(app->library));

    igText("Subdir: ");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(280.0f);
    igInputText("##subdir", s->subdir, sizeof(s->subdir), 0, NULL, NULL);
    igSameLine(0.0f, 4.0f);
    igTextDisabled("(blank = library root)");

    // Naming section.
    igSeparatorText("Naming");
    static const char *const naming_items[] = {
        "Keep original names", "Rename by pattern",
    };
    igText("Mode:   ");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(220.0f);
    igCombo_Str_arr("##naming", &s->naming, naming_items, 2, -1);
    if (s->naming == AP_IMPORT_NAME_PATTERN) {
        igText("Pattern:");
        igSameLine(0.0f, -1.0f);
        igSetNextItemWidth(280.0f);
        igInputText("##pattern", s->pattern, sizeof(s->pattern),
                    0, NULL, NULL);
        igTextDisabled(
            "tokens: {ORIG} {YYYY} {MM} {DD} {HH} {MIN} {SEC} {SEQ}");
    }

    // Duplicates section. Two distinct conflict-avoidance layers,
    // surfaced together so the difference is obvious:
    //
    //   - Library-wide content dedupe (BLAKE3 hash lookup against the
    //     photos table). Catches "same bytes already imported anywhere
    //     in the library". User-toggleable; on by default.
    //
    //   - Per-destination name collision. Triggers when a file with
    //     the same target filename already exists at the destination
    //     path. The byte-equality check (skip if identical) always
    //     runs as a safety net; the policy combo only governs the
    //     differing-content branch.
    igSeparatorText("Duplicates");
    igCheckbox("Skip files already in the library (content hash)",
               &s->dedupe_content);

    igText("On name collision:");
    igSameLine(0.0f, -1.0f);
    static const char *const collide_items[] = {
        "Skip", "Overwrite", "Auto-suffix",
    };
    igSetNextItemWidth(140.0f);
    igCombo_Str_arr("##collision", &s->collision, collide_items, 3, -1);
    igTextDisabled("Identical files at the same path are always skipped.");

    igSeparator();

    // Output preview line. Mirrors the export modal's preview row so
    // the user can see where files will land before clicking Import.
    {
        const char *leaf = path_leaf(app->import_source);
        const char *root = ap_library_root(app->library);
        igTextDisabled("Preview:");
        if (!app->import_source[0]) {
            igTextWrapped("(choose a source folder)");
        } else if (s->subdir[0]) {
            igTextWrapped("%s/  \xe2\x86\x92  %s/%s/", leaf, root, s->subdir);
        } else {
            igTextWrapped("%s/  \xe2\x86\x92  %s/", leaf, root);
        }
    }

    if (app->import_inflight) {
        igTextDisabled("Importing\xe2\x80\xa6 (progress shown in corner)");
    } else if (app->import_status[0]) {
        igTextWrapped("%s", app->import_status);
        const ap_import_report *r = &app->import_report;
        if (r->dup_content > 0) {
            igTextDisabled("%d already in library (skipped)", r->dup_content);
        }
        if (r->renamed_collision > 0) {
            igTextDisabled("%d renamed to avoid a name collision",
                           r->renamed_collision);
        }
        if (r->skip_collision > 0) {
            igTextDisabled("%d skipped (name collision, different content)",
                           r->skip_collision);
        }
        if (r->errored > 0) {
            igTextDisabled("%d error%s \xe2\x80\x94 see the log",
                           r->errored, r->errored == 1 ? "" : "s");
        }
    }

    igSeparator();

    bool can_import = app->import_source[0] != '\0' && !app->import_inflight;
    if (!can_import) igBeginDisabled(true);
    if (igButton("Import", (ImVec2_c){ 120.0f, 0.0f })) {
        ap_import_settings_save(app->library, s);
        const char *root = ap_library_root(app->library);
        submit_import_job(app, root, app->import_source, s);
    }
    if (!can_import) igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })) {
        igCloseCurrentPopup();
    }

    igEndPopup();
}

void draw_rename_library_modal(ap_app *app)
{
    if (app->rename_library_modal) {
        igOpenPopup_Str("Rename Library", 0);
        app->rename_library_modal = false;
    }
    if (!igBeginPopupModal("Rename Library", NULL, 0)) return;

    if (!app->library) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }

    igText("Display name for this library:");
    igText("(leave blank to clear and show the path)");
    igInputText("##name", app->rename_library_input,
                sizeof(app->rename_library_input), 0, NULL, NULL);

    bool submit = igButton("Save", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (submit) {
        ap_library_set_name(app->library, app->rename_library_input);
        refresh_window_title(app);
        igCloseCurrentPopup();
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

void draw_save_layout_modal(ap_app *app)
{
    if (app->save_layout_modal) {
        igOpenPopup_Str("Save Layout As", 0);
        app->save_layout_modal = false;
    }
    if (!igBeginPopupModal("Save Layout As", NULL, 0)) return;

    igText("Name for the layout:");
    igSetNextItemWidth(260.0f);
    igInputText("##layout_name", app->save_layout_input,
                sizeof(app->save_layout_input), 0, NULL, NULL);

    bool name_ok = app->save_layout_input[0] != '\0';
    if (!name_ok) igBeginDisabled(true);
    bool submit = igButton("Save", (ImVec2_c){ 120.0f, 0.0f });
    if (!name_ok) igEndDisabled();
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (submit && name_ok) {
        if (ap_layout_save_current_as(app->save_layout_input) == 0) {
            igCloseCurrentPopup();
        }
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

void draw_delete_modal(ap_app *app)
{
    if (app->delete_modal) {
        igOpenPopup_Str("Delete Photos", 0);
        app->delete_modal = false;
    }
    if (!igBeginPopupModal("Delete Photos", NULL,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    int n = (app->library && app->grid)
                ? ap_grid_selection_count(app->grid) : 0;
    if (n <= 0) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }

    const char *s = (n == 1) ? "" : "s";
    igText("Delete %d photo%s from disk?", n, s);
    igTextDisabled("The raw file%s and sidecar%s will be removed. "
                   "This cannot be undone.", s, s);
    igSeparator();

    bool confirm = igButton("Delete", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (confirm) {
        delete_grid_selection(app);
        igCloseCurrentPopup();
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

void draw_delete_edit_modal(ap_app *app)
{
    if (app->delete_edit_modal) {
        igOpenPopup_Str("Delete Photo", 0);
        app->delete_edit_modal = false;
    }
    if (!igBeginPopupModal("Delete Photo", NULL,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    if (!app->photo || app->photo_library_idx < 0) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }

    const char *rel = ap_library_photo_relative_path(app->library,
                                                     app->photo_library_idx);
    igText("Delete this photo from disk?");
    if (rel) igTextDisabled("%s", rel);
    igTextDisabled("The raw file and sidecar will be removed. "
                   "This cannot be undone.");
    igSeparator();

    bool confirm = igButton("Delete", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (confirm) {
        delete_edit_photo(app);
        igCloseCurrentPopup();
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}
