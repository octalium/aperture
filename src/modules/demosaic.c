#include "module.h"

#include "demosaic_comp_spv.h"

#include <string.h>

typedef struct {
    uint32_t channel_map[4];
    float    black_level[4];
    float    wb_mul[4];
    float    white_minus_black[4];
    float    cam_to_srgb_r0[4];
    float    cam_to_srgb_r1[4];
    float    cam_to_srgb_r2[4];
    uint32_t sensor_size_flip[4]; // .x = sensor_w, .y = sensor_h, .z = flip, .w = pad
} demosaic_push_t;

static int demosaic_pack_push(const ap_module *self,
                              const ap_edit_state *edit,
                              const ap_raw_metadata *meta,
                              void *push_out)
{
    (void)self;
    (void)edit;
    if (!meta) {
        return -1; // signal "skip" — no metadata, no demosaic.
    }

    demosaic_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));

    for (int i = 0; i < 4; i++) {
        pc->channel_map[i]       = (uint32_t)meta->channel_map[i];
        pc->black_level[i]       = meta->black_level[i];
        pc->wb_mul[i]            = meta->wb_mul[i];
        float dynamic            = meta->white_level - meta->black_level[i];
        pc->white_minus_black[i] = dynamic > 0.0f ? dynamic : 1.0f;
    }
    for (int j = 0; j < 3; j++) {
        pc->cam_to_srgb_r0[j] = meta->cam_to_srgb[0][j];
        pc->cam_to_srgb_r1[j] = meta->cam_to_srgb[1][j];
        pc->cam_to_srgb_r2[j] = meta->cam_to_srgb[2][j];
    }

    pc->sensor_size_flip[0] = (uint32_t)meta->sensor_width;
    pc->sensor_size_flip[1] = (uint32_t)meta->sensor_height;
    pc->sensor_size_flip[2] = (uint32_t)meta->flip;
    pc->sensor_size_flip[3] = 0;
    return 0;
}

const ap_module module_demosaic = {
    .name         = "demosaic",
    .display_name = "Demosaic",
    .category     = AP_MODULE_COLOR,
    .spv_data     = demosaic_comp_spv,
    .spv_size     = demosaic_comp_spv_size,
    .push_size    = sizeof(demosaic_push_t),
    .pack_push    = demosaic_pack_push,
};
