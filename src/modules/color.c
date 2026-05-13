#include "module.h"

#include "color_comp_spv.h"

#include <string.h>

typedef struct {
    float wb_mul[4];          // .xyz = R/G/B WB; .w unused
    float cam_to_srgb_r0[4];
    float cam_to_srgb_r1[4];
    float cam_to_srgb_r2[4];
} color_push_t;

static int color_pack_push(const ap_module *self,
                           const float *params,
                           const ap_raw_metadata *meta,
                           void *push_out)
{
    (void)self;
    (void)params;
    if (!meta) {
        return -1; // signal "skip" — no metadata, no color profile.
    }

    color_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));

    // G2 (channel 3) gets folded into G upstream in demosaic; the
    // post-demosaic image only carries 3 channels of WB, so we
    // collapse to R / G / B here.
    pc->wb_mul[0] = meta->wb_mul[0];
    pc->wb_mul[1] = meta->wb_mul[1];
    pc->wb_mul[2] = meta->wb_mul[2];

    for (int j = 0; j < 3; j++) {
        pc->cam_to_srgb_r0[j] = meta->cam_to_srgb[0][j];
        pc->cam_to_srgb_r1[j] = meta->cam_to_srgb[1][j];
        pc->cam_to_srgb_r2[j] = meta->cam_to_srgb[2][j];
    }
    return 0;
}

// User-visible but presently parameter-less: the WB multipliers and
// camera-to-sRGB matrix are determined by the raw's metadata. Disable
// in the edit stack to see uncorrected camera-space RGB.
const ap_module module_color = {
    .name           = "color",
    .display_name   = "Color",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .spv_data       = color_comp_spv,
    .spv_size       = color_comp_spv_size,
    .push_size      = sizeof(color_push_t),
    .pack_push      = color_pack_push,
};
