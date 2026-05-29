#include "panels.h"

#include "app/app.h"
#include "edit/stack.h"
#include "gpu/pipeline_graph.h"
#include "library/library.h"
#include "modules/module.h"
#include "photo/photo.h"
#include "sidecar/sidecar.h"
#include "ui/modal_kbd.h"

#include "cimgui.h"

#include "core/compat.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

// Any structural change to the edit stack (add / remove / move /
// enable / disable) needs the pipeline graph rebuilt. ap_app owns
// that - it has to re-point the GPU's current-graph pointer *and*
// the canvas binding, both of which referenced the now-freed old
// graph. Doing only the canvas rebind here was a use-after-free.
static void rebuild_after_change(ap_app *app, ap_photo *photo)
{
    (void)photo;
    ap_app_rebuild_photo_graph(app);
}

// State for the rename modal. Module-scoped so the popup survives
// across frames while the user types.
static int  g_rename_idx = -1;
static char g_rename_buf[AP_EDIT_DISPLAY_LEN];

static void open_rename_popup(int idx, const char *current)
{
    g_rename_idx = idx;
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", current ? current : "");
}

static bool draw_rename_modal(ap_app *app, ap_edit_stack *stack)
{
    static bool just_opened = false;
    if (g_rename_idx < 0) return false;
    if (!igIsPopupOpen_Str("Rename Edit", 0)) {
        igOpenPopup_Str("Rename Edit", 0);
        just_opened = true;
    }

    bool changed = false;
    if (igBeginPopupModal("Rename Edit", NULL,
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        igText("New name:");
        if (just_opened) igSetKeyboardFocusHere(0);
        bool enter_in_input = igInputText("##rn", g_rename_buf,
                                          sizeof(g_rename_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue,
                                          NULL, NULL);

        bool save = igButton("Save", (ImVec2_c){ 120.0f, 0.0f })
                 || enter_in_input
                 || ap_modal_enter_pressed();
        igSameLine(0.0f, -1.0f);
        bool clear = igButton("Clear", (ImVec2_c){ 120.0f, 0.0f });
        igSameLine(0.0f, -1.0f);
        bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })
                   || ap_modal_esc_pressed();

        if (save) {
            ap_edit_entry *e = ap_edit_stack_at(stack, g_rename_idx);
            if (e) {
                ap_app_edit_snapshot(app);
                snprintf(e->display_name, sizeof(e->display_name),
                         "%s", g_rename_buf);
                changed = true;
            }
            g_rename_idx = -1;
            igCloseCurrentPopup();
        } else if (clear) {
            ap_edit_entry *e = ap_edit_stack_at(stack, g_rename_idx);
            if (e) {
                ap_app_edit_snapshot(app);
                e->display_name[0] = '\0';
                changed = true;
            }
            g_rename_idx = -1;
            igCloseCurrentPopup();
        } else if (cancel) {
            g_rename_idx = -1;
            igCloseCurrentPopup();
        }
        just_opened = false;
        igEndPopup();
    } else {
        g_rename_idx = -1;
        just_opened = false;
    }
    return changed;
}

static bool    g_save_as_open       = false;
static bool    g_apply_open         = false;
static char    g_save_as_name[AP_PIPELINE_NAME_LEN] = {0};
static char    g_save_status[256]   = {0};
static char    g_apply_status[256]  = {0};
static bool    g_save_as_pending_overwrite = false;
static int64_t g_apply_pending_id   = -1;   // id of pipeline awaiting confirm
static char    g_apply_pending_name[AP_PIPELINE_NAME_LEN] = {0};

static void open_save_as_popup(void)
{
    g_save_as_open  = true;
    g_save_status[0] = '\0';
    g_save_as_pending_overwrite = false;
    g_save_as_name[0] = '\0';
}

static void open_apply_popup(void)
{
    g_apply_open          = true;
    g_apply_status[0]     = '\0';
    g_apply_pending_id    = -1;
    g_apply_pending_name[0] = '\0';
}

