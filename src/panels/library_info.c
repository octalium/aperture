#include "panels.h"

#include "app/app.h"
#include "library/library.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Library-mode info panel: library-wide settings and information in
// one dedicated window, analogous to the Groups panel. Shows the
// library name (editable), root path, photo count, and the default
// pipeline for new imports.

#define PIPELINES_MAX 64

static char g_name_buf[128] = {0};
static bool g_name_editing  = false;

static void library_info_draw(ap_app *app)
{
    if (!app) return;
    ap_library *lib = ap_app_library(app);
    if (!lib) return;

    if (!igBegin("Library##info", &ap_panel_visible_library_info, 0)) {
        igEnd();
        return;
    }

    igSeparatorText("Info");

    const char *name = ap_library_name(lib);
    const char *root = ap_library_root(lib);
    int         n    = ap_library_photo_count(lib);

    igText("Path:");
    igSameLine(0.0f, -1.0f);
    igTextDisabled("%s", root ? root : "(unknown)");

    char count_buf[64];
    snprintf(count_buf, sizeof(count_buf), "%d photo%s", n, n == 1 ? "" : "s");
    igText("Photos:");
    igSameLine(0.0f, -1.0f);
    igTextDisabled("%s", count_buf);

    igSeparatorText("Settings");

    igText("Name:");
    igSameLine(0.0f, -1.0f);
    if (!g_name_editing) {
        const char *display = (name && *name) ? name : "(not set)";
        igTextDisabled("%s", display);
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("Edit")) {
            snprintf(g_name_buf, sizeof(g_name_buf), "%s",
                     (name && *name) ? name : "");
            g_name_editing = true;
        }
    } else {
        igSetNextItemWidth(160.0f);
        bool enter = igInputText("##libname", g_name_buf, sizeof(g_name_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue,
                                 NULL, NULL);
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("Save") || enter) {
            ap_library_set_name(lib, g_name_buf);
            g_name_editing = false;
        }
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("Cancel")) {
            g_name_editing = false;
        }
    }

    igSpacing();

    igText("Default pipeline:");
    int64_t def_id = ap_library_default_pipeline_id(lib);
    ap_pipeline_def pipelines[PIPELINES_MAX];
    int np = ap_pipeline_list(pipelines, PIPELINES_MAX);

    const char *def_name = "(none)";
    int         def_idx  = -1;
    for (int i = 0; i < np; i++) {
        if (pipelines[i].id == def_id) {
            def_name = pipelines[i].name;
            def_idx  = i;
            break;
        }
    }

    if (np == 0) {
        igTextDisabled("(no pipelines saved)");
    } else {
        igSetNextItemWidth(180.0f);
        if (igBeginCombo("##defpipeline", def_name, 0)) {
            if (igSelectable_Bool("(none)", def_idx == -1, 0,
                                  (ImVec2_c){ 0.0f, 0.0f })) {
                ap_library_set_default_pipeline_id(lib, 0);
            }
            for (int i = 0; i < np; i++) {
                bool sel = (pipelines[i].id == def_id);
                if (igSelectable_Bool(pipelines[i].name, sel, 0,
                                      (ImVec2_c){ 0.0f, 0.0f })) {
                    ap_library_set_default_pipeline_id(lib, pipelines[i].id);
                }
            }
            igEndCombo();
        }
    }

    igEnd();
}

const ap_panel panel_library_info = {
    .name       = "library_info",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_info_draw,
    .visible    = &ap_panel_visible_library_info,
    .menu_label = "Library",
};
