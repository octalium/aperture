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
//   Left column:
//     Image      - per-photo settings (Respect EXIF, ...)
//     Histogram  - viewing aid
//   Right column:
//     Tools      - palette of every user-visible module
//     Edits      - the photo's stack (scrollable, drag to reorder,
//                  right-click for Rename / Reset / Remove)
//
// Per-entry config windows pop up centered on first appearance; the
// title is the entry's display name (auto-suffixed for duplicates,
// or whatever the user renamed it to). show_config is in-memory
// only.

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

// ---- Edits window ---------------------------------------------------

// State for the rename modal. Module-scoped so the popup survives
// across frames while the user types.
static int  g_rename_idx = -1;
static char g_rename_buf[AP_EDIT_DISPLAY_LEN];

static void open_rename_popup(int idx, const char *current)
{
    g_rename_idx = idx;
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", current ? current : "");
}

static bool draw_rename_modal(ap_edit_stack *stack)
{
    if (g_rename_idx < 0) return false;
    if (!igIsPopupOpen_Str("Rename Edit", 0)) {
        igOpenPopup_Str("Rename Edit", 0);
    }

    bool changed = false;
    if (igBeginPopupModal("Rename Edit", NULL,
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        igText("New name:");
        igInputText("##rn", g_rename_buf, sizeof(g_rename_buf), 0, NULL, NULL);

        if (igButton("Save", (ImVec2_c){ 120.0f, 0.0f })) {
            ap_edit_entry *e = ap_edit_stack_at(stack, g_rename_idx);
            if (e) {
                snprintf(e->display_name, sizeof(e->display_name),
                         "%s", g_rename_buf);
                changed = true;
            }
            g_rename_idx = -1;
            igCloseCurrentPopup();
        }
        igSameLine(0.0f, -1.0f);
        if (igButton("Clear", (ImVec2_c){ 120.0f, 0.0f })) {
            ap_edit_entry *e = ap_edit_stack_at(stack, g_rename_idx);
            if (e) {
                e->display_name[0] = '\0';
                changed = true;
            }
            g_rename_idx = -1;
            igCloseCurrentPopup();
        }
        igSameLine(0.0f, -1.0f);
        if (igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })) {
            g_rename_idx = -1;
            igCloseCurrentPopup();
        }
        igEndPopup();
    } else {
        g_rename_idx = -1;
    }
    return changed;
}

static void edits_window(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("Edits", NULL, 0)) {
        igEnd();
        return;
    }

    int n = ap_edit_stack_count(stack);

    igTextDisabled("drag rows to reorder  ·  right-click for actions");
    igSeparator();

    if (n == 0) {
        igTextDisabled("No edits applied. Click a tool to add one.");
        igEnd();
        return;
    }

    int  do_remove      = -1;
    int  do_reset       = -1;
    int  do_rename      = -1;
    int  drag_src       = -1;
    int  drag_dst       = -1;
    bool changed        = false;

    for (int i = 0; i < n; i++) {
        ap_edit_entry *e = ap_edit_stack_at(stack, i);
        char label_buf[AP_EDIT_DISPLAY_LEN];
        const char *label = ap_edit_stack_label_at(stack, i,
                                                   label_buf, sizeof(label_buf));

        igPushID_Int(i);

        bool enabled = e->enabled;
        if (igCheckbox("##en", &enabled)) {
            e->enabled = enabled;
            changed = true;
        }
        igSameLine(0.0f, -1.0f);

        // Reserve room on the right for the close button.
        float avail = igGetContentRegionAvail().x;
        ImVec2_c row_size = { avail - 28.0f, 0.0f };
        bool active = e->show_config;
        if (igSelectable_Bool(label, active,
                              ImGuiSelectableFlags_AllowOverlap, row_size)) {
            e->show_config = !e->show_config;
        }

        // Drag-and-drop reorder.
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

        // Right-click context menu on the row's primary hit-area.
        if (igBeginPopupContextItem(NULL, ImGuiPopupFlags_MouseButtonRight)) {
            if (igMenuItem_Bool("Open config", NULL, false, true)) {
                e->show_config = true;
            }
            if (igMenuItem_Bool("Rename...", NULL, false, true)) {
                do_rename = i;
            }
            if (igMenuItem_Bool("Reset parameters", NULL, false,
                                e->display_name[0] != 0 ||
                                ap_module_find(e->module_name) != NULL)) {
                do_reset = i;
            }
            igSeparator();
            if (igMenuItem_Bool("Remove", "Del", false, true)) {
                do_remove = i;
            }
            igEndPopup();
        }

        igSameLine(0.0f, -1.0f);
        if (igSmallButton("x")) do_remove = i;

        igPopID();
    }

    if (drag_src >= 0 && drag_dst >= 0) {
        ap_edit_stack_reorder(stack, drag_src, drag_dst);
        changed = true;
    }
    if (do_reset >= 0) {
        ap_edit_stack_reset(stack, do_reset);
        changed = true;
    }
    if (do_remove >= 0) {
        ap_edit_stack_remove(stack, do_remove);
        changed = true;
    }
    if (do_rename >= 0) {
        ap_edit_entry *e = ap_edit_stack_at(stack, do_rename);
        char buf[AP_EDIT_DISPLAY_LEN];
        const char *current = e->display_name[0]
            ? e->display_name
            : ap_edit_stack_label_at(stack, do_rename, buf, sizeof(buf));
        open_rename_popup(do_rename, current);
    }

    if (draw_rename_modal(stack)) changed = true;

    igEnd();

    if (changed) rebuild_after_change(app, photo);
}

