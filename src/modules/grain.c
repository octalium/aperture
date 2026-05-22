#include "module.h"

#include "grain_comp_spv.h"

#include "cimgui.h"

#include <stdlib.h>

typedef struct {
    float amount;
    float size;
    float mid_bias;
    float seed;
} grain_push_t;

enum {
    SLOT_AMOUNT  = 0,
    SLOT_SIZE    = 1,
    SLOT_BIAS    = 2,
    SLOT_SEED    = 3,
};

static const float       grain_defaults[] = { 0.0f, 1.0f, 0.8f, 0.137f };
static const char *const grain_names[]    = { "amount", "size", "mid_bias", "seed" };

static int grain_pack_push(const ap_module *self,
                           const float *params,
                           const char (*str_params)[AP_EDIT_STR_LEN],
                           const ap_raw_metadata *meta,
                           void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    grain_push_t *pc = push_out;
    pc->amount   = params ? params[SLOT_AMOUNT] : 0.0f;
    pc->size     = params ? params[SLOT_SIZE]   : 1.0f;
    pc->mid_bias = params ? params[SLOT_BIAS]   : 0.8f;
    pc->seed     = params ? params[SLOT_SEED]   : 0.137f;
    return 0;
}

static void grain_init_instance(float *params)
{
    // Randomise the seed so two Grain edits on the same or different
    // photos don't produce identical patterns. rand() seeded by the C
    // runtime is sufficient — grain seed is not a security value.
    params[SLOT_SEED] = (float)(rand() % 100000) / 10000.0f;
}

static void grain_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    ap_module_slider_reset(self, params, "Amount",   SLOT_AMOUNT, 0.0f,  0.5f,  "%.3f");
    ap_module_slider_reset(self, params, "Size",     SLOT_SIZE,   1.0f,  8.0f,  "%.1f");
    ap_module_slider_reset(self, params, "Midtones", SLOT_BIAS,   0.0f,  1.0f,  "%.2f");
    ap_module_slider_reset(self, params, "Seed",     SLOT_SEED,   0.0f, 10.0f,  "%.3f");
    igTextDisabled("noise is hash-based; sampled blue-noise lands in a follow-up");
}

const ap_module module_grain = {
    .name           = "grain",
    .display_name   = "Grain",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = grain_comp_spv,
    .spv_size       = grain_comp_spv_size,
    .push_size      = sizeof(grain_push_t),
    .pack_push      = grain_pack_push,
    .params_count   = 4,
    .params_default = grain_defaults,
    .params_names   = grain_names,
    .render_params  = grain_render,
    .init_instance  = grain_init_instance,
};
