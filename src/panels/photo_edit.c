#include "panels.h"

#include "edit/stack.h"
#include "gpu/canvas.h"
#include "modules/module.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// "Edits" panel: the per-photo effect stack. One row per entry with
// enable toggle, move up / down, remove, and select-to-focus. The
// focused entry's parameters render in a separate "Edit" panel; the
// available modules render in a "Tools" panel.

static const ImVec2_c kBtnAuto = { 0.0f, 0.0f };

static bool rebuild_after_change(ap_app *app, ap_photo *photo)
{
    ap_app_wait_idle(app);
    if (ap_photo_rebuild_graph(photo) != 0) {
        return false;
    }
    // Canvas keeps stale view/sampler pointers from the previous
    // graph - rebind it to the new graph's output.
    ap_pipeline_graph *graph = ap_photo_graph(photo);
    ap_canvas_set_input(ap_app_canvas(app),
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));
    return true;
}

static void edits_panel(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("edits", NULL, 0)) {
        igEnd();
        return;
    }

    bool respect = ap_photo_respect_orientation(photo);
    if (igCheckbox("Respect EXIF orientation", &respect)) {
        ap_photo_set_respect_orientation(photo, respect);
        // Orientation changes the graph's dims, so a full reopen is
        // simpler than rebuilding in-place against a now-stale texture.
        char path_copy[4096];
        snprintf(path_copy, sizeof(path_copy), "%s", ap_photo_path(photo));
        ap_app_open_photo(app, path_copy);
        igEnd();
        return;
    }

    igSeparator();

    int n = ap_edit_stack_count(stack);
    if (n == 0) {
        igTextDisabled("(no edits - pick one from Tools)");
    }

    int do_remove = -1;
    int do_move_up = -1, do_move_dn = -1;
    bool changed = false;

    for (int i = 0; i < n; i++) {
        ap_edit_entry *e = ap_edit_stack_at(stack, i);
        const ap_module *m = ap_module_find(e->module_name);
        igPushID_Int(i);

        bool enabled = e->enabled;
        if (igCheckbox("##en", &enabled)) {
            e->enabled = enabled;
            changed = true;
        }
        igSameLine(0.0f, -1.0f);

        const char *label = m ? m->display_name : e->module_name;
        bool focused = (ap_edit_stack_focus(stack) == i);
        if (igSelectable_Bool(label, focused, 0,
                              (ImVec2_c){ 0.0f, 0.0f })) {
            ap_edit_stack_set_focus(stack, i);
        }
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("^"))  do_move_up = i;
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("v"))  do_move_dn = i;
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("x"))  do_remove  = i;

        igPopID();
    }

    if (do_remove   >= 0) { ap_edit_stack_remove(stack, do_remove); changed = true; }
    if (do_move_up  >= 0) { ap_edit_stack_move(stack, do_move_up, -1); changed = true; }
    if (do_move_dn  >= 0) { ap_edit_stack_move(stack, do_move_dn, +1); changed = true; }

    igEnd();

    if (changed) rebuild_after_change(app, photo);
}

static void tools_panel(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("tools", NULL, 0)) {
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
                ap_edit_stack_set_focus(stack, idx);
                added = true;
            }
        }
    }
    igEnd();

    if (added) rebuild_after_change(app, photo);
}

static void edit_config_panel(ap_photo *photo, ap_edit_stack *stack)
{
    (void)photo;
    if (!igBegin("edit", NULL, 0)) {
        igEnd();
        return;
    }
    int idx = ap_edit_stack_focus(stack);
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    if (!e) {
        igTextDisabled("(select an edit)");
        igEnd();
        return;
    }
    const ap_module *m = ap_module_find(e->module_name);
    if (!m) {
        igTextDisabled("(unknown module: %s)", e->module_name);
        igEnd();
        return;
    }
    igText("%s", m->display_name);
    igSeparator();
    if (m->render_params) {
        m->render_params(m, e->params);
    } else {
        igTextDisabled("(no parameters)");
    }
    igEnd();
}

static void photo_edit_draw(ap_app *app)
{
    ap_photo *photo = ap_app_photo(app);
    if (!photo) return;
    ap_edit_stack *stack = ap_photo_stack(photo);
    if (!stack) return;

    edits_panel(app, photo, stack);
    tools_panel(app, photo, stack);
    edit_config_panel(photo, stack);
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