// ---- Tools window ---------------------------------------------------

static void tools_window(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("Tools", NULL, 0)) {
        igEnd();
        return;
    }

    igTextDisabled("click to add to the top of the stack");
    igSeparator();

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

// ---- Image window ---------------------------------------------------

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

    igEnd();
}

// ---- Histogram window -----------------------------------------------

static void histogram_window(ap_photo *photo)
{
    (void)photo;
    if (!igBegin("Histogram", NULL, 0)) {
        igEnd();
        return;
    }
    igTextDisabled("(coming soon: live histogram from the rendered image)");
    igEnd();
}

// ---- per-entry config windows ---------------------------------------

static bool config_window(ap_app *app, ap_photo *photo,
                          ap_edit_stack *stack, int idx)
{
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    if (!e->show_config) return false;
    const ap_module *m = ap_module_find(e->module_name);
    if (!m) return false;

    char label_buf[AP_EDIT_DISPLAY_LEN];
    const char *display = ap_edit_stack_label_at(stack, idx,
                                                 label_buf, sizeof(label_buf));
    // ###entry_<i> = stable hidden ID so each entry gets its own
    // window state even when names collide.
    char title[128];
    snprintf(title, sizeof(title), "%s###entry_%d", display, idx);

    ImGuiIO *io = igGetIO_Nil();
    ImVec2_c center = { 0.0f, 0.0f };
    if (io) {
        center.x = io->DisplaySize.x * 0.5f;
        center.y = io->DisplaySize.y * 0.5f;
    }
    igSetNextWindowPos(center, ImGuiCond_Appearing,
                       (ImVec2_c){ 0.5f, 0.5f });

    bool open = true;
    bool changed = false;

    igPushID_Int(idx);
    if (igBegin(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (igSmallButton("Reset")) {
            ap_edit_stack_reset(stack, idx);
            changed = true;
        }
        igSameLine(0.0f, -1.0f);
        igTextDisabled("double-click a value to reset it");
        igSeparator();

        if (m->render_params) {
            // Capture the pre-render snapshot, render, and compare so
            // a slider drag rebuilds the graph (params change but
            // pixels need re-dispatch).
            float before[AP_EDIT_PARAMS_SLOTS];
            memcpy(before, e->params, sizeof(before));
            m->render_params(m, e->params);
            // Per-slot double-click reset: ImGui doesn't expose a
            // "was this specific item double-clicked" lookup after
            // the fact, so each module's render_params is expected to
            // check itself via igIsItemHovered + igIsMouseDoubleClicked
            // (see exposure.c / tone.c). The whole-entry reset above
            // is the catch-all.
            if (memcmp(before, e->params, sizeof(before)) != 0) changed = true;
        } else {
            igTextDisabled("(no parameters)");
        }
    }
    igEnd();
    igPopID();

    if (!open) e->show_config = false;
    if (changed) rebuild_after_change(app, photo);
    return changed;
}

static void entry_config_windows(ap_app *app, ap_photo *photo,
                                 ap_edit_stack *stack)
{
    int n = ap_edit_stack_count(stack);
    for (int i = 0; i < n; i++) {
        config_window(app, photo, stack, i);
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
    histogram_window(photo);
    entry_config_windows(app, photo, stack);
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
