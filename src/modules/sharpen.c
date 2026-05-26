#include "module.h"

#include "sharpen_comp_spv.h"
#include "sharpen_blur_h_comp_spv.h"
#include "sharpen_gauss_combine_comp_spv.h"

#include "cimgui.h"

// Sharpen — two variants:
//   0  Unsharp 3x3   — single-pass 3x3 high-pass approximation.
//   1  Gaussian USM  — two-pass separable Gaussian unsharp mask; a
//                      real radius-controlled blur, fewer halo
//                      artefacts. First consumer of the multi-pass
//                      pipeline infrastructure.
//
// Slot 0 is the algorithm selector; 1..3 are the shared params.
// The 3x3 variant ignores radius.

enum {
    SLOT_ALGO      = 0,
    SLOT_AMOUNT    = 1,
    SLOT_THRESHOLD = 2,
    SLOT_RADIUS    = 3,
    SLOT_COUNT     = 4,
};

enum { VARIANT_3X3 = 0, VARIANT_GAUSS = 1 };

static const float sharpen_defaults[SLOT_COUNT] = {
    /* algorithm */ 0.0f,
    /* amount    */ 0.0f,
    /* threshold */ 0.0f,
    /* radius    */ 2.0f,
};
static const char *const sharpen_names[SLOT_COUNT] = {
    "algorithm", "amount", "threshold", "radius",
};

// --- 3x3 single-pass ----------------------------------------------

typedef struct {
    float amount;
    float threshold;
} sharpen_3x3_push_t;

static int sharpen_pack_3x3(const ap_module *self, const float *params,
                            const char (*str_params)[AP_EDIT_STR_LEN],
                            const ap_raw_metadata *meta, void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    sharpen_3x3_push_t *pc = push_out;
    pc->amount    = params ? params[SLOT_AMOUNT]    : 0.0f;
    pc->threshold = params ? params[SLOT_THRESHOLD] : 0.0f;
    return 0;
}

// --- Gaussian USM, two passes -------------------------------------

typedef struct {
    int   radius;
    float sigma;
    float amount;
    float threshold;
} sharpen_gauss_push_t;

static int sharpen_pack_gauss(const ap_module *self, const float *params,
                              const char (*str_params)[AP_EDIT_STR_LEN],
                              const ap_raw_metadata *meta, void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    sharpen_gauss_push_t *pc = push_out;
    int   radius = params ? (int)params[SLOT_RADIUS] : 2;
    if (radius < 1)  radius = 1;
    if (radius > 16) radius = 16;
    pc->radius    = radius;
    pc->sigma     = (float)radius * 0.5f;
    pc->amount    = params ? params[SLOT_AMOUNT]    : 0.0f;
    pc->threshold = params ? params[SLOT_THRESHOLD] : 0.0f;
    return 0;
}

static const ap_module_pass sharpen_gauss_passes[] = {
    {   // pass 0: horizontal blur -> scratch 0
        .spv_data  = sharpen_blur_h_comp_spv,
        .spv_size  = sharpen_blur_h_comp_spv_size,
        .push_size = sizeof(sharpen_gauss_push_t),
        .pack_push = sharpen_pack_gauss,
        .read0     = AP_PASS_BUF_IN,
        .read1     = AP_PASS_BUF_IN,
        .write     = AP_PASS_BUF_SCRATCH0,
    },
    {   // pass 1: vertical blur of scratch 0 + unsharp combine -> out
        .spv_data  = sharpen_gauss_combine_comp_spv,
        .spv_size  = sharpen_gauss_combine_comp_spv_size,
        .push_size = sizeof(sharpen_gauss_push_t),
        .pack_push = sharpen_pack_gauss,
        .read0     = AP_PASS_BUF_SCRATCH0,
        .read1     = AP_PASS_BUF_IN,
        .write     = AP_PASS_BUF_OUT,
    },
};

static const ap_module_variant sharpen_variants[] = {
    {
        .display_name = "Unsharp 3x3",
        .spv_data     = sharpen_comp_spv,
        .spv_size     = sharpen_comp_spv_size,
        .push_size    = sizeof(sharpen_3x3_push_t),
        .pack_push    = sharpen_pack_3x3,
    },
    {
        .display_name  = "Gaussian USM",
        .pass_count    = (int)(sizeof(sharpen_gauss_passes) /
                               sizeof(sharpen_gauss_passes[0])),
        .passes        = sharpen_gauss_passes,
        .scratch_count = 1,
    },
};

static void sharpen_render(const ap_module *self, float *params,
                           const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    ap_module_slider_reset(self, params, "Amount",    SLOT_AMOUNT,    0.0f, 5.0f,  "%.2f");
    ap_module_slider_reset(self, params, "Threshold", SLOT_THRESHOLD, 0.0f, 0.10f, "%.4f");
    if ((int)params[SLOT_ALGO] == VARIANT_GAUSS) {
        ap_module_slider_reset(self, params, "Radius", SLOT_RADIUS, 1.0f, 16.0f, "%.0f");
    }
}

const ap_module module_sharpen = {
    .name               = "sharpen",
    .display_name       = "Sharpen",
    .category           = AP_MODULE_DETAIL,
    .user_visible       = true,
    .params_count       = SLOT_COUNT,
    .params_default     = sharpen_defaults,
    .params_names       = sharpen_names,
    .render_params      = sharpen_render,
    .variant_count      = (int)(sizeof(sharpen_variants) /
                                sizeof(sharpen_variants[0])),
    .variants           = sharpen_variants,
    .variant_param_slot = SLOT_ALGO,
};
