#include "raw.h"

#include "core/log.h"

#include <libraw/libraw.h>

#include <stdlib.h>
#include <string.h>

// Standard D65 sRGB-from-XYZ matrix (Bradford-adapted). Used to compose with
// LibRaw's camera-XYZ matrix to land in linear sRGB primaries.
static const float SRGB_FROM_XYZ[3][3] = {
    {  3.2406f, -1.5372f, -0.4986f },
    { -0.9689f,  1.8758f,  0.0415f },
    {  0.0557f, -0.2040f,  1.0570f },
};

// LibRaw's FC() macro tells us which CFA channel was sampled at (row, col)
// given the packed pattern in idata.filters. 0=R, 1=G, 2=B, 3=G2.
static int filter_channel(uint32_t filters, int row, int col)
{
    return (filters >> ((((row << 1) & 14) + ((col & 1) << 1)))) & 3;
}

// LibRaw's cam_xyz[i][j] holds the contribution of camera channel i to
// XYZ component j. To convert camera-RGB → sRGB-linear we want a 3x3
// matrix M such that srgb = M * cam. That is: srgb_i = sum_j M[i][j] * cam[j],
// where srgb_i = sum_k SRGB_FROM_XYZ[i][k] * xyz_k, and xyz_k = sum_j cam_xyz[j][k] * cam_j.
// Combining: M[i][j] = sum_k SRGB_FROM_XYZ[i][k] * cam_xyz[j][k].
static void compose_cam_to_srgb(float cam_xyz[4][3], float out[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float v = 0.0f;
            for (int k = 0; k < 3; k++) {
                v += SRGB_FROM_XYZ[i][k] * cam_xyz[j][k];
            }
            out[i][j] = v;
        }
    }
}

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

    // Crop the visible region into a fresh tightly-packed buffer so the GPU
    // texture's dimensions match what the user sees.
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

    // Channel map at the visible top-left. The CFA pattern is a 2x2 tile,
    // so we sample (top + 0..1, left + 0..1).
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            out->meta.channel_map[dy * 2 + dx] = filter_channel(
                raw->idata.filters, top + dy, left + dx);
        }
    }

    // Black levels: cblack[0..3] is per-CFA-channel. Some cameras put the
    // common offset in raw->color.black; LibRaw's documented usage adds them.
    for (int i = 0; i < 4; i++) {
        out->meta.black_level[i] = (float)raw->color.cblack[i] + (float)raw->color.black;
    }
    out->meta.white_level = (float)raw->color.maximum;

    // WB multipliers: cam_mul is what LibRaw computes (typically the
    // as-shot WB). cam_mul[3] is the secondary green; if zero (mono-G
    // sensors), reuse cam_mul[1].
    for (int i = 0; i < 4; i++) {
        out->meta.wb_mul[i] = raw->color.cam_mul[i];
    }
    if (out->meta.wb_mul[3] == 0.0f) {
        out->meta.wb_mul[3] = out->meta.wb_mul[1];
    }
    // Normalize so the green multiplier is 1.0.
    if (out->meta.wb_mul[1] != 0.0f) {
        float g = out->meta.wb_mul[1];
        for (int i = 0; i < 4; i++) {
            out->meta.wb_mul[i] /= g;
        }
    }

    compose_cam_to_srgb(raw->color.cam_xyz, out->meta.cam_to_srgb);

    AP_INFO("loaded raw: %s (%dx%d, pattern=[%d,%d,%d,%d], black=%.0f, max=%.0f)",
            path, vis_w, vis_h,
            out->meta.channel_map[0], out->meta.channel_map[1],
            out->meta.channel_map[2], out->meta.channel_map[3],
            out->meta.black_level[0], out->meta.white_level);

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