static void edits_window(ap_app *app, ap_photo *photo, ap_edit_stack *stack)
{
    if (!igBegin("Edits", NULL, 0)) {
        igEnd();
        return;
    }

    int n = ap_edit_stack_count(stack);

    // Pipeline actions live at the top of the Edits window so both
    // appear regardless of whether the stack has any entries. Save-as
    // is disabled on an empty stack — nothing meaningful to save.
    if (n == 0) igBeginDisabled(true);
    if (igButton("Save as pipeline...", (ImVec2_c){ 0.0f, 0.0f })) {
        open_save_as_popup();
    }
    if (n == 0) igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (igButton("Apply pipeline...", (ImVec2_c){ 0.0f, 0.0f })) {
        open_apply_popup();
    }

    igSeparator();

    // Copy / Paste edits clipboard. Copy is disabled on an empty stack.
    if (n == 0) igBeginDisabled(true);
    if (igButton("Copy edits", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_app_copy_edits(app);
    }
    if (n == 0) igEndDisabled();
    igSameLine(0.0f, -1.0f);

    bool has_clipboard = ap_app_has_edit_clipboard(app);
    if (!has_clipboard) igBeginDisabled(true);
    if (igButton("Paste edits", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_app_paste_edits(app);
    }
    if (!has_clipboard) igEndDisabled();

    igSeparator();

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
    int  toggled_idx    = -1;   // entry_idx of the last enable checkbox
    bool toggled_val    = false; // new enabled value for that entry

    for (int i = 0; i < n; i++) {
        ap_edit_entry *e = ap_edit_stack_at(stack, i);
        char label_buf[AP_EDIT_DISPLAY_LEN];
        const char *label = ap_edit_stack_label_at(stack, i,
                                                   label_buf, sizeof(label_buf));

        igPushID_Int(i);

        bool enabled = e->enabled;
        if (igCheckbox("##en", &enabled)) {
            ap_app_edit_snapshot(app);
            e->enabled   = enabled;
            toggled_idx  = i;
            toggled_val  = enabled;
            changed      = true;
        }
        igSameLine(0.0f, -1.0f);

        // Reserve room on the right for the close button.
        float avail = igGetContentRegionAvail().x;
        ImVec2_c row_size = { avail - 28.0f, 0.0f };
        bool active = e->show_config;
        if (igSelectable_Bool(label, active,
                              ImGuiSelectableFlags_AllowOverlap, row_size)) {
            e->show_config = !e->show_config;
            ap_edit_stack_set_focus(stack, i);
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
            if (igMenuItem_Bool("Remove", NULL, false, true)) {
                do_remove = i;
            }
            igEndPopup();
        }

        igSameLine(0.0f, -1.0f);
        if (igSmallButton("x")) do_remove = i;

        igPopID();
    }

    if (drag_src >= 0 && drag_dst >= 0) {
        ap_app_edit_snapshot(app);
        ap_edit_stack_reorder(stack, drag_src, drag_dst);
        changed = true;
    }
    if (do_reset >= 0) {
        ap_app_edit_snapshot(app);
        ap_edit_stack_reset(stack, do_reset);
        changed = true;
    }
    if (do_remove >= 0) {
        ap_app_edit_snapshot(app);
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

    if (draw_rename_modal(app, stack)) changed = true;

    igEnd();

    if (changed) {
        // A pure enable/disable toggle can update the stage skip flag
        // in the existing graph without tearing it down and rebuilding.
        // If set_stage_skip returns -1 the stage isn't in the graph
        // (e.g. demosaic, which can't be skipped via copy), so fall
        // back to a full rebuild. Any structural change (remove, reorder,
        // reset) always rebuilds.
        bool needs_rebuild = (drag_src >= 0 || do_reset >= 0 ||
                              do_remove >= 0);
        if (!needs_rebuild && toggled_idx >= 0) {
            ap_pipeline_graph *graph = ap_photo_graph(photo);
            if (!graph || ap_pipeline_graph_set_stage_skip(
                              graph, toggled_idx, !toggled_val) != 0) {
                needs_rebuild = true;
            }
        } else if (toggled_idx < 0) {
            needs_rebuild = true;
        }
        if (needs_rebuild) rebuild_after_change(app, photo);
    }
}

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
        if (igSelectable_Bool(m->display_name, false, 0,
                              (ImVec2_c){ 0.0f, 0.0f })) {
            ap_app_edit_snapshot(app);
            if (ap_edit_stack_add(stack, m->name) >= 0) {
                added = true;
            }
        }
    }
    igEnd();

    if (added) rebuild_after_change(app, photo);
}

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

// GPU-computed 256-bin RGBL histogram of the rendered display image.
//
// The histogram shader runs at the tail of every pipeline graph dispatch,
// so it updates every frame the edit stack changes — including slider
// edits that only push push-constants. The host reads from a host-visible
// storage buffer that was made host-coherent by a memory barrier before
// the frame's fence signals.
//
// Channel layout in the GPU buffer (matching histogram.comp):
//   [0..255]   R
//   [256..511] G
//   [512..767] B
//   [768..1023] Luma (Rec.709)
//
// Clipping indicators: shadow clipping is shown when the sum of the
// lowest two bins for any channel is > 0.5% of total pixels; highlight
// clipping analogously for the highest two bins.

#define HIST_BINS 256

static struct {
    uint32_t bins[4][HIST_BINS]; // R, G, B, Luma
    uint32_t max_bin;
    bool     valid;
} g_hist;

static bool g_hist_log_scale = false;

static void histogram_refresh(ap_pipeline_graph *graph)
{
    uint32_t raw[1024];
    if (!ap_pipeline_graph_histogram_read(graph, raw)) {
        g_hist.valid = false;
        return;
    }
    for (int c = 0; c < 4; c++) {
        for (int b = 0; b < HIST_BINS; b++) {
            g_hist.bins[c][b] = raw[c * HIST_BINS + b];
        }
    }
    uint32_t mx = 0;
    for (int c = 0; c < 4; c++) {
        for (int b = 0; b < HIST_BINS; b++) {
            if (g_hist.bins[c][b] > mx) mx = g_hist.bins[c][b];
        }
    }
    g_hist.max_bin = mx;
    g_hist.valid   = (mx > 0);
}

static float bin_height(uint32_t count, uint32_t max_bin)
{
    if (max_bin == 0) return 0.0f;
    if (g_hist_log_scale) {
        float denom = log1pf((float)max_bin);
        if (denom <= 0.0f) return 0.0f;
        return log1pf((float)count) / denom;
    }
    return (float)count / (float)max_bin;
}

// Fraction of total pixels in the first/last `n_bins` bins for channel c.
static float clipping_fraction(int c, int n_bins, bool high)
{
    uint64_t clipped = 0;
    uint64_t total   = 0;
    for (int b = 0; b < HIST_BINS; b++) {
        total += g_hist.bins[c][b];
        if (!high && b < n_bins)             clipped += g_hist.bins[c][b];
        if (high  && b >= HIST_BINS - n_bins) clipped += g_hist.bins[c][b];
    }
    if (total == 0) return 0.0f;
    return (float)clipped / (float)total;
}

static void histogram_window(ap_photo *photo)
{
    if (!igBegin("Histogram", NULL, 0)) {
        igEnd();
        return;
    }

    ap_pipeline_graph *graph = ap_photo_graph(photo);
    if (graph) {
        histogram_refresh(graph);
    } else {
        g_hist.valid = false;
    }

    // Shadow / highlight clipping indicators. A channel is "clipped"
    // when more than 0.5% of its pixels land in the outermost 2 bins.
    // Draw a coloured strip at the left (shadow) or right (highlight)
    // edge of the plot area. R, G, B each get their own thin column;
    // the indicator is only drawn when that channel clips.
    bool shadow_clip[3]    = { false, false, false };
    bool highlight_clip[3] = { false, false, false };
    if (g_hist.valid) {
        for (int c = 0; c < 3; c++) {
            shadow_clip[c]    = clipping_fraction(c, 2, false) > 0.005f;
            highlight_clip[c] = clipping_fraction(c, 2, true)  > 0.005f;
        }
    }

    igCheckbox("log scale", &g_hist_log_scale);

    ImVec2_c origin = igGetCursorScreenPos();
    ImVec2_c avail  = igGetContentRegionAvail();
    float plot_w = avail.x;
    float plot_h = avail.y;
    if (plot_w < 64.0f) plot_w = 64.0f;
    if (plot_h < 60.0f) plot_h = 60.0f;

    ImDrawList *dl = igGetWindowDrawList();
    ImVec2_c tl = origin;
    ImVec2_c br = { origin.x + plot_w, origin.y + plot_h };
    ImDrawList_AddRectFilled(dl, tl, br, 0xFF101418, 0.0f, 0);

    if (g_hist.valid) {
        // Channel colors — ABGR-packed; alpha 0x80 lets overlapping
        // channels mix visually. Channel 3 = Luma, drawn last so it sits
        // on top as a reference curve.
        const uint32_t colors[4] = {
            0x800000FF, // R
            0x8000FF00, // G
            0x80FF0000, // B
            0xA0FFFFFF, // Luma (white, slightly more opaque)
        };

        float x_step = plot_w / (float)HIST_BINS;
        for (int b = 0; b < HIST_BINS; b++) {
            float x0 = origin.x + (float)b * x_step;
            float x1 = origin.x + (float)(b + 1) * x_step;
            float cx = 0.5f * (x0 + x1);
            for (int c = 0; c < 4; c++) {
                float t = bin_height(g_hist.bins[c][b], g_hist.max_bin);
                if (t <= 0.0f) continue;
                float y = origin.y + plot_h - t * plot_h;
                ImDrawList_AddLine(dl,
                                   (ImVec2_c){ cx, origin.y + plot_h },
                                   (ImVec2_c){ cx, y },
                                   colors[c], 1.0f);
            }
        }

        // Clipping indicators: 4-px-wide coloured strips at the left
        // (shadow) and right (highlight) edges of the plot.
        const uint32_t clip_colors[3] = {
            0xFF2222FF, // R shadow/highlight
            0xFF22FF22, // G
            0xFFFF2222, // B
        };
        float strip_w = 4.0f;
        for (int c = 0; c < 3; c++) {
            float x_shadow = origin.x + (float)c * strip_w;
            if (shadow_clip[c]) {
                ImDrawList_AddRectFilled(dl,
                    (ImVec2_c){ x_shadow, origin.y },
                    (ImVec2_c){ x_shadow + strip_w, origin.y + plot_h },
                    clip_colors[c] & 0x60FFFFFF, 0.0f, 0);
            }
            float x_high = origin.x + plot_w - (float)(3 - c) * strip_w;
            if (highlight_clip[c]) {
                ImDrawList_AddRectFilled(dl,
                    (ImVec2_c){ x_high, origin.y },
                    (ImVec2_c){ x_high + strip_w, origin.y + plot_h },
                    clip_colors[c] & 0x60FFFFFF, 0.0f, 0);
            }
        }
    } else {
        ImVec2_c text_pos = { origin.x + 8.0f, origin.y + 8.0f };
        ImDrawList_AddText_Vec2(dl, text_pos, 0xFF888888, "(no data)", NULL);
    }

    igDummy((ImVec2_c){ plot_w, plot_h });

    igEnd();
}

static void config_window(ap_app *app, ap_photo *photo,
                          ap_edit_stack *stack, int idx)
{
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    if (!e->show_config) return;
    const ap_module *m = ap_module_find(e->module_name);
    if (!m) return;

    char label_buf[AP_EDIT_DISPLAY_LEN];
    const char *display = ap_edit_stack_label_at(stack, idx,
                                                 label_buf, sizeof(label_buf));
    // ###entry_<id> = stable hidden ID derived from the entry's
    // per-session stable id, not its array index. Window state
    // (position, size) follows the entry on drag-reorder.
    char title[128];
    snprintf(title, sizeof(title), "%s###entry_%u", display, e->id);

    ImGuiIO *io = igGetIO_Nil();
    ImVec2_c center = { 0.0f, 0.0f };
    if (io) {
        center.x = io->DisplaySize.x * 0.5f;
        center.y = io->DisplaySize.y * 0.5f;
    }
    igSetNextWindowPos(center, ImGuiCond_Appearing,
                       (ImVec2_c){ 0.5f, 0.5f });

    bool open = true;

    igPushID_Int(idx);
    if (igBegin(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (igSmallButton("Reset")) {
            ap_app_edit_snapshot(app);
            ap_edit_stack_reset(stack, idx);
        }
        igSameLine(0.0f, -1.0f);
        igTextDisabled("double-click a value to reset it");
        igSeparator();

        // Algorithm dropdown — only for modules that genuinely have
        // a choice of algorithms (variant_count > 0). A one-item
        // dropdown on a fixed-behavior module (Transform, Vignette,
        // ...) is itself a "dropdown where it doesn't make sense";
        // when such a module gains real variants the selector
        // appears automatically. Snapshot the slot first so a flip
        // can trigger a graph rebuild — variant switches change
        // shader bytecode, not just push constants.
        bool  has_variants = (m->variant_count > 0 &&
                              m->variant_param_slot >= 0 &&
                              m->variant_param_slot < AP_EDIT_PARAMS_SLOTS);
        float old_variant_val = has_variants
            ? e->params[m->variant_param_slot] : 0.0f;

        if (has_variants) {
            float pre_variant = e->params[m->variant_param_slot];
            ap_module_render_variant_combo(m, e->params);
            if ((int)e->params[m->variant_param_slot] != (int)pre_variant) {
                // Variant changed: snapshot with the pre-change state.
                // Temporarily restore the old variant, snapshot, then put
                // the new value back.
                float post_variant                = e->params[m->variant_param_slot];
                e->params[m->variant_param_slot]  = pre_variant;
                ap_app_edit_snapshot(app);
                e->params[m->variant_param_slot]  = post_variant;
            }
            igSeparator();
        }

        bool want_rebuild     = false;
        bool snapshot_req     = false;

        // Seed the canvas-tool request with the tool currently armed
        // *for this entry* — a module that draws an arm/disarm toggle
        // reads this back to show its pressed state. A tool armed on a
        // different entry reads as NONE here so each entry's button
        // reflects only its own state.
        ap_canvas_tool tool_req =
            (ap_app_canvas_tool_entry(app) == idx)
                ? ap_app_canvas_tool(app) : AP_CANVAS_TOOL_NONE;

        // Capture pre-render state: if a slider drag activates inside
        // render_params, we snapshot from here (before the drag changed
        // any params) so the undo step restores the pre-drag stack.
        ap_edit_stack pre_render = *stack;

        if (m->render_params) {
            ap_module_render_ctx ctx = {
                .image_width        = ap_photo_width(photo),
                .image_height       = ap_photo_height(photo),
                .str_params         = e->str_params,
                .request_rebuild    = &want_rebuild,
                .snapshot_requested = &snapshot_req,
                .request_canvas_tool = &tool_req,
                .file_meta          = ap_photo_file_meta(photo),
                .app                = app,
            };
            ap_module_render_ctx_push(&ctx);
            m->render_params(m, e->params, &ctx);
            ap_module_render_ctx_pop();
        } else {
            igTextDisabled("(no parameters)");
        }

        // Forward a tool change to the app. set_canvas_tool toggles
        // when the same (tool, entry) is re-armed, so a module that
        // writes its own tool every time the button is pressed gets
        // press-on / press-off behaviour for free.
        {
            ap_canvas_tool live =
                (ap_app_canvas_tool_entry(app) == idx)
                    ? ap_app_canvas_tool(app) : AP_CANVAS_TOOL_NONE;
            if (tool_req != live) {
                ap_app_set_canvas_tool(app, tool_req, idx);
            }
        }

        if (snapshot_req && photo) {
            ap_edit_history_snapshot(ap_photo_history(photo), &pre_render);
        }

        if (want_rebuild ||
            (has_variants &&
             (int)e->params[m->variant_param_slot] != (int)old_variant_val)) {
            ap_app_rebuild_photo_graph(app);
        }
    }
    igEnd();
    igPopID();

    if (!open) e->show_config = false;
}

static void entry_config_windows(ap_app *app, ap_photo *photo,
                                 ap_edit_stack *stack)
{
    int n = ap_edit_stack_count(stack);
    for (int i = 0; i < n; i++) {
        config_window(app, photo, stack, i);
    }
}

static void draw_save_as_pipeline_modal(ap_app *app, ap_photo *photo,
                                        const ap_edit_stack *stack)
{
    static bool just_opened = false;
    if (g_save_as_open) {
        igOpenPopup_Str("Save as Pipeline", 0);
        g_save_as_open = false;
        just_opened = true;
    }
    if (!igBeginPopupModal("Save as Pipeline", NULL, 0)) {
        just_opened = false;
        return;
    }
    (void)app;
    (void)photo;

    igText("Name for the new pipeline:");
    igSetNextItemWidth(280.0f);
    if (just_opened) igSetKeyboardFocusHere(0);
    bool enter_in_input = igInputText("##pipeline_name", g_save_as_name,
                                      sizeof(g_save_as_name),
                                      ImGuiInputTextFlags_EnterReturnsTrue,
                                      NULL, NULL);
    // invalidate pending overwrite when the user edits the name
    if (igIsItemEdited()) {
        g_save_as_pending_overwrite = false;
    }

    bool name_ok = g_save_as_name[0] != '\0';

    bool save_clicked = false;
    if (!name_ok) igBeginDisabled(true);
    save_clicked = igButton(g_save_as_pending_overwrite ? "Overwrite"
                                                        : "Save",
                            (ImVec2_c){ 120.0f, 0.0f });
    if (name_ok && (enter_in_input || ap_modal_enter_pressed())) {
        save_clicked = true;
    }
    if (!name_ok) igEndDisabled();
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })
               || ap_modal_esc_pressed();

    if (g_save_status[0]) {
        igSeparator();
        igTextDisabled("%s", g_save_status);
    }

    // The default pipeline is locked: the library re-seeds it on every
    // open, so any overwrite would be silently undone. Steer the user
    // to Duplicate + edit the copy.
    bool is_default_name =
        (strcasecmp(g_save_as_name, "default") == 0);

    if (save_clicked && name_ok) {
        if (is_default_name) {
            snprintf(g_save_status, sizeof(g_save_status),
                     "\"default\" is the built-in baseline and can't be "
                     "overwritten. Save under a different name and set it "
                     "as the library default from the Pipelines panel.");
            g_save_as_pending_overwrite = false;
        } else if (g_save_as_pending_overwrite) {
            ap_pipeline_def existing;
            if (ap_pipeline_get_by_name(g_save_as_name, &existing) == 0) {
                if (ap_pipeline_update(existing.id, NULL, stack) == 0) {
                    snprintf(g_save_status, sizeof(g_save_status),
                             "Pipeline \"%s\" overwritten.",
                             g_save_as_name);
                    igCloseCurrentPopup();
                    g_save_as_pending_overwrite = false;
                } else {
                    snprintf(g_save_status, sizeof(g_save_status),
                             "Overwrite failed.");
                }
            } else {
                // Race: existing row vanished between probe and write.
                snprintf(g_save_status, sizeof(g_save_status),
                         "Pipeline \"%s\" no longer exists; click Save "
                         "to create it fresh.", g_save_as_name);
                g_save_as_pending_overwrite = false;
            }
        } else {
            int64_t new_id = 0;
            if (ap_pipeline_create(g_save_as_name, stack, &new_id) == 0) {
                snprintf(g_save_status, sizeof(g_save_status),
                         "Saved as \"%s\".", g_save_as_name);
                igCloseCurrentPopup();
            } else {
                // Probably a name collision — flip into overwrite mode
                // so the next click confirms.
                ap_pipeline_def probe;
                if (ap_pipeline_get_by_name(g_save_as_name, &probe) == 0) {
                    g_save_as_pending_overwrite = true;
                    snprintf(g_save_status, sizeof(g_save_status),
                             "A pipeline named \"%s\" already exists. "
                             "Click Overwrite to replace it.",
                             g_save_as_name);
                } else {
                    snprintf(g_save_status, sizeof(g_save_status),
                             "Save failed.");
                }
            }
        }
    } else if (cancel) {
        igCloseCurrentPopup();
        g_save_as_pending_overwrite = false;
    }
    just_opened = false;
    igEndPopup();
}

