#include "panels.h"

#include "app/app.h"
#include "library/library.h"
#include "output/export.h"

#include "cimgui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Export-mode Presets panel: save / load / delete named bundles of
// export settings. Each preset is persisted in the library db's
// `export_presets` table (SPEC ss library-db). Loading a preset
// overwrites the in-memory ap_export_settings in place so the other
// export panels (Format, Quality, Naming, Destination) immediately
// reflect the loaded values.

#define PRESETS_MAX AP_EXPORT_PRESETS_MAX

static int64_t g_selected_id  = 0;
static char    g_save_name[AP_EXPORT_PRESET_NAME_LEN] = {0};
static char    g_status[256] = {0};

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

static void export_presets_draw(ap_app *app)
{
    if (!app) return;
    ap_library *lib = ap_app_library(app);
    ap_export_settings *s = ap_app_export_settings(app);
    if (!s) return;

    if (!igBegin("Presets##export", NULL, 0)) {
        igEnd();
        return;
    }

    if (!lib) {
        igTextDisabled("open a library to use presets");
        igEnd();
        return;
    }

    ap_export_preset list[PRESETS_MAX];
    int n = ap_export_preset_list(lib, list, PRESETS_MAX);
    if (n < 0) n = 0;

    igTextDisabled("named bundles of export settings");
    igSeparator();

    if (n == 0) {
        igTextDisabled("(no presets saved yet)");
    }
    for (int i = 0; i < n; i++) {
        igPushID_Int(i);
        bool sel = (list[i].id == g_selected_id);
        if (igSelectable_Bool(list[i].name, sel,
                              ImGuiSelectableFlags_AllowDoubleClick,
                              (ImVec2_c){ 0, 0 })) {
            g_selected_id = list[i].id;
        }
        igPopID();
    }

    igSeparator();

    bool has_sel = g_selected_id > 0;

    if (!has_sel) igBeginDisabled(true);
    if (igButton("Load", (ImVec2_c){ 80.0f, 0.0f })) {
        ap_export_preset p;
        if (ap_export_preset_load(lib, g_selected_id, &p) == 0) {
            *s = p.settings;
            set_status("Loaded \"%s\".", p.name);
        } else {
            set_status("Load failed.");
        }
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Delete", (ImVec2_c){ 80.0f, 0.0f })) {
        if (ap_export_preset_delete(lib, g_selected_id) == 0) {
            set_status("Deleted.");
            g_selected_id = 0;
        } else {
            set_status("Delete failed.");
        }
    }
    if (!has_sel) igEndDisabled();

    igSpacing();
    igText("Save current settings as");
    igSetNextItemWidth(200.0f);
    igInputText("##preset_name", g_save_name, sizeof(g_save_name),
                0, NULL, NULL);
    igSameLine(0.0f, -1.0f);
    bool can_save = g_save_name[0] != '\0';
    if (!can_save) igBeginDisabled(true);
    if (igButton("Save", (ImVec2_c){ 60.0f, 0.0f })) {
        if (ap_export_preset_save(lib, g_save_name, s) == 0) {
            set_status("Saved \"%s\".", g_save_name);
            g_save_name[0] = '\0';
        } else {
            set_status("Save failed (db error).");
        }
    }
    if (!can_save) igEndDisabled();

    if (g_status[0]) {
        igSeparator();
        igTextDisabled("%s", g_status);
    }

    igEnd();
}

const ap_panel panel_export_presets = {
    .name = "export_presets",
    .mode = AP_MODE_EXPORT,
    .draw = export_presets_draw,
};
