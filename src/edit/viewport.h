#ifndef APERTURE_EDIT_VIEWPORT_H
#define APERTURE_EDIT_VIEWPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The viewport: how the rendered full-frame image is framed for
// display + export. Crop, rotation, flip, and per-axis scale all
// compose into it. The pixel pipeline (demosaic / color / tone /
// detail) never sees it — the canvas applies it when displaying and
// the export path rasterizes through it. Set by the Transform module
// (src/modules/transform.c).
//
// Rotation auto-zooms so the rotated source still covers the frame
// (the "straighten" behavior) — so any crop sub-rect of the rotated
// frame is border-free by construction.
typedef struct {
    float crop_x0, crop_y0, crop_x1, crop_y1; // normalized [0,1]
    float rotation_deg;
    int   flip_x, flip_y;                     // 0 or 1
    float scale_x, scale_y;                   // per-axis output stretch
} ap_viewport;

// The identity viewport: full frame, no rotation / flip, scale 1.
ap_viewport ap_viewport_identity(void);

// Decode a Transform edit-entry's param array into a viewport.
// Implemented by the Transform module (src/modules/transform.c),
// which owns the param slot layout. Pass NULL for the identity.
ap_viewport ap_transform_viewport(const float *params);

// Framed output dimensions for a given source size. out_w/out_h are
// the crop's pixel size times the per-axis scale, clamped to >= 1.
void ap_viewport_output_size(const ap_viewport *vp,
                             int src_w, int src_h,
                             int *out_w, int *out_h);

// Auto-zoom factor: scale needed so the rotated source rectangle
// still covers the full frame. 1.0 at rotation 0.
float ap_viewport_autozoom(const ap_viewport *vp, int src_w, int src_h);

// Resample an RGBA8 source image through the viewport into a freshly
// malloc'd framed buffer (caller frees). Bilinear. *out_w / *out_h
// receive the framed dimensions. Returns NULL on allocation failure.
uint8_t *ap_viewport_resample_rgba8(const ap_viewport *vp,
                                    const uint8_t *src,
                                    int src_w, int src_h,
                                    int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif
