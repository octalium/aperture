#ifndef APERTURE_GPU_GRID_H
#define APERTURE_GPU_GRID_H

#include "gpu/gpu.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// Long-lived graphics pipeline that paints a contact-sheet grid into
// the swapchain attachment. Cells are placeholder-colored — real
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
