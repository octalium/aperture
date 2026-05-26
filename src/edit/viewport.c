#include "edit/viewport.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ap_viewport ap_viewport_identity(void)
{
    ap_viewport vp = {
        .crop_x0 = 0.0f, .crop_y0 = 0.0f,
        .crop_x1 = 1.0f, .crop_y1 = 1.0f,
        .rotation_deg = 0.0f,
        .flip_x = 0, .flip_y = 0,
        .scale_x = 1.0f, .scale_y = 1.0f,
    };
    return vp;
}

void ap_viewport_output_size(const ap_viewport *vp,
                             int src_w, int src_h,
                             int *out_w, int *out_h)
{
    // Output dimensions are the crop's pixel size — scale does NOT
    // change resolution. Scale stretches the content in place within
    // this fixed frame (see backward_map).
    float cw = (vp->crop_x1 - vp->crop_x0) * (float)src_w;
    float ch = (vp->crop_y1 - vp->crop_y0) * (float)src_h;
    int w = (int)(cw + 0.5f);
    int h = (int)(ch + 0.5f);
    if (out_w) *out_w = w > 1 ? w : 1;
    if (out_h) *out_h = h > 1 ? h : 1;
}

float ap_viewport_autozoom(const ap_viewport *vp, int src_w, int src_h)
{
    if (src_w <= 0 || src_h <= 0) return 1.0f;
    float rad = vp->rotation_deg * (float)(M_PI / 180.0);
    float ca = fabsf(cosf(rad));
    float sa = fabsf(sinf(rad));
    float fw = (float)src_w;
    float fh = (float)src_h;
    float sx = (fw * ca + fh * sa) / fw;
    float sy = (fw * sa + fh * ca) / fh;
    return sx > sy ? sx : sy;
}

// Backward map: framed output pixel -> source pixel. Mirrors the
// canvas fragment shader so on-screen + exported framing match.
static void backward_map(const ap_viewport *vp, int src_w, int src_h,
                         int out_w, int out_h, float zoom,
                         float ox, float oy,
                         float *src_x, float *src_y)
{
    float tx = (ox + 0.5f) / (float)out_w;
    float ty = (oy + 0.5f) / (float)out_h;
    if (vp->flip_x) tx = 1.0f - tx;
    if (vp->flip_y) ty = 1.0f - ty;

    // Scale: stretch the content in place about the frame center.
    // >1 enlarges (samples a sub-range of the crop), <1 shrinks
    // (samples beyond the crop). Resolution is unchanged.
    float sx = vp->scale_x > 1e-4f ? vp->scale_x : 1e-4f;
    float sy = vp->scale_y > 1e-4f ? vp->scale_y : 1e-4f;
    tx = 0.5f + (tx - 0.5f) / sx;
    ty = 0.5f + (ty - 0.5f) / sy;

    // Into the crop sub-rect of the rotated frame.
    float rfx = vp->crop_x0 + tx * (vp->crop_x1 - vp->crop_x0);
    float rfy = vp->crop_y0 + ty * (vp->crop_y1 - vp->crop_y0);

    // Orientation backward map: rotate in pixel space, un-zoom.
    float cx = (rfx - 0.5f) * (float)src_w / zoom;
    float cy = (rfy - 0.5f) * (float)src_h / zoom;
    float rad = -vp->rotation_deg * (float)(M_PI / 180.0);
    float cs = cosf(rad);
    float sn = sinf(rad);
    *src_x = (cx * cs - cy * sn) + (float)src_w * 0.5f;
    *src_y = (cx * sn + cy * cs) + (float)src_h * 0.5f;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// fx/fy are in continuous pixel space — the center of texel i sits at
// i + 0.5, so an exact texel-center coordinate resamples losslessly.
static void sample_bilinear(const uint8_t *src, int sw, int sh,
                            float fx, float fy, uint8_t *out_px)
{
    float gx = fx - 0.5f;
    float gy = fy - 0.5f;
    int x0 = (int)floorf(gx);
    int y0 = (int)floorf(gy);
    float dx = gx - (float)x0;
    float dy = gy - (float)y0;

    int xa = clampi(x0,     0, sw - 1);
    int xb = clampi(x0 + 1, 0, sw - 1);
    int ya = clampi(y0,     0, sh - 1);
    int yb = clampi(y0 + 1, 0, sh - 1);

    const uint8_t *p00 = src + ((size_t)ya * sw + xa) * 4;
    const uint8_t *p10 = src + ((size_t)ya * sw + xb) * 4;
    const uint8_t *p01 = src + ((size_t)yb * sw + xa) * 4;
    const uint8_t *p11 = src + ((size_t)yb * sw + xb) * 4;
    for (int c = 0; c < 4; c++) {
        float top = (float)p00[c] * (1.0f - dx) + (float)p10[c] * dx;
        float bot = (float)p01[c] * (1.0f - dx) + (float)p11[c] * dx;
        float v   = top * (1.0f - dy) + bot * dy;
        out_px[c] = (uint8_t)(v + 0.5f);
    }
}

uint8_t *ap_viewport_resample_rgba8(const ap_viewport *vp,
                                    const uint8_t *src,
                                    int src_w, int src_h,
                                    int *out_w, int *out_h)
{
    int ow, oh;
    ap_viewport_output_size(vp, src_w, src_h, &ow, &oh);
    uint8_t *dst = malloc((size_t)ow * (size_t)oh * 4u);
    if (!dst) return NULL;

    float zoom = ap_viewport_autozoom(vp, src_w, src_h);
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            float sx, sy;
            backward_map(vp, src_w, src_h, ow, oh, zoom,
                         (float)x, (float)y, &sx, &sy);
            sample_bilinear(src, src_w, src_h, sx, sy,
                            dst + ((size_t)y * ow + x) * 4);
        }
    }
    if (out_w) *out_w = ow;
    if (out_h) *out_h = oh;
    return dst;
}
