#include "module.h"

#include "wb.h"

#include "wb_comp_spv.h"

#include "cimgui.h"

#include <math.h>
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
    if (!params) return;

    // Eyedropper: arm the canvas tool that samples a clicked pixel and
    // solves the multipliers to neutralise it. The button reads as a
    // toggle — pressing it again disarms (the config window treats a
    // re-armed tool that way). The solve switches the entry to Manual,
    // so the picker is offered from either variant.
    bool armed = (*ctx->request_canvas_tool == AP_CANVAS_TOOL_WB_EYEDROPPER);
    if (armed) {
        ImVec4_c on = { 0.20f, 0.45f, 0.70f, 1.0f };
        igPushStyleColor_Vec4(ImGuiCol_Button, on);
    }
    if (igButton(armed ? "Picking — click a neutral pixel"
                       : "Pick neutral (eyedropper)",
                 (ImVec2_c){ 0.0f, 0.0f })) {
        *ctx->request_canvas_tool = armed ? AP_CANVAS_TOOL_NONE
                                          : AP_CANVAS_TOOL_WB_EYEDROPPER;
    }
    if (armed) igPopStyleColor(1);
    igTextDisabled("click a grey / white surface to neutralise it");
    igSeparator();

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

// sRGB EOTF: 8-bit sRGB component -> linear [0,1].
static float srgb_to_linear(float c)
{
    c /= 255.0f;
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

bool ap_wb_apply_neutral_pick(float *params,
                              float sample_r, float sample_g, float sample_b)
{
    if (!params) return false;

    float lr = srgb_to_linear(sample_r);
    float lg = srgb_to_linear(sample_g);
    float lb = srgb_to_linear(sample_b);

    // A near-black pixel carries no usable chroma — refuse rather than
    // amplifying sensor noise into wild multipliers.
    if (lr + lg + lb < 0.003f) return false;

    // Current multipliers. The As Shot variant has none of its own, so
    // start the solve from the camera-neutral 1/1/1 the Manual sliders
    // default to; a second pick then refines off the result.
    int variant = (int)params[SLOT_ALGO];
    float mr = (variant == VARIANT_MANUAL && params[SLOT_R] > 0.0f)
                   ? params[SLOT_R] : 1.0f;
    float mg = (variant == VARIANT_MANUAL && params[SLOT_G] > 0.0f)
                   ? params[SLOT_G] : 1.0f;
    float mb = (variant == VARIANT_MANUAL && params[SLOT_B] > 0.0f)
                   ? params[SLOT_B] : 1.0f;

    // Scale each channel by the correction that drives the sample to
    // grey, anchored on green so overall exposure is unchanged.
    const float eps = 1e-4f;
    float ref = lg > eps ? lg : eps;
    mr *= ref / (lr > eps ? lr : eps);
    mb *= ref / (lb > eps ? lb : eps);

    // The wb shader clamps nothing itself; keep the multipliers inside
    // the Manual sliders' [0.1, 4.0] range so the result stays editable.
    if (mr < 0.1f) mr = 0.1f; else if (mr > 4.0f) mr = 4.0f;
    if (mg < 0.1f) mg = 0.1f; else if (mg > 4.0f) mg = 4.0f;
    if (mb < 0.1f) mb = 0.1f; else if (mb > 4.0f) mb = 4.0f;

    params[SLOT_ALGO] = (float)VARIANT_MANUAL;
    params[SLOT_R]    = mr;
    params[SLOT_G]    = mg;
    params[SLOT_B]    = mb;
    return true;
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
