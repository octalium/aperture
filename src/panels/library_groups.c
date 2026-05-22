#include "panels.h"

#include "app/app.h"
#include "library/library.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Library-mode Groups panel: the group list with click-to-filter,
// group create / rename / delete, and assigning the grid selection to
// a group. Group membership lives in the photo sidecars; the registry
// (which groups exist) lives in the library db — see #167 / #175.

#define GROUPS_LIST_MAX 256

static bool g_visible = false;
static char g_new_group[AP_GROUP_NAME_LEN]   = {0};
static char g_rename_from[AP_GROUP_NAME_LEN] = {0};  // "" = not renaming
static char g_rename_buf[AP_GROUP_NAME_LEN]  = {0};
static char g_status[200]                    = {0};

// Photos in `name`, or — when name is NULL — photos in no group.
static int count_in_group(ap_library *lib, const char *name)
{
    int total = ap_library_photo_count(lib);
    int hits  = 0;
    for (int i = 0; i < total; i++) {
        const ap_photo_groups *g = ap_library_photo_groups(lib, i);
        int gc = g ? g->count : 0;
        if (!name) {
            if (gc == 0) hits++;
            continue;
        }
        for (int k = 0; k < gc; k++) {
            if (strcmp(g->names[k], name) == 0) { hits++; break; }
        }
    }
    return hits;
}

static void library_groups_draw(ap_app *app)
{
    if (!app) return;
    ap_library *lib = ap_app_library(app);
    if (!lib) return;

    if (!igBegin("Groups##library", &g_visible, 0)) {
        igEnd();
        return;
    }

    const ImVec2_c zero = { 0.0f, 0.0f };
    int  fkind          = ap_app_group_filter_kind(app);
    const char *fname   = ap_app_group_filter_name(app);
    int  sel_count      = ap_app_grid_selection_count(app);

    // --- the two built-in filter rows -------------------------------
    if (igSelectable_Bool("All Photos", fkind == AP_GROUP_FILTER_ALL,
                          0, zero)) {
        ap_app_set_group_filter(app, AP_GROUP_FILTER_ALL, NULL);
    }
    char ung[64];
    snprintf(ung, sizeof(ung), "Ungrouped (%d)", count_in_group(lib, NULL));
    if (igSelectable_Bool(ung, fkind == AP_GROUP_FILTER_UNGROUPED,
                          0, zero)) {
        ap_app_set_group_filter(app, AP_GROUP_FILTER_UNGROUPED, NULL);
    }

    igSeparator();

    // --- the registered groups --------------------------------------
    char names[GROUPS_LIST_MAX][AP_GROUP_NAME_LEN];
    int  gn = ap_library_group_list(lib, names, GROUPS_LIST_MAX);
    if (gn == 0) {
        igTextDisabled("(no groups yet)");
    }

    for (int i = 0; i < gn; i++) {
        igPushID_Int(i);
        bool renaming = (g_rename_from[0] &&
                         strcmp(g_rename_from, names[i]) == 0);
        if (renaming) {
            igSetNextItemWidth(150.0f);
            igInputText("##rn", g_rename_buf, sizeof(g_rename_buf),
                        0, NULL, NULL);
            igSameLine(0.0f, -1.0f);
            if (igButton("Save", zero) && g_rename_buf[0]) {
                ap_library_rename_group(lib, g_rename_from, g_rename_buf);
                if (fkind == AP_GROUP_FILTER_GROUP &&
                    strcmp(fname, g_rename_from) == 0) {
                    ap_app_set_group_filter(app, AP_GROUP_FILTER_GROUP,
                                            g_rename_buf);
                }
                snprintf(g_status, sizeof(g_status), "Renamed to %s",
                         g_rename_buf);
                g_rename_from[0] = '\0';
            }
            igSameLine(0.0f, -1.0f);
            if (igButton("Cancel", zero)) {
                g_rename_from[0] = '\0';
            }
            igPopID();
            continue;
        }

        char label[AP_GROUP_NAME_LEN + 32];
        snprintf(label, sizeof(label), "%s (%d)", names[i],
                 count_in_group(lib, names[i]));
        bool active = (fkind == AP_GROUP_FILTER_GROUP &&
                       strcmp(fname, names[i]) == 0);
        if (igSelectable_Bool(label, active, 0, zero)) {
            ap_app_set_group_filter(app, AP_GROUP_FILTER_GROUP, names[i]);
        }
        if (igBeginDragDropTarget()) {
            const ImGuiPayload *payload =
                igAcceptDragDropPayload("AP_THUMB_DRAG", 0);
            if (payload) {
                int w = ap_app_assign_selection_to_group(app, names[i], true);
                snprintf(g_status, sizeof(g_status),
                         "Added %d to %s", w, names[i]);
            }
            igEndDragDropTarget();
        }
        if (igBeginPopupContextItem("##ctx",
                                    ImGuiPopupFlags_MouseButtonRight)) {
            if (sel_count > 0) {
                char it[64];
                snprintf(it, sizeof(it), "Add %d selected", sel_count);
                if (igMenuItem_Bool(it, NULL, false, true)) {
                    int w = ap_app_assign_selection_to_group(
                        app, names[i], true);
                    snprintf(g_status, sizeof(g_status),
                             "Added %d to %s", w, names[i]);
                }
                snprintf(it, sizeof(it), "Remove %d selected", sel_count);
                if (igMenuItem_Bool(it, NULL, false, true)) {
                    int w = ap_app_assign_selection_to_group(
                        app, names[i], false);
                    snprintf(g_status, sizeof(g_status),
                             "Removed %d from %s", w, names[i]);
                }
                igSeparator();
            }
            if (igMenuItem_Bool("Rename...", NULL, false, true)) {
                snprintf(g_rename_from, sizeof(g_rename_from), "%s",
                         names[i]);
                snprintf(g_rename_buf, sizeof(g_rename_buf), "%s",
                         names[i]);
            }
            if (igMenuItem_Bool("Delete", NULL, false, true)) {
                ap_library_delete_group(lib, names[i]);
                if (fkind == AP_GROUP_FILTER_GROUP &&
                    strcmp(fname, names[i]) == 0) {
                    ap_app_set_group_filter(app, AP_GROUP_FILTER_ALL, NULL);
                }
                snprintf(g_status, sizeof(g_status), "Deleted %s",
                         names[i]);
            }
            igEndPopup();
        }
        igPopID();
    }

    igSeparator();

    // --- create a new (empty) group ---------------------------------
    igSetNextItemWidth(150.0f);
    bool enter = igInputText("##newgroup", g_new_group,
                             sizeof(g_new_group),
                             ImGuiInputTextFlags_EnterReturnsTrue,
                             NULL, NULL);
    igSameLine(0.0f, -1.0f);
    if ((igButton("New Group", zero) || enter) && g_new_group[0]) {
        ap_library_group_create(lib, g_new_group);
        snprintf(g_status, sizeof(g_status), "Created %s", g_new_group);
        g_new_group[0] = '\0';
    }

    igTextDisabled("drag selected thumbnails onto a group to assign,");
    igTextDisabled("or right-click a group to assign, rename, or delete");
    if (g_status[0]) {
        igSeparator();
        igTextDisabled("%s", g_status);
    }

    igEnd();
}

const ap_panel panel_library_groups = {
    .name       = "library_groups",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_groups_draw,
    .visible    = &g_visible,
    .menu_label = "Groups",
};
