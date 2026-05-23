#include "panels.h"

#include "app/app.h"
#include "library/library.h"
#include "photo/culling.h"

#include "cimgui.h"

#include <string.h>

// Library-mode Filter panel: orders + narrows the visible grid. Sort
// order, filename search, and culling-state filters (rating / flag /
// colour label) all live here. Each filter criterion is independent;
// active criteria combine with AND. All fields default to "any" (no
// restriction). The panel is visible by default.

static bool g_visible = true;

// Each filter is a Combo so the dropdown's items live in the popup's
// own ID scope — that lets every combo reuse the "Any" label without
// the ID collision that plain row-of-selectables would trigger.
static int combo_pick(const char *id, const char *label, int current,
                      const char *const items[], int count)
{
    int picked = current;
    igText("%s", label);
    igSameLine(80.0f, -1.0f);
    igSetNextItemWidth(-1.0f);
    if (igBeginCombo(id, items[current], 0)) {
        for (int i = 0; i < count; i++) {
            bool sel = (i == current);
            if (igSelectable_Bool(items[i], sel, 0,
                                  (ImVec2_c){ 0.0f, 0.0f }) && !sel) {
                picked = i;
            }
            if (sel) igSetItemDefaultFocus();
        }
        igEndCombo();
    }
    return picked;
}

static void library_filter_draw(ap_app *app)
{
    if (!app) return;
    if (!ap_app_library(app)) return;

    if (!igBegin("Filter##library", &g_visible, 0)) {
        igEnd();
        return;
    }

    // -- Sort --------------------------------------------------------
    static const char *sort_labels[] = {
        "Filename", "Capture time", "Date added", "Rating",
    };
    static const ap_library_sort sort_values[] = {
        AP_SORT_PATH, AP_SORT_CAPTURE_TIME, AP_SORT_ADDED_AT, AP_SORT_RATING,
    };
    const int sort_count = (int)(sizeof(sort_labels) / sizeof(sort_labels[0]));
    ap_library_sort cur_sort = ap_app_sort(app);
    int cur_sort_idx = 0;
    for (int i = 0; i < sort_count; i++) {
        if (sort_values[i] == cur_sort) { cur_sort_idx = i; break; }
    }
    int new_sort_idx = combo_pick("##sort", "Sort by", cur_sort_idx,
                                  sort_labels, sort_count);
    if (new_sort_idx != cur_sort_idx) {
        ap_app_set_sort(app, sort_values[new_sort_idx]);
    }

    // -- Search ------------------------------------------------------
    igText("Search");
    igSameLine(80.0f, -1.0f);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", ap_app_search(app));
    igSetNextItemWidth(buf[0] ? -60.0f : -1.0f);
    if (igInputTextWithHint("##search", "filename filter", buf, sizeof(buf),
                            0, NULL, NULL)) {
        ap_app_set_search(app, buf);
    }
    if (buf[0]) {
        igSameLine(0.0f, 4.0f);
        if (igButton("Clear##search", (ImVec2_c){ 0.0f, 0.0f })) {
            ap_app_set_search(app, "");
        }
    }

    igSpacing();
    igSeparator();
    igSpacing();

    // -- Culling filters --------------------------------------------
    ap_culling_filter cf = ap_app_culling_filter(app);
    bool changed = false;

    static const char *rating_labels[] = {
        "Any", ">= 1", ">= 2", ">= 3", ">= 4", ">= 5",
    };
    int new_rating = combo_pick("##rating", "Rating", cf.rating_min,
                                rating_labels,
                                (int)(sizeof(rating_labels) /
                                      sizeof(rating_labels[0])));
    if (new_rating != cf.rating_min) {
        cf.rating_min = new_rating;
        changed = true;
    }

    static const char *flag_labels[] = { "Any", "Pick", "Reject" };
    static const ap_flag flag_values[] = {
        AP_FLAG_NONE, AP_FLAG_PICK, AP_FLAG_REJECT,
    };
    int cur_flag_idx = 0;
    for (int i = 0; i < 3; i++) {
        if (flag_values[i] == cf.flag) { cur_flag_idx = i; break; }
    }
    int new_flag_idx = combo_pick("##flag", "Flag", cur_flag_idx,
                                  flag_labels, 3);
    if (new_flag_idx != cur_flag_idx) {
        cf.flag = flag_values[new_flag_idx];
        changed = true;
    }

    // Colour combo: a swatch is drawn beside the current-selection
    // text and beside each item in the popup. Built inline since the
    // generic combo_pick doesn't carry per-item colour.
    static const struct {
        ap_color_label val;
        const char    *label;
    } color_opts[] = {
        { AP_COLOR_NONE,   "Any"    },
        { AP_COLOR_RED,    "Red"    },
        { AP_COLOR_YELLOW, "Yellow" },
        { AP_COLOR_GREEN,  "Green"  },
        { AP_COLOR_BLUE,   "Blue"   },
        { AP_COLOR_PURPLE, "Purple" },
    };
    int cur_color_idx = 0;
    for (int i = 0; i < AP_COLOR_LABEL_COUNT; i++) {
        if (color_opts[i].val == cf.color) { cur_color_idx = i; break; }
    }
    igText("Color");
    igSameLine(80.0f, -1.0f);
    igSetNextItemWidth(-1.0f);
    if (igBeginCombo("##color", color_opts[cur_color_idx].label, 0)) {
        for (int i = 0; i < AP_COLOR_LABEL_COUNT; i++) {
            unsigned rgba = ap_color_label_rgba(color_opts[i].val);
            if (rgba) {
                ImVec4_c col = igColorConvertU32ToFloat4(rgba);
                igColorButton("##sw", col,
                              ImGuiColorEditFlags_NoTooltip |
                              ImGuiColorEditFlags_NoBorder  |
                              ImGuiColorEditFlags_NoPicker,
                              (ImVec2_c){ 10.0f, 10.0f });
                igSameLine(0.0f, 6.0f);
            }
            bool sel = (i == cur_color_idx);
            if (igSelectable_Bool(color_opts[i].label, sel, 0,
                                  (ImVec2_c){ 0.0f, 0.0f }) && !sel) {
                cf.color = color_opts[i].val;
                changed = true;
            }
            if (sel) igSetItemDefaultFocus();
        }
        igEndCombo();
    }

    bool any_active = (cf.rating_min > 0 || cf.flag != AP_FLAG_NONE ||
                       cf.color != AP_COLOR_NONE);
    igSpacing();
    if (!any_active) igBeginDisabled(true);
    if (igButton("Clear filters", (ImVec2_c){ -1.0f, 0.0f })) {
        cf = (ap_culling_filter){ 0, AP_FLAG_NONE, AP_COLOR_NONE };
        changed = true;
    }
    if (!any_active) igEndDisabled();

    if (changed) {
        ap_app_set_culling_filter(app, cf);
    }

    igEnd();
}

const ap_panel panel_library_filter = {
    .name       = "library_filter",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_filter_draw,
    .visible    = &g_visible,
    .menu_label = "Filter",
};
