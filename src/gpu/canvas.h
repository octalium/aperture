#ifndef APERTURE_GPU_CANVAS_H
#define APERTURE_GPU_CANVAS_H

// Default user-zoom at photo open / view reset. Slightly below 1.0
// (fit-to-window) to leave a visible margin around the image.
#define AP_CANVAS_DEFAULT_ZOOM 0.9f

#include "edit/viewport.h"
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

// Set the viewport — crop / rotation / flip / scale — the canvas
// displays the input image through. The pipeline still renders the
// full frame; the canvas applies this transform at presentation.
// Pass NULL for the identity viewport. See src/edit/viewport.h.
void ap_canvas_set_viewport(ap_canvas *canvas, const ap_viewport *vp);

// Confine the canvas to a sub-rect of the swapchain (the dockspace
// central node) so it fits beside the docked panels rather than behind
// them. Pass w/h <= 0 to render to the full window.
void ap_canvas_set_render_rect(ap_canvas *canvas, int x, int y, int w, int h);

// View-state controls. Pan units are window pixels; zoom is a
// multiplier on top of fit-to-window (1.0 = fit). win_width /
// win_height are needed by ap_canvas_pan for clamp math; pass 0 for
// both to skip clamping (e.g. in tests).
void  ap_canvas_reset_view(ap_canvas *canvas);
void  ap_canvas_pan(ap_canvas *canvas, float dx, float dy,
                    int win_width, int win_height);
void  ap_canvas_zoom_at(ap_canvas *canvas, float factor,
                        float anchor_screen_x, float anchor_screen_y,
                        int win_width, int win_height);
void  ap_canvas_set_zoom(ap_canvas *canvas, float zoom,
                         int win_width, int win_height);
float ap_canvas_zoom(const ap_canvas *canvas);

// Effective scale of the displayed image in screen-pixels per source-pixel,
// combining user zoom and the fit-to-window factor. win_width / win_height
// are the display dimensions. Returns 0 when the canvas has no input.
float ap_canvas_effective_scale(const ap_canvas *canvas,
                                int win_width, int win_height);

// Records the canvas blit. Caller is responsible for being inside an
// active vkCmdBeginRendering pass with a color attachment that matches
// the canvas's color format. No-op if no input is bound.
void ap_canvas_record(ap_canvas *canvas, VkCommandBuffer cmd,
                      int win_width, int win_height);

#ifdef __cplusplus
}
#endif

#endif
