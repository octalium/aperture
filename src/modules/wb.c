#include "module.h"

#include "wb_comp_spv.h"

#include "cimgui.h"

#include <string.h>

// White Balance — variant 0 is "As Shot" (camera-baked multipliers from
// the raw's metadata); variant 1 is "Manual" (R/G/B sliders the user
// drives directly). Both variants share `wb.comp` — the only thing
// that differs is which numbers get packed into the push-constants.
//
// Color-temperature presets (Daylight / Shade / Tungsten / ...) need
// per-camera calibration data the codebase doesn't carry yet; they're
// a follow-up when that data is on the table. Until then, the user
// can pick Manual and tune R/G/B by hand.

typedef struct {
    float wb_mul[4];   // .xyz = R/G/B multipliers; .w unused
} wb_push_t;

enum {
    SLOT_ALGO = 0,
    SLOT_R    = 1,
    SLOT_G    = 2,
    SLOT_B    = 3,
};

enum {
    VARIANT_AS_SHOT = 0,
    VARIANT_MANUAL  = 1,
};

static const float wb_defaults[] = { 0.0f, 1.0f, 1.0f, 1.0f };
static const char *const wb_names[] = {
    "algorithm",
    "manual_r",
    "manual_g",
    "manual_b",
};

static int wb_pack_as_shot(const ap_module *self,
                           const float *params,
                           const char (*str_params)[AP_EDIT_STR_LEN],
                           const ap_raw_metadata *meta,
                           void *push_out)
{
    (void)self;
    (void)params;
    (void)str_params;
    if (!meta) return -1;
    wb_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));
    pc->wb_mul[0] = meta->wb_mul[0];
    pc->wb_mul[1] = meta->wb_mul[1];
    pc->wb_mul[2] = meta->wb_mul[2];
    return 0;
}

static int wb_pack_manual(const ap_module *self,
                          const float *params,
                          const char (*str_params)[AP_EDIT_STR_LEN],
                          const ap_raw_metadata *meta,
                          void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    wb_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));
    pc->wb_mul[0] = params ? params[SLOT_R] : 1.0f;
    pc->wb_mul[1] = params ? params[SLOT_G] : 1.0f;
    pc->wb_mul[2] = params ? params[SLOT_B] : 1.0f;
    return 0;
}

static const ap_module_variant wb_variants[] = {
    [VARIANT_AS_SHOT] = {
        .display_name = "As Shot",
        .spv_data     = wb_comp_spv,
        .spv_size     = wb_comp_spv_size,
        .push_size    = sizeof(wb_push_t),
        .pack_push    = wb_pack_as_shot,
    },
    [VARIANT_MANUAL] = {
        .display_name = "Manual",
        .spv_data     = wb_comp_spv,
        .spv_size     = wb_comp_spv_size,
        .push_size    = sizeof(wb_push_t),
        .pack_push    = wb_pack_manual,
    },
};

static void wb_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    // The algorithm dropdown is drawn centrally by the config window.
    int variant = (int)params[SLOT_ALGO];
    if (variant == VARIANT_MANUAL) {
        ap_module_slider_reset(self, params, "R", SLOT_R, 0.1f, 4.0f, "%.3f");
        ap_module_slider_reset(self, params, "G", SLOT_G, 0.1f, 4.0f, "%.3f");
        ap_module_slider_reset(self, params, "B", SLOT_B, 0.1f, 4.0f, "%.3f");
    } else {
        igTextDisabled("multipliers from the raw's metadata");
    }
}

const ap_module module_wb = {
    .name               = "wb",
    .display_name       = "White Balance",
    .category           = AP_MODULE_COLOR,
    .user_visible       = true,
    .params_count       = 4,
    .params_default     = wb_defaults,
    .params_names       = wb_names,
    .render_params      = wb_render,
    .variant_count      = (int)(sizeof(wb_variants) / sizeof(wb_variants[0])),
    .variants           = wb_variants,
    .variant_param_slot = SLOT_ALGO,
};
