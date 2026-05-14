#include "module.h"

#include "profile_comp_spv.h"

#include "cimgui.h"

#include <string.h>

typedef struct {
    float cam_to_srgb_r0[4];
    float cam_to_srgb_r1[4];
    float cam_to_srgb_r2[4];
} profile_push_t;

enum { SLOT_ALGO = 0 };

static const float       profile_defaults[] = { 0.0f };
static const char *const profile_names[]    = { "algorithm" };

// Today the matrix comes from LibRaw's per-camera profile (rgb_cam).
// Future variants would pick a different source (Adobe Standard, ACES,
// custom ICC, ...) and rewrite the matrix accordingly.
static const char *const profile_algorithms[] = {
    "Camera Native",
};
#define PROFILE_ALGO_COUNT \
    ((int)(sizeof(profile_algorithms) / sizeof(profile_algorithms[0])))

static int profile_pack_push(const ap_module *self,
                             const float *params,
                             const ap_raw_metadata *meta,
                             void *push_out)
{
    (void)self;
    (void)params;
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

static void profile_render(const ap_module *self, float *params)
{
    (void)self;
    if (!params) return;
    int algo = (int)params[SLOT_ALGO];
    if (algo < 0 || algo >= PROFILE_ALGO_COUNT) algo = 0;
    if (igCombo_Str_arr("Algorithm", &algo, profile_algorithms,
                        PROFILE_ALGO_COUNT, -1)) {
        params[SLOT_ALGO] = (float)algo;
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
    .params_count   = 1,
    .params_default = profile_defaults,
    .params_names   = profile_names,
    .render_params  = profile_render,
};
