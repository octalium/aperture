#include "module.h"

#include "exposure_comp_spv.h"
#include "exposure_soft_comp_spv.h"

#include "cimgui.h"

// Exposure ships two variants:
// - **Linear**: classic c *= exp2(EV); clips highlights overflowing 1.0.
// - **Soft**: same midtone gain, but with soft knees at the highlight
//   and shadow ends so the user can roll off blown / crushed regions
//   instead of letting them clip.

typedef struct {
    float exposure_ev;
} exposure_linear_push_t;

typedef struct {
    float exposure_ev;
    float highlight_knee;
    float shadow_knee;
} exposure_soft_push_t;

enum {
    SLOT_ALGO            = 0,
    SLOT_EV              = 1,
    SLOT_HIGHLIGHT_KNEE  = 2,
    SLOT_SHADOW_KNEE     = 3,
};

enum {
    VARIANT_LINEAR = 0,
    VARIANT_SOFT   = 1,
};

static const float       exposure_defaults[] = { 0.0f, 0.0f, 0.0f, 0.0f };
static const char *const exposure_names[]    = {
    "algorithm", "ev", "highlight_knee", "shadow_knee",
};

static int exposure_pack_linear(const ap_module *self,
                                const float *params,
                                const char (*str_params)[AP_EDIT_STR_LEN],
                                const ap_raw_metadata *meta,
                                void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    exposure_linear_push_t *pc = push_out;
    pc->exposure_ev = params ? params[SLOT_EV] : 0.0f;
    return 0;
}

static int exposure_pack_soft(const ap_module *self,
                              const float *params,
                              const char (*str_params)[AP_EDIT_STR_LEN],
                              const ap_raw_metadata *meta,
                              void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    exposure_soft_push_t *pc = push_out;
    pc->exposure_ev    = params ? params[SLOT_EV]             : 0.0f;
    pc->highlight_knee = params ? params[SLOT_HIGHLIGHT_KNEE] : 0.0f;
    pc->shadow_knee    = params ? params[SLOT_SHADOW_KNEE]    : 0.0f;
    return 0;
}

static const ap_module_variant exposure_variants[] = {
    [VARIANT_LINEAR] = {
        .display_name = "Linear",
        .spv_data     = exposure_comp_spv,
        .spv_size     = exposure_comp_spv_size,
        .push_size    = sizeof(exposure_linear_push_t),
        .pack_push    = exposure_pack_linear,
    },
    [VARIANT_SOFT] = {
        .display_name = "Soft",
        .spv_data     = exposure_soft_comp_spv,
        .spv_size     = exposure_soft_comp_spv_size,
        .push_size    = sizeof(exposure_soft_push_t),
        .pack_push    = exposure_pack_soft,
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

static void exposure_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    // The algorithm dropdown is drawn centrally by the config window.
    slider_with_reset(self, params, "EV", SLOT_EV, -5.0f, 5.0f, "%.2f");

    int variant = (int)params[SLOT_ALGO];
    if (variant == VARIANT_SOFT) {
        slider_with_reset(self, params, "Highlight knee", SLOT_HIGHLIGHT_KNEE,
                          0.0f, 1.0f, "%.3f");
        slider_with_reset(self, params, "Shadow knee", SLOT_SHADOW_KNEE,
                          0.0f, 1.0f, "%.3f");
    }
}

const ap_module module_exposure = {
    .name               = "exposure",
    .display_name       = "Exposure",
    .category           = AP_MODULE_TONE,
    .user_visible       = true,
    .params_count       = 4,
    .params_default     = exposure_defaults,
    .params_names       = exposure_names,
    .render_params      = exposure_render,
    .variant_count      = (int)(sizeof(exposure_variants) / sizeof(exposure_variants[0])),
    .variants           = exposure_variants,
    .variant_param_slot = SLOT_ALGO,
};
