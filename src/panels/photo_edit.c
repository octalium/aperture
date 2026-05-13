#include "panels.h"

#include "edit/stack.h"
#include "gpu/canvas.h"
#include "modules/module.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Photo-mode workspace:
//
//   Image  - left dock: per-photo settings + viewing aids
//   Edits  - right dock (tab): the photo's stack, scrollable,
//            click row to toggle its config window, drag rows to
//            reorder.
//   Tools  - right dock (same tab group): scrollable list of every
//            user-visible module. Click adds to top of stack and
//            opens its config window.
//
// Plus a per-entry config window centered on the viewport the first
// time it appears (ImGuiCond_Appearing). show_config is in-memory
// only - every session starts with all config windows closed.

static void rebuild_after_change(ap_app *app, ap_photo *photo)
{
    ap_app_wait_idle(app);
    if (ap_photo_rebuild_graph(photo) != 0) return;
    ap_pipeline_graph *graph = ap_photo_graph(photo);
    ap_canvas_set_input(ap_app_canvas(app),
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));
}

// ---- Edits window ----------------------------------------------------

static void edits_window(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("Edits", NULL, 0)) {
        igEnd();
        return;
    }

    int n = ap_edit_stack_count(stack);
    if (n == 0) {
        igTextDisabled("No edits applied. Click a tool to add one.");
        igEnd();
        return;
    }

    int  do_remove = -1;
    int  drag_src = -1, drag_dst = -1;
    bool changed = false;

    for (int i = 0; i < n; i++) {
        ap_edit_entry *e = ap_edit_stack_at(stack, i);
        const ap_module *m = ap_module_find(e->module_name);
        const char *label = m ? m->display_name : e->module_name;

        igPushID_Int(i);

        bool enabled = e->enabled;
        if (igCheckbox("##en", &enabled)) {
            e->enabled = enabled;
            changed = true;
        }
        igSameLine(0.0f, -1.0f);

        // The selectable spans most of the row; leave a strip on the
        // right for the close button.
        float avail = igGetContentRegionAvail().x;
        ImVec2_c row_size = { avail - 24.0f, 0.0f };
        bool active = e->show_config;
        if (igSelectable_Bool(label, active,
                              ImGuiSelectableFlags_AllowOverlap, row_size)) {
            e->show_config = !e->show_config;
        }

        // Drag the row to reorder.
        if (igBeginDragDropSource(0)) {
            igSetDragDropPayload("AP_EDIT_IDX", &i, sizeof(int), 0);
            igText("%s", label);
            igEndDragDropSource();
        }
        if (igBeginDragDropTarget()) {
            const ImGuiPayload *payload =
                igAcceptDragDropPayload("AP_EDIT_IDX", 0);
            if (payload && payload->DataSize == (int)sizeof(int)) {
                int src = *(const int *)payload->Data;
                if (src != i) {
                    drag_src = src;
                    drag_dst = i;
                }
            }
            igEndDragDropTarget();
        }

        igSameLine(0.0f, -1.0f);
        if (igSmallButton("x")) do_remove = i;

        igPopID();
    }

    if (drag_src >= 0 && drag_dst >= 0) {
        ap_edit_stack_reorder(stack, drag_src, drag_dst);
        changed = true;
    }
    if (do_remove >= 0) {
        ap_edit_stack_remove(stack, do_remove);
        changed = true;
    }

    igEnd();

    if (changed) rebuild_after_change(app, photo);
}

// ---- Tools window ----------------------------------------------------

static void tools_window(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("Tools", NULL, 0)) {
        igEnd();
        return;
    }

    bool added = false;
    for (const ap_module *const *p = ap_module_registry; *p; p++) {
        const ap_module *m = *p;
        if (!m->user_visible) continue;
        if (igSelectable_Bool(m->display_name, false,
                              ImGuiSelectableFlags_AllowDoubleClick,
                              (ImVec2_c){ 0.0f, 0.0f })) {
            int idx = ap_edit_stack_add(stack, m->name);
            if (idx >= 0) {
                ap_edit_entry *e = ap_edit_stack_at(stack, idx);
                e->show_config = true;
                added = true;
            }
        }
    }
    igEnd();

    if (added) rebuild_after_change(app, photo);
}

// ---- Image window ----------------------------------------------------

static void image_window(ap_app *app, ap_photo *photo)
{
    if (!igBegin("Image", NULL, 0)) {
        igEnd();
        return;
    }

    if (igCollapsingHeader_TreeNodeFlags("Settings",
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
        bool respect = ap_photo_respect_orientation(photo);
        if (igCheckbox("Respect EXIF orientation", &respect)) {
            ap_photo_set_respect_orientation(photo, respect);
            // Orientation changes graph dims, so a full reopen is
            // simpler than rebuilding in-place against a now-stale
            // input texture.
            char path_copy[4096];
            snprintf(path_copy, sizeof(path_copy), "%s", ap_photo_path(photo));
            ap_app_open_photo(app, path_copy);
            igEnd();
            return;
        }
    }

    if (igCollapsingHeader_TreeNodeFlags("Histogram", 0)) {
        igTextDisabled("(coming soon)");
    }

    igEnd();
}

// ---- per-entry config windows ---------------------------------------

static void entry_config_windows(ap_edit_stack *stack)
{
    int n = ap_edit_stack_count(stack);
    ImGuiIO *io = igGetIO_Nil();
    ImVec2_c center = { 0.0f, 0.0f };
    if (io) {
        center.x = io->DisplaySize.x * 0.5f;
        center.y = io->DisplaySize.y * 0.5f;
    }

    for (int i = 0; i < n; i++) {
        ap_edit_entry *e = ap_edit_stack_at(stack, i);
        if (!e->show_config) continue;

        const ap_module *m = ap_module_find(e->module_name);
        const char *display = m ? m->display_name : e->module_name;
        char title[128];
        snprintf(title, sizeof(title), "%s  #%d###entry_%d", display, i + 1, i);

        igPushID_Int(i);
        igSetNextWindowPos(center, ImGuiCond_Appearing,
                           (ImVec2_c){ 0.5f, 0.5f });
        bool open = true;
        if (igBegin(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m && m->render_params) {
                m->render_params(m, e->params);
            } else {
                igTextDisabled("(no parameters)");
            }
        }
        igEnd();
        if (!open) e->show_config = false;
        igPopID();
    }
}

// ---- panel entry point ----------------------------------------------

static void photo_edit_draw(ap_app *app)
{
    ap_photo *photo = ap_app_photo(app);
    if (!photo) return;
    ap_edit_stack *stack = ap_photo_stack(photo);
    if (!stack) return;

    edits_window(app, photo, stack);
    tools_window(app, photo, stack);
    image_window(app, photo);
    entry_config_windows(stack);
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
