#include "module.h"

#include "profile_comp_spv.h"

#include "color/icc.h"
#include "cimgui.h"

#include <string.h>

typedef struct {
    float cam_to_srgb_r0[4];
    float cam_to_srgb_r1[4];
    float cam_to_srgb_r2[4];
} profile_push_t;

enum { SLOT_ALGO = 0 };

static const float       profile_defaults[]  = { 0.0f };
static const char *const profile_names[]      = { "algorithm" };
static const char *const profile_str_names[]  = { "profile_path" };
enum { STR_PROFILE_PATH = 0 };

// Matrix source:
//   Camera Native — LibRaw's per-camera matrix (rgb_cam) from the raw
//                   metadata. Always available; the fallback.
//   ICC Profile   — the camera->linear-sRGB matrix derived from a
//                   matrix/shaper .icc at `profile_path`. Falls back
//                   to Camera Native when the file is missing or is
//                   not a matrix profile.
static const char *const profile_algorithms[] = {
    "Camera Native",
    "ICC Profile",
};
enum { ALGO_CAMERA = 0, ALGO_ICC = 1 };
#define PROFILE_ALGO_COUNT \
    ((int)(sizeof(profile_algorithms) / sizeof(profile_algorithms[0])))

// Pack a row-major 3x3 into the vec4-padded push rows.
static void pack_matrix(profile_push_t *pc, const float m[9])
{
    for (int j = 0; j < 3; j++) {
        pc->cam_to_srgb_r0[j] = m[0 + j];
        pc->cam_to_srgb_r1[j] = m[3 + j];
        pc->cam_to_srgb_r2[j] = m[6 + j];
    }
}

static int profile_pack_push(const ap_module *self,
                             const float *params,
                             const char (*str_params)[AP_EDIT_STR_LEN],
                             const ap_raw_metadata *meta,
                             void *push_out)
{
    (void)self;
    if (!meta) return -1;

    profile_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));

    int algo = params ? (int)params[SLOT_ALGO] : ALGO_CAMERA;

    if (algo == ALGO_ICC && str_params && str_params[STR_PROFILE_PATH][0]) {
        float m[9];
        if (ap_icc_camera_matrix(str_params[STR_PROFILE_PATH], m) == 0) {
            pack_matrix(pc, m);
            return 0;
        }
        // Load failed (missing / not a matrix profile) — ap_icc has
        // already logged it; fall through to the camera-native matrix.
    }

    for (int j = 0; j < 3; j++) {
        pc->cam_to_srgb_r0[j] = meta->cam_to_srgb[0][j];
        pc->cam_to_srgb_r1[j] = meta->cam_to_srgb[1][j];
        pc->cam_to_srgb_r2[j] = meta->cam_to_srgb[2][j];
    }
    return 0;
}

static void profile_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)self;
    if (!params) return;
    int algo = (int)params[SLOT_ALGO];
    if (algo < 0 || algo >= PROFILE_ALGO_COUNT) algo = 0;
    if (igCombo_Str_arr("Algorithm", &algo, profile_algorithms,
                        PROFILE_ALGO_COUNT, -1)) {
        params[SLOT_ALGO] = (float)algo;
    }

    // The path field is only meaningful for the ICC source.
    if (algo == ALGO_ICC) {
        igInputText("Profile path", ctx->str_params[STR_PROFILE_PATH],
                    AP_EDIT_STR_LEN, 0, NULL, NULL);
        igTextDisabled("matrix/shaper .icc; camera matrix used if it "
                       "can't be loaded");
    }
}

const ap_module module_profile = {
    .name           = "profile",
    .display_name   = "Color Profile",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .spv_data       = profile_comp_spv,
    .spv_size       = profile_comp_spv_size,
    .push_size      = sizeof(profile_push_t),
    .pack_push      = profile_pack_push,
    .params_count     = 1,
    .params_default   = profile_defaults,
    .params_names     = profile_names,
    .render_params    = profile_render,
    .str_params_count = 1,
    .str_params_names = profile_str_names,
};
