#include "module.h"

#include "demosaic_comp_spv.h"

#include "cimgui.h"

#include <string.h>

typedef struct {
    uint32_t channel_map[4];
    float    black_level[4];
    float    white_minus_black[4];
    uint32_t sensor_size_flip[4]; // .x = sensor_w, .y = sensor_h, .z = flip
} demosaic_push_t;

enum { SLOT_ALGO = 0 };

static const float       demosaic_defaults[] = { 0.0f };
static const char *const demosaic_names[]    = { "algorithm" };

// Algorithm options. The shader is currently bilinear-3x3 only, so
// the dropdown has a single choice; the slot is plumbed for future
// alternatives (Malvar-He-Cutler, AHD, ...) to land without a
// schema change.
static const char *const demosaic_algorithms[] = {
    "Bilinear 3x3",
};
#define DEMOSAIC_ALGO_COUNT \
    ((int)(sizeof(demosaic_algorithms) / sizeof(demosaic_algorithms[0])))

static int demosaic_pack_push(const ap_module *self,
                              const float *params,
                              const ap_raw_metadata *meta,
                              void *push_out)
{
    (void)self;
    (void)params;
    if (!meta) {
        return -1; // signal "skip" - no metadata, no demosaic.
    }

    demosaic_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));

    for (int i = 0; i < 4; i++) {
        pc->channel_map[i]       = (uint32_t)meta->channel_map[i];
        pc->black_level[i]       = meta->black_level[i];
        float dynamic            = meta->white_level - meta->black_level[i];
        pc->white_minus_black[i] = dynamic > 0.0f ? dynamic : 1.0f;
    }

    pc->sensor_size_flip[0] = (uint32_t)meta->sensor_width;
    pc->sensor_size_flip[1] = (uint32_t)meta->sensor_height;
    pc->sensor_size_flip[2] = (uint32_t)meta->flip;
    pc->sensor_size_flip[3] = 0;
    return 0;
}

static void demosaic_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    (void)self;
    if (!params) return;
    int algo = (int)params[SLOT_ALGO];
    if (algo < 0 || algo >= DEMOSAIC_ALGO_COUNT) algo = 0;
    if (igCombo_Str_arr("Algorithm", &algo, demosaic_algorithms,
                        DEMOSAIC_ALGO_COUNT, -1)) {
        params[SLOT_ALGO] = (float)algo;
    }
}

const ap_module module_demosaic = {
    .name           = "demosaic",
    .display_name   = "Demosaic",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .spv_data       = demosaic_comp_spv,
    .spv_size       = demosaic_comp_spv_size,
    .push_size      = sizeof(demosaic_push_t),
    .pack_push      = demosaic_pack_push,
    .params_count   = 1,
    .params_default = demosaic_defaults,
    .params_names   = demosaic_names,
    .render_params  = demosaic_render,
};
