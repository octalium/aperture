#include "panels.h"

#include "app/app.h"
#include "photo/culling.h"

#include "cimgui.h"

// Library-mode Filter panel: restrict the visible grid by rating threshold,
// pick/reject flag, and colour label. Each criterion is independent; all
// active criteria must match (AND semantics). All fields default to "any"
// (no restriction). The panel is visible by default.

static bool g_visible = true;

// A compact selectable row that renders `label` with optional left-side
// colour swatch. Returns true when clicked while not already selected.
static bool filter_selectable(const char *id, const char *label,
                               bool active, unsigned swatch_rgba)
{
    bool clicked = false;
    if (swatch_rgba) {
        ImVec4_c col = igColorConvertU32ToFloat4(swatch_rgba);
        igColorButton(id, col,
                      ImGuiColorEditFlags_NoTooltip |
                      ImGuiColorEditFlags_NoBorder  |
                      ImGuiColorEditFlags_NoPicker,
                      (ImVec2_c){ 10.0f, 10.0f });
        igSameLine(0.0f, 4.0f);
    }
    if (igSelectable_Bool(label, active, 0, (ImVec2_c){ 0.0f, 0.0f })) {
        clicked = !active;
    }
    return clicked;
}

static void library_filter_draw(ap_app *app)
{
    if (!app) return;
    if (!ap_app_library(app)) return;

    if (!igBegin("Filter##library", &g_visible, 0)) {
        igEnd();
        return;
    }

    ap_culling_filter cf = ap_app_culling_filter(app);
    bool changed = false;

    igText("Rating");

    static const char *rating_labels[] = {
        "Any", ">= 1", ">= 2", ">= 3", ">= 4", ">= 5",
    };
    for (int r = 0; r <= AP_RATING_MAX; r++) {
        bool active = (cf.rating_min == r);
        if (igSelectable_Bool(rating_labels[r], active, 0,
                              (ImVec2_c){ 0.0f, 0.0f }) && !active) {
            cf.rating_min = r;
            changed = true;
        }
        if (r < AP_RATING_MAX) igSameLine(0.0f, 6.0f);
    }

    igSpacing();
    igSeparator();
    igSpacing();

    igText("Flag");

    struct { ap_flag val; const char *label; } flag_opts[] = {
        { AP_FLAG_NONE,   "Any"    },
        { AP_FLAG_PICK,   "Pick"   },
        { AP_FLAG_REJECT, "Reject" },
    };
    for (int i = 0; i < 3; i++) {
        bool active = (cf.flag == flag_opts[i].val);
        if (igSelectable_Bool(flag_opts[i].label, active, 0,
                              (ImVec2_c){ 0.0f, 0.0f }) && !active) {
            cf.flag = flag_opts[i].val;
            changed = true;
        }
        if (i < 2) igSameLine(0.0f, 6.0f);
    }

    igSpacing();
    igSeparator();
    igSpacing();

    igText("Color");

    static const struct {
        ap_color_label val;
        const char    *id;
        const char    *label;
    } color_opts[] = {
        { AP_COLOR_NONE,   "##cf_none",   "Any"    },
        { AP_COLOR_RED,    "##cf_red",    "Red"    },
        { AP_COLOR_YELLOW, "##cf_yellow", "Yellow" },
        { AP_COLOR_GREEN,  "##cf_green",  "Green"  },
        { AP_COLOR_BLUE,   "##cf_blue",   "Blue"   },
        { AP_COLOR_PURPLE, "##cf_purple", "Purple" },
    };
    for (int i = 0; i < AP_COLOR_LABEL_COUNT; i++) {
        bool active = (cf.color == color_opts[i].val);
        unsigned swatch = ap_color_label_rgba(color_opts[i].val);
        if (filter_selectable(color_opts[i].id, color_opts[i].label,
                              active, swatch) && !active) {
            cf.color = color_opts[i].val;
            changed = true;
        }
    }

    igSpacing();
    igSeparator();
    igSpacing();

    bool any_active = (cf.rating_min > 0 || cf.flag != AP_FLAG_NONE ||
                       cf.color != AP_COLOR_NONE);
    if (!any_active) {
        igBeginDisabled(true);
    }
    if (igButton("Clear All", (ImVec2_c){ 0.0f, 0.0f })) {
        cf = (ap_culling_filter){ 0, AP_FLAG_NONE, AP_COLOR_NONE };
        changed = true;
    }
    if (!any_active) {
        igEndDisabled();
    }

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
