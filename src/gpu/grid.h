#ifndef APERTURE_GPU_GRID_H
#define APERTURE_GPU_GRID_H

#include "gpu/gpu.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// Long-lived graphics pipeline that paints a contact-sheet grid into
// the swapchain attachment. Cells are placeholder-colored - real
// thumbnail textures arrive in a follow-up.
//
// Owns layout (cells per row, cell size, gap) and selection state.
// Mutually exclusive with ap_canvas: the app binds whichever is
// appropriate for the current mode.
ap_grid *ap_grid_create(ap_gpu *g);
void     ap_grid_destroy(ap_grid *grid);

// Set the number of photos in the current library. Pass 0 to clear.
void ap_grid_set_photo_count(ap_grid *grid, int count);

// Focused cell. Clamped to [0, count); no-op on empty grids. The
// focus is the cell that arrow keys move from and that Enter / Space
// opens. It is always in the selection set as a post-condition of
// any select_* call.
void ap_grid_set_selected(ap_grid *grid, int idx);
int  ap_grid_selected(const ap_grid *grid);
int  ap_grid_photo_count(const ap_grid *grid);

// Multi-selection model. The selection set tracks which cells are
// "marked"; the focus is one specific cell. A plain click maps to
// select_only; Shift-click to select_range from focus to idx;
// Ctrl-click to select_toggle.
void ap_grid_select_only  (ap_grid *grid, int idx);
void ap_grid_select_toggle(ap_grid *grid, int idx);
void ap_grid_select_range (ap_grid *grid, int anchor_idx, int idx);
bool ap_grid_is_selected  (const ap_grid *grid, int idx);
int  ap_grid_selection_count(const ap_grid *grid);

// Number of cells per row at the supplied window dims, derived from
// the grid's own layout knobs.
int  ap_grid_cells_per_row(const ap_grid *grid, int win_width, int win_height);

// Cell-size zoom (pixels per side). Clamped to a sane range.
int  ap_grid_cell_size(const ap_grid *grid);
void ap_grid_set_cell_size(ap_grid *grid, int px);
void ap_grid_reset_cell_size(ap_grid *grid);

// Change cell size and adjust scroll_y_target so the content point
// currently under (screen_x, screen_y) stays stationary after the
// resize. Clamps cell size to the valid range. No-op on empty grids.
void ap_grid_zoom_at(ap_grid *grid, int new_cell_px,
                     float screen_x, float screen_y,
                     int win_width, int win_height);

// Vertical scroll. `dy` is in window pixels - positive scrolls
// content up (reveals content below). Clamps to [0, max_scroll]
// based on the supplied window dims and current photo count.
void ap_grid_scroll(ap_grid *grid, float dy, int win_width, int win_height);

// Adjust scroll so the cell at `idx` is fully visible. No-op if
// already visible or if idx is out of range.
void ap_grid_ensure_visible(ap_grid *grid, int idx,
                            int win_width, int win_height);

// Bind a thumbnail texture into the grid's descriptor array at the
// given slot. Pass view = VK_NULL_HANDLE to revert the slot to the
// shared placeholder. Safe to call mid-frame thanks to
// UPDATE_AFTER_BIND.
void ap_grid_set_thumbnail(ap_grid *grid, int idx,
                           VkImageView view, VkSampler sampler);

// How many full rows fit vertically in the active render rect. Used
// by PageUp / PageDown to advance exactly one viewport of rows.
int ap_grid_rows_per_page(const ap_grid *grid, int win_width, int win_height);

// Advance the eased scroll position and cell size toward their targets.
// Call once per frame before ap_grid_record, passing the frame delta
// time in seconds (from ImGuiIO::DeltaTime).
void ap_grid_update(ap_grid *grid, float dt);

// Set the hovered cell index (or -1 when no cell is under the cursor).
// Called every frame before ap_grid_record so the push constant is
// current when the draw call goes out. The grid uses this to tint the
// hovered cell with a subtle highlight in the fragment shader.
void ap_grid_set_hover(ap_grid *grid, int idx);

// Restrict the grid render + layout to a sub-rect of the framebuffer.
// Used by app.c to fit the grid to the ImGui dockspace's central node,
// so docked panels don't paint over the thumb area and the grid
// reflows when the user resizes panels. Pass w == 0 (or h == 0) to
// clear and revert to full-window rendering. The grid stores absolute
// framebuffer coords; ap_grid_hit_test and ap_grid_cell_rect still
// speak in window-absolute coords on the outside, but apply the
// origin offset internally.
void ap_grid_set_render_rect(ap_grid *grid, int x, int y, int w, int h);

// Hit-test a screen-space point against the grid. Returns the cell
// index, or -1 if the point misses any cell. Layout is recomputed
// each call from the supplied window dims so callers don't have to
// duplicate the math.
int ap_grid_hit_test(const ap_grid *grid,
                     float screen_x, float screen_y,
                     int win_width, int win_height);

// Replace the selection with every cell whose on-screen rect overlaps
// the given screen-space axis-aligned rectangle (normalised internally,
// so sx0/sy0 may be > sx1/sy1). Clears the selection when no cells
// overlap. Used by the rubber-band marquee in drive_grid_input.
void ap_grid_select_rect(ap_grid *grid,
                         float sx0, float sy0, float sx1, float sy1,
                         int win_width, int win_height);

// Compute the on-screen rect of a given cell (for ImGui label
// overlays). Returns 0 on success; -1 if idx is out of range.
int ap_grid_cell_rect(const ap_grid *grid, int idx,
                      int win_width, int win_height,
                      float *out_x, float *out_y,
                      float *out_w, float *out_h);

// Records the grid blit. Caller must be inside an active
// vkCmdBeginRendering pass with a color attachment matching the
// swapchain format.
void ap_grid_record(ap_grid *grid, VkCommandBuffer cmd,
                    int win_width, int win_height);

#ifdef __cplusplus
}
#endif

#endif
