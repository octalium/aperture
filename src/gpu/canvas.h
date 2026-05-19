#ifndef APERTURE_GPU_CANVAS_H
#define APERTURE_GPU_CANVAS_H

#include "gpu/gpu.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// Long-lived graphics pipeline that blits an image onto the swapchain
// attachment with a view transform (zoom/pan, aspect-correct,
// letterboxed). Created once for the lifetime of the window; the
// currently-displayed photo is bound via ap_canvas_set_input.
//
// The pipeline is created against the GPU's current swapchain format
// via VK_KHR_dynamic_rendering.
ap_canvas *ap_canvas_create(ap_gpu *g);
void       ap_canvas_destroy(ap_canvas *canvas);

// Bind a sampled image as the canvas content. Pass view = VK_NULL_HANDLE
// to clear (canvas will record nothing).
void ap_canvas_set_input(ap_canvas *canvas,
                         VkImageView view, VkSampler sampler,
                         int image_width, int image_height);

// Restrict the displayed region to a normalized sub-rect of the input
// image — the "Crop as framing" path. The pipeline still renders the
// full frame; the canvas fits this sub-rect to the window. Defaults
// to the full frame (0,0,1,1). Coords are clamped + kept
// non-degenerate.
void ap_canvas_set_crop(ap_canvas *canvas,
                        float x0, float y0, float x1, float y1);

// View-state controls. Pan units are window pixels; zoom is a
// multiplier on top of fit-to-window (1.0 = fit).
void  ap_canvas_reset_view(ap_canvas *canvas);
void  ap_canvas_pan(ap_canvas *canvas, float dx, float dy);
void  ap_canvas_zoom_at(ap_canvas *canvas, float factor,
                        float anchor_screen_x, float anchor_screen_y,
                        int win_width, int win_height);
void  ap_canvas_set_zoom(ap_canvas *canvas, float zoom,
                         int win_width, int win_height);
float ap_canvas_zoom(const ap_canvas *canvas);

// Records the canvas blit. Caller is responsible for being inside an
// active vkCmdBeginRendering pass with a color attachment that matches
// the canvas's color format. No-op if no input is bound.
void ap_canvas_record(ap_canvas *canvas, VkCommandBuffer cmd,
                      int win_width, int win_height);

#ifdef __cplusplus
}
#endif

#endif
