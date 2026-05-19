#include "panels.h"

#include "edit/stack.h"
#include "modules/module.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
            if (ap_edit_stack_add(stack, m->name) >= 0) {
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

// CPU-sampled 256-bin RGB histogram of the rendered display image. The
// readback path is the same one the photo-close thumbnail uses
// (ap_photo_readback_rgba → thumb image), so this is cheap: a few
// hundred KB and ~75 K bin increments per recompute.
//
// Recomputes when the photo pointer or its pipeline graph pointer
// changes — i.e. photo open / close / swap and structural pipeline
// rebuilds. The change is detected this frame and the actual readback
// is deferred to the next frame so the GPU has finished recording +
// running the new graph at least once; otherwise the readback would
// pull whatever stale pixels happen to sit in the new display image.
//
// Slider-only edits (push-constant updates with no graph rebuild) do
// not move the histogram in v1 — the issue's stated trade-off. A
// follow-up can promote this to a GPU compute pass that writes to a
// persistent storage buffer for true per-frame updates.

#define HIST_BINS 256

static struct {
    const ap_photo          *photo;
    const ap_pipeline_graph *graph;
    uint32_t                 bins[3][HIST_BINS];
    uint32_t                 max_bin;
    bool                     valid;
    bool                     pending;
} g_hist;

static bool g_hist_log_scale = false;

static void histogram_recompute(ap_photo *photo)
{
    g_hist.valid = false;

    uint8_t *rgba = NULL;
    int      w    = 0;
    int      h    = 0;
    if (ap_photo_readback_rgba(photo, &rgba, &w, &h) != 0) {
        return;
    }

    memset(g_hist.bins, 0, sizeof(g_hist.bins));
    int n = w * h;
    for (int i = 0; i < n; i++) {
        g_hist.bins[0][rgba[i * 4 + 0]]++;
        g_hist.bins[1][rgba[i * 4 + 1]]++;
        g_hist.bins[2][rgba[i * 4 + 2]]++;
    }
    free(rgba);

    uint32_t mx = 0;
    for (int c = 0; c < 3; c++) {
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
        // log1p so empty bins still map to 0. Normalise by log1p(max).
        float denom = log1pf((float)max_bin);
        if (denom <= 0.0f) return 0.0f;
        return log1pf((float)count) / denom;
    }
    return (float)count / (float)max_bin;
}

static void histogram_window(ap_photo *photo)
{
    if (!igBegin("Histogram", NULL, 0)) {
        igEnd();
        return;
    }

    ap_pipeline_graph *graph = ap_photo_graph(photo);
    if (photo != g_hist.photo || graph != g_hist.graph) {
        g_hist.photo   = photo;
        g_hist.graph   = graph;
        g_hist.valid   = false;
        g_hist.pending = true;
    } else if (g_hist.pending) {
        // The graph pointer was new last frame; by now the renderer
        // has recorded + submitted at least one frame against it, so
        // the display image holds the pixels we want to sample.
        histogram_recompute(photo);
        g_hist.pending = false;
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
        // Channel colors are ABGR-packed; alpha 0x80 lets overlapping
        // channels mix visually.
        const uint32_t colors[3] = {
            0x800000FF, // R
            0x8000FF00, // G
            0x80FF0000, // B
        };

        // One vertical line per bin per channel. The line covers the
        // full bin width so adjacent bins read as a continuous curve
        // even when the plot is narrower than 256 px.
        float x_step = plot_w / (float)HIST_BINS;
        for (int b = 0; b < HIST_BINS; b++) {
            float x0 = origin.x + (float)b * x_step;
            float x1 = origin.x + (float)(b + 1) * x_step;
            float cx = 0.5f * (x0 + x1);
            for (int c = 0; c < 3; c++) {
                float t = bin_height(g_hist.bins[c][b], g_hist.max_bin);
                if (t <= 0.0f) continue;
                float y = origin.y + plot_h - t * plot_h;
                ImDrawList_AddLine(dl,
                                   (ImVec2_c){ cx, origin.y + plot_h },
                                   (ImVec2_c){ cx, y },
                                   colors[c], 1.0f);
            }
        }
    } else {
        const char *msg = g_hist.pending ? "rendering..." : "(no data)";
        ImVec2_c text_pos = { origin.x + 8.0f, origin.y + 8.0f };
        ImDrawList_AddText_Vec2(dl, text_pos, 0xFF888888, msg, NULL);
    }

    // Reserve layout space so igEnd / surrounding widgets see the
    // plot's footprint.
    igDummy((ImVec2_c){ plot_w, plot_h });

    igEnd();
}

// ---- per-entry config windows ---------------------------------------

static void config_window(ap_photo *photo, ap_edit_stack *stack, int idx)
{
    (void)photo;
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    if (!e->show_config) return;
    const ap_module *m = ap_module_find(e->module_name);
    if (!m) return;

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

    igPushID_Int(idx);
    if (igBegin(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (igSmallButton("Reset")) {
            // Param change only - flows to the GPU via push
            // constants on the next record. No graph rebuild needed.
            ap_edit_stack_reset(stack, idx);
        }
        igSameLine(0.0f, -1.0f);
        igTextDisabled("double-click a value to reset it");
        igSeparator();

        if (m->render_params) {
            m->render_params(m, e->params);
        } else {
            igTextDisabled("(no parameters)");
        }
    }
    igEnd();
    igPopID();

    if (!open) e->show_config = false;
}

static void entry_config_windows(ap_photo *photo, ap_edit_stack *stack)
{
    int n = ap_edit_stack_count(stack);
    for (int i = 0; i < n; i++) {
        config_window(photo, stack, i);
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
    entry_config_windows(photo, stack);
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
