#define _POSIX_C_SOURCE 200809L

#include "panels.h"

#include "app/app.h"
#include "edit/stack.h"
#include "library/library.h"

#include "cimgui.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Library-mode Pipelines panel: list + apply + set-default + delete
// + duplicate + rename. Opts into the optional-panel pattern from #84,
// so it's off by default (Edit > Pipelines) and closable via the X.
//
// Editing a pipeline's *contents* happens through the "apply to photo
// → edit there → save back as same name" loop that lands in PR 3
// (photo-mode Save-as-pipeline). This panel intentionally doesn't
// duplicate the photo-mode Tools / Edits editor.

#define PIPELINES_MAX 64

static int64_t g_selected_id    = 0;
static int64_t g_rename_for_id  = 0;     // which id g_rename_buf reflects
static char    g_rename_buf[AP_PIPELINE_NAME_LEN] = {0};
static char    g_status[256] = {0};

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

// Refresh g_rename_buf from the named pipeline when the selection
// switches; lets the user type into the buffer freely between rows.
static void sync_rename_buf(const ap_pipeline_def *list, int n)
{
    if (g_rename_for_id == g_selected_id) return;
    g_rename_for_id = g_selected_id;
    g_rename_buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (list[i].id == g_selected_id) {
            snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", list[i].name);
            return;
        }
    }
}

static void library_pipelines_draw(ap_app *app)
{
    if (!app) return;
    ap_library *lib = ap_app_library(app);
    if (!lib) return;

    if (!igBegin("Pipelines##library", &ap_panel_visible_library_pipelines, 0)) {
        igEnd();
        return;
    }

    ap_pipeline_def list[PIPELINES_MAX];
    int n = ap_pipeline_list(list, PIPELINES_MAX);
    if (n < 0) n = 0;

    int64_t lib_default = ap_library_default_pipeline_id(lib);

    igTextDisabled("library pipelines apply across the grid selection");
    igSeparator();

    // List rows. Empty state when there are no pipelines (shouldn't
    // happen in practice — the registry always seeds the default —
    // but defensive against a future bare-registry case).
    if (n == 0) {
        igTextDisabled("(no pipelines)");
    }
    for (int i = 0; i < n; i++) {
        igPushID_Int(i);
        char label[AP_PIPELINE_NAME_LEN + 64];
        int entries = ap_edit_stack_count(&list[i].stack);
        bool is_lib_default = (lib_default == list[i].id);
        snprintf(label, sizeof(label), "%s  (%d edit%s)%s",
                 list[i].name,
                 entries, entries == 1 ? "" : "s",
                 is_lib_default ? "  [library default]" : "");
        bool sel = (list[i].id == g_selected_id);
        if (igSelectable_Bool(label, sel,
                              ImGuiSelectableFlags_AllowDoubleClick,
                              (ImVec2_c){ 0, 0 })) {
            g_selected_id = list[i].id;
        }
        igPopID();
    }
    sync_rename_buf(list, n);

    igSeparator();

    // Actions on the selected pipeline.
    bool has_sel = g_selected_id > 0;
    int sel_count = 0;
    {
        // Count selected photos via the library/grid pair — the
        // selection count for "Apply to selection" enablement.
        // We can't reach into the grid directly without exposing more
        // through app.h; ap_app_apply_pipeline_to_selection happens
        // to return -1 with no grid, so we don't need to gate the
        // button by exact count. Just disable when no library.
        sel_count = 0;  // unused for now; future visual hint
        (void)sel_count;
    }

    if (!has_sel) igBeginDisabled(true);

    const ImVec2_c btn_size = { -FLT_MIN, 0.0f };

    if (igButton("Apply to selection", btn_size)) {
        int wrote = ap_app_apply_pipeline_to_selection(app, g_selected_id);
        if (wrote < 0) {
            set_status("Apply failed (no library / grid).");
        } else if (wrote == 0) {
            set_status("Nothing applied: no photos selected.");
        } else {
            set_status("Applied to %d photo%s.",
                       wrote, wrote == 1 ? "" : "s");
        }
    }
    bool is_lib_default = (lib_default == g_selected_id);
    if (igButton(is_lib_default ? "Clear library default"
                                 : "Set as library default",
                 btn_size)) {
        int64_t target = is_lib_default ? 0 : g_selected_id;
        if (ap_library_set_default_pipeline_id(lib, target) == 0) {
            set_status(target ? "Library default updated."
                              : "Library default cleared.");
        } else {
            set_status("Failed to update library default.");
        }
    }
    if (igButton("Duplicate", btn_size)) {
        ap_pipeline_def def;
        if (ap_pipeline_get(g_selected_id, &def) == 0) {
            // " copy" suffix; cap the source so the join fits. Use
            // memcpy rather than snprintf to dodge gcc's
            // format-truncation pessimism on bounded inputs.
            static const char SUFFIX[] = " copy";
            char  new_name[AP_PIPELINE_NAME_LEN];
            const size_t src_cap = sizeof(new_name) - sizeof(SUFFIX);
            size_t src_len = strnlen(def.name, src_cap);
            memcpy(new_name, def.name, src_len);
            memcpy(new_name + src_len, SUFFIX, sizeof(SUFFIX));
            int64_t new_id = 0;
            if (ap_pipeline_create(new_name, &def.stack, &new_id) == 0) {
                g_selected_id = new_id;
                g_rename_for_id = 0;  // force re-sync on next frame
                set_status("Duplicated as \"%s\".", new_name);
            } else {
                set_status("Duplicate failed (name collision?).");
            }
        }
    }
    // Delete: protected for the registry default *and* the per-library
    // default (clearing it first is the explicit gesture).
    bool deletable = !is_lib_default;
    if (!deletable) igBeginDisabled(true);
    if (igButton("Delete", btn_size)) {
        if (ap_pipeline_delete(g_selected_id) == 0) {
            set_status("Deleted.");
            g_selected_id = 0;
            g_rename_for_id = 0;
        } else {
            set_status("Cannot delete (default protected).");
        }
    }
    if (!deletable) igEndDisabled();

    if (!has_sel) igEndDisabled();

    // Inline rename: input + Save, only when a row is selected.
    if (has_sel) {
        igSpacing();
        igSetNextItemWidth(-FLT_MIN);
        igInputText("##rename", g_rename_buf, sizeof(g_rename_buf),
                    0, NULL, NULL);
        bool can_rename = g_rename_buf[0] != '\0';
        if (!can_rename) igBeginDisabled(true);
        if (igButton("Rename", btn_size)) {
            if (ap_pipeline_update(g_selected_id, g_rename_buf, NULL) == 0) {
                set_status("Renamed to \"%s\".", g_rename_buf);
                g_rename_for_id = 0;
            } else {
                set_status("Rename failed (name collision?).");
            }
        }
        if (!can_rename) igEndDisabled();
    }

    if (g_status[0]) {
        igSeparator();
        igTextDisabled("%s", g_status);
    }

    igEnd();
}

const ap_panel panel_library_pipelines = {
    .name       = "library_pipelines",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_pipelines_draw,
    .visible    = &ap_panel_visible_library_pipelines,
    .menu_label = "Pipelines",
};
