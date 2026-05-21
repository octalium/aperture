#include "module.h"

#include "profile_comp_spv.h"
#include "profile_lut_comp_spv.h"

#include "color/icc.h"
#include "cimgui.h"

#include <string.h>

// Color Profile — two variants:
//   0  Camera Native — LibRaw's per-camera 3x3 matrix (rgb_cam) from
//                      the raw metadata, applied as a matrix transform.
//   1  ICC Profile   — an .icc profile baked into a 3D colour LUT.
//                      One LUT path covers matrix and cLUT profiles
//                      alike; trilinear interpolation reproduces a
//                      matrix transform exactly within [0,1].
//
// Both output linear sRGB-primaries RGB; the downstream output-transfer
// stage applies the display EOTF.

typedef struct {
    float cam_to_srgb_r0[4];
    float cam_to_srgb_r1[4];
    float cam_to_srgb_r2[4];
} profile_push_t;

enum { SLOT_ALGO = 0 };
enum { ALGO_CAMERA = 0, ALGO_ICC = 1 };
enum { STR_PROFILE_PATH = 0 };

static const float       profile_defaults[]  = { 0.0f };
static const char *const profile_names[]     = { "algorithm" };
static const char *const profile_str_names[] = { "profile_path" };

// --- Camera Native: matrix from raw metadata -----------------------

static int profile_pack_push(const ap_module *self, const float *params,
                             const char (*str_params)[AP_EDIT_STR_LEN],
                             const ap_raw_metadata *meta, void *push_out)
{
    (void)self;
    (void)params;
    (void)str_params;
    if (!meta) return -1;

    profile_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));
    for (int j = 0; j < 3; j++) {
        pc->cam_to_srgb_r0[j] = meta->cam_to_srgb[0][j];
        pc->cam_to_srgb_r1[j] = meta->cam_to_srgb[1][j];
        pc->cam_to_srgb_r2[j] = meta->cam_to_srgb[2][j];
    }
    return 0;
}

// --- ICC Profile: bake the .icc into a 3D LUT ----------------------

static int profile_bake_lut(const float *params,
                            const char (*str_params)[AP_EDIT_STR_LEN],
                            const ap_raw_metadata *meta, float *out_lut)
{
    (void)params;
    (void)meta;
    if (!str_params || !str_params[STR_PROFILE_PATH][0]) {
        return -1;   // no path set — graph substitutes an identity LUT
    }
    return ap_icc_bake_lut(str_params[STR_PROFILE_PATH], out_lut);
}

static const ap_module_variant profile_variants[] = {
    {
        .display_name = "Camera Native",
        .spv_data     = profile_comp_spv,
        .spv_size     = profile_comp_spv_size,
        .push_size    = sizeof(profile_push_t),
        .pack_push    = profile_pack_push,
    },
    {
        .display_name = "ICC Profile",
        .spv_data     = profile_lut_comp_spv,
        .spv_size     = profile_lut_comp_spv_size,
        .bake_lut     = profile_bake_lut,
    },
};

static void profile_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)self;
    if (!params) return;

    // The variant combo is drawn by the panel; only the ICC variant
    // has a parameter of its own — the profile path.
    if ((int)params[SLOT_ALGO] != ALGO_ICC) return;

    igInputText("Profile path", ctx->str_params[STR_PROFILE_PATH],
                AP_EDIT_STR_LEN, 0, NULL, NULL);
    // Re-bake the LUT once the path edit is committed (Enter / focus
    // loss) — the LUT is built when the graph is assembled.
    if (igIsItemDeactivatedAfterEdit()) {
        *ctx->request_rebuild = true;
    }
    igTextDisabled(".icc profile (matrix or cLUT); pass-through if it "
                   "can't be loaded");
}

const ap_module module_profile = {
    .name           = "profile",
    .display_name   = "Color Profile",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .params_count       = 1,
    .params_default     = profile_defaults,
    .params_names       = profile_names,
    .render_params      = profile_render,
    .str_params_count   = 1,
    .str_params_names   = profile_str_names,
    .variant_count      = (int)(sizeof(profile_variants) /
                                sizeof(profile_variants[0])),
    .variants           = profile_variants,
    .variant_param_slot = SLOT_ALGO,
};
