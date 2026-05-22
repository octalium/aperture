#include "panels.h"

#include "app/app.h"
#include "library/library.h"

#include "cimgui.h"

#include <string.h>

// Library-mode Sort & Search panel: a compact sort selector and a
// search box that filter the visible grid in real time.

// Sort and search are core library functions, not optional tools —
// the panel is visible by default.
static bool g_visible = true;

static void library_sort_search_draw(ap_app *app)
{
    if (!app) return;
    if (!ap_app_library(app)) return;

    if (!igBegin("Sort & Search##library", &g_visible, 0)) {
        igEnd();
        return;
    }

    ap_library_sort cur_sort = ap_app_sort(app);

    igText("Sort by");
    igSameLine(0.0f, -1.0f);

    static const char *sort_labels[] = {
        "Filename",
        "Capture time",
        "Date added",
    };
    static const ap_library_sort sort_values[] = {
        AP_SORT_PATH,
        AP_SORT_CAPTURE_TIME,
        AP_SORT_ADDED_AT,
    };
    int cur_idx = 0;
    for (int i = 0; i < 3; i++) {
        if (sort_values[i] == cur_sort) { cur_idx = i; break; }
    }

    igSetNextItemWidth(-1.0f);
    if (igBeginCombo("##sort", sort_labels[cur_idx], 0)) {
        for (int i = 0; i < 3; i++) {
            bool sel = (i == cur_idx);
            if (igSelectable_Bool(sort_labels[i], sel, 0,
                                  (ImVec2_c){ 0.0f, 0.0f }) && !sel) {
                ap_app_set_sort(app, sort_values[i]);
            }
            if (sel) igSetItemDefaultFocus();
        }
        igEndCombo();
    }

    igSpacing();
    igText("Search");

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", ap_app_search(app));

    igSetNextItemWidth(-1.0f);
    if (igInputTextWithHint("##search", "filename filter", buf, sizeof(buf),
                            0, NULL, NULL)) {
        ap_app_set_search(app, buf);
    }

    if (buf[0]) {
        if (igButton("Clear", (ImVec2_c){ 0.0f, 0.0f })) {
            ap_app_set_search(app, "");
        }
    }

    igEnd();
}

const ap_panel panel_library_sort_search = {
    .name       = "library_sort_search",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_sort_search_draw,
    .visible    = &g_visible,
    .menu_label = "Sort & Search",
};
