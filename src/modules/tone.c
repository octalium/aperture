#include "module.h"

#include "tone_comp_spv.h"
#include "tone_filmic_comp_spv.h"

#include "cimgui.h"

// Tone curve module. Two variants:
//   0 — Sigmoid: anchored logistic with contrast + pivot.
//   1 — Filmic:  Uncharted 2 / Hable operator. All curve constants
//                (shoulder / linear / toe knees + linear white) are
//                user-facing sliders; defaults reproduce Hable's
//                original numbers.
//
// Slot layout keeps Sigmoid at the original positions (0 = contrast,
// 1 = pivot) so old sidecars keep rendering. Variant selector at
// slot 7; Filmic params 8..15. `algorithm` defaults to 0.

typedef struct {
    float contrast;
    float pivot;
} tone_sigmoid_push_t;

typedef struct {
    float exposure;
    float A, B, C, D, E, F, W;
} tone_filmic_push_t;

enum {
    SLOT_CONTRAST  = 0,
    SLOT_PIVOT     = 1,
    SLOT_ALGO      = 7,
    SLOT_FILMIC_EX = 8,
    SLOT_FILMIC_A  = 9,
    SLOT_FILMIC_B  = 10,
    SLOT_FILMIC_C  = 11,
    SLOT_FILMIC_D  = 12,
    SLOT_FILMIC_E  = 13,
    SLOT_FILMIC_F  = 14,
    SLOT_FILMIC_W  = 15,
};

enum {
    VARIANT_SIGMOID = 0,
    VARIANT_FILMIC  = 1,
};

static const float tone_defaults[] = {
    /*  0 contrast        */ 1.0f,
    /*  1 pivot           */ 0.18f,
    /*  2 reserved        */ 0.0f,
    /*  3 reserved        */ 0.0f,
    /*  4 reserved        */ 0.0f,
    /*  5 reserved        */ 0.0f,
    /*  6 reserved        */ 0.0f,
    /*  7 algorithm       */ 0.0f,
    /*  8 filmic_exposure */ 1.0f,
    /*  9 filmic_A        */ 0.15f,
    /* 10 filmic_B        */ 0.50f,
    /* 11 filmic_C        */ 0.10f,
    /* 12 filmic_D        */ 0.20f,
    /* 13 filmic_E        */ 0.02f,
    /* 14 filmic_F        */ 0.30f,
    /* 15 filmic_W        */ 11.2f,
};
static const char *const tone_names[] = {
    "contrast", "pivot",
    "reserved2", "reserved3", "reserved4", "reserved5", "reserved6",
    "algorithm", "filmic_exposure",
    "filmic_shoulder", "filmic_linear_strength", "filmic_linear_angle",
    "filmic_toe_strength", "filmic_toe_num", "filmic_toe_denom",
    "filmic_white",
};

static int tone_pack_sigmoid(const ap_module *self,
                             const float *params,
                             const char (*str_params)[AP_EDIT_STR_LEN],
                             const ap_raw_metadata *meta,
                             void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    tone_sigmoid_push_t *pc = push_out;
    pc->contrast = params ? params[SLOT_CONTRAST] : 1.0f;
    pc->pivot    = params ? params[SLOT_PIVOT]    : 0.18f;
    return 0;
}

static int tone_pack_filmic(const ap_module *self,
                            const float *params,
                            const char (*str_params)[AP_EDIT_STR_LEN],
                            const ap_raw_metadata *meta,
                            void *push_out)
{
    (void)str_params;
    (void)meta;
    tone_filmic_push_t *pc = push_out;
    const float *d = self->params_default;
    pc->exposure = params ? params[SLOT_FILMIC_EX] : d[SLOT_FILMIC_EX];
    pc->A = params ? params[SLOT_FILMIC_A] : d[SLOT_FILMIC_A];
    pc->B = params ? params[SLOT_FILMIC_B] : d[SLOT_FILMIC_B];
    pc->C = params ? params[SLOT_FILMIC_C] : d[SLOT_FILMIC_C];
    pc->D = params ? params[SLOT_FILMIC_D] : d[SLOT_FILMIC_D];
    pc->E = params ? params[SLOT_FILMIC_E] : d[SLOT_FILMIC_E];
    pc->F = params ? params[SLOT_FILMIC_F] : d[SLOT_FILMIC_F];
    pc->W = params ? params[SLOT_FILMIC_W] : d[SLOT_FILMIC_W];
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

static void tone_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;

    int variant = (int)params[SLOT_ALGO];
    if (variant == VARIANT_FILMIC) {
        slider_with_reset(self, params, "Exposure",        SLOT_FILMIC_EX, 0.1f,  4.0f,  "%.2f");
        slider_with_reset(self, params, "Shoulder",        SLOT_FILMIC_A,  0.01f, 1.0f,  "%.3f");
        slider_with_reset(self, params, "Linear strength", SLOT_FILMIC_B,  0.01f, 1.0f,  "%.3f");
        slider_with_reset(self, params, "Linear angle",    SLOT_FILMIC_C,  0.0f,  1.0f,  "%.3f");
        slider_with_reset(self, params, "Toe strength",    SLOT_FILMIC_D,  0.0f,  1.0f,  "%.3f");
        slider_with_reset(self, params, "Toe numerator",   SLOT_FILMIC_E,  0.0f,  0.2f,  "%.3f");
        slider_with_reset(self, params, "Toe denominator", SLOT_FILMIC_F,  0.05f, 1.0f,  "%.3f");
        slider_with_reset(self, params, "Linear white",    SLOT_FILMIC_W,  1.0f,  20.0f, "%.1f");
    } else {
        slider_with_reset(self, params, "Contrast", SLOT_CONTRAST, 0.5f,  4.0f,  "%.2f");
        slider_with_reset(self, params, "Pivot",    SLOT_PIVOT,    0.05f, 0.5f,  "%.3f");
    }
}

const ap_module module_tone = {
    .name               = "tone",
    .display_name       = "Tone",
    .category           = AP_MODULE_TONE,
    .user_visible       = true,
    .params_count       = 16,
    .params_default     = tone_defaults,
    .params_names       = tone_names,
    .render_params      = tone_render,
    .variant_count      = (int)(sizeof(tone_variants) / sizeof(tone_variants[0])),
    .variants           = tone_variants,
    .variant_param_slot = SLOT_ALGO,
};
