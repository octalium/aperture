#include "module.h"

#include "tone_comp_spv.h"
#include "tone_filmic_comp_spv.h"

#include "cimgui.h"

// Tone curve module. Two variants:
//   0 — Sigmoid (existing): anchored logistic with contrast + pivot.
//   1 — Filmic (new):       Uncharted 2 / Hable operator with one
//                           \`exposure\` knob; curve constants are
//                           tasteful defaults (more knobs come as a
//                           follow-up variant if the user wants them).
//
// Slot layout keeps Sigmoid at the original positions (0 = contrast,
// 1 = pivot) so old sidecars keep rendering correctly. The variant
// selector lives at slot 7; Filmic-specific params start at slot 8.
// `algorithm` defaults to 0 — existing photos read as Sigmoid with
// their saved contrast/pivot intact.

typedef struct {
    float contrast;
    float pivot;
} tone_sigmoid_push_t;

typedef struct {
    float exposure;
} tone_filmic_push_t;

enum {
    SLOT_CONTRAST  = 0,
    SLOT_PIVOT     = 1,
    SLOT_ALGO      = 7,
    SLOT_FILMIC_EX = 8,
};

enum {
    VARIANT_SIGMOID = 0,
    VARIANT_FILMIC  = 1,
};

static const float tone_defaults[] = {
    /* 0 contrast       */ 1.0f,
    /* 1 pivot          */ 0.18f,
    /* 2 reserved       */ 0.0f,
    /* 3 reserved       */ 0.0f,
    /* 4 reserved       */ 0.0f,
    /* 5 reserved       */ 0.0f,
    /* 6 reserved       */ 0.0f,
    /* 7 algorithm      */ 0.0f,
    /* 8 filmic_exposure*/ 1.0f,
};
static const char *const tone_names[] = {
    "contrast", "pivot",
    "reserved2", "reserved3", "reserved4", "reserved5", "reserved6",
    "algorithm", "filmic_exposure",
};

static int tone_pack_sigmoid(const ap_module *self,
                             const float *params,
                             const ap_raw_metadata *meta,
                             void *push_out)
{
    (void)self;
    (void)meta;
    tone_sigmoid_push_t *pc = push_out;
    pc->contrast = params ? params[SLOT_CONTRAST] : 1.0f;
    pc->pivot    = params ? params[SLOT_PIVOT]    : 0.18f;
    return 0;
}

static int tone_pack_filmic(const ap_module *self,
                            const float *params,
                            const ap_raw_metadata *meta,
                            void *push_out)
{
    (void)self;
    (void)meta;
    tone_filmic_push_t *pc = push_out;
    pc->exposure = params ? params[SLOT_FILMIC_EX] : 1.0f;
    return 0;
}

static const ap_module_variant tone_variants[] = {
    [VARIANT_SIGMOID] = {
        .display_name = "Sigmoid",
        .spv_data     = tone_comp_spv,
        .spv_size     = tone_comp_spv_size,
        .push_size    = sizeof(tone_sigmoid_push_t),
        .pack_push    = tone_pack_sigmoid,
    },
    [VARIANT_FILMIC] = {
        .display_name = "Filmic",
        .spv_data     = tone_filmic_comp_spv,
        .spv_size     = tone_filmic_comp_spv_size,
        .push_size    = sizeof(tone_filmic_push_t),
        .pack_push    = tone_pack_filmic,
    },
};

static void slider_with_reset(const ap_module *self, float *params,
                              const char *label, int slot,
                              float lo, float hi, const char *fmt)
{
    igSliderFloat(label, &params[slot], lo, hi, fmt, 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[slot] = self->params_default[slot];
    }
}

static void tone_render(const ap_module *self, float *params)
{
    if (!params) return;
    ap_module_render_variant_combo(self, params);

    int variant = (int)params[SLOT_ALGO];
    if (variant == VARIANT_FILMIC) {
        slider_with_reset(self, params, "Exposure", SLOT_FILMIC_EX,
                          0.1f, 4.0f, "%.2f");
        igTextDisabled("curve constants are tasteful defaults; more knobs in a follow-up");
    } else {
        slider_with_reset(self, params, "Contrast", SLOT_CONTRAST,
                          0.5f, 4.0f, "%.2f");
        slider_with_reset(self, params, "Pivot", SLOT_PIVOT,
                          0.05f, 0.5f, "%.3f");
    }
}

const ap_module module_tone = {
    .name               = "tone",
    .display_name       = "Tone",
    .category           = AP_MODULE_TONE,
    .user_visible       = true,
    .params_count       = 9,
    .params_default     = tone_defaults,
    .params_names       = tone_names,
    .render_params      = tone_render,
    .variant_count      = (int)(sizeof(tone_variants) / sizeof(tone_variants[0])),
    .variants           = tone_variants,
    .variant_param_slot = SLOT_ALGO,
};
