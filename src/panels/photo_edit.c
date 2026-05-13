#include "panels.h"

#include "edit/stack.h"
#include "gpu/canvas.h"
#include "modules/module.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Three photo-mode windows + one short-lived window per visible
// stack entry. Names map to user intent:
//
//   Edits   - the per-photo stack, top-to-bottom = first-to-last.
//             Reorder, enable / disable, click row to open the
//             entry's config window.
//   Tools   - palette of every registered user-visible module. Click
//             pushes a new entry on top of the stack and opens its
//             config window.
//   Image   - per-photo settings (EXIF orientation) and viewing aids
//             (histogram lands here in a follow-up).
//
// Per-entry config windows are ephemeral - state lives on the entry
// in memory only, never persisted; each open starts centered on the
// viewport (ImGuiCond_Appearing) so reopening a photo doesn't carry
// stale layout.

// ---- helpers ----------------------------------------------------------

static const ImVec2_c kBtnAuto = { 0.0f, 0.0f };

static void rebuild_after_change(ap_app *app, ap_photo *photo)
{
    ap_app_wait_idle(app);
    if (ap_photo_rebuild_graph(photo) != 0) return;
    // Canvas keeps stale view/sampler from the previous graph — rebind.
    ap_pipeline_graph *graph = ap_photo_graph(photo);
    ap_canvas_set_input(ap_app_canvas(app),
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));
}

// ---- Edits window -----------------------------------------------------

static void edits_window(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("Edits", NULL, 0)) {
        igEnd();
        return;
    }

    int n = ap_edit_stack_count(stack);
    if (n == 0) {
        igTextDisabled("(no edits — pick one from Tools)");
        igEnd();
        return;
    }

    int do_remove  = -1;
    int do_move_up = -1, do_move_dn = -1;
    bool stack_changed = false;

    for (int i = 0; i < n; i++) {
        ap_edit_entry *e = ap_edit_stack_at(stack, i);
        const ap_module *m = ap_module_find(e->module_name);
        igPushID_Int(i);

        bool enabled = e->enabled;
        if (igCheckbox("##en", &enabled)) {
            e->enabled = enabled;
            stack_changed = true;
        }
        igSameLine(0.0f, -1.0f);

        const char *label = m ? m->display_name : e->module_name;
        // The row label is the click target that opens the entry's
        // config window. Reuses Selectable for the hit area; the
        // toggle for `show_config` lives directly on the entry.
        bool selected = e->show_config;
        if (igSelectable_Bool(label, selected, 0,
                              (ImVec2_c){ 0.0f, 0.0f })) {
            e->show_config = !e->show_config;
        }
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("^"))  do_move_up = i;
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("v"))  do_move_dn = i;
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("x"))  do_remove  = i;

        igPopID();
    }

    if (do_remove  >= 0) {
        ap_edit_stack_remove(stack, do_remove);
        stack_changed = true;
    }
    if (do_move_up >= 0) {
        ap_edit_stack_move(stack, do_move_up, -1);
        stack_changed = true;
    }
    if (do_move_dn >= 0) {
        ap_edit_stack_move(stack, do_move_dn, +1);
        stack_changed = true;
    }

    igEnd();

    if (stack_changed) rebuild_after_change(app, photo);
}

// ---- Tools window -----------------------------------------------------

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
        if (igButton(m->display_name, kBtnAuto)) {
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

// ---- Image window -----------------------------------------------------

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
            // Orientation flip changes the graph's dims, so a full
            // reopen is simpler than rebuilding in-place against a
            // now-mismatched input texture.
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

// ---- per-entry config windows ----------------------------------------

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

// ---- panel entry point -----------------------------------------------

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
