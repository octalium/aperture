#include "module.h"

#include "denoise_comp_spv.h"

#include "cimgui.h"

// Single-pass bilateral denoise. Two params — strength (intensity
// sigma) and radius (spatial sigma in pixels). v1 keeps the kernel
// small (max radius 4) to fit a single compute pass; large-radius
// quality lands as a separable two-pass variant under the same
// module via #107's variants[].

typedef struct {
    float strength;
    float radius;
} denoise_push_t;

enum { SLOT_STRENGTH = 0, SLOT_RADIUS = 1 };

static const float       denoise_defaults[] = { 0.0f, 1.0f };
static const char *const denoise_names[]    = { "strength", "radius" };

static int denoise_pack_push(const ap_module *self,
                             const float *params,
                             const ap_raw_metadata *meta,
                             void *push_out)
{
    (void)self;
    (void)meta;
    denoise_push_t *pc = push_out;
    pc->strength = params ? params[SLOT_STRENGTH] : 0.0f;
    pc->radius   = params ? params[SLOT_RADIUS]   : 1.0f;
    return 0;
}

static void slider_with_reset(const ap_module *self, float *params,
                              const char *label, int slot,
                              float lo, float hi, const char *fmt)
{
    igSliderFloat(label, &params[slot], lo, hi, fmt, 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[slot] = self->params_default[slot];
    }
}

static void denoise_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    slider_with_reset(self, params, "Strength", SLOT_STRENGTH, 0.0f, 0.20f, "%.4f");
    slider_with_reset(self, params, "Radius",   SLOT_RADIUS,   1.0f, 4.0f,  "%.1f");
    igTextDisabled("v1 = single-pass bilateral; separable + NLM variants land later");
}

const ap_module module_denoise = {
    .name           = "denoise",
    .display_name   = "Denoise",
    .category       = AP_MODULE_DETAIL,
    .user_visible   = true,
    .spv_data       = denoise_comp_spv,
    .spv_size       = denoise_comp_spv_size,
    .push_size      = sizeof(denoise_push_t),
    .pack_push      = denoise_pack_push,
    .params_count   = 2,
    .params_default = denoise_defaults,
    .params_names   = denoise_names,
    .render_params  = denoise_render,
};
