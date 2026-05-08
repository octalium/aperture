#include "raw.h"

#include "core/log.h"

#include <libraw/libraw.h>

#include <stdlib.h>
#include <string.h>

int ap_raw_load(const char *path, ap_raw_image *out)
{
    libraw_data_t *raw = libraw_init(0);
    if (!raw) {
        AP_ERROR("libraw_init failed");
        return -1;
    }

    int err = libraw_open_file(raw, path);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("libraw_open_file(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    err = libraw_unpack(raw);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("libraw_unpack(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    if (!raw->rawdata.raw_image) {
        AP_ERROR("%s: not a Bayer raw (no raw_image plane); X-Trans / Foveon "
                 "are not yet supported", path);
        libraw_close(raw);
        return -1;
    }

    int raw_w = raw->sizes.raw_width;
    int raw_h = raw->sizes.raw_height;
    int vis_w = raw->sizes.width;
    int vis_h = raw->sizes.height;
    int left  = raw->sizes.left_margin;
    int top   = raw->sizes.top_margin;

    if (vis_w <= 0 || vis_h <= 0 || left + vis_w > raw_w || top + vis_h > raw_h) {
        AP_ERROR("%s: implausible visible region (%dx%d at %d,%d in raw %dx%d)",
                 path, vis_w, vis_h, left, top, raw_w, raw_h);
        libraw_close(raw);
        return -1;
    }

    uint16_t *bayer = malloc((size_t)vis_w * (size_t)vis_h * sizeof(*bayer));
    if (!bayer) {
        AP_ERROR("ap_raw_load: bayer buffer alloc failed (%zu bytes)",
                 (size_t)vis_w * (size_t)vis_h * sizeof(*bayer));
        libraw_close(raw);
        return -1;
    }

    for (int y = 0; y < vis_h; y++) {
        const uint16_t *src = raw->rawdata.raw_image + (size_t)(top + y) * raw_w + left;
        memcpy(bayer + (size_t)y * vis_w, src, (size_t)vis_w * sizeof(*src));
    }

    out->bayer  = bayer;
    out->width  = vis_w;
    out->height = vis_h;

    // Channel map at the visible top-left.
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            out->meta.channel_map[dy * 2 + dx] =
                libraw_COLOR(raw, top + dy, left + dx);
        }
    }

    // Black levels: per-channel cblack[0..3] plus the global offset.
    for (int i = 0; i < 4; i++) {
        out->meta.black_level[i] = (float)raw->color.cblack[i] + (float)raw->color.black;
    }
    out->meta.white_level = (float)raw->color.maximum;

    // Use LibRaw's pre-computed rgb_cam (post-WB camera → sRGB-linear)
    // and pre_mul (the WB that pairs with it). These are derived inside
    // LibRaw from cam_xyz the same way dcraw does — using LibRaw's
    // values directly is the reliable path. rgb_cam is [3][4]; for
    // Bayer we use the leading [3][3].
    for (int i = 0; i < 3; i++) {
        for (int r = 0; r < 3; r++) {
            out->meta.cam_to_srgb[i][r] = raw->color.rgb_cam[i][r];
        }
    }

    for (int i = 0; i < 4; i++) {
        out->meta.wb_mul[i] = raw->color.pre_mul[i];
    }
    if (out->meta.wb_mul[3] == 0.0f) {
        out->meta.wb_mul[3] = out->meta.wb_mul[1];
    }
    // Normalize so the green multiplier is 1.0 (matches the shader's
    // assumption and keeps render values in a stable scale).
    if (out->meta.wb_mul[1] != 0.0f) {
        float g = out->meta.wb_mul[1];
        for (int i = 0; i < 4; i++) {
            out->meta.wb_mul[i] /= g;
        }
        // rgb_cam is calibrated against unnormalized pre_mul; rescale
        // the matrix by the inverse of the green factor we just divided
        // out so the shader's WB step cancels properly.
        for (int i = 0; i < 3; i++) {
            for (int r = 0; r < 3; r++) {
                out->meta.cam_to_srgb[i][r] *= g;
            }
        }
    }

    AP_INFO("loaded raw: %s (%dx%d, %s %s)",
            path, vis_w, vis_h,
            raw->idata.make, raw->idata.model);

    libraw_close(raw);
    return 0;
}

void ap_raw_image_free(ap_raw_image *img)
{
    if (!img) return;
    free(img->bayer);
    img->bayer  = NULL;
    img->width  = 0;
    img->height = 0;
}
