#include "module.h"

#include "wb_comp_spv.h"

#include "cimgui.h"

#include <string.h>

typedef struct {
    float wb_mul[4];   // .xyz = R/G/B multipliers; .w unused
} wb_push_t;

enum { SLOT_ALGO = 0 };

static const float       wb_defaults[] = { 0.0f };
static const char *const wb_names[]    = { "algorithm" };

// Algorithm options. Today only "As Shot" (use the camera-baked
// multipliers from the raw metadata) ships; the slot is plumbed for
// future variants — Daylight, Custom Temp/Tint, Auto, Picker — to
// land without bumping the sidecar shape.
static const char *const wb_algorithms[] = {
    "As Shot",
};
#define WB_ALGO_COUNT \
    ((int)(sizeof(wb_algorithms) / sizeof(wb_algorithms[0])))

static int wb_pack_push(const ap_module *self,
                        const float *params,
                        const ap_raw_metadata *meta,
                        void *push_out)
{
    (void)self;
    (void)params;
    if (!meta) return -1;

    wb_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));
    // G2 has been folded into G upstream in demosaic, so we collapse
    // to R / G / B here. WB is normalized to G = 1 in raw.c.
    pc->wb_mul[0] = meta->wb_mul[0];
    pc->wb_mul[1] = meta->wb_mul[1];
    pc->wb_mul[2] = meta->wb_mul[2];
    return 0;
}

static void wb_render(const ap_module *self, float *params)
{
    (void)self;
    if (!params) return;
    int algo = (int)params[SLOT_ALGO];
    if (algo < 0 || algo >= WB_ALGO_COUNT) algo = 0;
    if (igCombo_Str_arr("Algorithm", &algo, wb_algorithms,
                        WB_ALGO_COUNT, -1)) {
        params[SLOT_ALGO] = (float)algo;
    }
}

const ap_module module_wb = {
    .name           = "wb",
    .display_name   = "White Balance",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .spv_data       = wb_comp_spv,
    .spv_size       = wb_comp_spv_size,
    .push_size      = sizeof(wb_push_t),
    .pack_push      = wb_pack_push,
    .params_count   = 1,
    .params_default = wb_defaults,
    .params_names   = wb_names,
    .render_params  = wb_render,
};
