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

// Selection. Index is clamped to [0, count). No-ops on empty grids.
void ap_grid_set_selected(ap_grid *grid, int idx);
int  ap_grid_selected(const ap_grid *grid);
int  ap_grid_photo_count(const ap_grid *grid);

// Number of cells per row at the supplied window dims, derived from
// the grid's own layout knobs.
int  ap_grid_cells_per_row(const ap_grid *grid, int win_width, int win_height);

// Cell-size zoom (pixels per side). Clamped to a sane range.
int  ap_grid_cell_size(const ap_grid *grid);
void ap_grid_set_cell_size(ap_grid *grid, int px);

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

// Hit-test a screen-space point against the grid. Returns the cell
// index, or -1 if the point misses any cell. Layout is recomputed
// each call from the supplied window dims so callers don't have to
// duplicate the math.
int ap_grid_hit_test(const ap_grid *grid,
                     float screen_x, float screen_y,
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