#define APPLY_LIST_MAX 64

static void draw_apply_pipeline_modal(ap_app *app, ap_photo *photo,
                                      ap_edit_stack *stack)
{
    if (g_apply_open) {
        igOpenPopup_Str("Apply Pipeline", 0);
        g_apply_open = false;
    }
    if (!igBeginPopupModal("Apply Pipeline", NULL, 0)) return;

    (void)photo;

    if (g_apply_pending_id < 0) {
        igText("Replace the current edit stack with:");
        igSeparator();

        ap_pipeline_def list[APPLY_LIST_MAX];
        int n = ap_pipeline_list(list, APPLY_LIST_MAX);
        if (n <= 0) {
            igTextDisabled("(no pipelines available)");
        } else {
            for (int i = 0; i < n; i++) {
                char label[AP_PIPELINE_NAME_LEN + 32];
                int entries = ap_edit_stack_count(&list[i].stack);
                snprintf(label, sizeof(label), "%s  (%d edit%s)",
                         list[i].name, entries, entries == 1 ? "" : "s");
                if (igSelectable_Bool(label, false, 0, (ImVec2_c){ 0, 0 })) {
                    // Stage the selection — a confirmation click applies it.
                    g_apply_pending_id = list[i].id;
                    snprintf(g_apply_pending_name, sizeof(g_apply_pending_name),
                             "%s", list[i].name);
                }
            }
        }

        igSeparator();
        if (igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f })
            || ap_modal_esc_pressed()) {
            igCloseCurrentPopup();
        }
    } else {
        igText("Replace the entire edit stack with \"%s\"?",
               g_apply_pending_name);
        igTextDisabled("This overwrites the current edits and cannot be undone.");
        igSeparator();

        // Destructive carve-out: Enter only fires when Apply is
        // explicitly focused. Esc steps back to the selection list.
        bool apply_clicked = igButton("Apply", (ImVec2_c){ 120.0f, 0.0f });
        bool apply_focused = igIsItemFocused();
        igSameLine(0.0f, -1.0f);
        bool back = igButton("Back", (ImVec2_c){ 120.0f, 0.0f })
                 || ap_modal_esc_pressed();

        if (apply_clicked
            || (apply_focused && ap_modal_enter_pressed())) {
            ap_app_edit_snapshot(app);
            if (ap_pipeline_apply_to_stack(g_apply_pending_id, stack) == 0) {
                ap_app_rebuild_photo_graph(app);
                snprintf(g_apply_status, sizeof(g_apply_status),
                         "Applied \"%s\".", g_apply_pending_name);
            } else {
                snprintf(g_apply_status, sizeof(g_apply_status),
                         "Apply failed.");
            }
            g_apply_pending_id = -1;
            igCloseCurrentPopup();
        } else if (back) {
            g_apply_pending_id = -1;
        }
    }

    if (g_apply_status[0]) {
        igSeparator();
        igTextDisabled("%s", g_apply_status);
    }

    igEndPopup();
}

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
    draw_save_as_pipeline_modal(app, photo, stack);
    draw_apply_pipeline_modal(app, photo, stack);
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
