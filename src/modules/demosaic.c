#include "module.h"

#include "demosaic_comp_spv.h"
#include "demosaic_malvar_comp_spv.h"
#include "demosaic_adaptive_comp_spv.h"
#include "ahd_interp_comp_spv.h"
#include "ahd_select_comp_spv.h"

#include <string.h>

// Demosaic. Three single-pass variants plus one multi-pass:
//   0  Bilinear 3x3 — fast, soft, zipper artefacts on fine detail.
//   1  Malvar 2004  — 5x5 gradient-corrected linear; sharp, cheap.
//   2  Adaptive     — Hamilton-Adams edge-directed green + colour-
//                     difference chroma; best edge behaviour.
//   3  AHD          — three-pass: interpolate both axes, then pick
//                     per pixel the locally more homogeneous result.
//
// The single-pass variants share the same push constants (Bayer
// pattern, black levels, sensor size + EXIF flip) and pack_push; they
// differ only in the interpolation shader. AHD reuses that same push
// layout, additionally signalling the forced axis through the spare
// sensor_size_flip.w slot (see ahd_pack_v).

typedef struct {
    uint32_t channel_map[4];
    float    black_level[4];
    float    white_minus_black[4];
    uint32_t sensor_size_flip[4]; // .x = sensor_w, .y = sensor_h, .z = flip
} demosaic_push_t;

enum { SLOT_ALGO = 0 };

static const float       demosaic_defaults[] = { 0.0f };
static const char *const demosaic_names[]    = { "algorithm" };

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

// AHD. The two interpolation passes share ahd_interp.comp and the
// demosaic push layout; pass 1 forces the vertical axis by stamping
// sensor_size_flip.w. The select pass reads the two candidates and
// needs no parameters.
static int ahd_pack_v(const ap_module *self, const float *params,
                      const ap_raw_metadata *meta, void *push_out)
{
    int rc = demosaic_pack_push(self, params, meta, push_out);
    if (rc != 0) return rc;            // propagate the "no metadata" skip
    ((demosaic_push_t *)push_out)->sensor_size_flip[3] = 1;
    return 0;
}

static const ap_module_pass ahd_passes[] = {
    {   // pass 0: horizontal-axis demosaic -> scratch 0
        .spv_data  = ahd_interp_comp_spv,
        .spv_size  = ahd_interp_comp_spv_size,
        .push_size = sizeof(demosaic_push_t),
        .pack_push = demosaic_pack_push,    // sensor_size_flip.w = 0
        .read0     = AP_PASS_BUF_IN,
        .read1     = AP_PASS_BUF_IN,
        .write     = AP_PASS_BUF_SCRATCH0,
    },
    {   // pass 1: vertical-axis demosaic -> scratch 1
        .spv_data  = ahd_interp_comp_spv,
        .spv_size  = ahd_interp_comp_spv_size,
        .push_size = sizeof(demosaic_push_t),
        .pack_push = ahd_pack_v,            // sensor_size_flip.w = 1
        .read0     = AP_PASS_BUF_IN,
        .read1     = AP_PASS_BUF_IN,
        .write     = AP_PASS_BUF_SCRATCH1,
    },
    {   // pass 2: homogeneity-directed select -> out
        .spv_data  = ahd_select_comp_spv,
        .spv_size  = ahd_select_comp_spv_size,
        .push_size = 0,
        .pack_push = NULL,
        .read0     = AP_PASS_BUF_SCRATCH0,
        .read1     = AP_PASS_BUF_SCRATCH1,
        .write     = AP_PASS_BUF_OUT,
    },
};

static const ap_module_variant demosaic_variants[] = {
    {
        .display_name = "Bilinear 3x3",
        .spv_data     = demosaic_comp_spv,
        .spv_size     = demosaic_comp_spv_size,
        .push_size    = sizeof(demosaic_push_t),
        .pack_push    = demosaic_pack_push,
    },
    {
        .display_name = "Malvar 2004",
        .spv_data     = demosaic_malvar_comp_spv,
        .spv_size     = demosaic_malvar_comp_spv_size,
        .push_size    = sizeof(demosaic_push_t),
        .pack_push    = demosaic_pack_push,
    },
    {
        .display_name = "Adaptive",
        .spv_data     = demosaic_adaptive_comp_spv,
        .spv_size     = demosaic_adaptive_comp_spv_size,
        .push_size    = sizeof(demosaic_push_t),
        .pack_push    = demosaic_pack_push,
    },
    {
        .display_name  = "AHD",
        .pass_count    = (int)(sizeof(ahd_passes) / sizeof(ahd_passes[0])),
        .passes        = ahd_passes,
        .scratch_count = 2,
    },
};

const ap_module module_demosaic = {
    .name               = "demosaic",
    .display_name       = "Demosaic",
    .category           = AP_MODULE_COLOR,
    .user_visible       = true,
    .params_count       = 1,
    .params_default     = demosaic_defaults,
    .params_names       = demosaic_names,
    .render_params      = NULL,  // only param is the algorithm — the
                                 // config window draws that combo.
    .variant_count      = (int)(sizeof(demosaic_variants) /
                                sizeof(demosaic_variants[0])),
    .variants           = demosaic_variants,
    .variant_param_slot = SLOT_ALGO,
};
