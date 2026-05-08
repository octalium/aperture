#include "panels.h"

#include "core/log.h"
#include "library/library.h"

#include "cimgui.h"

#include <string.h>

#define MAX_REGISTRY_ROWS  64
#define PATH_INPUT_LEN     4096

static char g_open_path[PATH_INPUT_LEN];

static const char *basename_of(const char *path)
{
    if (!path || !*path) return path;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void libraries_draw(ap_app *app)
{
    ap_library *lib = ap_app_library(app);

    if (!igBegin("libraries", NULL, 0)) {
        igEnd();
        return;
    }

    if (lib) {
        igTextWrapped("Open: %s", ap_library_root(lib));
        igText("Photos: %d", ap_library_photo_count(lib));
        igSeparator();
        if (igButton("Close library", (ImVec2_c){ 0.0f, 0.0f })) {
            ap_app_close_library(app);
            igEnd();
            return;
        }
        igSeparator();
    }

    igText("Open by path:");
    igInputText("##path", g_open_path, sizeof(g_open_path), 0, NULL, NULL);
    igSameLine(0.0f, -1.0f);
    if (igButton("Open", (ImVec2_c){ 0.0f, 0.0f }) && g_open_path[0]) {
        if (ap_app_open_library(app, g_open_path) == 0) {
            g_open_path[0] = '\0';
        }
    }

    igSeparator();
    igText("Recent libraries:");

    ap_registry_entry rows[MAX_REGISTRY_ROWS];
    int n = ap_registry_list(rows, MAX_REGISTRY_ROWS);
    if (n <= 0) {
        igTextDisabled("(none yet)");
        igEnd();
        return;
    }

    for (int i = 0; i < n; i++) {
        const char *base = basename_of(rows[i].path);
        char label[256];
        snprintf(label, sizeof(label), "%s##%s", base, rows[i].id);
        if (igButton(label, (ImVec2_c){ 0.0f, 0.0f })) {
            if (ap_app_open_library(app, rows[i].path) != 0) {
                AP_ERROR("libraries: failed to open %s", rows[i].path);
            }
        }
        if (igIsItemHovered(0)) {
            igSetTooltip("%s", rows[i].path);
        }
    }
    igEnd();
}

const ap_panel panel_libraries = {
    .name = "libraries",
    .mode = AP_MODE_LIBRARY,
    .draw = libraries_draw,
};
